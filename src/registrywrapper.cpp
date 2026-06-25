#include "registrywrapper.h"
#include "commonutils.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cctype>
#include <algorithm>

using nlohmann::ordered_json;

// ============================================================================
//  Small string helpers (file-local)
// ============================================================================

namespace {

//ASCII lowercase (registry keys/value names are case-insensitive in Windows/Wine).
char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool CIEqual(const std::string &A, const std::string &B)
{
    if (A.size() != B.size()) return false;
    for (size_t i = 0; i < A.size(); ++i)
        if (LowerAscii(A[i]) != LowerAscii(B[i])) return false;
    return true;
}

std::string ToLowerAscii(std::string S)
{
    for (char &c : S) c = LowerAscii(c);
    return S;
}

//True if the value line's payload ends with a bare backslash (hex line-continuation).
//A quoted string ends with '"', a dword with a hex digit — only multiline hex ends with '\'.
bool EndsWithContinuation(const std::string &Line)
{
    return !Line.empty() && Line.back() == '\\';
}

//Decode a wine \xHH (1-4 hex digit) escape starting at S[i] (S[i]=='x'); returns the char value and
//advances i past the hex digits. Only the low byte is kept (sufficient for ASCII key/name matching).
unsigned ReadHexEscape(const std::string &S, size_t &i)
{
    ++i; // skip 'x'
    unsigned val = 0; int n = 0;
    while (i < S.size() && n < 4 && std::isxdigit((unsigned char)S[i]))
    {
        char c = S[i];
        val = val * 16 + (c <= '9' ? c - '0' : (LowerAscii(c) - 'a' + 10));
        ++i; ++n;
    }
    return val;
}

//Unescape a wine value-string body (between the quotes): \\ -> \, \" -> ", \xHH -> char, \c -> c.
std::string ValueUnescape(const std::string &S)
{
    std::string Out; Out.reserve(S.size());
    for (size_t i = 0; i < S.size(); ++i)
    {
        if (S[i] == '\\' && i + 1 < S.size())
        {
            char n = S[i + 1];
            if (n == 'x') { size_t j = i + 1; unsigned v = ReadHexEscape(S, j); Out.push_back(char(v & 0xff)); i = j - 1; }
            else { Out.push_back(n); ++i; }
        }
        else Out.push_back(S[i]);
    }
    return Out;
}

//Unescape a key-path segment (\x5c -> backslash, \x5b/\x5d -> [], \xHH -> char). Used only to build
//the decoded Name for addressing/diffing; output always re-emits the verbatim RawSegment.
std::string PathUnescape(const std::string &S)
{
    std::string Out; Out.reserve(S.size());
    for (size_t i = 0; i < S.size(); ++i)
    {
        if (S[i] == '\\' && i + 1 < S.size() && S[i + 1] == 'x')
        { size_t j = i + 1; unsigned v = ReadHexEscape(S, j); Out.push_back(char(v & 0xff)); i = j - 1; }
        else if (S[i] == '\\' && i + 1 < S.size()) { Out.push_back(S[i + 1]); ++i; }
        else Out.push_back(S[i]);
    }
    return Out;
}

//Escape a value string / value name for output: \ -> \\, " -> \", control/non-ASCII -> \xHH.
std::string EscapeValueStr(const std::string &S)
{
    std::string Out; Out.reserve(S.size() + 8);
    char buf[8];
    for (unsigned char c : S)
    {
        if (c == '\\') Out += "\\\\";
        else if (c == '"') Out += "\\\"";
        else if (c < 0x20 || c > 0x7e) { std::snprintf(buf, sizeof buf, "\\x%02x", c); Out += buf; }
        else Out.push_back(char(c));
    }
    return Out;
}

//Escape a key-path segment for output: \ -> \x5c, [ -> \x5b, ] -> \x5d, control/non-ASCII -> \xHH.
//(The \\ level separator is emitted by the caller, never by this function.)
std::string EscapeSegment(const std::string &S)
{
    std::string Out; Out.reserve(S.size() + 8);
    char buf[8];
    for (unsigned char c : S)
    {
        if (c == '\\' || c == '[' || c == ']' || c < 0x20 || c > 0x7e)
        { std::snprintf(buf, sizeof buf, "\\x%02x", c); Out += buf; }
        else Out.push_back(char(c));
    }
    return Out;
}

//Scan a quoted string starting at S[Open]=='"'; fill Decoded with the unescaped body and set Close to
//the index of the terminating quote. Returns false if no closing quote is found.
bool ScanQuoted(const std::string &S, size_t Open, std::string &Decoded, size_t &Close)
{
    std::string body;
    size_t i = Open + 1;
    for (; i < S.size(); ++i)
    {
        char c = S[i];
        if (c == '\\' && i + 1 < S.size()) { body.push_back(c); body.push_back(S[i + 1]); ++i; }
        else if (c == '"') { Close = i; Decoded = ValueUnescape(body); return true; }
        else body.push_back(c);
    }
    return false;
}

//Split a bracket path on the two-char level separator "\\". A single backslash inside a segment is
//part of a \xHH escape (e.g. \x5c), never a separator, so scanning for the "\\" pair is unambiguous.
std::vector<std::string> SplitOnDoubleBackslash(const std::string &S)
{
    std::vector<std::string> Out;
    std::string cur;
    for (size_t i = 0; i < S.size(); ++i)
    {
        if (S[i] == '\\' && i + 1 < S.size() && S[i + 1] == '\\') { Out.push_back(cur); cur.clear(); ++i; }
        else cur.push_back(S[i]);
    }
    Out.push_back(cur);
    return Out;
}

//Split a caller-supplied path (manifest/query form) on single backslashes into decoded segments.
std::vector<std::string> SplitOnSingleBackslash(const std::string &S)
{
    std::vector<std::string> Out;
    std::string cur;
    for (char c : S) { if (c == '\\') { Out.push_back(cur); cur.clear(); } else cur.push_back(c); }
    if (!cur.empty() || !S.empty()) Out.push_back(cur);
    return Out;
}

std::string Trim(const std::string &S)
{
    size_t a = S.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = S.find_last_not_of(" \t");
    return S.substr(a, b - a + 1);
}

//Normalize a hex/dword payload for equality comparison: drop continuation backslashes and whitespace.
std::string NormalizePayload(const std::string &S)
{
    std::string Out;
    for (char c : S) if (c != '\\' && c != '\n' && c != '\r' && c != ' ' && c != '\t') Out.push_back(c);
    return Out;
}

} // namespace

