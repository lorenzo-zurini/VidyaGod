#include "boxprefix.h"
#include "registrywrapper.h"
#include "platform/platform.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BoxPrefix {

namespace {

std::string Lower(std::string S)
{
    for (char &C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
    return S;
}

std::string Backslashed(std::string S)
{
    std::replace(S.begin(), S.end(), '/', '\\');
    while (S.size() > 3 && S.back() == '\\') S.pop_back();   // "C:\" keeps its separator
    return S;
}

// Whether Path is Root or below it (both backslashed, compared without case); Rest = the part below.
bool Below(const std::string &Path, const std::string &Root, std::string &Rest)
{
    if (Root.empty()) return false;
    const std::string P = Lower(Path), R = Lower(Root);
    if (P == R) { Rest.clear(); return true; }
    if (P.size() > R.size() && P.compare(0, R.size(), R) == 0 && P[R.size()] == '\\') { Rest = Path.substr(R.size() + 1); return true; }
    return false;
}

std::string Slashed(std::string S) { std::replace(S.begin(), S.end(), '\\', '/'); return S; }

std::string DriveRoot(const nlohmann::ordered_json &V)
{
    std::string R = V.is_string() ? V.get<std::string>() : std::string();
    std::replace(R.begin(), R.end(), '\\', '/');
    while (!R.empty() && R.back() == '/') R.pop_back();
    while (!R.empty() && R.front() == '/') R.erase(R.begin());
    return R;
}

bool LayoutBelow(const std::string &Rel, const std::string &Root)
{
    if (Root.empty()) return false;
    const std::string L = Lower(Rel), R = Lower(Root);
    return L == R || (L.size() > R.size() && L.compare(0, R.size(), R) == 0 && L[R.size()] == '/');
}

std::string GenericRel(const std::filesystem::path &P, const std::filesystem::path &Base)
{
    return P.lexically_relative(Base).generic_string();
}

// '*' and '?' over one name, without case (Windows names).
bool Glob(const std::string &Pat, const std::string &Name)
{
    const std::string P = Lower(Pat), N = Lower(Name);
    size_t p = 0, n = 0, Star = std::string::npos, Mark = 0;
    while (n < N.size())
    {
        if (p < P.size() && (P[p] == '?' || P[p] == N[n])) { ++p; ++n; }
        else if (p < P.size() && P[p] == '*') { Star = p++; Mark = n; }
        else if (Star != std::string::npos) { p = Star + 1; n = ++Mark; }
        else return false;
    }
    while (p < P.size() && P[p] == '*') ++p;
    return p == P.size();
}

//By streams: std::filesystem::copy_file with overwrite_existing failed "File exists" on Windows (MinGW) onto a file
//already in the vidyagodfs mount — every kept file the game had saved before. The source is opened first and the copy
//written beside the destination, then put in its place: a source that cannot be read, or a copy cut short, never
//leaves a kept save truncated.
bool CopyFile(const std::filesystem::path &From, const std::filesystem::path &To, std::vector<std::string> &Errors)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    std::ifstream In(From, std::ios::binary);
    if (!In) { Errors.push_back(From.string() + ": cannot be read — " + To.string() + " left as it was"); return false; }
    fs::create_directories(To.parent_path(), Ec);
    fs::path Tmp = To;
    Tmp += ".vidyagod-copy";
    {
        std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
        if (Out && In.peek() != std::ifstream::traits_type::eof()) Out << In.rdbuf();   // an empty file: nothing to stream
        Out.close();
        std::error_code Se;
        const auto Want = fs::file_size(From, Se), Got = fs::file_size(Tmp, Ec);
        if (!Out || In.bad() || Se || Ec || Want != Got)   // a read cut short leaves the copy short, and nothing else says so
        {
            fs::remove(Tmp, Ec);
            Errors.push_back(From.string() + " -> " + To.string() + ": cannot copy — the destination is left as it was");
            return false;
        }
    }
    fs::rename(Tmp, To, Ec);
    if (Ec) { std::error_code Re; fs::remove(To, Re); Ec.clear(); fs::rename(Tmp, To, Ec); }   // a rename that will not replace
    if (Ec) { Errors.push_back(Tmp.string() + " -> " + To.string() + ": " + Ec.message() + " (the copy is kept at the first)"); return false; }
    return true;
}

} // namespace


