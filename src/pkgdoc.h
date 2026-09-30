#ifndef PKGDOC_H
#define PKGDOC_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PkgDoc — the package editor's document: one package's nodes, the position each has on this machine's canvas,
// undo/redo, and the generation-6 save.
//
// A node is named by the CID of its canonical bytes (MetaPackageFormat §1.7) and stored as `<cid>.json`. In the
// editor each node carries a HANDLE beside its JSON — never inside it: the CID of the file it was loaded from, or
// `draft-…` for a node made in this session. References between nodes (NODE / ANY / NOT) name handles, so a wire
// drawn to a new node works before that node has a CID.
//
// SAVING IS A RE-MINT, not a write-back. Editing a node changes its bytes and therefore its name, and every node that
// names it changes too — in this package and in any other (a game names the fonts library by CID). Save orders the
// package's nodes by reference, gives each the CID of its bytes with references already renamed, writes the changed
// ones as `<cid>.json`, then follows the renames through the whole library (re-minting every referrer, recursively,
// and each copy of a renamed node another package keeps) and into the instances that remember a node by CID
// (their GRAFTS, their RUNNER_CHAIN). Replaced files are removed only after every write succeeded. After a save the
// handles ARE the new CIDs, in the document and in its undo history alike.
//
// Pure: no Qt, no ImGui — the whole contract is testable in vg_tests.
// ---------------------------------------------------------------------------

namespace PkgDoc
{
using json = nlohmann::ordered_json;

//Visit every node reference a node makes: a NODE or NOT layer's string, each string member of an ANY layer.
void ForEachRef(const json &Node, const std::function<void(const std::string &)> &Fn);
//A copy of Node with every reference found in M renamed (refs not in M are kept).
json RemapRefs(const json &Node, const std::map<std::string, std::string> &M);
//"bafkrei…": a CIDv1 raw node name, the only thing a node file may be called in a published package.
bool LooksLikeCid(const std::string &S);

struct Pos { float X = 0.0f, Y = 0.0f; };

struct SaveReport
{
    bool Ok = false;
    std::string Error;                                   // why nothing (or not everything) was saved
    std::map<std::string, std::string> Renamed;          // every name that changed: this package's handles and the
                                                         // library nodes the cascade re-minted (old -> new CID)
    int Written = 0, Removed = 0, Cascaded = 0, InstancesUpdated = 0;
    std::vector<std::string> Kept;                       // deleted here, kept on disk: something still names it
    std::vector<std::string> Warnings;                   // saved, but something needs the author (a file not removed)
    std::vector<std::string> Log;                        // one line per file written/removed, for the editor log
};

class Document
{
public:
    // ---- loading ----------------------------------------------------------------------------------------------
    //Every node file in Dir (a *.json that is a node object; anything else is left alone and never touched). A file
    //named by a CID is that node's handle; a legacy file carrying a "CID" field is named by it; any other file by the
    //CID of its bytes. Returns what was odd (a file whose bytes do not hash to its name, a duplicate), one line each.
    //Clears the undo history; the result is clean (not dirty).
    std::vector<std::string> Load(const std::filesystem::path &Dir);
    //The *.json files the last Load could not parse: not nodes, not known to be anything else — a publish reports them.
    const std::vector<std::string> &Unparseable() const { return BadJson; }
    //Replace the whole content with these nodes (no files, no positions, no history; clean).
    void Reset(std::vector<std::pair<std::string, json>> Nodes);

    // ---- reading ----------------------------------------------------------------------------------------------
    int Count() const { return (int)Entries.size(); }
    const std::string &Handle(int I) const { return Entries[(size_t)I].Handle; }
    const json &Node(int I) const { return *Entries[(size_t)I].Node; }
    int IndexOf(const std::string &Handle) const;         // -1 if absent
    //The file this node came from ("" for a node made in this session).
    const std::string &File(int I) const { return Entries[(size_t)I].File; }
    //Every node's handle -> its LABEL (the in-package names a wire or a picker shows).
    std::map<std::string, std::string> Labels() const;

