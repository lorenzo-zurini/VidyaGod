#include "fileedits.h"
#include "persistlayer.h"   // HasSavedCopy — a user-owned file's package default
#include "commonutils.h"   // Log*
#include "varsubst.h"      // %variable% substitution in FileEdit VALUEs (e.g. config_info's %RunnerMount%/%TempPath%)

#include <algorithm>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <vector>

//Collects DLLOVERRIDE values from all DllOverride subcomponents into DLLOverrides.
//The collected strings are later joined and assigned to WINEDLLOVERRIDES in Execute().
//Returns false immediately if any DllOverride subcomponent has a null DLLOVERRIDE field.
bool FileEdits::ProcessDLLOverrides(struct ContainerParams &ContainerParams)
{
    LogOut("FileEdits::ProcessDLLOverrides", "Processing DLL Overrides.");
    bool Ok = true;
    for (const auto &SubComponentJSON : ContainerParams.SubComponentsArray)
    {
        if (SubComponentJSON.value("TYPE", std::string()) != "DllOverride") continue;
        // A null DLLOVERRIDE is malformed package data: report it but keep collecting the valid ones —
        // abandoning the loop mid-way used to silently drop every override after the bad entry.
        if (SubComponentJSON.contains("DLLOVERRIDE") && !SubComponentJSON["DLLOVERRIDE"].is_null())
            ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"]);
        else
        {
            LogErr("FileEdits::ProcessDLLOverrides", "DllOverride with null/missing DLLOVERRIDE — skipped.");
            Ok = false;
        }
    }
    return Ok;
}

//Processes FileEdit subcomponents in two passes, split by the OVERRIDE flag.
//  OverridePass=false (base): writes under BaseDir — the DEFAULTDATA layer, between the component
//    layers and the WRITELAYER, so the user's persisted writes naturally shadow them.
//  OverridePass=true  (override): writes to RuntimePath via the union, COW-ing directly into
//    WRITELAYER — wins unconditionally over any previous state, including the user's.
//BaseDir defaults to RuntimePath (override) / DefPrefixPath (base) when not supplied.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (already done in BuildSubComponentsArray).

//An edit path must stay INSIDE its base. A package arrives from a peer by CID, so FILE is untrusted input, and
//neither the leading-separator strip nor is_absolute() stops "../../../.config/autostart/x.desktop" — nor, on
//Windows, a drive-absolute "C:/Users/..." (no leading separator, and operator/ then discards the base) or a
//drive-relative "C:foo" (resolved against that drive's current directory). Normalising and then requiring the
//result to be under the base covers all of them with one rule.
bool FileEdits::PathEscapesBase(const std::filesystem::path &Joined, const std::filesystem::path &Base,
                            std::filesystem::path &OutNormalised)
{
    OutNormalised = Joined.lexically_normal();
    const std::filesystem::path Rel = OutNormalised.lexically_relative(Base.lexically_normal());
    //Empty means the two share no common root at all (a different drive, or an absolute path that replaced
    //the base). Otherwise the first COMPONENT decides: comparing the first two CHARACTERS instead would
    //refuse a legitimate "..config/settings.ini", whose first component is "..config", not "..".
    if (Rel.empty()) return true;
    return *Rel.begin() == "..";
}

bool FileEdits::LinkLeadsOut(const std::filesystem::path &Base, const std::filesystem::path &Normalised, std::string &Why)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    const fs::path B = Base.lexically_normal(), Rel = Normalised.lexically_normal().lexically_relative(B);
    if (Rel.empty()) { Why = Normalised.string() + " is not under " + B.string(); return true; }
    fs::path Cur = B, Root;
    for (const fs::path &Part : Rel)
    {
        if (Part == ".") continue;
        Cur /= Part;
        const fs::file_type T = fs::symlink_status(Cur, Ec).type();
        if (T == fs::file_type::not_found) return false;   // nothing below a missing folder exists to follow
        if (T == fs::file_type::regular || T == fs::file_type::directory) continue;
        //A FIFO (or socket, device) the game made in a kept folder: opening it would block the launch for good.
        if (T != fs::file_type::symlink) { Why = Cur.string() + " is not a plain file or folder"; return true; }
        const fs::path To = fs::canonical(Cur, Ec);
        if (Ec) { Why = Cur.string() + " is a link that leads nowhere"; return true; }
        if (Root.empty()) Root = fs::canonical(B, Ec);
        const fs::path In = To.lexically_relative(Root);
        if (Ec || In.empty() || *In.begin() == "..") { Why = Cur.string() + " is a link out of " + B.string(); return true; }
    }
    return false;
}

