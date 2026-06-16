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
};

#endif // JSONOPERATIONS_H