namespace {

//Whether a folder from Base down to Base/Rel's parent (Base itself not counted) is a link: a write or a delete there
//would land wherever the link points.
bool LinkOnTheWay(const std::filesystem::path &Base, const std::string &Rel)
{
    std::filesystem::path At = Base;
    const std::filesystem::path R(Rel);
    for (const auto &Part : R.parent_path())
    {
        At /= Part;
        if (Platform::IsLink(At)) return true;
    }
    return false;
}

//The regular files below Root (generic paths relative to it), never through a link. False when a folder could not be
//listed (named in Errors): the list is then incomplete.
bool FilesBelow(const std::filesystem::path &Root, std::vector<std::string> &Out, std::vector<std::string> &Errors,
                std::vector<std::string> *Dirs = nullptr, const std::function<bool(const std::string &)> &Enter = {})
{
    namespace fs = std::filesystem;
    bool Ok = true;
    std::vector<fs::path> Todo = {Root};
    while (!Todo.empty())
    {
        const fs::path D = Todo.back(); Todo.pop_back();
        std::error_code Ec;
        for (fs::directory_iterator It(D, Ec), End; !Ec && It != End; It.increment(Ec))
        {
            const fs::path P = It->path();
            if (Platform::IsLink(P)) continue;
            std::error_code Ie;
            if (It->is_directory(Ie))
            {
                if (Enter && !Enter(GenericRel(P, Root))) continue;
                Todo.push_back(P);
                if (Dirs) Dirs->push_back(GenericRel(P, Root));
            }
            else if (It->is_regular_file(Ie)) Out.push_back(GenericRel(P, Root));
        }
        if (Ec) { Errors.push_back(D.string() + ": " + Ec.message()); Ok = false; }
    }
    return Ok;
}

// regedit's string escaping: only the backslash and the quote.
std::string RegEscape(const std::string &S)
{
    std::string Out;
    for (char C : S) { if (C == '\\' || C == '"') Out.push_back('\\'); Out.push_back(C); }
    return Out;
}

void AppendUtf16(std::string &Out, uint32_t Cp)
{
    auto Unit = [&](uint32_t U) { Out.push_back(static_cast<char>(U & 0xff)); Out.push_back(static_cast<char>((U >> 8) & 0xff)); };
    if (Cp >= 0x10000) { Cp -= 0x10000; Unit(0xD800 + (Cp >> 10)); Unit(0xDC00 + (Cp & 0x3ff)); }
    else Unit(Cp);
}

// A wine typed string's body (str(N):"…" — \\, \", \0 and \xHHHH, the hex a UTF-16 unit) as UTF-16LE units, NUL-ended.
std::string WineStrToUtf16(const std::string &Body)
{
    std::string Out;
    for (size_t i = 0; i < Body.size(); ++i)
    {
        const unsigned char C = static_cast<unsigned char>(Body[i]);
        if (C == '\\' && i + 1 < Body.size())
        {
            const char N = Body[++i];
            if (N == '0') { AppendUtf16(Out, 0); continue; }
            if (N == 'x')
            {
                uint32_t V = 0; size_t K = 0;
                while (K < 4 && i + 1 < Body.size() && std::isxdigit(static_cast<unsigned char>(Body[i + 1])))
                { V = V * 16 + static_cast<uint32_t>(std::stoul(std::string(1, Body[++i]), nullptr, 16)); ++K; }
                AppendUtf16(Out, V);
                continue;
            }
            AppendUtf16(Out, static_cast<unsigned char>(N));
            continue;
        }
        // UTF-8 sequence → code point
        uint32_t Cp = C; size_t More = 0;
        if (C >= 0xF0) { Cp = C & 0x07; More = 3; } else if (C >= 0xE0) { Cp = C & 0x0F; More = 2; } else if (C >= 0xC0) { Cp = C & 0x1F; More = 1; }
        for (; More && i + 1 < Body.size(); --More) Cp = (Cp << 6) | (static_cast<unsigned char>(Body[++i]) & 0x3F);
        AppendUtf16(Out, Cp);
    }
    AppendUtf16(Out, 0);
    return Out;
}

std::string HexBytes(const std::string &Bytes)
{
    std::string Out; char B[4];
    for (size_t i = 0; i < Bytes.size(); ++i)
    {
        std::snprintf(B, sizeof B, "%02x", static_cast<unsigned char>(Bytes[i]));
        if (i) Out.push_back(',');
        Out += B;
    }
    return Out;
}

} // namespace