bool FileEdits::ProcessFileEdits(struct ContainerParams &ContainerParams, bool OverridePass,
                                        const std::filesystem::path &BaseDir)
{
    std::filesystem::path BasePath = !BaseDir.empty() ? BaseDir
                                     : (OverridePass ? ContainerParams.RuntimePath : ContainerParams.DefPrefixPath);
    LogOut("FileEdits::ProcessFileEdits",
           std::string(OverridePass ? "Override" : "Base") + " FileEdit pass. Base: " + BasePath.string());

    // FILE and VALUE are %variable%-substituted (like layer TARGETs / runner ENV), so a FileEdit can write runtime-
    // derived content — e.g. proton's config_info marker whose paths are "%RunnerMount%/files/..." / "%TempPath%".
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();

    const std::string PassName = OverridePass ? "OVERRIDE" : "BASE";
    bool Ok = true;
    size_t Attempted = 0, Failed = 0;
    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "FileEdit") continue;
        if (Sub.value("OVERRIDE", false) != OverridePass) continue;

        std::string Mode  = Sub.value("MODE",  std::string());
        std::string File  = Sub.value("FILE",  std::string());
        std::string Value = Sub.value("VALUE", std::string());
        const std::string Key = Sub.value("KEY", std::string());
        VarSubst::StringVariableSubstitution(File,  Vars);
        VarSubst::StringVariableSubstitution(Value, Vars);
        //An edit path is ALWAYS relative to its pass base (spec ch.15), so a path that is not is re-anchored
        //rather than followed. The overwhelmingly common way to get one is the house style
        //"%PrefixRoot%/drive_c/..." under a runner where PrefixRoot is "" (native), which yields
        //"/drive_c/...": BasePath / File then discards the base entirely and the edit lands outside the
        //runtime. Worms 4 shipped exactly that and printed one line nobody read; the launch-matrix fixture
        //reproduced it with every edit failing at once.
        //
        //Tested with a LEADING-SEPARATOR check rather than is_absolute(), because is_absolute() is false for
        //"/drive_c/x" on WINDOWS (a root-directory with no root-name) — so the one case this exists for would
        //not have been caught there at all, while "C:/Users/..." would have been "re-anchored" by a strip loop
        //that removes nothing, logging a claim that never happened.
        {
            const std::string Before = File;
            //Strip ONLY when the absolute path does not already land inside the base. "%RuntimePath%/game/x"
            //substitutes to a real absolute path that IS inside it — stripping that one turned
            //"/home/.../RUNTIME/game/x" into "<base>/home/.../RUNTIME/game/x", a path that exists nowhere,
            //where before it resolved correctly. Only the PrefixRoot-is-empty shape ("/drive_c/...") needs
            //re-anchoring, and that one does escape.
            std::filesystem::path AlreadyInside;
            if (std::filesystem::path(File).is_absolute()
                && !FileEdits::PathEscapesBase(std::filesystem::path(File), BasePath, AlreadyInside))
            {
                File = AlreadyInside.string();
            }
            else
            while (!File.empty() && (File.front() == '/' || File.front() == '\\')) File.erase(File.begin());
            if (File != Before)
                LogWarn("FileEdits::ProcessFileEdits",
                        "FILE '" + Before + "' is root-relative — an edit path is relative to its base, so it "
                        "was re-anchored to '" + File + "' under the " + PassName + " pass base. Author it relative (a leading %variable% "
                        "that is empty on this runner is the usual cause).");
            else if (std::filesystem::path(File).is_absolute())
                LogWarn("FileEdits::ProcessFileEdits",
                        "FILE '" + File + "' is ABSOLUTE and escapes the " + PassName + " pass base — the edit will not land where "
                        "anything reads it. Author it relative to the base.");
        }

        //A Delete removes what the runtime composed beneath it, through the mount: only the override pass has one. And
        //never where the user's state lives — a removal in a kept folder would be stored with their saves.
        if (Mode == "Delete" && (!OverridePass || Sub.contains("KEPT")))
        {
            LogErr("FileEdits::ProcessFileEdits", "Delete of '" + File + "' refused: "
                   + (Sub.contains("KEPT") ? "it reaches the user's kept '" + (Sub["KEPT"].is_string() ? Sub["KEPT"].get<std::string>() : Sub["KEPT"].dump()) + "' (a package never deletes the user's state)"
                                           : std::string("a Delete runs only after the mount (OVERRIDE)")));
            ++Attempted; ++Failed; Ok = false;
            continue;
        }

        //A package default for a user-owned file: the user's saved copy stands once there is one.
        if (Sub.value("IF_UNSAVED", false) && PersistLayer::HasSavedCopy(ContainerParams, File))
        {
            LogOut("FileEdits::ProcessFileEdits", "'" + File + "' is the user's (saved, or deleted by the game in a kept "
                   "folder) — the package default "
                   + (Mode.empty() ? std::string("edit") : Mode) + " is not re-applied.");
            continue;
        }

        std::filesystem::path FilePath;
        if (PathEscapesBase(BasePath / File, BasePath, FilePath))
        {
            LogErr("FileEdits::ProcessFileEdits",
                   "FILE '" + File + "' resolves OUTSIDE the " + PassName + " pass base ('" + BasePath.string()
                   + "') — refused. An edit may only write inside its own base.");
            ++Attempted; ++Failed; Ok = false;
            continue;
        }
        if (std::string Why; LinkLeadsOut(BasePath, FilePath, Why))
        {
            LogErr("FileEdits::ProcessFileEdits", "FILE '" + File + "' is not written: " + Why
                   + " — an edit never writes through a link (a game can leave one in a kept folder).");
            ++Attempted; ++Failed; Ok = false;
            continue;
        }
        ++Attempted;

        //A Delete is judged again by the place it RESOLVED to, not the address it was written as: "videos/../Saves",
        //"./Saves", another anchor naming the same folder or a %variable% segment all reach a kept folder that the
        //lowering's textual check never saw. Removing through a kept folder's passthrough deletes the user's real files.
        if (Mode == "Delete")
            if (std::string Kept = FileEdits::KeptPathMet(ContainerParams, FilePath); !Kept.empty())
            {
                LogErr("FileEdits::ProcessFileEdits", "Delete of '" + File + "' refused: it resolves to the user's kept '"
                       + Kept + "' (a package never deletes the user's state)");
                ++Failed; Ok = false;
                continue;
            }

        bool EditOk = true;
        if (Mode == "ConfigWrite")
            EditOk = FileEdits::ConfigWrite(Key, Value, FilePath);
        else if (Mode == "JsonSet")
            EditOk = FileEdits::JsonSet(Key, Value, FilePath);
        else if (Mode == "XmlSet")
            EditOk = FileEdits::XmlSet(Key, Value, FilePath);
        else if (Mode == "Overwrite")
            EditOk = FileEdits::FileOverwrite(Value, FilePath);
        else if (Mode == "AppendLine")
            EditOk = FileEdits::AppendLine(Value, FilePath);
        else if (Mode == "Delete")
            EditOk = FileEdits::DeletePath(FilePath);
        else
        {
            LogWarn("FileEdits::ProcessFileEdits", "Unknown MODE: '" + Mode + "' — skipping.");
            EditOk = false;
        }
        if (!EditOk)
        {
            ++Failed;
            Ok = false;
            //Name the edit, not just the symptom. "Could not open file for reading" on its own gives no clue
            //WHICH package setting was lost.
            LogErr("FileEdits::ProcessFileEdits",
                   PassName + " FileEdit FAILED — mode=" + (Mode.empty() ? "(none)" : Mode)
                   + (Key.empty() ? std::string() : (" key='" + Key + "'"))
                   + " file='" + FilePath.string() + "'");
            //The single most common cause, and the one that is invisible without being told: a ConfigWrite reads
            //the file before rewriting it, and zip-supplied content only exists once the runtime is mounted.
            if (Mode == "ConfigWrite" && !OverridePass)
                LogErr("FileEdits::ProcessFileEdits",
                       "   ^ ConfigWrite in the BASE pass runs BEFORE the content is mounted, so the file does not "
                       "exist yet. Add \"OVERRIDE\": true to this FileEdit.");
        }
    }
    if (Attempted > 0)
        LogOut("FileEdits::ProcessFileEdits",
               PassName + " pass: " + std::to_string(Attempted - Failed) + "/" + std::to_string(Attempted)
               + " edit(s) applied" + (Failed ? (", " + std::to_string(Failed) + " FAILED") : std::string()));
    return Ok;
}

