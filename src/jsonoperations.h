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

    //LEGACY: Builds a flat SubComponentsArray from MANIFEST.json filtered by integer Recipe indices.
    //Superseded by ContainerWrapper::BuildSubComponentsArray which uses string ComponentIDs.
    //Kept for compatibility with older tooling.
    static nlohmann::ordered_json GetSubComponents(QString MetaDataPath, QList<int> Recipe);

    //LEGACY: Performs %VARIABLE% substitution on all string values in OriginalArray
    //using the Qt-based VariableValues map.
    //Superseded by ContainerWrapper::StringVariableSubstitution which operates on raw strings.
    static nlohmann::ordered_json ReplaceVariables(nlohmann::ordered_json OriginalArray, QMap<QString, QString> VariableValues);
};

#endif // JSONOPERATIONS_H
