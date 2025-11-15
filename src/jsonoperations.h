#ifndef JSONOPERATIONS_H
#define JSONOPERATIONS_H

#include <QJsonDocument>
#include <iostream>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
#include <QFile>

#include "nlohmann/json.hpp"

class JSONOps
{
public:
    JSONOps();


    static nlohmann::ordered_json * LoadJSON(QFile * JSONFile);
    static bool SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile);
    static nlohmann::ordered_json GetSubComponents(QString MetaDataPath, QList<int> Recipe);
    static nlohmann::ordered_json ReplaceVariables(nlohmann::ordered_json OriginalArray, QMap<QString, QString> VariableValues);
};

#endif // JSONOPERATIONS_H
