#include "pkgactions.h"
#include "noderefs.h"

#include "pkgcanvas.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "authoringsessionwindow.h"
#include "asyncwork.h"
#include "vgdelta.h"
#include "bytesource.h"
#include "commonutils.h"

#include <QDir>
#include <QImageReader>
#include <QCoreApplication>
#include <QFileDialog>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#include <iterator>
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <zip.h>
#include <filesystem>
#include <fstream>
#include <functional>

using json = nlohmann::ordered_json;
#include "pkgactions_p.h"

//Up to Max bytes of a program's start (or all of a small file): enough to tell what it runs on.
//GOG's placeholders -> anchors; "" when the placeholder means nothing here. In a value or a file's text a Windows
//path is wanted (:winpath); in an address, the plain anchor.
static std::string GogPath(std::string P, bool Win)
{
    static const std::vector<std::pair<std::string, std::string>> Places = {
        {"{app}", "GameDir"}, {"{userdocs}", "Documents"}, {"{userappdata}", "AppData"},
        {"{localappdata}", "LocalAppData"}, {"{commonappdata}", "ProgramData"}, {"{userprofile}", "UserProfile"}};
    //Inno writes a literal "{" as "{{" (a GUID: "{{99797420-...}"): kept aside while the placeholders are read.
    static const std::string Brace = "\x01";
    for (size_t At; (At = P.find("{{")) != std::string::npos;) P.replace(At, 2, Brace);
    for (const auto & [Ph, Anchor] : Places)
        for (size_t At; (At = P.find(Ph)) != std::string::npos;)
            P.replace(At, Ph.size(), "%" + Anchor + (Win ? ":winpath" : "") + "%");
    if (P.find('{') != std::string::npos) return {};
    for (size_t At; (At = P.find(Brace)) != std::string::npos;) P.replace(At, 1, "{");
    if (!Win) std::replace(P.begin(), P.end(), '\\', '/');
    return P;
}