// ============================================================================
//  Root / path helpers
// ============================================================================

HiveRoot RegistryWrapper::RootForPath(const std::string &FullPath)
{
    std::string P = ToLowerAscii(FullPath);
    if (P.rfind("hkcu", 0) == 0 || P.rfind("hkey_current_user", 0) == 0) return HiveRoot::User;
    if (P.rfind("hku\\.default", 0) == 0 || P.rfind("hkey_users\\.default", 0) == 0) return HiveRoot::UserDefault;
    return HiveRoot::Machine; // HKLM / HKEY_LOCAL_MACHINE / HKCR (Classes lives under Machine) / default
}

std::string RegistryWrapper::NormalizeRootPrefix(std::string Path)
{
    auto Replace = [&](const char *From, const char *To) {
        std::string lp = ToLowerAscii(Path), lf = ToLowerAscii(From);
        if (lp.rfind(lf, 0) == 0) { Path = std::string(To) + Path.substr(std::string(From).size()); return true; }
        return false;
    };
    Replace("HKEY_LOCAL_MACHINE", "HKLM") || Replace("HKEY_CURRENT_USER", "HKCU") ||
        Replace("HKEY_USERS", "HKU") || Replace("HKEY_CLASSES_ROOT", "HKCR");
    return Path;
}

