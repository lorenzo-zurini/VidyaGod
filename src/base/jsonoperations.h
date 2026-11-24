#ifndef JSONOPERATIONS_H
#define JSONOPERATIONS_H

#include <QJsonDocument>
#include <iostream>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
#include <QFile>

#include "nlohmann/json.hpp"

#include <filesystem>

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

    //The one way the app's config (DataDir/GlobalConfig.JSON) is written. Refused — false, nothing written — while the
    //config is read-only (a call run beside the app that holds the data dir: AppPaths::SetConfigReadOnly), so no
    //second process ever writes the file the app keeps in memory and saves over.
    static bool SaveConfig(const nlohmann::ordered_json &Config, const std::filesystem::path &DataDir);
    //SaveConfig with the write on a worker: the text is taken here, on the caller's thread (it is the config as of
    //this call), and written there. Every config write — this and SaveConfig — is numbered when its text is taken, and
    //one older than a write already made is dropped: a later SaveConfig is never overwritten by an earlier text that
    //was still waiting. False only when refused (read-only); a failed write is logged.
    static bool SaveConfigAsync(const nlohmann::ordered_json &Config, const std::filesystem::path &DataDir);
    //Returns once every config write handed to the worker is over (the app's exit).
    static void FlushConfigWrites();
    //TEST ONLY: config files committed since start; hold the worker before it writes (true) or let it go (false).
    static long long DebugConfigWrites();
    static void DebugHoldConfigWrites(bool Hold);
};

#endif // JSONOPERATIONS_H