PkgActions::GogSteps PkgActions::GogInstallSteps(const json & Script, const std::filesystem::path & SupportDir, const std::string & Arch)
{
    GogSteps Out;
    if (!Script.is_object() || !Script.contains("actions") || !Script["actions"].is_array()) return Out;
    json Reg = json::object(), Keep = json::object();
    int RegValues = 0;
    std::vector<std::pair<std::string, json>> Inis;       // an ini file's address -> the ConfigWrites GOG sets in it
    for (const json & A : Script["actions"])
    {
        if (!A.is_object() || !A.contains("install") || !A["install"].is_object()) continue;
        //For every language, or English: the package is the English install.
        bool Lang = !A.contains("languages");
        if (A.contains("languages") && A["languages"].is_array())
            for (const json & L : A["languages"]) if (L == "*" || L == "en-US") Lang = true;
        if (!Lang) continue;
        const json & I = A["install"];
        const std::string Act = StrOf(I, "action"), Name = StrOf(A, "name");
        const json Arg = I.contains("arguments") && I["arguments"].is_object() ? I["arguments"] : json::object();
        auto Skip = [&](const std::string & Why) { Out.Skipped.push_back((Name.empty() ? Act : Name) + " - " + Why); };
        if (Act == "saveToFile")
        {
            const std::string At = GogPath(StrOf(Arg, "filename"), false), Text = GogPath(StrOf(Arg, "text"), true);
            if (At.empty() || StrOf(Arg, "text").empty()) { Skip("a file outside the places a package knows"); continue; }
            Out.Layers.push_back(json{{"COMMENT", "GOG's installer writes this file" + (Name.empty() ? std::string() : " (" + Name + ")") + "."},
                                      {"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", Text}} })}, {"TARGET", "FILES/" + At}});
            Out.Said.push_back("writes " + At.substr(At.find_last_of('/') + 1));
        }
        else if (Act == "setRegistry")
        {
            std::string Root = StrOf(Arg, "root"), Sub = StrOf(Arg, "subkey");
            if (Root == "HKEY_LOCAL_MACHINE") Root = "HKLM"; else if (Root == "HKEY_CURRENT_USER") Root = "HKCU";
            if (Sub.find("GOG.com") != std::string::npos) { Skip("GOG Galaxy's own record of the install"); continue; }
            if ((Root != "HKLM" && Root != "HKCU") || Sub.empty()) { Skip("a registry root a package does not write"); continue; }
            std::string Val = StrOf(Arg, "valueData");
            std::string Type = StrOf(Arg, "valueType");
            for (char & C : Type) C = (char)std::tolower((unsigned char)C);
            if (Type == "dword")
            {
                //Inno's "$" is hexadecimal.
                unsigned long N = 0;
                try { N = Val.rfind('$', 0) == 0 ? std::stoul(Val.substr(1), nullptr, 16) : std::stoul(Val); }
                catch (const std::exception &) { Skip("a DWORD that is no number (" + Val + ")"); continue; }
                char B[16]; std::snprintf(B, sizeof B, "dword:%08lx", N); Val = B;
            }
            else if (Type == "string") { Val = GogPath(Val, true); if (Val.empty() && !StrOf(Arg, "valueData").empty()) { Skip("a value naming a place a package does not know"); continue; } }
            else if (Type == "binary")
            {
                //GOG writes the bytes as base64 (CD keys); Wine's .reg spells them "hex:e3,00,...".
                const QByteArray Bytes = QByteArray::fromBase64(QByteArray::fromStdString(Val), QByteArray::AbortOnBase64DecodingErrors);
                if (Bytes.isEmpty() && !Val.empty()) { Skip("binary data that is not base64"); continue; }
                Val = "hex:";
                for (int i = 0; i < Bytes.size(); ++i)
                {
                    char B[4]; std::snprintf(B, sizeof B, i ? ",%02x" : "%02x", (unsigned char)Bytes[i]); Val += B;
                }
            }
            //A step with no value only clears the key before the install writes it: a fresh prefix has nothing to clear.
            else if (Type.empty() && !Arg.contains("valueName")) continue;
            else { Skip("a registry type it does not translate (" + Type + ")"); continue; }
            json * K = &Reg[Root];
            std::string Part;
            std::istringstream Ss(Sub);
            while (std::getline(Ss, Part, '\\')) if (!Part.empty()) K = &(*K)[Part];
            const std::string Vn = StrOf(Arg, "valueName");
            (*K)[Vn.empty() ? "@" : Vn] = Val;
            ++RegValues;
        }
        else if (Act == "supportData")
        {
            const std::string Src = StrOf(Arg, "source"), Dst = GogPath(StrOf(Arg, "target"), false);
            std::string T = StrOf(Arg, "type");
            for (char & C : T) C = (char)std::tolower((unsigned char)C);
            if (T != "file" || Src.rfind("{supportDir}", 0) != 0 || Dst.empty()) { Skip("not a file it can place"); continue; }
            const std::filesystem::path From = SupportDir / Src.substr(13);
            std::ifstream In(From, std::ios::binary);
            const std::string Body((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            if (!In.good() && Body.empty()) { Skip("its file " + Src + " is not beside the game"); continue; }
            const std::string Leaf = Dst.substr(Dst.find_last_of('/') + 1), Dir = Dst.substr(0, Dst.find_last_of('/'));
            if (Body.find('\0') == std::string::npos)
                Out.Layers.push_back(json{{"COMMENT", "GOG's installer copies this default in (" + Leaf + ")."},
                                          {"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", Body}} })}, {"TARGET", "FILES/" + Dst}});
            else
            {
                Out.Copies.push_back({From, Leaf});
                Out.Layers.push_back(json{{"COMMENT", "GOG's installer copies this file in."}, {"FILE", Leaf}, {"TARGET", "FILES/" + Dir}});
            }
            Out.Said.push_back("puts " + Leaf + " in " + Dir);
        }
        else if (Act == "savePath")
        {
            std::string P = GogPath(StrOf(Arg, "savePath"), false);
            if (P.empty()) { Skip("a save folder outside the places a package knows"); continue; }
            if (P.back() != '/') P += '/';
            Keep["FILES/" + P] = true;
            Out.Said.push_back("saves in " + P);
        }
        else if (Act == "execute" || Act == "Execute") Skip("runs " + StrOf(Arg, "executable") + " - a package runs no installer");
        else if (Act == "installSDB") Skip("a Windows compatibility database - Wine applies none");
        else if (Act == "setIni")
        {
            //A line of an ini file: KEY= by its prefix (a ConfigWrite holds no section), the value's places as anchors.
            const std::string At = GogPath(StrOf(Arg, "filename"), false), Key = StrOf(Arg, "keyName"), Raw = StrOf(Arg, "keyValue");
            const std::string Val = GogPath(Raw, true);
            if (At.empty() || Key.empty()) { Skip("an ini file outside the places a package knows"); continue; }
            if (Val.empty() && !Raw.empty()) { Skip("a value naming a place a package does not know"); continue; }
            //Another machine's drive (a CD's "G:\") means nothing in a package, and the validator refuses it.
            static const std::regex Drive(R"((^|[^A-Za-z0-9])[A-Za-z]:[\\/])");
            if (std::regex_search(Val, Drive)) { Skip("a value naming a drive (" + Raw + ")"); continue; }
            auto It = std::find_if(Inis.begin(), Inis.end(), [&](const auto & F) { return F.first == At; });
            if (It == Inis.end()) It = Inis.insert(Inis.end(), {At, json::array()});
            It->second.push_back(json{{"MODE", "ConfigWrite"}, {"KEY", Key + "="}, {"VALUE", Val}});
        }
        else Skip("\"" + Act + "\" is not translated");
    }
    for (auto & [At, Ops] : Inis)
    {
        const std::string Leaf = At.substr(At.find_last_of('/') + 1);
        Out.Said.push_back("sets " + std::to_string(Ops.size()) + " line" + (Ops.size() == 1 ? "" : "s") + " of " + Leaf);
        Out.Layers.push_back(json{{"COMMENT", "What GOG's installer sets in " + Leaf + "."}, {"EDIT", std::move(Ops)}, {"TARGET", "FILES/" + At}});
    }
    if (RegValues)
    {
        //Before the seeds and the saves: a REG layer is facts, read first.
        //ARCH is the reading program's bitness; unknown, it is left for the author (the validator asks for it).
        json L = json{{"COMMENT", "The registry values GOG's installer writes."}, {"REG", Reg}};
        if (!Arch.empty()) L["ARCH"] = json::array({Arch});
        Out.Layers.insert(Out.Layers.begin(), std::move(L));
        Out.Said.insert(Out.Said.begin(), "sets " + std::to_string(RegValues) + " registry value" + (RegValues == 1 ? "" : "s"));
    }
    if (!Keep.empty()) Out.Layers.push_back(json{{"COMMENT", "Saves and settings, where GOG's installer says the game keeps them."}, {"KEEP", Keep}});
    return Out;
}