//Returns the path relative to its hive root (the part after the root token), single-backslash form.
std::string RegistryWrapper::StripRootPrefix(const std::string &FullPath)
{
    std::string P = NormalizeRootPrefix(FullPath);
    // Drop the leading root token. UserDefault is two levels (HKU\.Default); others are one.
    std::string lp = ToLowerAscii(P);
    auto After = [&](size_t n) { return (n < P.size() && P[n] == '\\') ? P.substr(n + 1) : (n >= P.size() ? std::string() : P.substr(n)); };
    if (lp.rfind("hku\\.default", 0) == 0) return After(std::string("hku\\.default").size());
    if (lp.rfind("hklm", 0) == 0) return After(4);
    if (lp.rfind("hkcu", 0) == 0) return After(4);
    if (lp.rfind("hkcr", 0) == 0) return After(4);
    if (lp.rfind("hku",  0) == 0) return After(3);
    // No recognized root token — treat the whole thing as relative.
    return P;
}

// ============================================================================
//  Loading / parsing
// ============================================================================

//Find a direct child by (case-insensitive) decoded name, creating it if absent.
static RegistryKey *FindOrCreateChild(RegistryKey *Parent, const std::string &Name, const std::string &RawSegment)
{
    for (auto &C : Parent->Children)
        if (CIEqual(C->Name, Name)) { if (C->RawSegment.empty() && !RawSegment.empty()) C->RawSegment = RawSegment; return C.get(); }
    auto Node = std::make_unique<RegistryKey>();
    Node->Name = Name;
    Node->RawSegment = RawSegment.empty() ? EscapeSegment(Name) : RawSegment;
    RegistryKey *Ptr = Node.get();
    Parent->Children.push_back(std::move(Node));
    return Ptr;
}

//Classify a value payload (the verbatim text after '='), setting Type / decoded Str / type codes.
static void ClassifyPayload(const std::string &Payload, RegistryValue &V)
{
    V.RawPayload = Payload;
    if (!Payload.empty() && Payload[0] == '"')
    {
        std::string decoded; size_t close;
        V.Type = RegType::Sz;
        if (ScanQuoted(Payload, 0, decoded, close)) V.Str = decoded;
    }
    else if (Payload.rfind("dword:", 0) == 0) V.Type = RegType::Dword;
    else if (Payload.rfind("hex:", 0) == 0)    V.Type = RegType::Hex;
    else if (Payload.rfind("hex(", 0) == 0)
    {
        V.Type = RegType::HexTyped;
        size_t close = Payload.find(')');
        if (close != std::string::npos) V.HexTypeCode = (uint32_t)std::strtoul(Payload.substr(4, close - 4).c_str(), nullptr, 16);
    }
    else if (Payload.rfind("str(", 0) == 0)
    {
        V.Type = RegType::StrTyped;
        size_t close = Payload.find(')');
        if (close != std::string::npos) V.StrTypeCode = (uint32_t)std::strtoul(Payload.substr(4, close - 4).c_str(), nullptr, 16);
    }
    else V.Type = RegType::Unknown;
}

bool RegistryWrapper::LoadHiveFile(const std::filesystem::path &File, HiveRoot Root)
{
    std::ifstream In(File, std::ios::binary);
    if (!In) { LogWarn("RegistryWrapper::LoadHiveFile", "Cannot open " + File.string()); return false; }

    Hive H;
    H.Root = Root;
    H.RootKey = std::make_unique<RegistryKey>();

    std::string Line;
    bool InBody = false;
    RegistryKey *Cur = nullptr;

    while (std::getline(In, Line))
    {
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();

        if (!InBody)
        {
            if (!Line.empty() && Line[0] == '[') InBody = true;        // first key block begins the body
            else { H.RawHeaderLines += Line; H.RawHeaderLines += '\n'; continue; }
        }

        if (!Line.empty() && Line[0] == '[')
        {
            // Key block header: [escaped\\path] <decimal-timestamp>
            size_t close = Line.find(']');
            std::string inside = (close == std::string::npos) ? Line.substr(1) : Line.substr(1, close - 1);
            std::string ts = (close != std::string::npos) ? Trim(Line.substr(close + 1)) : "";
            RegistryKey *node = H.RootKey.get();
            for (const std::string &rawSeg : SplitOnDoubleBackslash(inside))
                node = FindOrCreateChild(node, PathUnescape(rawSeg), rawSeg);
            node->Timestamp = ts;
            node->Explicit = true; // this key has its own block in the source
            Cur = node;
            continue;
        }
        if (!Line.empty() && Line[0] == '#') { if (Cur) Cur->RawMetaLines.push_back(Line); continue; }
        if (Line.empty()) continue;

        // Value line. Join hex line-continuations (trailing '\') verbatim before classifying.
        std::string Full = Line;
        while (EndsWithContinuation(Full))
        {
            std::string Cont;
            if (!std::getline(In, Cont)) break;
            if (!Cont.empty() && Cont.back() == '\r') Cont.pop_back();
            Full += '\n';
            Full += Cont;
        }
        if (!Cur) continue;

        RegistryValue V;
        std::string name;
        size_t eq;
        if (Full[0] == '@') { V.RawName = "@"; name = ""; eq = 1; }
        else
        {
            std::string decoded; size_t close;
            if (Full[0] != '"' || !ScanQuoted(Full, 0, decoded, close)) continue; // malformed
            V.RawName = Full.substr(0, close + 1);
            name = decoded;
            eq = close + 1;
        }
        if (eq >= Full.size() || Full[eq] != '=') continue;
        ClassifyPayload(Full.substr(eq + 1), V);
        Cur->Values.emplace_back(name, std::move(V));
    }

    Hives[Root] = std::move(H);
    return true;
}

