#pragma once
// Runners every install has without downloading anything: the ones a machine can host and whose whole job is to run
// the game as itself. Each is the exact node published in VidyaGodRunners (same bytes, so the same CID): a copy on
// disk, or one received from a friend, is the same node, never a second one.

#include <filesystem>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace BuiltinRunners {

struct Runner
{
    std::string Folder;            // its package folder under VidyaGodRunners (where it is published)
    nlohmann::ordered_json Node;   // the node, as published
};

const std::vector<Runner> &All();

// Adds to a working tree the built-in runners Machine hosts (their entry's HOST). The freeze indexes each by its CID,
// so a copy already on disk is the same node, kept once. Their bundle dir is their folder under LibraryRoot.
void AddTo(std::map<std::string, nlohmann::ordered_json> &Tree, std::map<std::string, std::filesystem::path> &Dirs,
           const std::filesystem::path &LibraryRoot, const std::string &Machine);

}
