#ifndef NODEREFS_H
#define NODEREFS_H

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

// NodeRefs — a node's references to other nodes, edited in its LAYERS: what it contains (NODE), never meets (NOT),
// rides on (ANY members).
namespace NodeRefs
{

//The ONE walk over where references live (NODE and NOT layers' strings, each string member of an ANY layer) — reading,
//closure walks, freeze, the CID write-back and the editor's renames all go through it, so they cannot disagree.
//A value that is not a node object (an object with a LAYERS list) names nothing.

//Visit every reference a node makes, in layer order.
void Each(const nlohmann::ordered_json &Node, const std::function<void(const std::string &Ref)> &Fn);
//What it contains (its NODE refs, in order) and, into *Requires, what it rides on and never meets (ANY members, NOTs).
std::vector<std::string> Contained(const nlohmann::ordered_json &Node, std::vector<std::string> *Requires = nullptr);
//Rewrite every reference through Map. Returns whether anything changed.
bool Remap(nlohmann::ordered_json &Node, const std::function<std::string(const std::string &)> &Map);
//A copy with every reference found in M renamed (the rest kept).
nlohmann::ordered_json Remapped(const nlohmann::ordered_json &Node, const std::map<std::string, std::string> &M);

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