//A Unity game's app.info ("<company>\n<product>") -> the KEEP layer for where it saves: its LocalLow folder and its
//PlayerPrefs key. Null when the text is not that.
json PkgActions::UnityKeep(const std::string & AppInfo)
{
    std::istringstream In(AppInfo);
    std::string Company, Product;
    std::getline(In, Company); std::getline(In, Product);
    for (std::string * S : {&Company, &Product})
        while (!S->empty() && (S->back() == '\r' || S->back() == ' ')) S->pop_back();
    if (Company.empty() || Product.empty() || Company.find_first_of("/\\%") != std::string::npos
        || Product.find_first_of("/\\%") != std::string::npos)
        return nullptr;
    return json{{"COMMENT", "The game's saves and settings: a Unity game keeps them under its company and product (" + Company + ", " + Product + ")."},
                {"KEEP", json{{"FILES/%UserProfile%/AppData/LocalLow/" + Company + "/" + Product + "/", true},
                              {"REG/HKCU/Software/" + Company + "/" + Product, true}}}};
}

std::string PkgActions::PlatformOf(const std::string & Name, const std::string & Head)
{
    std::string L = Name;
    for (char & C : L) C = (char)std::tolower((unsigned char)C);
    auto Ends = [&](const char * E) { const size_t N = std::strlen(E); return L.size() >= N && L.compare(L.size() - N, N, E) == 0; };
    if (Head.size() >= 20 && Head.compare(0, 4, "\x7f" "ELF") == 0) return Head[4] == 2 ? "linux64" : "linux32";
    if (Ends(".sh") || Ends(".x86_64") || Ends(".appimage")) return "linux64";
    if (Head.size() >= 64 && Head.compare(0, 2, "MZ") == 0)
    {
        //The PE header's offset is at 0x3C; past it, "PE\0\0" and the machine (0x14c i386, 0x8664 x64).
        const uint32_t Off = (uint8_t)Head[0x3C] | ((uint8_t)Head[0x3D] << 8) | ((uint8_t)Head[0x3E] << 16) | ((uint32_t)(uint8_t)Head[0x3F] << 24);
        if ((size_t)Off + 6 <= Head.size() && Head.compare(Off, 4, std::string("PE\0\0", 4)) == 0)
        {
            const uint16_t Machine = (uint8_t)Head[Off + 4] | ((uint8_t)Head[Off + 5] << 8);
            return Machine == 0x8664 ? "win64" : "win32";
        }
        if ((size_t)Off + 2 <= Head.size() && (Head.compare(Off, 2, "NE") == 0 || Head.compare(Off, 2, "LE") == 0)) return "win32";
        return "dos";                                     // MZ alone: a DOS program
    }
    if (Ends(".com") || Ends(".bat")) return "dos";
    return {};
}