std::string BoxRelForReal(const std::string &RealPath, const std::map<std::string, std::string> &Host)
{
    const std::string P = Backslashed(RealPath);
    if (P.size() < 2 || !std::isalpha(static_cast<unsigned char>(P[0])) || P[1] != ':') return {};
    auto Host1 = [&](const char *K) { const auto It = Host.find(K); return It == Host.end() ? std::string() : Backslashed(It->second); };
    std::string Rest;
    //The public profile and the user's both sit in C:\Users; neither is below the other, so the order is free.
    for (const auto &[Key, BoxDir] : {std::pair<const char *, const char *>{"Public", "user/public"},
                                      {"UserProfile", "user/current"}, {"ProgramData", "user/all"}})
        if (Below(P, Host1(Key), Rest)) return Rest.empty() ? std::string(BoxDir) : std::string(BoxDir) + "/" + Slashed(Rest);
    const std::string Drive(1, static_cast<char>(std::toupper(static_cast<unsigned char>(P[0]))));
    const std::string After = P.size() > 3 ? P.substr(3) : std::string();
    return After.empty() ? "drive/" + Drive : "drive/" + Drive + "/" + Slashed(After);
}

std::string RealForLayout(const std::string &LayoutRel, const nlohmann::ordered_json &Drives)
{
    if (!Drives.is_object()) return {};
    std::string Rel = LayoutRel;
    std::replace(Rel.begin(), Rel.end(), '\\', '/');
    for (const auto &[Drive, V] : Drives.items())
    {
        const std::string Root = DriveRoot(V);
        if (Drive.size() < 2 || Drive[1] != ':' || !LayoutBelow(Rel, Root)) continue;
        const std::string Rest = Rel.size() > Root.size() ? Rel.substr(Root.size() + 1) : std::string();
        return Drive.substr(0, 2) + "\\" + Backslashed(Rest);
    }
    return {};
}

bool InBox(const std::string &LayoutRel, const std::string &ContentRoot, const nlohmann::ordered_json &Drives)
{
    std::string Rel = LayoutRel;
    std::replace(Rel.begin(), Rel.end(), '\\', '/');
    while (!Rel.empty() && Rel.front() == '/') Rel.erase(Rel.begin());
    std::string Content = ContentRoot;
    std::replace(Content.begin(), Content.end(), '\\', '/');
    while (!Content.empty() && Content.back() == '/') Content.pop_back();
    return !RealForLayout(Rel, Drives).empty() && !LayoutBelow(Rel, Content);
}

bool Materialise(const std::filesystem::path &RuntimePath, const std::string &ContentRoot, const nlohmann::ordered_json &Drives,
                 const std::map<std::string, std::string> &Host, const std::filesystem::path &BoxRoot,
                 std::vector<std::string> &Errors, std::vector<std::string> *Placed)
{
    namespace fs = std::filesystem;
    const size_t Before = Errors.size();
    if (!Drives.is_object()) return true;
    for (const auto &[Drive, V] : Drives.items())
    {
        (void)Drive;
        const std::string Root = DriveRoot(V);
        std::error_code Ec;
        const fs::path Top = RuntimePath / Root;
        if (Root.empty() || Platform::IsLink(Top) || !fs::is_directory(Top, Ec)) continue;
        std::vector<std::string> Files;
        //Never into the game folder: the game reads it from the mount, and listing it (through the mount) costs a walk
        //of the whole game and can fail on a folder the box never needed.
        FilesBelow(Top, Files, Errors, nullptr, [&](const std::string &Dir) { return InBox(GenericRel(Top / Dir, RuntimePath), ContentRoot, Drives); });
        for (const std::string &F : Files)
        {
            const std::string Rel = GenericRel(Top / F, RuntimePath);
            if (!InBox(Rel, ContentRoot, Drives)) continue;
            const std::string Box = BoxRelForReal(RealForLayout(Rel, Drives), Host);
            if (Box.empty()) continue;
            if (CopyFile(Top / F, BoxRoot / Box, Errors) && Placed) Placed->push_back(Box);
        }
    }
    return Errors.size() == Before;
}

