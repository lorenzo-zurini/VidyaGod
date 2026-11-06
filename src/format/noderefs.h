#ifndef NODEREFS_H
#define NODEREFS_H

#include <nlohmann/json.hpp>

#include <string>

// NodeRefs — a node's references to other nodes, edited in its LAYERS: what it contains (NODE), never meets (NOT),
// rides on (ANY members).
namespace NodeRefs
{

//Contain `Ref`: a NODE layer, after the node's leading NODE layers (what it contains lies beneath its own layers).
//No-op if a NODE layer already names it. Returns whether it changed.
bool Add(nlohmann::ordered_json &Node, const std::string &Ref);
//...or at a chosen place in the fold: a layer of `Type` ("NODE" or "NOT") naming Ref, before layer At (-1: as Add
//places it). No-op if a layer of that type already names it.
bool Insert(nlohmann::ordered_json &Node, const std::string &Type, const std::string &Ref, int At);
//Whether a layer names Ref (as NODE, NOT or an ANY member).
bool Names(const nlohmann::ordered_json &Layer, const std::string &Ref);
//Remove every reference naming `Ref` (NODE layers, ANY members, NOT layers). Returns whether anything changed.
bool Remove(nlohmann::ordered_json &Node, const std::string &Ref);

} // namespace NodeRefs

#endif // NODEREFS_H
