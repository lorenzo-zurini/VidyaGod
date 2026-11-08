#ifndef PKGACTIONS_P_H
#define PKGACTIONS_P_H

// What PkgActions' files share (pkgactions.cpp, pkgactions_probes.cpp, pkgactions_views.cpp). Not for anyone else.

#include <nlohmann/json.hpp>

#include <string>

namespace PkgActionsUi
{
using json = nlohmann::ordered_json;
//A type-checked string read. The editor works on RAW on-disk JSON — deliberately, since it is the tool you
//open to fix a node the format rejects — so any field may be any type, and nlohmann's value() throws on a
//mismatch AND on a non-object. These run on every action, with no handler anywhere above them.
inline std::string StrOf(const json & N, const char * Key)
{
    return (N.is_object() && N.contains(Key) && N[Key].is_string()) ? N[Key].get<std::string>() : std::string();
}
inline bool HasLayers(const json & N) { return N.is_object() && N.contains("LAYERS") && N["LAYERS"].is_array(); }
}
using namespace PkgActionsUi;

#endif // PKGACTIONS_P_H