bool SyncKeepsBack(const std::filesystem::path &RuntimePath, const std::string &ContentRoot, const nlohmann::ordered_json &Drives,
                   const std::map<std::string, std::string> &Host, const std::filesystem::path &BoxRoot,
                   const std::vector<PersistTarget> &KeepDirs, const std::vector<PersistTarget> &KeepFiles,
                   const std::vector<std::string> &Placed, std::vector<std::string> &Errors)
{
    namespace fs = std::filesystem;
    const size_t Before = Errors.size();
    auto BoxOf = [&](const std::string &Rel) { return BoxRelForReal(RealForLayout(Rel, Drives), Host); };
    //Into the runtime, never through a link the game made there (it would write wherever the link points).
    auto Put = [&](const fs::path &From, const std::string &RuntimeRel) {
        if (LinkOnTheWay(RuntimePath, RuntimeRel) || Platform::IsLink(RuntimePath / RuntimeRel))
        { Errors.push_back((RuntimePath / RuntimeRel).string() + ": a link is on its way — not written through"); return; }
        CopyFile(From, RuntimePath / RuntimeRel, Errors);
    };
    //What the game deleted: a file the box was GIVEN (Placed) that it no longer has. Nothing else is ever taken for
    //deleted — not a file the box never got (a copy that failed, an empty folder), nor anything when the box is missing.
    auto Gone = [&](const std::string &BoxRel, const std::string &RuntimeRel) {
        std::error_code Ec;
        const bool There = fs::exists(BoxRoot / BoxRel, Ec);
        if (Ec) { Errors.push_back((BoxRoot / BoxRel).string() + ": " + Ec.message() + " — not taken for deleted"); return; }
        if (There || Platform::IsLink(BoxRoot / BoxRel)) return;
        const fs::path R = RuntimePath / RuntimeRel;
        if (LinkOnTheWay(RuntimePath, RuntimeRel)) { Errors.push_back(R.string() + ": a link is on its way — not deleted through"); return; }
        if (!Platform::IsLink(R) && !fs::is_regular_file(R, Ec)) return;
        if (!fs::remove(R, Ec) && Ec) Errors.push_back(R.string() + " (deleted by the game): " + Ec.message());
    };
    auto Under = [](const std::string &P, const std::string &Dir, std::string &Rest) {
        const std::string LP = Lower(P), LD = Lower(Dir);
        if (LP.size() <= LD.size() + 1 || LP.compare(0, LD.size(), LD) != 0 || LP[LD.size()] != '/') return false;
        Rest = P.substr(Dir.size() + 1);
        return true;
    };
    for (const PersistTarget &D : KeepDirs)
    {
        if (D.Path.empty() || !InBox(D.Path, ContentRoot, Drives)) continue;
        const std::string B = BoxOf(D.Path);
        std::error_code Ec;
        const fs::path Src = BoxRoot / B;
        if (B.empty() || Platform::IsLink(Src) || !fs::is_directory(Src, Ec)) continue;   // no copy in the box: nothing is touched
        std::vector<std::string> Files, Dirs;
        const bool Listed = FilesBelow(Src, Files, Errors, &Dirs);
        for (const std::string &R : Dirs)
            if (!LinkOnTheWay(RuntimePath, D.Path + "/" + R + "/x")) fs::create_directories(RuntimePath / D.Path / R, Ec);
        for (const std::string &R : Files) Put(Src / R, D.Path + "/" + R);
        if (!Listed) continue;   // a box folder that could not be read whole says nothing about what the game deleted
        std::string Rest;
        for (const std::string &P : Placed)
            if (Under(P, B, Rest)) Gone(P, D.Path + "/" + Rest);
    }
    for (const PersistTarget &F : KeepFiles)
    {
        if (F.Path.empty() || !InBox(F.Path, ContentRoot, Drives)) continue;
        const fs::path Rel(F.Path);
        const std::string Leaf = Rel.filename().string();
        std::error_code Ec;
        if (Leaf.find_first_of("*?") == std::string::npos)
        {
            const std::string B = BoxOf(F.Path);
            if (B.empty()) continue;
            if (!Platform::IsLink(BoxRoot / B) && fs::is_regular_file(BoxRoot / B, Ec)) Put(BoxRoot / B, F.Path);
            else if (std::any_of(Placed.begin(), Placed.end(), [&](const std::string &P) { return Lower(P) == Lower(B); }))
                Gone(B, F.Path);
            continue;
        }
        const std::string Dir = Rel.parent_path().generic_string();
        const std::string B = BoxOf(Dir);
        if (B.empty() || Platform::IsLink(BoxRoot / B) || !fs::is_directory(BoxRoot / B, Ec)) continue;
        for (const fs::directory_entry &E : fs::directory_iterator(BoxRoot / B, Ec))
            if (!Platform::IsLink(E.path()) && E.is_regular_file(Ec) && Glob(Leaf, E.path().filename().string()))
                Put(E.path(), Dir + "/" + E.path().filename().string());
        if (Ec) { Errors.push_back((BoxRoot / B).string() + ": " + Ec.message()); continue; }
        std::string Rest;
        for (const std::string &P : Placed)
            if (Under(P, B, Rest) && Rest.find('/') == std::string::npos && Glob(Leaf, Rest)) Gone(P, Dir + "/" + Rest);
    }
    return Errors.size() == Before;
}