bool RegistryWrapper::LoadPrefix(const std::filesystem::path &PrefixPath)
{
    struct { const char *File; HiveRoot Root; } Files[] = {
        { "system.reg",  HiveRoot::Machine },
        { "user.reg",    HiveRoot::User },
        { "userdef.reg", HiveRoot::UserDefault },
    };
    bool Ok = true;
    for (const auto &F : Files)
    {
        std::filesystem::path P = PrefixPath / F.File;
        if (std::filesystem::exists(P)) Ok = LoadHiveFile(P, F.Root) && Ok;
    }
    return Ok;
}

// ============================================================================
//  Saving / serializing
// ============================================================================

//Emit one value line (verbatim when clean; synthesized when dirtied by authoring/SetValue).
static void EmitValue(std::ostream &Out, const std::string &Name, const RegistryValue &V)
{
    if (!V.Dirty) { Out << V.RawName << "=" << V.RawPayload << "\n"; return; }
    Out << (Name.empty() ? std::string("@") : "\"" + EscapeValueStr(Name) + "\"") << "=";
    if (V.Type == RegType::Sz) Out << "\"" << EscapeValueStr(V.Str) << "\"";
    else                       Out << V.RawPayload; // dword:/hex:/... already in storage form
    Out << "\n";
}

//Recursively emit a key and its descendants (depth-first preorder, preserving insertion order — the
//same order Wine writes, so an untouched hive round-trips in place). ParentEsc is the escaped path of
//the parent (no brackets); empty for the top level.
static void EmitKey(std::ostream &Out, const RegistryKey *Node, const std::string &ParentEsc, bool &AnyEmitted)
{
    for (const auto &Child : Node->Children)
    {
        std::string seg = Child->RawSegment.empty() ? EscapeSegment(Child->Name) : Child->RawSegment;
        std::string full = ParentEsc.empty() ? seg : ParentEsc + "\\\\" + seg;

        // Only keys that had their own block (or were authored) are written — Wine omits pure
        // intermediate container keys, which are implied by their descendants' paths.
        if (Child->Explicit)
        {
            // Blocks are separated by a single blank line; the header supplies the one before the
            // first block, and Wine writes none after the last — so emit the separator up front.
            if (AnyEmitted) Out << "\n";
            Out << "[" << full << "]";
            if (!Child->Timestamp.empty()) Out << " " << Child->Timestamp;
            Out << "\n";
            for (const std::string &Meta : Child->RawMetaLines) Out << Meta << "\n";
            for (const auto &[Name, Val] : Child->Values) EmitValue(Out, Name, Val);
            AnyEmitted = true;
        }

        EmitKey(Out, Child.get(), full, AnyEmitted);
    }
}