//Rewrites FilePath in-place: any line whose content starts with Key is replaced by Key+Value.
//All other lines are preserved verbatim. Used to patch INI-style config files where
//the key acts as a line prefix (e.g. "Resolution=") rather than a standalone token.
//Returns false if the file cannot be opened for reading or writing.
bool FileEdits::ConfigWrite(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("FileEdits::ConfigWrite", "FilePath: " + FilePath.string() + " Key: " + Key + " Value: " + Value);
    // Binary I/O throughout (read AND the write below): on Windows a text-mode stream translates '\n' to CRLF on
    // write, so the SAME edit produced different bytes than on Linux — breaking cross-platform determinism (and
    // content hashes) and failing the round-trip tests. Binary writes '\n' verbatim (LF) on every platform and
    // preserves any existing '\r' in a line, matching the Linux behavior packages are authored against.
    std::ifstream inFile(FilePath, std::ios::binary);
    if (!inFile.is_open())
    {
        LogErr("FileEdits::ConfigWrite", "Could not open file for reading: " + FilePath.string());
        return false;
    }

    //Read all lines first so the file can be truncated and rewritten without a temporary file.
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(inFile, line))
    {
        lines.push_back(line);
    }

    inFile.close();

    std::ofstream outFile(FilePath, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
    {
        LogErr("FileEdits::ConfigWrite", "Could not open file for writing: " + FilePath.string());
        return false;
    }

    size_t Matched = 0;
    for (auto& currentLine : lines)
    {
        //Match by prefix: if the line starts with Key, replace the whole line with Key+Value.
        if (currentLine.length() >= Key.length() && currentLine.compare(0, Key.length(), Key) == 0)
        {
            currentLine = Key + Value;
            ++Matched;
            outFile << currentLine << '\n';
        }
        else
        {
            outFile << currentLine << '\n';
        }
    }

    outFile.close();
    //A KEY that matches NOTHING rewrites the file unchanged and reports success — a typo in the key, or a config
    //whose format changed, then silently does nothing at all. This is the same class of failure as the base-pass
    //ConfigWrite: the edit "worked", the setting never applied. Say so.
    if (Matched == 0)
        LogWarn("FileEdits::ConfigWrite",
                "key '" + Key + "' matched NO line in " + FilePath.string()
                + " — the file was rewritten unchanged and this setting had no effect. Check the key spelling "
                  "(it is matched as a literal line PREFIX, including any spaces around '=').");
    return true;
}

namespace {

//How deep a JsonSet may reach and how deep a document it writes: serialising is recursive, so an unbounded depth (a
//package's pointer or value, or a game's own file) would overflow the stack.
constexpr size_t MaxJsonDepth = 128;

size_t JsonDepth(const nlohmann::ordered_json &Root)
{
    size_t Max = 0;
    std::vector<std::pair<const nlohmann::ordered_json *, size_t>> Stack = { { &Root, 1 } };
    while (!Stack.empty())
    {
        const auto [J, D] = Stack.back();
        Stack.pop_back();
        Max = std::max(Max, D);
        if (J->is_structured() && D <= MaxJsonDepth)
            for (const auto &C : *J) Stack.push_back({ &C, D + 1 });
    }
    return Max;
}

}

