#include "fileedits.h"
#include "persistlayer.h"   // HasSavedCopy — a user-owned file's package default
#include "commonutils.h"   // Log*
#include "varsubst.h"      // %variable% substitution in FileEdit VALUEs (e.g. config_info's %RunnerMount%/%TempPath%)

#include <algorithm>
#include <cctype>
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
            ContainerParams.DLLOverrides.push_back(SubComponentJSON["DLLOVERRIDE"].is_string()   // written out here (%% → %)
                ? nlohmann::ordered_json(VarSubst::Unescape(SubComponentJSON["DLLOVERRIDE"].get<std::string>())) : SubComponentJSON["DLLOVERRIDE"]);
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
        std::string Key = Sub.value("KEY", std::string());
        std::string Section = Sub.value("SECTION", std::string());   // ConfigWrite: the INI section / TOML table
        //The one final substitution of what the resolve handed over (it keeps a literal '%' doubled): %% → %.
        VarSubst::StringVariableSubstitution(File,  Vars);
        VarSubst::StringVariableSubstitution(Value, Vars);
        VarSubst::StringVariableSubstitution(Key,   Vars);
        VarSubst::StringVariableSubstitution(Section, Vars);
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
        //IF_EXISTS: the file is the game's to make (a profile written on its first run). Until it does, the edit is
        //skipped: never a stub the game would then read as its own, never a failure.
        if (const auto If = Sub.find("IF_EXISTS"); If != Sub.end() && *If == true)
            if (std::error_code Ec; !std::filesystem::exists(FilePath, Ec))
            {
                LogOut("FileEdits::ProcessFileEdits", "'" + File + "' does not exist yet — the IF_EXISTS "
                       + (Mode.empty() ? std::string("edit") : Mode) + " is skipped.");
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
            EditOk = FileEdits::ConfigWrite(Key, Value, FilePath, Section);
        else if (Mode == "JsonSet")
            EditOk = FileEdits::JsonSet(Key, Value, FilePath);
        else if (Mode == "XmlSet")
            EditOk = FileEdits::XmlSet(Key, Value, FilePath);
        else if (Mode == "Overwrite")
            EditOk = FileEdits::FileOverwrite(Value, FilePath);
        else if (Mode == "AppendLine")
            EditOk = FileEdits::AppendLine(Value, FilePath);
        else if (Mode == "LineFlag")
            EditOk = FileEdits::LineFlag(Key, Value, FilePath);
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
namespace {
//A line without its '\r' and outer blanks (and the first line's byte-order mark).
std::string Trimmed(std::string S)
{
    if (S.rfind("\xEF\xBB\xBF", 0) == 0) S.erase(0, 3);
    while (!S.empty() && (S.back() == '\r' || S.back() == ' ' || S.back() == '\t')) S.pop_back();
    const size_t F = S.find_first_not_of(" \t");
    return F == std::string::npos ? std::string() : S.substr(F);
}
//A section header's name as compared: "[a.b]" / "[[a]]" with the blanks around its dotted parts dropped
//("[ display . quality ]" is "[display.quality]"; "[Video Settings]" keeps its blank) and a comment after it
//("[a] # c", "[a] ; c") gone. Empty when the line is no header: not in brackets, an empty name, or a bracket inside.
std::string HeaderName(const std::string &Line)
{
    const std::string T = Trimmed(Line);
    if (T.empty() || T[0] != '[') return {};
    const bool Double = T.rfind("[[", 0) == 0;
    std::vector<std::string> Parts(1);
    bool Quoted = false;
    char Q = 0;
    size_t I = Double ? 2 : 1;
    for (; I < T.size(); ++I)
    {
        const char C = T[I];
        if (Quoted) { Parts.back() += C; if (C == Q) Quoted = false; continue; }
        if (C == '"' || C == '\'') { Quoted = true; Q = C; Parts.back() += C; continue; }
        if (C == ']') break;
        if (C == '[') return {};
        if (C == '.') Parts.emplace_back(); else Parts.back() += C;
    }
    if (I >= T.size()) return {};
    if (Double && (I + 1 >= T.size() || T[I + 1] != ']')) return {};
    const std::string Rest = Trimmed(T.substr(I + (Double ? 2 : 1)));
    if (!Rest.empty() && Rest[0] != '#' && Rest[0] != ';') return {};
    std::string Name;
    for (size_t P = 0; P < Parts.size(); ++P) Name += (P ? "." : "") + Trimmed(Parts[P]);
    if (Trimmed(Name).empty()) return {};
    return (Double ? "[[" : "[") + Name + (Double ? "]]" : "]");
}
std::string Lower(std::string S) { for (char &C : S) C = (char)std::tolower((unsigned char)C); return S; }
//Which lines are section headers: a header-shaped line outside a value that runs over several lines - a TOML array
//("l = [" up to its closing ']') or a multi-line string (between triple quotes) - whose own lines can start with '['.
std::vector<bool> HeaderLines(const std::vector<std::string> &Lines)
{
    std::vector<bool> Out(Lines.size(), false);
    int Depth = 0;                 // open brackets of a multi-line array
    std::string InString;          // the triple quote a multi-line string is open with
    for (size_t I = 0; I < Lines.size(); ++I)
    {
        const std::string &L = Lines[I];
        size_t From = 0;
        if (!InString.empty())
        {
            const size_t E = L.find(InString);
            if (E == std::string::npos) continue;
            From = E + 3;
            InString.clear();
        }
        else if (Depth == 0)
        {
            if (!HeaderName(L).empty()) { Out[I] = true; continue; }
            const size_t Eq = L.find('=');                       // only a value that opens an array or a string is followed
            if (Eq == std::string::npos) continue;
            From = Eq + 1;
            const std::string V = Trimmed(L.substr(From));
            if (V.empty() || (V[0] != '[' && V.rfind("'''", 0) != 0 && V.rfind("\"\"\"", 0) != 0)) continue;
        }
        char Q = 0;
        for (size_t C = From; C < L.size(); ++C)
        {
            const char Ch = L[C];
            if (!Q && (L.compare(C, 3, "'''") == 0 || L.compare(C, 3, "\"\"\"") == 0))
            {
                const std::string Open = L.substr(C, 3);
                const size_t E = L.find(Open, C + 3);
                if (E == std::string::npos) { InString = Open; break; }
                C = E + 2;
                continue;
            }
            if (Q) { if (Ch == '\\' && Q == '"') ++C; else if (Ch == Q) Q = 0; continue; }
            if (Ch == '"' || Ch == '\'') { Q = Ch; continue; }
            if (Ch == '#') break;
            if (Ch == '[') ++Depth;
            else if (Ch == ']' && Depth > 0) --Depth;
        }
    }
    return Out;
}
//A section's lines in Lines: (Header, End) - its header's index and the line after its last; false when it has none.
//The header is found by name: exactly, else (not in TOML, where case makes another table) ignoring case - an INI's
//[graphics] for [Graphics]. A section runs to the next header line (the next section, table or array of tables).
bool SectionRange(const std::vector<std::string> &Lines, const std::string &Section, bool Toml, size_t &Header, size_t &End)
{
    const std::string Want = HeaderName(Section);
    if (Want.empty()) return false;
    const std::vector<bool> Is = HeaderLines(Lines);
    std::optional<size_t> Found;
    for (size_t I = 0; I < Lines.size() && !Found; ++I) if (Is[I] && HeaderName(Lines[I]) == Want) Found = I;
    for (size_t I = 0; I < Lines.size() && !Found && !Toml; ++I) if (Is[I] && Lower(HeaderName(Lines[I])) == Lower(Want)) Found = I;
    if (!Found) return false;
    Header = *Found;
    End = Header + 1;
    while (End < Lines.size() && !Is[End]) ++End;
    return true;
}
std::vector<std::string> LinesOf(const std::string &Text)
{
    std::vector<std::string> Out;
    std::istringstream In(Text);
    for (std::string L; std::getline(In, L);) Out.push_back(L);
    return Out;
}
bool StartsWith(const std::string &Line, const std::string &Key) { return Line.size() >= Key.size() && Line.compare(0, Key.size(), Key) == 0; }
//Where a line in a section holds Key's value: the offset of the value in the line, or npos when it is another key's
//line. The line starting with Key as written is that; else, for a "name = " Key, the line naming the same key with
//other spacing ("surface_scale=2", "  surface_scale = 2"), its value after the '=' and the blanks following it.
size_t ValueAtKey(const std::string &Line, const std::string &Key)
{
    if (StartsWith(Line, Key)) return Key.size();
    const size_t Eq = Key.find('=');
    if (Eq == std::string::npos || Trimmed(Key.substr(Eq + 1)) != "") return std::string::npos;
    const std::string Name = Trimmed(Key.substr(0, Eq));
    if (Name.empty()) return std::string::npos;
    size_t P = Line.find_first_not_of(" \t");
    if (P == std::string::npos || Line.compare(P, Name.size(), Name) != 0) return std::string::npos;
    P = Line.find_first_not_of(" \t", P + Name.size());
    if (P == std::string::npos || Line[P] != '=') return std::string::npos;
    P = Line.find_first_not_of(" \t", P + 1);
    return P == std::string::npos ? Line.size() : P;
}
}

bool FileEdits::IsToml(const std::filesystem::path &File) { return Lower(File.extension().string()) == ".toml"; }

std::optional<std::string> FileEdits::ConfigRead(const std::string &Text, const std::string &Key, const std::string &Section, bool Toml)
{
    const std::vector<std::string> Lines = LinesOf(Text);
    size_t From = 0, To = Lines.size(), Header = 0;
    if (!Section.empty() && !SectionRange(Lines, Section, Toml, Header, To)) return std::nullopt;
    if (!Section.empty()) From = Header + 1;
    for (size_t I = From; I < To; ++I)
    {
        const size_t At = Section.empty() ? (StartsWith(Lines[I], Key) ? Key.size() : std::string::npos) : ValueAtKey(Lines[I], Key);
        if (At == std::string::npos) continue;
        std::string V = Lines[I].substr(At);
        if (!V.empty() && V.back() == '\r') V.pop_back();
        return V;
    }
    return std::nullopt;
}

bool FileEdits::ConfigWrite(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath,
                            const std::string &Section)
{
    LogOut("FileEdits::ConfigWrite", "FilePath: " + FilePath.string() + (Section.empty() ? std::string() : " Section: " + Section)
           + " Key: " + Key + " Value: " + Value);
    if (!Section.empty())
    {
        //In its section: the key's line replaced (its indent kept), else added at the section's end (before the blank
        //lines closing it), else the section made at the file's end. A file not there yet is made (an emulator's
        //config before its first run). A file in CRLF gets CRLF lines.
        std::string Text;
        std::error_code Ec;
        if (std::filesystem::exists(FilePath, Ec))
        {
            std::ifstream In(FilePath, std::ios::binary);
            if (!In.is_open()) { LogErr("FileEdits::ConfigWrite", "Could not open file for reading: " + FilePath.string()); return false; }
            Text.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
        }
        std::vector<std::string> Lines = LinesOf(Text);
        const std::string Eol = !Lines.empty() && !Lines[0].empty() && Lines[0].back() == '\r' ? "\r" : "";
        const bool Blank = Text.find_first_not_of(" \t\r\n") == std::string::npos;
        size_t Header = 0, End = 0;
        if (SectionRange(Lines, Section, IsToml(FilePath), Header, End))
        {
            size_t Matched = 0;
            for (size_t I = Header + 1; I < End; ++I)
                if (ValueAtKey(Lines[I], Key) != std::string::npos)
                {
                    const bool Cr = !Lines[I].empty() && Lines[I].back() == '\r';
                    const size_t Ind = Lines[I].find_first_not_of(" \t");
                    Lines[I] = Lines[I].substr(0, StartsWith(Lines[I], Key) ? 0 : Ind) + Key + Value + (Cr ? "\r" : "");
                    ++Matched;
                }
            if (!Matched)
            {
                size_t At = End;
                while (At > Header + 1 && Trimmed(Lines[At - 1]).empty()) --At;
                Lines.insert(Lines.begin() + (std::ptrdiff_t)At, Key + Value + Eol);
                LogWarn("FileEdits::ConfigWrite", "no '" + Key + "' in " + Section + " of " + FilePath.string() + " — added it there");
            }
        }
        else
        {
            if (!Lines.empty() && !Trimmed(Lines.back()).empty()) Lines.push_back(Eol);
            Lines.push_back(Trimmed(Section) + Eol);
            Lines.push_back(Key + Value + Eol);
            if (!Blank)
                LogWarn("FileEdits::ConfigWrite", "no " + Section + " in " + FilePath.string() + " — added it at the end with '" + Key + "'");
        }
        if (FilePath.has_parent_path()) std::filesystem::create_directories(FilePath.parent_path(), Ec);
        std::ofstream Out(FilePath, std::ios::binary | std::ios::trunc);
        if (!Out.is_open()) { LogErr("FileEdits::ConfigWrite", "Could not open file for writing: " + FilePath.string()); return false; }
        for (const std::string &L : Lines) Out << L << '\n';
        return static_cast<bool>(Out.flush());
    }
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
        }
        outFile << currentLine << '\n';
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

//A predicate: [@name='v'] an attribute's value, [name='v'] a child element's text (Age of Mythology's <keymapdata> are
//told apart only by their children: <name>Select Group 1</name>, <actiontype>0</actiontype>).
struct XPred { std::string Name, Value; bool Child = false; };
struct XStep { std::string Name; std::vector<XPred> Where; };
struct XPath { std::vector<XStep> Steps; std::string Attr; };   // Attr "": the element's text

//"/a/b[@k='v'][c='w']/@x" — one document's part of a KEY. Steps are separated by '/', outside a predicate's quotes.
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
            static const char *Form = "a predicate is [@attribute='value'] or [child='text']";
            const bool Child = S.compare(I, 2, "[@") != 0;
            const size_t N1 = I + (Child ? 1 : 2);
            const size_t Eq = S.find('=', I);
            if (Eq == std::string::npos || Eq + 1 >= S.size() || (S[Eq + 1] != '\'' && S[Eq + 1] != '"')) { Why = Form; return false; }
            const size_t Q = S.find(S[Eq + 1], Eq + 2);
            if (Q == std::string::npos || Q + 1 >= S.size() || S[Q + 1] != ']') { Why = Form; return false; }
            const std::string Name = S.substr(N1, Eq - N1);
            if (!XmlName(Name))
            { Why = "'" + std::string(Child ? "" : "@") + Name + "' in a predicate is no " + (Child ? "element" : "attribute") + " name"; return false; }
            St.Where.push_back(XPred{ Name, S.substr(Eq + 2, Q - Eq - 2), Child });
            I = Q + 2;
        }
        P.Steps.push_back(std::move(St));
        if (I == S.size()) break;
        if (S[I] != '/') { Why = "unexpected '" + std::string(1, S[I]) + "' in the path"; return false; }
        ++I;
    }
    if (P.Steps.empty()) { Why = "the path names no element"; return false; }
    //An element told apart by its children holds elements: it has no text of its own to set.
    if (P.Attr.empty())
        for (const XPred &W : P.Steps.back().Where)
            if (W.Child) { Why = "<" + P.Steps.back().Name + "> holds the [" + W.Name + "] child, so it has no text to set: name a child or an attribute"; return false; }
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