bool RegistryWrapper::SaveHiveFile(const std::filesystem::path &File, HiveRoot Root) const
{
    auto It = Hives.find(Root);
    if (It == Hives.end()) return true; // nothing loaded/authored for this hive — leave the file as-is
    const Hive &H = It->second;

    std::ofstream Out(File, std::ios::binary | std::ios::trunc);
    if (!Out) { LogErr("RegistryWrapper::SaveHiveFile", "Cannot write " + File.string()); return false; }

    Out << H.RawHeaderLines;
    bool AnyEmitted = false;
    if (H.RootKey) EmitKey(Out, H.RootKey.get(), "", AnyEmitted);
    return true;
}

bool RegistryWrapper::SavePrefix(const std::filesystem::path &PrefixPath) const
{
    struct { const char *File; HiveRoot Root; } Files[] = {
        { "system.reg",  HiveRoot::Machine },
        { "user.reg",    HiveRoot::User },
        { "userdef.reg", HiveRoot::UserDefault },
    };
    bool Ok = true;
    for (const auto &F : Files)
        if (Hives.count(F.Root)) Ok = SaveHiveFile(PrefixPath / F.File, F.Root) && Ok;
    return Ok;
}

// ============================================================================
//  Path-addressed access
// ============================================================================

//Ensure the hive for Root exists in the map (synthesizing a minimal header for authoring-only hives).
RegistryKey *RegistryWrapper::EnsureKey(const std::string &FullPath)
{
    HiveRoot root = RootForPath(FullPath);
    Hive &H = Hives[root];
    if (!H.RootKey)
    {
        H.Root = root;
        H.RootKey = std::make_unique<RegistryKey>();
        if (H.RawHeaderLines.empty())
        {
            const char *RootComment =
                root == HiveRoot::Machine ? "REGISTRY\\\\Machine" :
                root == HiveRoot::User    ? "REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000" :
                                            "REGISTRY\\\\User\\\\.Default";
            H.RawHeaderLines = std::string("WINE REGISTRY Version 2\n;; All keys relative to ")
                             + RootComment + "\n\n#arch=win64\n\n"; // trailing blank = separator before block 1
        }
    }
    RegistryKey *node = H.RootKey.get();
    for (const std::string &seg : SplitOnSingleBackslash(StripRootPrefix(FullPath)))
    {
        if (seg.empty()) continue;
        node = FindOrCreateChild(node, seg, "");
    }
    node->Explicit = true; // an authored key is written even if it has no values
    return node;
}

RegistryKey *RegistryWrapper::GetKey(const std::string &FullPath)
{
    HiveRoot root = RootForPath(FullPath);
    auto It = Hives.find(root);
    if (It == Hives.end() || !It->second.RootKey) return nullptr;
    RegistryKey *node = It->second.RootKey.get();
    for (const std::string &seg : SplitOnSingleBackslash(StripRootPrefix(FullPath)))
    {
        if (seg.empty()) continue;
        RegistryKey *next = nullptr;
        for (auto &C : node->Children) if (CIEqual(C->Name, seg)) { next = C.get(); break; }
        if (!next) return nullptr;
        node = next;
    }
    return node;
}

bool RegistryWrapper::DeleteKey(const std::string &FullPath)
{
    HiveRoot root = RootForPath(FullPath);
    auto It = Hives.find(root);
    if (It == Hives.end() || !It->second.RootKey) return false;
    std::vector<std::string> segs;
    for (const std::string &s : SplitOnSingleBackslash(StripRootPrefix(FullPath))) if (!s.empty()) segs.push_back(s);
    if (segs.empty()) return false;
    RegistryKey *node = It->second.RootKey.get();
    for (size_t i = 0; i + 1 < segs.size(); ++i)
    {
        RegistryKey *next = nullptr;
        for (auto &C : node->Children) if (CIEqual(C->Name, segs[i])) { next = C.get(); break; }
        if (!next) return false;
        node = next;
    }
    for (auto It2 = node->Children.begin(); It2 != node->Children.end(); ++It2)
        if (CIEqual((*It2)->Name, segs.back())) { node->Children.erase(It2); return true; }
    return false;
}