std::vector<std::string> KeptNotInBox(const std::filesystem::path &RuntimePath, const std::string &ContentRoot,
                                      const nlohmann::ordered_json &Drives, const std::map<std::string, std::string> &Host,
                                      const std::filesystem::path &BoxRoot, const std::vector<PersistTarget> &KeepDirs,
                                      const std::vector<PersistTarget> &KeepFiles)
{
    namespace fs = std::filesystem;
    std::vector<std::string> Missing;
    auto BoxOf = [&](const std::string &Rel) { return BoxRelForReal(RealForLayout(Rel, Drives), Host); };
    //One kept runtime file: the box must hold it whole (a plain file of the same size) where the game will look.
    auto Check = [&](const std::string &RuntimeRel) {
        std::error_code Ec;
        const fs::path R = RuntimePath / RuntimeRel;
        if (Platform::IsLink(R) || !fs::is_regular_file(R, Ec)) return;                 // nothing of the user's to carry
        const std::string B = BoxOf(RuntimeRel);
        if (B.empty()) { Missing.push_back(R.string() + ": no drive lays it out"); return; }
        const fs::path In = BoxRoot / B;
        std::error_code E1, E2;
        if (Platform::IsLink(In) || !fs::is_regular_file(In, E1) || fs::file_size(In, E1) != fs::file_size(R, E2) || E1 || E2)
            Missing.push_back(R.string() + " -> " + In.string());
    };
    for (const PersistTarget &D : KeepDirs)
    {
        if (D.Path.empty() || !InBox(D.Path, ContentRoot, Drives)) continue;
        std::error_code Ec;
        if (Platform::IsLink(RuntimePath / D.Path) || !fs::is_directory(RuntimePath / D.Path, Ec)) continue;   // nothing kept there yet
        std::vector<std::string> Files, Errors;
        //A kept folder that cannot be listed whole is not known to have reached the box.
        if (!FilesBelow(RuntimePath / D.Path, Files, Errors)) for (const std::string &E : Errors) Missing.push_back(E);
        for (const std::string &F : Files) Check(D.Path + "/" + F);
    }
    for (const PersistTarget &F : KeepFiles)
    {
        if (F.Path.empty() || !InBox(F.Path, ContentRoot, Drives)) continue;
        const fs::path Rel(F.Path);
        const std::string Leaf = Rel.filename().string(), Dir = Rel.parent_path().generic_string();
        if (Leaf.find_first_of("*?") == std::string::npos) { Check(F.Path); continue; }
        std::error_code Ec;
        for (fs::directory_iterator It(RuntimePath / Dir, Ec), End; !Ec && It != End; It.increment(Ec))
            if (Glob(Leaf, It->path().filename().string())) Check(Dir + "/" + It->path().filename().string());
    }
    return Missing;
}

