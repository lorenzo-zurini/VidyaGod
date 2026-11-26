#ifndef NODEREFS_H
#define NODEREFS_H

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

// NodeRefs — a node's references to other nodes, edited in its LAYERS: what it contains (NODE), what it goes into
// (INTO members), and the node CIDs its conditions name (WHEN, MetaPackageFormat 08 §8.6).
namespace NodeRefs
{

//The ONE walk over where references live (NODE strings, INTO members, node CIDs inside a layer's WHEN) — reading,
//closure walks, freeze, the CID write-back and the editor's renames all go through it, so they cannot disagree.
//A value that is not a node object (an object with a LAYERS list) names nothing.

//Visit every reference a node makes, in layer order.
void Each(const nlohmann::ordered_json &Node, const std::function<void(const std::string &Ref)> &Fn);
//What it contains (its NODE refs, in order) and, into *Requires, what it goes into and what its conditions name.
std::vector<std::string> Contained(const nlohmann::ordered_json &Node, std::vector<std::string> *Requires = nullptr);
//What a part goes INTO: the members of its leading INTO layers, in order (empty for a node that is not a part).
std::vector<std::string> IntoOf(const nlohmann::ordered_json &Node);
//Its HARD links: what it contains, then what it goes into. Every closure follows these (sharing, hydrating, keeping a
//node another one still needs); the node CIDs a condition names are never followed.
std::vector<std::string> Linked(const nlohmann::ordered_json &Node);
//The node CIDs a WHEN condition names (bare words shaped like a node CID; never inside quotes or a %token%).
std::vector<std::string> CidsInWhen(const std::string &Expr);
//Rewrite every reference through Map (a CID inside a condition is renamed in place). Returns whether anything changed.
bool Remap(nlohmann::ordered_json &Node, const std::function<std::string(const std::string &)> &Map);
//A copy with every reference found in M renamed (the rest kept).
nlohmann::ordered_json Remapped(const nlohmann::ordered_json &Node, const std::map<std::string, std::string> &M);

//Contain `Ref`: a NODE layer, after the node's leading NODE layers (what it contains lies beneath its own layers).
//No-op if a NODE layer already names it. Returns whether it changed.
bool Add(nlohmann::ordered_json &Node, const std::string &Ref);
//...or a reference of `Type`: "NODE" before layer At (-1: as Add places it), or "INTO": Ref joins INTO layer At of the
//node's leading run (-1 or 0: the first; one past the last: a new layer, so the part goes into Ref as well). No-op if it
//is already named so, false when At is past that.
bool Insert(nlohmann::ordered_json &Node, const std::string &Type, const std::string &Ref, int At);
//Whether a layer names Ref (as NODE, an INTO member, or a CID in its WHEN).
bool Names(const nlohmann::ordered_json &Layer, const std::string &Ref);
//Remove what names `Ref` as structure (NODE layers, INTO members; a condition naming it is kept: absent reads false).
bool Remove(nlohmann::ordered_json &Node, const std::string &Ref);

} // namespace NodeRefs

#endif // NODEREFS_H
