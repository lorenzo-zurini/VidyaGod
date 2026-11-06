#ifndef CONTENTFILE_H
#define CONTENTFILE_H

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ContentFile — where a package keeps a content file. A node names its file ("ZIP": "DxWnd.zip") and its SOURCE says
// which bytes; the name is aesthetic, the CID is the identity. When one CID claims a name in a package (the usual
// case) the file is that name. When two or more do — two versions of a library kept side by side because different
// installed games still contain each — every one of them lives at its BUMPED name, "<stem> [<cid tail>]<ext>", so no
// version ever reads another's bytes. The rule is a function of the package's node files only: the same on every
// machine, before and after a download.
// ---------------------------------------------------------------------------
namespace ContentFile
{

//"DxWnd.zip" + "QmAbc…12345678" → "DxWnd [12345678].zip" (a nested name keeps its folders).
std::string Bumped(const std::string &Name, const std::string &Cid);

//Names compared without case: on a case-insensitive file system (Windows) "dx.zip" and "DX.zip" are one file, so two
//CIDs claiming them must be bumped apart on every platform alike.
struct NameLess { bool operator()(const std::string &A, const std::string &B) const; };
//Name → the CIDs the package's node files claim for it (content layers' SOURCE, tile covers). Cached per folder,
//re-read when its node files change (content landing beside them does not).
using ClaimMap = std::map<std::string, std::set<std::string>, NameLess>;
std::shared_ptr<const ClaimMap> Claims(const std::filesystem::path &Dir);

//The same over explicit node files (a package as it would be after a change).
ClaimMap ClaimsOfFiles(const std::vector<std::filesystem::path> &NodeFiles);

//The file for Name holding Cid in package Dir: the name itself, or its bumped name when the package's nodes claim
//Name for more than one CID. A name without a CID, an anchored (%…%) or absolute name is the name as given.
std::filesystem::path Path(const std::filesystem::path &Dir, const std::string &Name, const std::string &Cid);

//Path() as it would be if the package's nodes claimed Claims (for planning a change before it is made).
std::filesystem::path PathUnder(const std::filesystem::path &Dir, const std::string &Name, const std::string &Cid,
                                const ClaimMap &Claims);

}

#endif // CONTENTFILE_H