std::string ToRegFile(const RegistryWrapper &RW, size_t *Skipped)
{
    size_t Left = 0;
    std::ostringstream Out;
    Out << "Windows Registry Editor Version 5.00\r\n";
    RW.ForEachKey([&](const std::string &Full, const RegistryKey &K) {
        const std::string L = Lower(Full);
        if (L.rfind("hku\\", 0) == 0) { Left += 1 + K.Values.size(); return; }
        std::string Name = Full;
        if (L.rfind("hklm\\", 0) == 0) Name = "HKEY_LOCAL_MACHINE" + Full.substr(4);
        else if (L.rfind("hkcu\\", 0) == 0) Name = "HKEY_CURRENT_USER" + Full.substr(4);
        Out << "\r\n[" << Name << "]\r\n";
        for (const auto &[VName, V] : K.Values)
        {
            std::string Payload;
            switch (V.Type)
            {
                case RegType::Sz:
                    //A line break cannot stand inside a quoted .reg string: regedit spells such a value hex(1).
                    Payload = V.Str.find_first_of("\r\n") == std::string::npos ? "\"" + RegEscape(V.Str) + "\""
                                                                                 : "hex(1):" + HexBytes(WineStrToUtf16(RegEscape(V.Str)));
                    break;
                case RegType::Dword:
                case RegType::Hex:
                case RegType::HexTyped: Payload = V.RawPayload; break;
                case RegType::StrTyped:
                {
                    //str(N):"…" — wine's typed string; regedit spells it hex(N): over its UTF-16LE units.
                    const size_t Open = V.RawPayload.find('"'), Close = V.RawPayload.rfind('"');
                    const size_t Paren = V.RawPayload.find(')');
                    if (Open == std::string::npos || Close <= Open || Paren == std::string::npos) { ++Left; continue; }
                    Payload = V.RawPayload.substr(0, Paren + 1);
                    Payload.replace(0, 3, "hex");
                    Payload += ":" + HexBytes(WineStrToUtf16(V.RawPayload.substr(Open + 1, Close - Open - 1)));
                    break;
                }
                default: ++Left; continue;
            }
            //A wine hex payload wraps with "\\\n  "; regedit's line ends are CRLF.
            const std::string Line = (VName.empty() ? std::string("@") : "\"" + RegEscape(VName) + "\"") + "=" + Payload;
            for (size_t I = 0; I < Line.size(); ++I) { if (Line[I] == '\n' && (I == 0 || Line[I - 1] != '\r')) Out << '\r'; Out << Line[I]; }
            Out << "\r\n";
        }
    });
    if (Skipped) *Skipped = Left;
    return Out.str();
}

