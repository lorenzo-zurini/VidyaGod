#ifndef PACKAGECATALOG_P_H
#define PACKAGECATALOG_P_H

#include <string>

// Internal (cross-TU) helpers of the PackageCatalog service — split across packagecatalog.cpp and
// packagecatalog_publish.cpp (P6). Not part of the public catalog API.
namespace PackageCatalog {

//What one bundle dir contains, per a cheap node scan: its first DeclareLibraryItem UID/name + whether any
//node is launchable / a runner. Valid=false when the dir holds no nodes at all.
struct BundleIdentity { std::string Uid, Name; bool HasLaunchable = false, HasRunner = false; bool Valid = false; };
BundleIdentity ScanBundleIdentity(const std::string &BundleDir);

} // namespace PackageCatalog

#endif // PACKAGECATALOG_P_H