//Deep-clones a key node (values/metadata/timestamp/explicit-flag and all descendants).
static std::unique_ptr<RegistryKey> CloneKey(const RegistryKey &Src)
{
    auto K = std::make_unique<RegistryKey>();
    K->Name = Src.Name;
    K->RawSegment = Src.RawSegment;
    K->Explicit = Src.Explicit;
    K->Timestamp = Src.Timestamp;
    K->RawMetaLines = Src.RawMetaLines;
    K->Values = Src.Values;
    for (const auto &C : Src.Children) K->Children.push_back(CloneKey(*C));
    return K;
}

bool RegistryWrapper::MergeKeyFrom(const RegistryWrapper &Src, const std::string &FullPath)
{
    const RegistryKey *SrcKey = const_cast<RegistryWrapper &>(Src).GetKey(FullPath);
    if (!SrcKey) return false;
    RegistryKey *Dst = EnsureKey(FullPath); // creates the path, marks the target Explicit
    // Replace the destination's content with a deep copy of the source subtree. The copied values
    // keep their verbatim RawName/RawPayload, so they re-serialize faithfully (no Dirty needed).
    Dst->Values       = SrcKey->Values;
    Dst->Timestamp    = SrcKey->Timestamp;
    Dst->RawMetaLines = SrcKey->RawMetaLines;
    Dst->Explicit     = true;
    Dst->Children.clear();
    for (const auto &C : SrcKey->Children) Dst->Children.push_back(CloneKey(*C));
    return true;
}

const RegistryValue *RegistryWrapper::GetValue(const std::string &FullPath, const std::string &Name) const
{
    RegistryKey *K = const_cast<RegistryWrapper *>(this)->GetKey(FullPath);
    if (!K) return nullptr;
    for (const auto &[N, V] : K->Values) if (CIEqual(N, Name)) return &V;
    return nullptr;
}

void RegistryWrapper::SetValue(const std::string &FullPath, const std::string &Name, const RegistryValue &V)
{
    RegistryKey *K = EnsureKey(FullPath);
    RegistryValue NV = V;
    NV.Dirty = true; // authored — synthesize on save
    for (auto &[N, Existing] : K->Values)
        if (CIEqual(N, Name)) { Existing = NV; return; }
    K->Values.emplace_back(Name, std::move(NV));
}

bool RegistryWrapper::DeleteValue(const std::string &FullPath, const std::string &Name)
{
    RegistryKey *K = GetKey(FullPath);
    if (!K) return false;
    for (auto It = K->Values.begin(); It != K->Values.end(); ++It)
        if (CIEqual(It->first, Name)) { K->Values.erase(It); return true; }
    return false;
}

// ============================================================================
//  Manifest <-> value conversion, RegEdit application
// ============================================================================

RegistryValue RegistryWrapper::ManifestStringToValue(const ordered_json &V)
{
    RegistryValue RV;
    RV.Dirty = true;
    if (V.is_null())            { RV.Type = RegType::Sz; RV.Str = ""; RV.RawPayload = "\"\""; return RV; }
    if (V.is_boolean())         { RV.Type = RegType::Dword; RV.RawPayload = V.get<bool>() ? "dword:00000001" : "dword:00000000"; return RV; }
    if (V.is_number())          { RV.Type = RegType::Dword; char b[24]; std::snprintf(b, sizeof b, "dword:%08x", (unsigned)V.get<long long>()); RV.RawPayload = b; return RV; }
    if (V.is_string())
    {
        std::string S = V.get<std::string>();
        if      (S.rfind("dword:", 0) == 0) { RV.Type = RegType::Dword;    RV.RawPayload = S; }
        else if (S.rfind("hex:",   0) == 0) { RV.Type = RegType::Hex;      RV.RawPayload = S; }
        else if (S.rfind("hex(",   0) == 0) { RV.Type = RegType::HexTyped; RV.RawPayload = S; }
        else if (S.rfind("str(",   0) == 0) { RV.Type = RegType::StrTyped; RV.RawPayload = S; }
        else                                { RV.Type = RegType::Sz; RV.Str = S; RV.RawPayload = "\"" + EscapeValueStr(S) + "\""; }
        return RV;
    }
    RV.Type = RegType::Sz; RV.RawPayload = "\"\""; return RV;
}