    // ---- editing ----------------------------------------------------------------------------------------------
    //Every edit bumps Revision() and leaves the document dirty. None of them records an undo step by itself: the
    //caller ends a gesture with Commit(), so typing a name is one step, not one per character.
    void Replace(int I, json Node);
    int  Add(json Node, const std::string &Handle = std::string());   // "" → a fresh draft handle; returns its index
    //Removes the node, every reference to it from the other nodes, and its position.
    bool Remove(int I);
    //Adds a reference from Child to Parent: a NODE layer (containment), an ANY member or a NOT layer.
    enum class RefKind : std::uint8_t { Node, Any, Not };
    bool Link(int Child, const std::string &ParentHandle, RefKind Kind);

    // ---- positions (this machine's canvas; part of undo, never part of a node) --------------------------------
    const std::map<std::string, Pos> &Positions() const { return Where; }
    void SetPos(const std::string &Handle, Pos P);
    void ClearPos(const std::string &Handle);
    void SetPositions(std::map<std::string, Pos> P);     // wholesale (loading this machine's layout)

    // ---- undo -------------------------------------------------------------------------------------------------
    //End a gesture: if anything changed since the last Commit, the state before it becomes one undo step.
    void Commit();
    bool CanUndo() const { return !UndoStack.empty() || Changed; }
    bool Pending() const { return Changed; }             // edited since the last Commit
    bool CanRedo() const { return !RedoStack.empty(); }
    bool Undo();
    bool Redo();

    // ---- state ------------------------------------------------------------------------------------------------
    uint64_t Revision() const { return Rev; }            // bumps on every change, positions included
    uint64_t ContentRevision() const { return ContentRev; }   // bumps when node content changes (not positions)
    bool Dirty() const { return ContentRev != SavedContentRev; }   // node content differs from what is on disk
    //Handles changed by the last Save (old -> new) — the canvas moves its per-node view state across with it.
    //Consumed: the call clears it.
    std::map<std::string, std::string> TakeRenames();

    // ---- saving -----------------------------------------------------------------------------------------------
    //Save the package into Dir (see the header comment). Roots: every tree whose packages may name its nodes (the
    //library and packages added from elsewhere); an unreadable one refuses the save.
    //UserDataRoot: the instances to rename remembered CIDs in ("" = none). On success the document is clean and
    //its handles are the CIDs on disk.
    SaveReport Save(const std::filesystem::path &Dir, const std::vector<std::filesystem::path> &Roots,
                    const std::filesystem::path &UserDataRoot);
    //One root (or none: "" = this package only, nothing else is followed — tests and throwaway folders).
    SaveReport Save(const std::filesystem::path &Dir, const std::filesystem::path &LibraryRoot,
                    const std::filesystem::path &UserDataRoot);

private:
    struct Entry
    {
        std::string Handle;
        std::shared_ptr<const json> Node;                 // shared with undo snapshots until edited
        std::string File;
    };
    struct Snapshot { std::vector<Entry> Entries; std::map<std::string, Pos> Where; uint64_t ContentRev = 0; };

    std::vector<Entry> Entries;
    std::set<std::string> LoadedFiles;                   // node files this package had on disk at the last Load/Save
    std::vector<std::string> BadJson;
    std::map<std::string, Pos> Where;
    std::vector<Snapshot> UndoStack, RedoStack;
    Snapshot Before;                                     // the state at the last Commit
    bool Changed = false;                                // something changed since the last Commit
    uint64_t Rev = 1, ContentRev = 1, SavedContentRev = 1, NextContentRev = 2;
    std::map<std::string, std::string> Renames;

    void Touch(bool Content);
    Snapshot Take() const { return Snapshot{Entries, Where, ContentRev}; }
    void Restore(const Snapshot &S);
    static void RenameIn(std::vector<Entry> &E, std::map<std::string, Pos> &W, const std::map<std::string, std::string> &M);
    std::string NewDraftHandle() const;
};

}

#endif // PKGDOC_H
