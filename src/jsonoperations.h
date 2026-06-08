#ifndef JSONOPERATIONS_H
#define JSONOPERATIONS_H

#include <QJsonDocument>
#include <iostream>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
#include <QFile>

#include "nlohmann/json.hpp"

//Static utility class for loading and saving JSON files using nlohmann::ordered_json.
//Using ordered_json preserves key insertion order, which matters for registry patches
//and other structured data that must be written back to disk in a predictable order.
class JSONOps
{
public:
    JSONOps();

    //Reads JSONFile from disk and parses it into JSONDocument.
    //Returns 0 (false) on success, 1 (true) on any failure (file missing, bad JSON, etc.).
    //The inverted bool convention matches the shell exit-code pattern used elsewhere.
    static bool LoadJSON(QFile * JSONFile, nlohmann::ordered_json * JSONDocument);

    //Serializes JSONDocument to disk at JSONFile with 4-space indentation.
    //Returns true on success, false if the file cannot be opened for writing.
    static bool SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile);

    //MODULAR MANIFESTS
    //Assembles a package's manifest from EVERY *.json file in PackageDir. Files are parsed and
    //merged (alphabetical filename order) via MergeManifestInto. No file is privileged — identity
    //and cross-file dependencies resolve in the merged result. Invalid/unreadable files are
    //skipped with a warning. Returns true if at least one usable JSON was found.
    static bool AssembleManifest(const QString &PackageDir, nlohmann::ordered_json &Out, std::vector<std::string> &Warnings);

    //Recursively merges Overlay into Base. Objects merge key-by-key; arrays go through MergeArrays;
    //differing scalars keep Base's value and append a "conflict" warning. Ctx is a dotted path used
    //in warning messages.
    static void MergeManifestInto(nlohmann::ordered_json &Base, const nlohmann::ordered_json &Overlay, const std::string &Ctx, std::vector<std::string> &Warnings);

    //Merges two arrays. If ArrayKey has a known ID field (GAMES→GAMEID, COMPONENTS→COMPONENTID,
    //VARIANTS→VARIANT_ID, RUNNERS→RUNNER_ID) and every element is an object carrying it, merges by ID
    //(same ID → MergeManifestInto, new ID → append). Otherwise concatenates (e.g. SUBCOMPONENTS).
    //Returns the merged array.
    static nlohmann::ordered_json MergeArrays(const nlohmann::ordered_json &Base, const nlohmann::ordered_json &Overlay, const std::string &ArrayKey, const std::string &Ctx, std::vector<std::string> &Warnings);

    //Validates an assembled manifest. Errors block launch (missing identity, dangling/cyclic
    //PARENTCOMPONENT, dangling variant ENDPOINTS, conflicting duplicate COMPONENTIDs). Warnings are
    //advisory (no HOST_PLATFORM, empty games/variants, runner without GUEST_PLATFORM, etc.).
    static void ValidateManifest(const nlohmann::ordered_json &Assembled, std::vector<std::string> &Errors, std::vector<std::string> &Warnings);

    //Archetype tests: a package appears in the library if it HasGames, and is offered to the runner
    //resolver if it HasRunners. (Both = self-contained bundle; neither = pure dependency.)
    static bool HasGames(const nlohmann::ordered_json &Doc);
    static bool HasRunners(const nlohmann::ordered_json &Doc);
};

#endif // JSONOPERATIONS_H