bool FileEdits::JsonSet(const std::string &Pointer, const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("FileEdits::JsonSet", "FilePath: " + FilePath.string() + " Pointer: " + Pointer + " Value: " + Value);
    nlohmann::ordered_json Doc = nlohmann::ordered_json::object();
    {
        std::ifstream In(FilePath, std::ios::binary);
        const std::string Text = In ? std::string(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()) : std::string();
        if (Text.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            Doc = nlohmann::ordered_json::parse(Text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
            if (Doc.is_discarded())
            {
                LogErr("FileEdits::JsonSet", FilePath.string() + " is not JSON — left as it is.");
                return false;
            }
        }
    }
    nlohmann::ordered_json V = nlohmann::ordered_json::parse(Value, nullptr, /*allow_exceptions=*/false);
    if (V.is_discarded()) V = Value;
    std::string Text;
    try
    {
        const nlohmann::ordered_json::json_pointer Ptr(Pointer);
        if (static_cast<size_t>(std::count(Pointer.begin(), Pointer.end(), '/')) > MaxJsonDepth)
        {
            LogErr("FileEdits::JsonSet", "cannot set '" + Pointer.substr(0, 80) + "...' in " + FilePath.string()
                   + ": the pointer is deeper than " + std::to_string(MaxJsonDepth) + " levels");
            return false;
        }
        //An array is indexed within its bounds (or appended to, at its end or with "-"): an index past the end makes
        //the library pad the array with nulls, so a package naming /list/4000000000 would allocate until the machine
        //runs out of memory. Where the path is missing it is created as objects, which costs one level per token.
        const nlohmann::ordered_json *At = &Doc;
        for (size_t Pos = 0; At && Pos < Pointer.size();)
        {
            const size_t End = std::min(Pointer.find('/', Pos + 1), Pointer.size());
            std::string Tok = Pointer.substr(Pos + 1, End - Pos - 1);
            for (size_t K; (K = Tok.find("~1")) != std::string::npos;) Tok.replace(K, 2, "/");
            for (size_t K; (K = Tok.find("~0")) != std::string::npos;) Tok.replace(K, 2, "~");
            Pos = End;
            if (At->is_object()) { At = At->contains(Tok) ? &(*At)[Tok] : nullptr; continue; }
            if (!At->is_array()) break;   // a scalar on the way: the set below refuses it
            const bool Index = !Tok.empty() && Tok.size() <= 18 && Tok.find_first_not_of("0123456789") == std::string::npos;
            if (Tok != "-" && (!Index || std::stoull(Tok) > At->size()))
            {
                LogErr("FileEdits::JsonSet", "cannot set '" + Pointer + "' in " + FilePath.string() + ": '" + Tok
                       + "' is not an index within the array (size " + std::to_string(At->size()) + ")");
                return false;
            }
            At = Tok != "-" && std::stoull(Tok) < At->size() ? &(*At)[std::stoull(Tok)] : nullptr;
        }
        Doc[Ptr] = std::move(V);
        if (JsonDepth(Doc) > MaxJsonDepth)
        {
            LogErr("FileEdits::JsonSet", "cannot set '" + Pointer + "' in " + FilePath.string() + ": the result nests deeper than "
                   + std::to_string(MaxJsonDepth) + " levels");
            return false;
        }
        //Serialised in full before the file is touched: a value that cannot be written (not UTF-8) fails here, and the
        //file stays as it was.
        Text = Doc.dump(4) + "\n";
    }
    catch (const nlohmann::json::exception &Ex)
    {
        LogErr("FileEdits::JsonSet", "cannot set '" + Pointer + "' in " + FilePath.string() + ": " + Ex.what());
        return false;
    }
    std::error_code Ec;
    std::filesystem::create_directories(FilePath.parent_path(), Ec);
    std::ofstream Out(FilePath, std::ios::binary | std::ios::trunc);   // binary: LF on every platform
    if (!Out) { LogErr("FileEdits::JsonSet", "Could not open for writing: " + FilePath.string()); return false; }
    Out << Text;
    return static_cast<bool>(Out);
}

//===================================================================================================================
//                                                   XmlSet
//===================================================================================================================
//A small, strict reader of the XML a game writes: elements, attributes, text, CDATA, comments, processing instructions
//and a DOCTYPE, located by byte offset. An edit replaces one span of the original text, so everything else in the file
//(its layout, its quoting, its comments, its declaration) is kept byte for byte.
namespace {

struct XAttr { std::string Name; size_t ValStart = 0, ValEnd = 0; char Quote = '"'; };
struct XElem
{
    std::string Name;
    size_t TagStart = 0;         // '<'
    size_t AttrsEnd = 0;         // after the name or the last attribute's closing quote
    size_t TagEnd = 0;           // after the start tag's '>'
    size_t CloseStart = 0;       // '<' of the end tag (== TagEnd for an empty element)
    size_t CloseEnd = 0;         // after the end tag's '>' (== TagEnd for an empty element)
    bool Empty = false;          // <a/>
    std::vector<XAttr> Attrs;
    std::vector<size_t> Children;
};
struct XDoc { std::vector<XElem> Elems; long Root = -1; };

constexpr size_t MaxXmlDepth = 256;

bool XmlNameChar(char C) { return !(C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == '/' || C == '>' || C == '=' || C == '<' || C == '"' || C == '\''); }

//An XML Name (letters, digits, '_', ':', '-', '.', and any non-ASCII byte; not starting with a digit, '-' or '.'):
//what a KEY may name, since a created element writes it into the file as it is.
bool XmlName(const std::string &N)
{
    if (N.empty()) return false;
    const auto Start = [](unsigned char C) { return std::isalpha(C) || C == '_' || C == ':' || C >= 0x80; };
    if (!Start(static_cast<unsigned char>(N[0]))) return false;
    return std::all_of(N.begin() + 1, N.end(), [&](char C) {
        const unsigned char U = static_cast<unsigned char>(C);
        return Start(U) || std::isdigit(U) || C == '-' || C == '.';
    });
}

bool ParseXml(const std::string &T, XDoc &D, std::string &Why)
{
    std::vector<size_t> Stack;
    size_t P = T.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
    const auto skipTo = [&](const char *End) {
        const size_t E = T.find(End, P);
        if (E == std::string::npos) return false;
        P = E + std::strlen(End);
        return true;
    };
    while (P < T.size())
    {
        if (T[P] != '<')
        {
            const size_t N = T.find('<', P);
            if (Stack.empty() && T.find_first_not_of(" \t\r\n", P) < std::min(N, T.size())) { Why = "text outside the root element"; return false; }
            P = N == std::string::npos ? T.size() : N;
            continue;
        }
        if (T.compare(P, 4, "<!--") == 0) { if (!skipTo("-->")) { Why = "an unclosed comment"; return false; } continue; }
        if (T.compare(P, 9, "<![CDATA[") == 0)
        {
            if (Stack.empty()) { Why = "CDATA outside the root element"; return false; }
            if (!skipTo("]]>")) { Why = "an unclosed CDATA section"; return false; }
            continue;
        }
        if (T.compare(P, 2, "<?") == 0) { if (!skipTo("?>")) { Why = "an unclosed processing instruction"; return false; } continue; }
        if (T.compare(P, 2, "<!") == 0)                                       // DOCTYPE, its internal subset in [ ]
        {
            if (!Stack.empty() || D.Root >= 0) { Why = "a declaration outside the prolog"; return false; }
            int Br = 0; char Q = 0; size_t I = P + 2;
            for (; I < T.size(); ++I)
            {
                const char C = T[I];
                if (Q) { if (C == Q) Q = 0; continue; }
                if (C == '"' || C == '\'') Q = C;
                else if (C == '[') ++Br;
                else if (C == ']') --Br;
                else if (C == '>' && Br <= 0) break;
            }
            if (I >= T.size()) { Why = "an unclosed declaration"; return false; }
            P = I + 1;
            continue;
        }
        if (T.compare(P, 2, "</") == 0)
        {
            size_t I = P + 2, S = I;
            while (I < T.size() && XmlNameChar(T[I])) ++I;
            const std::string Name = T.substr(S, I - S);
            while (I < T.size() && (T[I] == ' ' || T[I] == '\t' || T[I] == '\r' || T[I] == '\n')) ++I;
            if (I >= T.size() || T[I] != '>') { Why = "a malformed end tag"; return false; }
            if (Stack.empty() || D.Elems[Stack.back()].Name != Name) { Why = "an end tag </" + Name + "> that closes nothing open"; return false; }
            D.Elems[Stack.back()].CloseStart = P;
            D.Elems[Stack.back()].CloseEnd = I + 1;
            Stack.pop_back();
            P = I + 1;
            continue;
        }
        XElem E;
        E.TagStart = P;
        size_t I = P + 1, S = I;
        while (I < T.size() && XmlNameChar(T[I])) ++I;
        E.Name = T.substr(S, I - S);
        if (E.Name.empty()) { Why = "a '<' that starts no tag"; return false; }
        E.AttrsEnd = I;
        for (;;)
        {
            while (I < T.size() && (T[I] == ' ' || T[I] == '\t' || T[I] == '\r' || T[I] == '\n')) ++I;
            if (I >= T.size()) { Why = "an unclosed tag <" + E.Name; return false; }
            if (T.compare(I, 2, "/>") == 0) { E.Empty = true; I += 2; break; }
            if (T[I] == '>') { ++I; break; }
            XAttr A;
            const size_t N0 = I;
            while (I < T.size() && XmlNameChar(T[I])) ++I;
            A.Name = T.substr(N0, I - N0);
            while (I < T.size() && (T[I] == ' ' || T[I] == '\t' || T[I] == '\r' || T[I] == '\n')) ++I;
            if (A.Name.empty() || I >= T.size() || T[I] != '=') { Why = "a malformed attribute in <" + E.Name; return false; }
            ++I;
            while (I < T.size() && (T[I] == ' ' || T[I] == '\t' || T[I] == '\r' || T[I] == '\n')) ++I;
            if (I >= T.size() || (T[I] != '"' && T[I] != '\'')) { Why = "an unquoted attribute value in <" + E.Name; return false; }
            A.Quote = T[I];
            A.ValStart = ++I;
            const size_t Q = T.find(A.Quote, I);
            if (Q == std::string::npos || std::find(T.begin() + static_cast<long>(I), T.begin() + static_cast<long>(Q), '<') != T.begin() + static_cast<long>(Q))
            { Why = "an unclosed attribute value in <" + E.Name; return false; }
            A.ValEnd = Q;
            I = Q + 1;
            E.AttrsEnd = I;
            E.Attrs.push_back(std::move(A));
        }
        E.TagEnd = I;
        if (E.Empty) E.CloseStart = E.CloseEnd = I;
        const size_t Id = D.Elems.size();
        if (Stack.empty())
        {
            if (D.Root >= 0) { Why = "more than one root element"; return false; }
            D.Root = static_cast<long>(Id);
        }
        else D.Elems[Stack.back()].Children.push_back(Id);
        D.Elems.push_back(std::move(E));
        if (!D.Elems[Id].Empty)
        {
            if (Stack.size() >= MaxXmlDepth) { Why = "elements nest deeper than " + std::to_string(MaxXmlDepth) + " levels"; return false; }
            Stack.push_back(Id);
        }
        P = I;
    }
    if (!Stack.empty()) { Why = "<" + D.Elems[Stack.back()].Name + "> is never closed"; return false; }
    return true;
}

void AppendUtf8(std::string &Out, unsigned long C)
{
    if (C < 0x80) Out += static_cast<char>(C);
    else if (C < 0x800) { Out += static_cast<char>(0xC0 | (C >> 6)); Out += static_cast<char>(0x80 | (C & 0x3F)); }
    else if (C < 0x10000) { Out += static_cast<char>(0xE0 | (C >> 12)); Out += static_cast<char>(0x80 | ((C >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (C & 0x3F)); }
    else { Out += static_cast<char>(0xF0 | (C >> 18)); Out += static_cast<char>(0x80 | ((C >> 12) & 0x3F)); Out += static_cast<char>(0x80 | ((C >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (C & 0x3F)); }
}

//The value an attribute or text holds: the five predefined entities and character references read; any other entity
//(one a DTD defines) is refused, since writing it back escaped would change what it says. An attribute's literal tab,
//CR and LF read as spaces, as XML normalises them (a reference to one stays that character).
bool XmlUnescape(const std::string &In, std::string &Out, bool Attribute = false)
{
    Out.clear();
    for (size_t I = 0; I < In.size(); ++I)
    {
        if (In[I] != '&')
        {
            if (Attribute && In[I] == '\r' && I + 1 < In.size() && In[I + 1] == '\n') continue;   // CRLF is one LF
            Out += Attribute && (In[I] == '\t' || In[I] == '\r' || In[I] == '\n') ? ' ' : In[I];
            continue;
        }
        const size_t E = In.find(';', I);
        if (E == std::string::npos) return false;
        const std::string N = In.substr(I + 1, E - I - 1);
        if (N == "lt") Out += '<'; else if (N == "gt") Out += '>'; else if (N == "amp") Out += '&';
        else if (N == "quot") Out += '"'; else if (N == "apos") Out += '\'';
        else if (N.size() > 1 && N[0] == '#')
        {
            const bool Hex = N[1] == 'x' || N[1] == 'X';
            const std::string Digits = N.substr(Hex ? 2 : 1);
            if (Digits.empty() || Digits.size() > 8 || Digits.find_first_not_of(Hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos) return false;
            const unsigned long C = std::stoul(Digits, nullptr, Hex ? 16 : 10);
            if (C == 0 || C > 0x10FFFF || (C >= 0xD800 && C <= 0xDFFF)) return false;   // no NUL, no surrogate
            AppendUtf8(Out, C);
        }
        else return false;
        I = E;
    }
    return true;
}

std::string XmlEscape(const std::string &In, char Quote)   // Quote 0: element text
{
    std::string Out;
    for (const char C : In)
        switch (C)
        {
        case '&': Out += "&amp;"; break;
        case '<': Out += "&lt;"; break;
        case '>': Out += "&gt;"; break;
        case '"': Out += Quote == '"' ? "&quot;" : "\""; break;
        case '\'': Out += Quote == '\'' ? "&apos;" : "'"; break;
        case '\n': Out += Quote ? "&#10;" : "\n"; break;
        case '\r': Out += Quote ? "&#13;" : "\r"; break;
        case '\t': Out += Quote ? "&#9;" : "\t"; break;
        default: Out += C;
        }
    return Out;
}

struct XStep { std::string Name; std::vector<std::pair<std::string, std::string>> Where; };
struct XPath { std::vector<XStep> Steps; std::string Attr; };   // Attr "": the element's text

//"/a/b[@k='v']/@x" — one document's part of a KEY. Steps are separated by '/', outside a predicate's quotes.
bool ParseXmlPath(const std::string &S, XPath &P, std::string &Why)
{
    if (S.empty() || S[0] != '/') { Why = "a path starts with '/'"; return false; }
    size_t I = 1;
    while (I <= S.size())
    {
        if (I == S.size()) { Why = "the path ends in '/'"; return false; }
        if (S[I] == '@')
        {
            P.Attr = S.substr(I + 1);
            if (!XmlName(P.Attr)) { Why = "'@" + P.Attr + "' is no attribute name"; return false; }
            break;
        }
        XStep St;
        const size_t N0 = I;
        while (I < S.size() && S[I] != '/' && S[I] != '[') ++I;
        St.Name = S.substr(N0, I - N0);
        if (!XmlName(St.Name)) { Why = "'" + St.Name + "' is no element name"; return false; }
        while (I < S.size() && S[I] == '[')
        {
            if (S.compare(I, 2, "[@") != 0) { Why = "a predicate is [@name='value']"; return false; }
            const size_t Eq = S.find('=', I);
            if (Eq == std::string::npos || Eq + 1 >= S.size() || (S[Eq + 1] != '\'' && S[Eq + 1] != '"')) { Why = "a predicate is [@name='value']"; return false; }
            const size_t Q = S.find(S[Eq + 1], Eq + 2);
            if (Q == std::string::npos || Q + 1 >= S.size() || S[Q + 1] != ']') { Why = "a predicate is [@name='value']"; return false; }
            const std::string Name = S.substr(I + 2, Eq - I - 2);
            if (!XmlName(Name)) { Why = "'@" + Name + "' in a predicate is no attribute name"; return false; }
            St.Where.emplace_back(Name, S.substr(Eq + 2, Q - Eq - 2));
            I = Q + 2;
        }
        P.Steps.push_back(std::move(St));
        if (I == S.size()) break;
        if (S[I] != '/') { Why = "unexpected '" + std::string(1, S[I]) + "' in the path"; return false; }
        ++I;
    }
    if (P.Steps.empty()) { Why = "the path names no element"; return false; }
    return true;
}

//A KEY: one path per document, '!' stepping into the XML an attribute (or text) holds. '!' inside quotes is literal.
bool SplitXmlKey(const std::string &Key, std::vector<XPath> &Out, std::string &Why)
{
    std::vector<std::string> Parts(1);
    char Q = 0;
    for (const char C : Key)
    {
        if (Q) { if (C == Q) Q = 0; }
        else if (C == '\'' || C == '"') Q = C;
        else if (C == '!') { Parts.emplace_back(); continue; }
        Parts.back() += C;
    }
    for (size_t I = 0; I < Parts.size(); ++I)
    {
        XPath P;
        if (!ParseXmlPath(Parts[I], P, Why)) return false;
        Out.push_back(std::move(P));
    }
    return true;
}

bool Matches(const std::string &T, const XElem &E, const XStep &S)
{
    if (E.Name != S.Name) return false;
    for (const auto &[K, V] : S.Where)
    {
        bool Hit = false;
        for (const XAttr &A : E.Attrs)
            if (A.Name == K)
            {
                std::string Val;
                Hit = XmlUnescape(T.substr(A.ValStart, A.ValEnd - A.ValStart), Val, true) && Val == V;
                break;
            }
        if (!Hit) return false;
    }
    return true;
}

//The text an element holds (its text and CDATA; "" when empty). False when it holds elements: that is not a value.
bool ElementText(const std::string &T, const XElem &E, std::string &Out)
{
    Out.clear();
    if (E.Empty) return true;
    if (!E.Children.empty()) return false;
    const std::string Raw = T.substr(E.TagEnd, E.CloseStart - E.TagEnd);
    for (size_t I = 0; I < Raw.size();)
    {
        if (Raw.compare(I, 9, "<![CDATA[") == 0) { const size_t C = Raw.find("]]>", I); Out += Raw.substr(I + 9, C - I - 9); I = C + 3; continue; }
        if (Raw.compare(I, 4, "<!--") == 0) { I = Raw.find("-->", I) + 3; continue; }
        if (Raw.compare(I, 2, "<?") == 0) { I = Raw.find("?>", I) + 2; continue; }
        if (Raw[I] == '<') return false;                                   // markup that is no text: not a value
        const size_t N = std::min(Raw.find('<', I), Raw.size());
        std::string Part;
        if (!XmlUnescape(Raw.substr(I, N - I), Part)) return false;
        Out += Part;
        I = N;
    }
    return true;
}

//New markup for Steps[From..] holding Leaf (an attribute value, or the innermost element's text), on one line.
std::string NewMarkup(const XPath &P, size_t From, const std::string &Leaf)
{
    std::string Open, Close;
    for (size_t I = From; I < P.Steps.size(); ++I)
    {
        const bool Last = I + 1 == P.Steps.size();
        std::string Tag = "<" + P.Steps[I].Name;
        for (const auto &[K, V] : P.Steps[I].Where) Tag += " " + K + "=\"" + XmlEscape(V, '"') + "\"";
        if (Last && !P.Attr.empty()) { Open += Tag + " " + P.Attr + "=\"" + XmlEscape(Leaf, '"') + "\" />"; break; }
        Open += Tag + ">";
        if (Last) Open += XmlEscape(Leaf, 0);
        Close = "</" + P.Steps[I].Name + ">" + Close;
    }
    return Open + Close;
}

//The indentation of the line the offset At is on (its leading spaces and tabs).
std::string IndentAt(const std::string &T, size_t At)
{
    const size_t L = T.rfind('\n', At == 0 ? 0 : At - 1);
    const size_t S = L == std::string::npos ? 0 : L + 1;
    size_t E = S;
    while (E < T.size() && (T[E] == ' ' || T[E] == '\t')) ++E;
    return E <= At ? T.substr(S, E - S) : std::string();
}

//Sets (Leaf non-null) or reads the value Paths[K..] names inside the document T. A set writes Out; a read fills Got.
bool XmlWalk(const std::string &T, const std::vector<XPath> &Paths, size_t K, const std::string *Leaf, std::string &Out,
             std::optional<std::string> &Got, std::string &Why, size_t Nest = 0)
{
    if (Nest > 16) { Why = "XML nests inside attributes deeper than 16 documents"; return false; }
    const XPath &P = Paths[K];
    const bool Inner = K + 1 < Paths.size();
    //The leaf's new value: for a nested document, the inner document with its own edit applied.
    const auto LeafFor = [&](const std::string &Current, std::string &V) -> bool {
        if (!Inner) { V = *Leaf; return true; }
        std::optional<std::string> Unused;
        return XmlWalk(Current, Paths, K + 1, Leaf, V, Unused, Why, Nest + 1);
    };
    const std::string NL = T.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    if (T.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        if (!Leaf) { Got.reset(); return true; }
        std::string V;
        if (!LeafFor(std::string(), V)) return false;
        Out = NewMarkup(P, 0, V) + NL;
        return true;
    }
    XDoc D;
    if (!ParseXml(T, D, Why)) return false;
    if (D.Root < 0) { Why = "no root element"; return false; }
    size_t Cur = static_cast<size_t>(D.Root);
    if (!Matches(T, D.Elems[Cur], P.Steps[0]))
    {
        Why = "the root element is <" + D.Elems[Cur].Name + ">, not the path's <" + P.Steps[0].Name + ">";
        return false;
    }
    for (size_t S = 1; S < P.Steps.size(); ++S)
    {
        long Next = -1;
        for (const size_t C : D.Elems[Cur].Children) if (Matches(T, D.Elems[C], P.Steps[S])) { Next = static_cast<long>(C); break; }
        if (Next >= 0) { Cur = static_cast<size_t>(Next); continue; }
        if (!Leaf) { Got.reset(); return true; }                                 // nothing there: no value
        //Missing from here on: created inside Cur, after its last child, indented like that child (else one level in).
        const XElem &Pa = D.Elems[Cur];
        std::string V;
        if (!LeafFor(std::string(), V)) return false;
        const std::string Markup = NewMarkup(P, S, V);
        if (Pa.Empty)
        {
            const std::string Ind = IndentAt(T, Pa.TagStart);
            Out = T.substr(0, Pa.AttrsEnd) + ">" + NL + Ind + "\t" + Markup + NL + Ind + "</" + Pa.Name + ">" + T.substr(Pa.TagEnd);
            return true;
        }
        if (!Pa.Children.empty())
        {
            const XElem &Last = D.Elems[Pa.Children.back()];
            Out = T.substr(0, Last.CloseEnd) + NL + IndentAt(T, Last.TagStart) + Markup + T.substr(Last.CloseEnd);
            return true;
        }
        const std::string Ind = IndentAt(T, Pa.TagStart);
        const std::string Inside = T.substr(Pa.TagEnd, Pa.CloseStart - Pa.TagEnd);
        if (Inside.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            Out = T.substr(0, Pa.TagEnd) + NL + Ind + "\t" + Markup + NL + Ind + T.substr(Pa.CloseStart);
            return true;
        }
        //Comments or processing instructions only: kept, the new element after them. Text: an element of mixed
        //content, which this edit does not write into.
        std::string Text;
        if (!ElementText(T, Pa, Text) || Text.find_first_not_of(" \t\r\n") != std::string::npos)
        { Why = "<" + Pa.Name + "> holds text; an element is not added to it"; return false; }
        const size_t LineStart = T.rfind('\n', Pa.CloseStart == 0 ? 0 : Pa.CloseStart - 1);
        const bool OwnLine = LineStart != std::string::npos && T.find_first_not_of(" \t\r", LineStart + 1) == Pa.CloseStart;
        const size_t Cut = OwnLine && LineStart > 0 && T[LineStart - 1] == '\r' ? LineStart - 1 : LineStart;
        Out = OwnLine ? T.substr(0, Cut) + NL + Ind + "\t" + Markup + T.substr(Cut)
                      : T.substr(0, Pa.CloseStart) + Markup + T.substr(Pa.CloseStart);
        return true;
    }
    const XElem &E = D.Elems[Cur];
    if (!P.Attr.empty())
    {
        const XAttr *A = nullptr;
        for (const XAttr &X : E.Attrs) if (X.Name == P.Attr) { A = &X; break; }
        std::string Current;
        if (A && !XmlUnescape(T.substr(A->ValStart, A->ValEnd - A->ValStart), Current, true))
        { Why = "@" + P.Attr + " holds an entity this edit cannot write back"; return false; }
        if (!Leaf)
        {
            if (!A) { Got.reset(); return true; }
            if (!Inner) { Got = Current; return true; }
            std::string Unused;
            return XmlWalk(Current, Paths, K + 1, nullptr, Unused, Got, Why, Nest + 1);
        }
        std::string V;
        if (!LeafFor(Current, V)) return false;
        if (A) Out = T.substr(0, A->ValStart) + XmlEscape(V, A->Quote) + T.substr(A->ValEnd);
        else Out = T.substr(0, E.AttrsEnd) + " " + P.Attr + "=\"" + XmlEscape(V, '"') + "\"" + T.substr(E.AttrsEnd);
        return true;
    }
    std::string Current;
    if (!ElementText(T, E, Current)) { Why = "<" + E.Name + "> holds elements or an entity this edit cannot write back, not a value"; return false; }
    if (!Leaf)
    {
        if (!Inner) { Got = Current; return true; }
        std::string Unused;
        return XmlWalk(Current, Paths, K + 1, nullptr, Unused, Got, Why, Nest + 1);
    }
    std::string V;
    if (!LeafFor(Current, V)) return false;
    if (E.Empty) Out = T.substr(0, E.AttrsEnd) + ">" + XmlEscape(V, 0) + "</" + E.Name + ">" + T.substr(E.TagEnd);
    else Out = T.substr(0, E.TagEnd) + XmlEscape(V, 0) + T.substr(E.CloseStart);
    return true;
}

} // namespace

bool FileEdits::XmlEdit(const std::string &Key, const std::string &Value, const std::string &In, std::string &Out, std::string &Why)
{
    std::vector<XPath> Paths;
    if (!SplitXmlKey(Key, Paths, Why)) return false;
    std::optional<std::string> Unused;
    return XmlWalk(In, Paths, 0, &Value, Out, Unused, Why);
}

std::optional<std::string> FileEdits::XmlGet(const std::string &Key, const std::string &In, std::string &Why)
{
    std::vector<XPath> Paths;
    if (!SplitXmlKey(Key, Paths, Why)) return std::nullopt;
    std::optional<std::string> Got;
    std::string Unused;
    if (!XmlWalk(In, Paths, 0, nullptr, Unused, Got, Why)) return std::nullopt;
    return Got;
}

bool FileEdits::XmlSet(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("FileEdits::XmlSet", "FilePath: " + FilePath.string() + " Key: " + Key + " Value: " + Value);
    std::string Text;
    {
        std::ifstream In(FilePath, std::ios::binary);
        if (In) Text.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    std::string Out, Why;
    if (!XmlEdit(Key, Value, Text, Out, Why))
    {
        LogErr("FileEdits::XmlSet", "cannot set '" + Key + "' in " + FilePath.string() + ": " + Why + " — left as it is.");
        return false;
    }
    if (Out == Text) return true;
    std::error_code Ec;
    std::filesystem::create_directories(FilePath.parent_path(), Ec);
    std::ofstream O(FilePath, std::ios::binary | std::ios::trunc);
    if (!O) { LogErr("FileEdits::XmlSet", "Could not open for writing: " + FilePath.string()); return false; }
    O << Out;
    return static_cast<bool>(O);
}

//Writes Value as the complete content of FilePath, creating parent directories if needed.
//Used for files whose entire content is the value (e.g. Warcraft III .w3k CD key files).
bool FileEdits::FileOverwrite(const std::string &Value, const std::filesystem::path &FilePath)
{
    LogOut("FileEdits::FileOverwrite", "Writing: " + FilePath.string());
    std::error_code ec;
    std::filesystem::create_directories(FilePath.parent_path(), ec);
    if (ec) { LogErr("FileEdits::FileOverwrite", "Could not create parent dirs: " + ec.message()); return false; }
    std::ofstream Out(FilePath, std::ios::binary | std::ios::out | std::ios::trunc);   // binary: verbatim bytes, no CRLF translation on Windows
    if (!Out) { LogErr("FileEdits::FileOverwrite", "Could not open for writing: " + FilePath.string()); return false; }
    Out << Value;
    return true;
}

std::string FileEdits::KeptPathMet(const struct ContainerParams &ContainerParams, const std::filesystem::path &FilePath)
{
    namespace fs = std::filesystem;
    //Compared as normalised, lower-cased generic paths: the runtime may be case-insensitive (Windows), and a kept
    //address is the user's whatever its case.
    const auto Norm = [](const fs::path &P) {
        std::string S = P.lexically_normal().generic_string();
        while (S.size() > 1 && S.back() == '/') S.pop_back();
        std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        return S;
    };
    //...and as the places they ARE: a link inside the runtime (Proton's "My Documents" -> "Documents") is another
    //spelling of a kept folder that remove_all would follow. Both sides are resolved through the links that exist.
    const auto Real = [&](const fs::path &P) { std::error_code Ec; const fs::path C = fs::weakly_canonical(P, Ec); return Ec ? Norm(P) : Norm(C); };
    const std::string Fl = Norm(FilePath), Fr = Real(FilePath);
    const auto Meets = [&](const std::string &RuntimeRel) {
        const fs::path K = ContainerParams.RuntimePath / RuntimeRel;
        const std::string Leaf = K.filename().string();
        const bool Pattern = Leaf.find_first_of("*?") != std::string::npos;
        for (const auto &[F, Kn] : { std::pair{ Fl, Norm(Pattern ? K.parent_path() : K) }, std::pair{ Fr, Real(Pattern ? K.parent_path() : K) } })
        {
            if (F == Kn || Kn.rfind(F + "/", 0) == 0) return true;                 // the kept place, or a folder holding it
            if (!Pattern && F.rfind(Kn + "/", 0) == 0) return true;               // inside a kept folder (or file)
            //A kept pattern keeps the matching files of ONE folder: a Delete of such a file, or of its folder.
            if (Pattern)
            {
                const size_t S = F.rfind('/');
                if (S != std::string::npos && F.substr(0, S) == Kn && WildcardMatch(Leaf, F.substr(S + 1))) return true;
            }
        }
        return false;
    };
    for (const auto *List : { &ContainerParams.KeepDirs, &ContainerParams.KeepFiles })
        for (const PersistTarget &T : *List)
            if (Meets(T.Path)) return T.Path.empty() ? std::string("(the whole runtime)") : T.Path;
    return std::string();
}

bool FileEdits::DeletePath(const std::filesystem::path &FilePath)
{
    std::error_code Ec;
    const std::filesystem::file_status St = std::filesystem::symlink_status(FilePath, Ec);
    if (Ec || St.type() == std::filesystem::file_type::not_found)
    {
        //Nothing there: a typo, or a file a layer beneath stopped providing. Either way the package's intent did not
        //happen, which is what a failed edit means.
        LogErr("FileEdits::DeletePath", "Nothing to delete at " + FilePath.string());
        return false;
    }
    //Through the mounted union: the file system records the removal in the session's write layer (a whiteout), so
    //the pristine content beneath is never touched. remove_all removes a link as a link and never follows it.
    const std::uintmax_t N = std::filesystem::remove_all(FilePath, Ec);
    if (Ec)
    {
        LogErr("FileEdits::DeletePath", "Could not delete " + FilePath.string() + ": " + Ec.message());
        return false;
    }
    LogOut("FileEdits::DeletePath", "Deleted " + FilePath.string() + " (" + std::to_string(N) + " entr" + (N == 1 ? "y" : "ies") + ")");
    return true;
}

bool FileEdits::AppendLine(const std::string &Value, const std::filesystem::path &FilePath)
{
    std::error_code Ec;
    std::filesystem::create_directories(FilePath.parent_path(), Ec);   // no-op for an existing dir

    // Read the current content: check for an identical line (idempotency) and whether it ends with a newline.
    std::string Content;
    {
        std::ifstream In(FilePath, std::ios::binary);
        if (In) Content.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    {
        std::istringstream Ss(Content);
        std::string Line;
        while (std::getline(Ss, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();   // tolerate CRLF
            if (Line == Value) { LogOut("FileEdits::AppendLine", "already present in " + FilePath.string() + ": " + Value); return true; }
        }
    }

    std::ofstream Out(FilePath, std::ios::binary | std::ios::app);   // binary: '\n' stays LF on Windows too (matches the binary read above)
    if (!Out) { LogErr("FileEdits::AppendLine", "Could not open for append: " + FilePath.string()); return false; }
    if (!Content.empty() && Content.back() != '\n') Out << '\n';        // don't fuse onto an unterminated last line
    Out << Value << '\n';
    LogOut("FileEdits::AppendLine", "Appended to " + FilePath.string() + ": " + Value);
    return true;
}