bool ElementText(const std::string &T, const XElem &E, std::string &Out);

//A child predicate holds when ANY child of that name has that text (XPath's comparison of a node-set to a string).
bool Matches(const std::string &T, const XDoc &D, const XElem &E, const XStep &S)
{
    if (E.Name != S.Name) return false;
    for (const XPred &W : S.Where)
    {
        bool Hit = false;
        if (W.Child)
            for (const size_t C : E.Children)
            {
                std::string Val;
                if (D.Elems[C].Name == W.Name && ElementText(T, D.Elems[C], Val) && Val == W.Value) { Hit = true; break; }
            }
        else
            for (const XAttr &A : E.Attrs)
                if (A.Name == W.Name)
                {
                    std::string Val;
                    Hit = XmlUnescape(T.substr(A.ValStart, A.ValEnd - A.ValStart), Val, true) && Val == W.Value;
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
        std::string Tag = "<" + P.Steps[I].Name, Kids;
        for (const XPred &W : P.Steps[I].Where)
            if (W.Child) Kids += "<" + W.Name + ">" + XmlEscape(W.Value, 0) + "</" + W.Name + ">";   // in predicate order
            else Tag += " " + W.Name + "=\"" + XmlEscape(W.Value, '"') + "\"";
        if (Last && !P.Attr.empty())
        {
            Tag += " " + P.Attr + "=\"" + XmlEscape(Leaf, '"') + "\"";
            Open += Kids.empty() ? Tag + " />" : Tag + ">" + Kids + "</" + P.Steps[I].Name + ">";
            break;
        }
        Open += Tag + ">" + Kids;
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
    if (!Matches(T, D, D.Elems[Cur], P.Steps[0]))
    {
        Why = "the root element is <" + D.Elems[Cur].Name + ">, not the path's <" + P.Steps[0].Name + ">";
        return false;
    }
    for (size_t S = 1; S < P.Steps.size(); ++S)
    {
        long Next = -1;
        for (const size_t C : D.Elems[Cur].Children) if (Matches(T, D, D.Elems[C], P.Steps[S])) { Next = static_cast<long>(C); break; }
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
            //On a line of its own when the last child is (an element written on one line stays on one line).
            const XElem &Last = D.Elems[Pa.Children.back()];
            const size_t Ls = Last.TagStart == 0 ? std::string::npos : T.rfind('\n', Last.TagStart - 1);
            const bool OwnLine = Ls != std::string::npos && T.find_first_not_of(" \t", Ls + 1) == Last.TagStart;
            Out = T.substr(0, Last.CloseEnd) + (OwnLine ? NL + IndentAt(T, Last.TagStart) : std::string()) + Markup + T.substr(Last.CloseEnd);
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

//A file a game writes in UTF-16 (Age of Mythology's profiles: FF FE, then <?xml … encoding="UTF-16"?>) is edited as
//UTF-8 and written back in its own byte order, with its byte-order mark. Strict both ways, so that every byte the edit
//does not change comes back as it was: an odd length, an unpaired surrogate or text that is not UTF-8 fails the op.
enum class XEnc { Utf8, Utf16Le, Utf16Be };

bool FromUtf16(const std::string &In, bool Le, std::string &Out)
{
    if (In.size() % 2) return false;
    const auto Unit = [&](size_t At) {
        const auto B0 = static_cast<unsigned char>(In[At]), B1 = static_cast<unsigned char>(In[At + 1]);
        return static_cast<uint32_t>(Le ? B0 | (B1 << 8) : (B0 << 8) | B1);
    };
    Out.clear();
    for (size_t I = 0; I < In.size(); I += 2)
    {
        uint32_t Cp = Unit(I);
        if (Cp >= 0xDC00 && Cp < 0xE000) return false;
        if (Cp >= 0xD800 && Cp < 0xDC00)
        {
            if (I + 3 >= In.size()) return false;
            const uint32_t Lo = Unit(I + 2);
            if (Lo < 0xDC00 || Lo >= 0xE000) return false;
            Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
            I += 2;
        }
        if (Cp < 0x80) Out += static_cast<char>(Cp);
        else if (Cp < 0x800) { Out += static_cast<char>(0xC0 | (Cp >> 6)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
        else if (Cp < 0x10000) { Out += static_cast<char>(0xE0 | (Cp >> 12)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
        else { Out += static_cast<char>(0xF0 | (Cp >> 18)); Out += static_cast<char>(0x80 | ((Cp >> 12) & 0x3F)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
    }
    return true;
}

bool ToUtf16(const std::string &In, bool Le, std::string &Out)
{
    const auto Put = [&](uint32_t U) {
        const char Hi = static_cast<char>(U >> 8), Lo = static_cast<char>(U & 0xFF);
        Out += Le ? Lo : Hi; Out += Le ? Hi : Lo;
    };
    Out.clear();
    for (size_t I = 0; I < In.size();)
    {
        const auto C = static_cast<unsigned char>(In[I]);
        const size_t More = C < 0x80 ? 0 : C >= 0xC2 && C < 0xE0 ? 1 : C >= 0xE0 && C < 0xF0 ? 2 : C >= 0xF0 && C < 0xF5 ? 3 : 4;
        if (More == 4 || I + More >= In.size()) return false;
        uint32_t Cp = More == 0 ? C : More == 1 ? C & 0x1F : More == 2 ? C & 0x0F : C & 0x07;
        for (size_t K = 1; K <= More; ++K)
        {
            const auto N = static_cast<unsigned char>(In[I + K]);
            if ((N & 0xC0) != 0x80) return false;
            Cp = (Cp << 6) | (N & 0x3F);
        }
        if ((More == 2 && (Cp < 0x800 || (Cp >= 0xD800 && Cp < 0xE000))) || (More == 3 && (Cp < 0x10000 || Cp > 0x10FFFF))) return false;
        if (Cp < 0x10000) Put(Cp);
        else { Put(0xD800 + ((Cp - 0x10000) >> 10)); Put(0xDC00 + ((Cp - 0x10000) & 0x3FF)); }
        I += More + 1;
    }
    return true;
}

//The text of In as UTF-8, and the encoding it came in (by its byte-order mark).
bool XmlDecode(const std::string &In, XEnc &E, std::string &Text, std::string &Why)
{
    E = XEnc::Utf8;
    const bool Le = In.rfind("\xFF\xFE", 0) == 0, Be = In.rfind("\xFE\xFF", 0) == 0;
    if (!Le && !Be) { Text = In; return true; }
    if (Le && In.size() >= 4 && In[2] == '\0' && In[3] == '\0') { Why = "a UTF-32 file, which this edit does not read"; return false; }
    if (!FromUtf16(In.substr(2), Le, Text)) { Why = "a UTF-16 file that is not valid UTF-16"; return false; }
    E = Le ? XEnc::Utf16Le : XEnc::Utf16Be;
    return true;
}

bool XmlEncode(const std::string &Text, XEnc E, std::string &Out, std::string &Why)
{
    if (E == XEnc::Utf8) { Out = Text; return true; }
    std::string Body;
    if (!ToUtf16(Text, E == XEnc::Utf16Le, Body)) { Why = "the value is not UTF-8 text, so it cannot be written into a UTF-16 file"; return false; }
    Out = (E == XEnc::Utf16Le ? "\xFF\xFE" : "\xFE\xFF") + Body;
    return true;
}

} // namespace

bool FileEdits::XmlEdit(const std::string &Key, const std::string &Value, const std::string &In, std::string &Out, std::string &Why)
{
    std::vector<XPath> Paths;
    if (!SplitXmlKey(Key, Paths, Why)) return false;
    XEnc E;
    std::string Text, Edited;
    if (!XmlDecode(In, E, Text, Why)) return false;
    std::optional<std::string> Unused;
    return XmlWalk(Text, Paths, 0, &Value, Edited, Unused, Why) && XmlEncode(Edited, E, Out, Why);
}

std::optional<std::string> FileEdits::XmlGet(const std::string &Key, const std::string &In, std::string &Why)
{
    std::vector<XPath> Paths;
    if (!SplitXmlKey(Key, Paths, Why)) return std::nullopt;
    XEnc E;
    std::string Text;
    if (!XmlDecode(In, E, Text, Why)) return std::nullopt;
    std::optional<std::string> Got;
    std::string Unused;
    if (!XmlWalk(Text, Paths, 0, nullptr, Unused, Got, Why)) return std::nullopt;
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

namespace {
std::string TrimLine(const std::string &S)
{
    const size_t B = S.find_first_not_of(" \t\r\f\v");
    if (B == std::string::npos) return std::string();
    return S.substr(B, S.find_last_not_of(" \t\r\f\v") - B + 1);
}
}

bool FileEdits::LineHas(const std::string &Key, const std::string &Text)
{
    const std::string K = TrimLine(Key);
    for (size_t P = 0; P < Text.size();)
    {
        const size_t E = std::min(Text.find('\n', P), Text.size());
        if (TrimLine(Text.substr(P, E - P)) == K) return true;
        P = E + 1;
    }
    return false;
}

bool FileEdits::LineFlagEdit(const std::string &Key, const std::string &Value, const std::string &In, std::string &Out, std::string &Why)
{
    const std::string K = TrimLine(Key);
    if (K.empty() || K.find('\n') != std::string::npos) { Why = "a LineFlag KEY is one non-empty line"; return false; }
    if (Value != "1" && Value != "0")
    {
        Why = "a LineFlag VALUE is 1 (the line is there) or 0 (it is not), not '" + Value + "'";
        return false;
    }
    if (Value == "1")
    {
        Out = In;
        if (LineHas(K, In)) return true;
        const std::string NL = In.find("\r\n") != std::string::npos ? "\r\n" : "\n";   // the file's own line ending
        if (!Out.empty() && Out.back() != '\n') Out += NL;                                 // never fused onto the last line
        Out += K + NL;
        return true;
    }
    Out.clear();
    for (size_t P = 0; P < In.size();)
    {
        const size_t E = In.find('\n', P);
        const size_t End = E == std::string::npos ? In.size() : E + 1;                     // the line with its ending
        if (TrimLine(In.substr(P, (E == std::string::npos ? In.size() : E) - P)) != K) Out.append(In, P, End - P);
        P = End;
    }
    return true;
}

bool FileEdits::LineFlag(const std::string &Key, const std::string &Value, const std::filesystem::path &FilePath)
{
    std::error_code Ec;
    const bool Exists = std::filesystem::exists(FilePath, Ec);
    std::string In, Out, Why;
    if (Exists)
    {
        std::ifstream F(FilePath, std::ios::binary);
        if (!F) { LogErr("FileEdits::LineFlag", "Could not open file for reading: " + FilePath.string()); return false; }
        In.assign(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
    }
    if (!FileEdits::LineFlagEdit(Key, Value, In, Out, Why)) { LogErr("FileEdits::LineFlag", Why + " (" + FilePath.string() + ")"); return false; }
    if (Out == In && (Exists || Value == "0"))
    {
        LogOut("FileEdits::LineFlag", "'" + Key + "' already " + (Value == "1" ? "present in " : "absent from ") + FilePath.string());
        return true;
    }
    std::filesystem::create_directories(FilePath.parent_path(), Ec);
    std::ofstream F(FilePath, std::ios::binary | std::ios::trunc);
    if (!F || !(F << Out) || !F.flush()) { LogErr("FileEdits::LineFlag", "Could not write " + FilePath.string()); return false; }
    LogOut("FileEdits::LineFlag", "'" + Key + "' " + (Value == "1" ? "added to " : "removed from ") + FilePath.string());
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