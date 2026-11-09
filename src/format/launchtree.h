#ifndef LAUNCHTREE_H
#define LAUNCHTREE_H

#include "fold.h"
#include "layerspec.h"   // VidyaGodFS: the mount spec
#include "treeplan.h"    // VidyaGodFS: what a spec makes of the tree

#include <map>
#include <set>
#include <string>
#include <vector>

// A resolved plan's files as VidyaGodFS reads them: the mount spec its content ops make (NodeLower::ContentOps — what
// the launch mounts, in guest coordinates), each spec layer's item in the plan, and the FS's plan of the tree: which
// file each delta's base CIDs name, what each delta hides, which delta cannot rebuild. The validator, the case fixer and
// `fold bases` ask this rather than model the tree themselves: one rule, the mount's. It is asked in the package's
// coordinates (%GameDir%), before the runner's layout maps them, and without a runner's own layers.
struct LaunchTree
{
    Spec S;
    std::vector<size_t> ItemOf;          // spec layer -> its item in the plan's sequence
    std::map<size_t, size_t> LayerOf;    // item -> its spec layer (an item nothing mounts has none)
    TreePlan T;

    static LaunchTree Of(const Fold::Plan &P);
};

// The member names of the archives a tree's zip and delta layers mount, read once per file: a zip's, a delta's rebuilt
// archive's ('/' separators — or, with Folders, every entry as the zip holds it, a folder's own entry ending in '/', for
// a rename to rewrite). Null when it cannot be read here
// (not present, a base it names not rebuilding). A failure of the files themselves is remembered — the same file with
// the same bases fails in every plan; one of the plan (a base it does not hold) is not. Rebuilding a delta lists
// every delta rebuilt on the way, so a chain is read once for its deepest member, not once per version. Shared across
// trees: a file is one archive, a delta's with the bases it names.
class ArchiveNames
{
public:
    const std::vector<std::string> *Of(const LaunchTree &L, size_t Layer, bool Folders);

private:
    std::map<std::string, std::vector<std::string>> All;   // archive key -> every entry, folders included
    std::map<std::string, std::vector<std::string>> Files;  // the same, folders left out
    std::set<std::string> Failed;                           // archive keys whose files did not rebuild
};

#endif // LAUNCHTREE_H