bool FromRegFile(const std::string &Text, RegistryWrapper &Out, std::string &Err)
{
    //Logical lines: CRLF or LF, a trailing backslash continues a hex value on the next line (its indent dropped).
    std::vector<std::string> Lines;
    {
        std::string Cur, Line;
        std::istringstream In(Text);
        bool Cont = false;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Cont) { size_t S = Line.find_first_not_of(" \t"); Line = S == std::string::npos ? std::string() : Line.substr(S); }
            Cur += Line;
            Cont = !Cur.empty() && Cur.back() == '\\' && Cur.find('=') != std::string::npos && Cur.find("=\"") == std::string::npos;
            if (Cont) { Cur.pop_back(); continue; }
            Lines.push_back(Cur); Cur.clear();
        }
        if (!Cur.empty()) Lines.push_back(Cur);
    }
    std::string Key;
    size_t No = 0;
    for (const std::string &L : Lines)
    {
        ++No;
        if (L.empty() || L[0] == ';') continue;
        if (No == 1 && L.find("Windows Registry Editor") != std::string::npos) continue;
        if (L.find("Windows Registry Editor") == 0 || L == "REGEDIT4") continue;
        if (L[0] == '[')
        {
            const size_t Close = L.rfind(']');
            if (Close == std::string::npos) { Err = "line " + std::to_string(No) + ": an unclosed key"; return false; }
            Key = L.substr(1, Close - 1);
            if (!Key.empty() && Key[0] == '-') { Key.clear(); continue; }   // a deletion: nothing to keep
            Key = RegistryWrapper::NormalizeRootPrefix(Key);
            Out.EnsureKey(Key);
            continue;
        }
        if (Key.empty()) continue;
        std::string Name; size_t I = 0;
        if (L[0] == '@') I = 1;
        else if (L[0] == '"')
        {
            for (I = 1; I < L.size() && L[I] != '"'; ++I)
            { if (L[I] == '\\' && I + 1 < L.size()) ++I; Name.push_back(L[I]); }
            ++I;
        }
        else { Err = "line " + std::to_string(No) + ": a value line that names no value"; return false; }
        if (I >= L.size() || L[I] != '=') { Err = "line " + std::to_string(No) + ": no '=' after the value name"; return false; }
        const std::string P = L.substr(I + 1);
        if (P == "-") continue;
        if (!P.empty() && P[0] == '"')
        {
            std::string S;
            for (size_t J = 1; J < P.size() && P[J] != '"'; ++J) { if (P[J] == '\\' && J + 1 < P.size()) ++J; S.push_back(P[J]); }
            Out.SetValue(Key, Name, RegistryWrapper::SzValue(S));
            continue;
        }
        std::string Compact;
        for (char C : P) if (C != ' ' && C != '\t') Compact.push_back(C);
        if (Compact.rfind("dword:", 0) == 0 || Compact.rfind("hex:", 0) == 0 || Compact.rfind("hex(", 0) == 0)
        {
            Out.SetValue(Key, Name, RegistryWrapper::ManifestStringToValue(Compact));
            continue;
        }
        Err = "line " + std::to_string(No) + ": a value of a kind this reader does not know: " + P.substr(0, 40);
        return false;
    }
    return true;
}

std::string Utf8ToUtf16Le(const std::string &Utf8)
{
    std::string Out = "\xFF\xFE";
    for (size_t i = 0; i < Utf8.size(); ++i)
    {
        const unsigned char C = static_cast<unsigned char>(Utf8[i]);
        uint32_t Cp = C; size_t More = 0;
        if (C >= 0xF0) { Cp = C & 0x07; More = 3; } else if (C >= 0xE0) { Cp = C & 0x0F; More = 2; } else if (C >= 0xC0) { Cp = C & 0x1F; More = 1; }
        for (; More && i + 1 < Utf8.size(); --More) Cp = (Cp << 6) | (static_cast<unsigned char>(Utf8[++i]) & 0x3F);
        AppendUtf16(Out, Cp);
    }
    return Out;
}

std::string Utf16LeToUtf8(const std::string &Bytes)
{
    std::string Out;
    size_t i = (Bytes.size() >= 2 && static_cast<unsigned char>(Bytes[0]) == 0xFF && static_cast<unsigned char>(Bytes[1]) == 0xFE) ? 2 : 0;
    auto Unit = [&](size_t At) { return static_cast<uint32_t>(static_cast<unsigned char>(Bytes[At]) | (static_cast<unsigned char>(Bytes[At + 1]) << 8)); };
    for (; i + 1 < Bytes.size(); i += 2)
    {
        uint32_t Cp = Unit(i);
        if (Cp >= 0xD800 && Cp < 0xDC00 && i + 3 < Bytes.size())
        {
            const uint32_t Lo = Unit(i + 2);
            if (Lo >= 0xDC00 && Lo < 0xE000) { Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00); i += 2; }
        }
        if (Cp < 0x80) Out.push_back(static_cast<char>(Cp));
        else if (Cp < 0x800) { Out.push_back(static_cast<char>(0xC0 | (Cp >> 6))); Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F))); }
        else if (Cp < 0x10000) { Out.push_back(static_cast<char>(0xE0 | (Cp >> 12))); Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F))); Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F))); }
        else { Out.push_back(static_cast<char>(0xF0 | (Cp >> 18))); Out.push_back(static_cast<char>(0x80 | ((Cp >> 12) & 0x3F))); Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F))); Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F))); }
    }
    return Out;
}

} // namespace BoxPrefix
