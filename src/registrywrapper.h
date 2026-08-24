#ifndef REGISTRYWRAPPER_H
#define REGISTRYWRAPPER_H

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <memory>
#include <cstdint>

//RegistryWrapper — reads, edits, diffs and writes wine's NATIVE on-disk registry files
//(system.reg / user.reg / userdef.reg, the "WINE REGISTRY Version 2" format). It replaces the
//former indirect path of generating "Windows Registry Editor Version 5.00" patch files and shelling
//out to `umu-run reg import`. Used by the runtime (apply manifest RegEdit subcomponents straight
//into a prefix) and by the package editor (snapshot + diff a prefix into RegEdit subcomponents).
//
//Format reference (from wine server/registry.c — save_all_subkeys / save_subkeys / dump_value /
//dump_strW): a header block, then flat key blocks "[A\\B\\C] <decimal-time>" each followed by
//#time/#class/#link metadata and value lines. Value encodings: bare "str" (REG_SZ), str(N):"..."
//(typed strings), dword:%08x, hex:bytes (REG_BINARY), hex(N):bytes (any other type N). Long hex
//wraps past column 76 with "\\\n  ". Strings escape \\, \" and \xHH for control/non-ASCII.
//
//Fidelity: every value retains its exact original post-'=' text (RawPayload). Untouched values are
//re-emitted verbatim, so loading and re-saving an unmodified hive is byte-faithful regardless of
//escaping edge cases; only values dirtied by SetValue/manifest authoring are re-synthesized.

//Logical type of a registry value, mapped from the wine textual encoding it was parsed from.
enum class RegType {
    Sz,        // bare "..."        REG_SZ              (decoded into Str)
    Dword,     // dword:%08x        REG_DWORD
    Hex,       // hex:bytes         REG_BINARY (type 3)
    HexTyped,  // hex(N):bytes      any other type N (incl. exotic wine-internal types)
    StrTyped,  // str(N):"..."      typed string (REG_EXPAND_SZ=2 / REG_MULTI_SZ=7 / ...)
    Unknown    // anything unrecognized — round-tripped verbatim
};

//A single registry value. RawName/RawPayload hold the exact left/right sides as they appeared in the
//file (RawPayload keeps hex line-continuations verbatim), so an untouched value re-emits byte-for-byte.
//For Sz, Str also holds the decoded (unescaped) string for diffing/authoring.
struct RegistryValue {
    RegType     Type = RegType::Sz;
    std::string Str;                 // decoded text (Sz only)
    std::string RawName;             // exact left-hand side: `@` or `"escaped name"`
    std::string RawPayload;          // exact original post-'=' text (continuations kept); verbatim when !Dirty
    bool        Dirty = false;       // SetValue / manifest authoring set this → serializer synthesizes
    uint32_t    HexTypeCode = 0;     // N for HexTyped
    uint32_t    StrTypeCode = 0;     // N for StrTyped
};

//A registry key node. Wine stores keys flat (one [full\\path] block each); we hold them as a tree
//for O(depth) addressing and clean diffing, flattening back to blocks on save. Value name "" is the
//default (@) value. Insertion order is preserved for stable, diff-friendly output.
struct RegistryKey {
    std::string Name;                                              // single decoded path segment
    std::string RawSegment;                                        // exact escaped segment as in the file (verbatim re-emit)
    bool        Explicit = false;                                  // had its own [block] in the file, or was authored — only these are emitted
    std::string Timestamp;                                         // decimal time from the "[..] N" line
    std::vector<std::string> RawMetaLines;                         // "#time=..", "#class=..", "#link"
    std::vector<std::pair<std::string, RegistryValue>> Values;     // ordered; "" == default value
    std::vector<std::unique_ptr<RegistryKey>> Children;
};

//Which hive file a key lives in. system.reg=HKLM, user.reg=HKCU, userdef.reg=HKU\.Default.
enum class HiveRoot { Machine, User, UserDefault };

