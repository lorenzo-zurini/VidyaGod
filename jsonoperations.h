#ifndef JSONOPERATIONS_H
#define JSONOPERATIONS_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
#include <QFile>

class JSONOps
{
public:
    JSONOps();

    static QJsonDocument * InitGlobalConfigJSON(QFile * GlobalConfigFile);
    static QJsonDocument * LoadJSON(QFile * JSONFile);
    static void SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile);
    static QJsonArray GetSubComponents(QString MetaDataPath, QList<int> Recipe);
    static QJsonArray ReplaceVariables(QJsonArray OriginalArray, QMap<QString, QString> VariableValues);
};

#endif // JSONOPERATIONS_H