int PkgActions::LikelyProgram(const std::vector<std::string> & Anchored)
{
    //Not the game: installers, uninstallers, crash reporters, redistributables and their setup tools.
    static const char * Not[] = {"unins", "setup", "install", "crash", "report", "vcredist", "dxsetup", "dotnet", "redist",
                                 "directx", "config", "patch", "update", "dosbox", "settings", "dos4gw"};
    std::vector<int> Keep;
    int Shallowest = INT_MAX;
    for (int I = 0; I < (int)Anchored.size(); ++I)
    {
        std::string B = Anchored[(size_t)I].substr(Anchored[(size_t)I].find_last_of('/') + 1);
        for (char & C : B) C = (char)std::tolower((unsigned char)C);
        bool Skip = false;
        for (const char * W : Not) if (B.find(W) != std::string::npos) Skip = true;
        if (Skip) continue;
        Keep.push_back(I);
    }
    //A batch file or .com beside real programs launches one of them (GTA's GTA.BAT runs its DOS build): the programs
    //are the candidates; scripts only when there is nothing else.
    auto Script = [&](int I) {
        std::string B = Anchored[(size_t)I];
        for (char & C : B) C = (char)std::tolower((unsigned char)C);
        return B.size() > 4 && (B.compare(B.size() - 4, 4, ".bat") == 0 || B.compare(B.size() - 4, 4, ".cmd") == 0 || B.compare(B.size() - 4, 4, ".com") == 0);
    };
    if (std::any_of(Keep.begin(), Keep.end(), [&](int I) { return !Script(I); }))
        Keep.erase(std::remove_if(Keep.begin(), Keep.end(), Script), Keep.end());
    for (const int I : Keep)
        Shallowest = std::min(Shallowest, (int)std::count(Anchored[(size_t)I].begin(), Anchored[(size_t)I].end(), '/'));
    //One left, or one alone at the top of the game's folder: that one. Several there: the author picks.
    int Found = -1;
    for (const int I : Keep)
        if ((int)std::count(Anchored[(size_t)I].begin(), Anchored[(size_t)I].end(), '/') == Shallowest)
        { if (Found >= 0) return -1; Found = I; }
    return Found;
}

//Parse a .reg export into editable rows. Pure text in, rows out - no dialogs, no model - so the parser
//(the part with all the traps: UTF-16, continuation lines, deletions, escapes) is directly testable.
std::vector<PkgGraph::RegRow> PkgActions::ParseRegExport(const QString & Text, int * DeletionsOut)
{
    // A binary value is written as `hex:aa,bb,\` with the rest on CONTINUATION lines. Reading line-by-line
    // silently TRUNCATED every such value to its first line (the continuations parse as lines with no '='),
    // producing a plausible-looking but WRONG value. Rejoin them before parsing anything.
    QStringList Lines;
    for (QString Raw : Text.split(QRegularExpression("\r?\n")))
    {
        QString L = Raw.trimmed();
        if (!Lines.isEmpty() && Lines.back().endsWith('\\'))
        {
            Lines.back().chop(1);
            Lines.back() += L;
            continue;
        }
        Lines << L;
    }

    std::vector<PkgGraph::RegRow> Rows;
    QString Section;
    int Deletions = 0;
    for (const QString &Line : Lines)
    {
        if (Line.isEmpty() || Line.startsWith(';')) continue;
        if (Line.startsWith('[') && Line.endsWith(']'))
        {
            Section = Line.mid(1, Line.size() - 2);
            if (Section.startsWith('-')) { Section.clear(); ++Deletions; continue; }   // a deletion section
            //The key ITSELF, flagged: an empty NAME is the key's default value, so without the flag this row
            //shadows a real `@=` line from the same section.
            { PkgGraph::RegRow R{Section.toStdString(), "", ""}; R.KeyOnly = true; Rows.push_back(std::move(R)); }
            continue;
        }
        if (Section.isEmpty()) continue;
        const int Eq = Line.indexOf('=');
        if (Eq <= 0) continue;
        QString Name = Line.left(Eq).trimmed(), Val = Line.mid(Eq + 1).trimmed();
        // `@` in a .reg file IS the key's default value, and on disk that is the EMPTY name — 48 of them live
        // in LAVFilters, and RegistryWrapper writes an empty name back out as `@` (registrywrapper.cpp).
        // Mapping it to the literal name "@" wrote a value CALLED @ instead, which is a different thing that
        // no program reads. (The earlier comment here claimed the opposite and blamed RegRowsInto for
        // dropping empty names — that WAS true, and was the actual bug; it is fixed at the source now.)
        if (Name == "@") Name.clear();
        else if (Name.startsWith('"') && Name.endsWith('"')) Name = Name.mid(1, Name.size() - 2);
        // `"Name"=-` DELETES the value. A RegEdit layer only ever writes, so there is nothing to import — and
        // importing it as the literal string "-" would write exactly the opposite of what the file asks.
        if (Val == "-") { ++Deletions; continue; }
        if (Val.startsWith('"') && Val.endsWith('"')) Val = Val.mid(1, Val.size() - 2);
        Val.replace("\\\"", "\"");                                          // .reg escapes quotes...
        Val.replace("\\\\", "\\");                                          // ...and backslashes
        if (Name.isEmpty() && Val.isEmpty()) continue;
        Rows.push_back({Section.toStdString(), Name.toStdString(), Val.toStdString()});
    }
    if (DeletionsOut) *DeletionsOut = Deletions;
    return Rows;
}