//One parsed hive file: its verbatim header (everything before the first key block) plus the tree.
struct Hive {
    HiveRoot                     Root = HiveRoot::Machine;
    std::string                  RawHeaderLines;   // "WINE REGISTRY Version 2\n;; ...\n#arch=..\n"
    std::unique_ptr<RegistryKey> RootKey;          // synthetic root; its Children are the top-level keys
};

class RegistryWrapper
{
public:
    RegistryWrapper() = default;

    //Loading. LoadPrefix loads whichever of the three files exist directly under PrefixPath; a
    //missing file yields an empty hive (not an error). Returns false only on a parse failure.
    bool LoadHiveFile(const std::filesystem::path &File, HiveRoot Root);
    bool LoadPrefix(const std::filesystem::path &PrefixPath);

    //Saving — emits the WINE REGISTRY Version 2 format.
    bool SaveHiveFile(const std::filesystem::path &File, HiveRoot Root) const;
    bool SavePrefix(const std::filesystem::path &PrefixPath) const;

    //Path-addressed access. FullPath uses the HK* abbreviations (e.g. "HKLM\\Software\\Foo");
    //the root prefix selects the hive and is stripped to the in-hive relative path.
    RegistryKey *        GetKey(const std::string &FullPath);
    RegistryKey *        EnsureKey(const std::string &FullPath);
    bool                 DeleteKey(const std::string &FullPath);
    //Deep-copies the key subtree at FullPath from Src into this wrapper (creating the path, replacing
    //any existing key at that location). Returns false if Src has no such key. Used for per-key
    //persistence: extract a subtree from one hive set and merge it into another.
    bool                 MergeKeyFrom(const RegistryWrapper &Src, const std::string &FullPath);
    //Recursive UNION merge of Src's subtree at FullPath into *this: source values overwrite same-named
    //destination values and source children merge recursively — destination-ONLY keys/values are
    //PRESERVED. (MergeKeyFrom REPLACES the destination subtree; union semantics are what registry
    //PERSISTENCE needs: the user's saved keys win where they exist, package/base defaults show through
    //everywhere else.) Accepts a bare root ("HKCU") to merge a whole hive.
    bool                 UnionMergeFrom(const RegistryWrapper &Src, const std::string &FullPath);
    const RegistryValue *GetValue(const std::string &FullPath, const std::string &Name) const;
    void                 SetValue(const std::string &FullPath, const std::string &Name, const RegistryValue &V);
    bool                 DeleteValue(const std::string &FullPath, const std::string &Name);

    //Manifest RegEdit application (runtime). ApplyRegEdit applies one {REGPATH,ARCHITECTURE,KEYVALUES}
    //subcomponent; absent/empty KEYVALUES means key-only creation. ApplyRegEdits walks a
    //SubComponentsArray and applies every RegEdit whose OVERRIDE flag equals WantOverride.
    bool ApplyRegEdit(const nlohmann::ordered_json &Sub);
    bool ApplyRegEdits(const nlohmann::ordered_json &SubComponentsArray, bool WantOverride);

    //Editor diff. Emits the add/replace deltas of *this relative to Baseline as a manifest RegEdit
    //subcomponent array (filtered by the system-key ignore list). Deletions are ignored.
    nlohmann::ordered_json DiffToRegEdits(const RegistryWrapper &Baseline) const;

    //Manifest-string <-> RegistryValue conversion (preserves the existing KEYVALUES encoding).
    static RegistryValue          ManifestStringToValue(const nlohmann::ordered_json &V);
    static nlohmann::ordered_json ValueToManifestString(const RegistryValue &V);

    //Path/root helpers.
    static HiveRoot    RootForPath(const std::string &FullPath);   // HKLM->Machine, HKCU->User, ...
    static std::string NormalizeRootPrefix(std::string Path);      // HKEY_LOCAL_MACHINE<->HKLM, etc.
    static std::string StripRootPrefix(const std::string &FullPath); // path relative to the hive root

private:
    std::map<HiveRoot, Hive> Hives;
};

#endif // REGISTRYWRAPPER_H