ordered_json RegistryWrapper::ValueToManifestString(const RegistryValue &V)
{
    if (V.Type == RegType::Sz) return V.Str;
    return V.RawPayload; // "dword:..", "hex:..", etc. — preserves the existing KEYVALUES encoding
}

//Re-applies WoW64 registry redirection that the manifest REGPATH dropped. On a 64-bit prefix, a 32-bit app's
//HKLM/HKCU \Software access is redirected by Windows/wine under a "Wow6432Node" key (inserted right after the
//"Software" segment). The manifest stores the natural, Wow6432Node-free REGPATH plus ARCHITECTURE; for a 32-bit
//RegEdit we must re-insert Wow6432Node so the key lands exactly where the 32-bit app reads it. (No-op for 64-bit
//edits, non-Software paths, or a path that already contains the node.) Without this, e.g. WipeoutXL's install-dir
//keys were written to the 64-bit view and the game never found them.
static std::string ArchRedirectRegPath(const std::string &RegPath, const std::string &Architecture)
{
    if (Architecture != "32") return RegPath;
    std::vector<std::string> Segs; std::string Cur;
    for (char C : RegPath) { if (C == '\\') { Segs.push_back(Cur); Cur.clear(); } else Cur.push_back(C); }
    Segs.push_back(Cur);

    std::vector<std::string> Out; bool Inserted = false;
    for (size_t i = 0; i < Segs.size(); ++i)
    {
        Out.push_back(Segs[i]);
        if (!Inserted && CIEqual(Segs[i], "Software"))
        {
            if (i + 1 >= Segs.size() || !CIEqual(Segs[i + 1], "Wow6432Node")) Out.emplace_back("Wow6432Node");
            Inserted = true;
        }
    }
    if (!Inserted) return RegPath;                                   // not a \Software path → no redirection
    std::string Res;
    for (size_t i = 0; i < Out.size(); ++i) { if (i) Res += "\\"; Res += Out[i]; }
    return Res;
}

bool RegistryWrapper::ApplyRegEdit(const ordered_json &Sub)
{
    std::string RegPath = Sub.value("REGPATH", std::string());
    if (RegPath.empty()) { LogWarn("RegistryWrapper::ApplyRegEdit", "RegEdit with empty REGPATH (skipped)."); return false; }
    RegPath = ArchRedirectRegPath(RegPath, Sub.value("ARCHITECTURE", std::string()));   // 32-bit → under Wow6432Node
    EnsureKey(RegPath); // key-only creation when there are no KEYVALUES
    if (Sub.contains("KEYVALUES") && Sub["KEYVALUES"].is_object())
        for (auto &[K, Val] : Sub["KEYVALUES"].items())
            SetValue(RegPath, K, ManifestStringToValue(Val));
    return true;
}

bool RegistryWrapper::ApplyRegEdits(const ordered_json &SubComponentsArray, bool WantOverride)
{
    if (!SubComponentsArray.is_array()) return true;
    for (const auto &Sub : SubComponentsArray)
    {
        if (!Sub.is_object()) continue;
        if (Sub.value("TYPE", std::string()) != "RegEdit") continue;
        if (Sub.value("OVERRIDE", false) != WantOverride) continue;
        ApplyRegEdit(Sub);
    }
    return true;
}

// ============================================================================
//  Diff -> RegEdit subcomponents (editor capture)
// ============================================================================

namespace {

//Keys whose changes are wine/system noise and never authored into a package (substring match on the
//hive-relative path, backslash form, case-insensitive).
const std::vector<std::string> kIgnoreSubstrings = {
    "Software\\Microsoft\\Windows",
    "Software\\Microsoft\\Cryptography",
    "Software\\Wow6432Node\\Microsoft\\Windows",
    "Software\\Classes",
    "Software\\Microsoft\\ActiveMovie",
    "Software\\Wine",
};

bool Ignored(const std::string &RelPath)
{
    std::string L = ToLowerAscii(RelPath);
    for (const std::string &Sub : kIgnoreSubstrings)
        if (L.find(ToLowerAscii(Sub)) != std::string::npos) return true;
    return false;
}

bool ValuesEqual(const RegistryValue &A, const RegistryValue &B)
{
    if (A.Type != B.Type) return false;
    if (A.Type == RegType::Sz) return A.Str == B.Str;
    return NormalizePayload(A.RawPayload) == NormalizePayload(B.RawPayload);
}

//Remove the "Wow6432Node\" token from a hive-relative path (the manifest REGPATH drops it; the 32-bit
//view is recorded via ARCHITECTURE instead).
std::string StripWow(const std::string &Rel)
{
    std::string Out; size_t i = 0;
    std::vector<std::string> segs;
    { std::string cur; for (char c : Rel) { if (c == '\\') { segs.push_back(cur); cur.clear(); } else cur.push_back(c); } segs.push_back(cur); }
    for (const std::string &s : segs) { if (CIEqual(s, "Wow6432Node")) continue; if (!Out.empty()) Out += "\\"; Out += s; }
    (void)i; return Out;
}

//Recursively walk a key subtree, comparing each value against Baseline; collect add/replace deltas
//grouped by REGPATH into Groups (REGPATH -> {ARCHITECTURE, KEYVALUES}), in stable tree order.
void WalkDiff(const RegistryKey *Node, const std::string &RootLabel, const std::string &RelPath,
              const RegistryWrapper &Baseline, ordered_json &Groups, std::vector<std::string> &Order)
{
    for (const auto &Child : Node->Children)
    {
        std::string childRel = RelPath.empty() ? Child->Name : RelPath + "\\" + Child->Name;
        if (!Ignored(childRel))
        {
            for (const auto &[Name, Val] : Child->Values)
            {
                std::string fullPath = RootLabel + "\\" + childRel;
                const RegistryValue *Base = Baseline.GetValue(fullPath, Name);
                if (Base && ValuesEqual(*Base, Val)) continue;          // unchanged
                std::string regPath = RootLabel + "\\" + StripWow(childRel);
                std::string arch = (ToLowerAscii(childRel).find("wow6432node") != std::string::npos) ? "32" : "64";
                if (!Groups.contains(regPath))
                {
                    Groups[regPath] = ordered_json::object();
                    Groups[regPath]["ARCHITECTURE"] = arch;
                    Groups[regPath]["KEYVALUES"] = ordered_json::object();
                    Order.push_back(regPath);
                }
                // An empty (Default) value (Name "" → "") carries no information beyond "this key exists". Capture the
                // key (key-only — EnsureKey creates it) but omit the {"":""} noise. Named values and non-empty defaults
                // are kept. The group was already created above, so the key still gets emitted.
                if (Name.empty() && Val.Type == RegType::Sz && Val.Str.empty()) continue;
                Groups[regPath]["KEYVALUES"][Name] = RegistryWrapper::ValueToManifestString(Val);
            }
        }
        WalkDiff(Child.get(), RootLabel, childRel, Baseline, Groups, Order);
    }
}

} // namespace

ordered_json RegistryWrapper::DiffToRegEdits(const RegistryWrapper &Baseline) const
{
    ordered_json Groups = ordered_json::object();
    std::vector<std::string> Order;

    struct { HiveRoot Root; const char *Label; } Roots[] = {
        { HiveRoot::Machine, "HKLM" }, { HiveRoot::User, "HKCU" },
    };
    for (const auto &R : Roots)
    {
        auto It = Hives.find(R.Root);
        if (It == Hives.end() || !It->second.RootKey) continue;
        WalkDiff(It->second.RootKey.get(), R.Label, "", Baseline, Groups, Order);
    }

    ordered_json Out = ordered_json::array();
    for (const std::string &RegPath : Order)
    {
        ordered_json Sub = ordered_json::object();
        Sub["TYPE"] = "RegEdit";
        Sub["REGPATH"] = RegPath;
        Sub["ARCHITECTURE"] = Groups[RegPath]["ARCHITECTURE"];
        // Omit KEYVALUES entirely for a key-only edit (no values) — cleaner than an empty {} and exactly the spec's
        // "a RegEdit with no KEYVALUES creates the key itself".
        if (!Groups[RegPath]["KEYVALUES"].empty()) Sub["KEYVALUES"] = Groups[RegPath]["KEYVALUES"];
        Out.push_back(Sub);
    }
    return Out;
}
