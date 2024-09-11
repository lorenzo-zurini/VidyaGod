#ifndef JSONOPERATIONS_H
#define JSONOPERATIONS_H

#include <QJsonDocument>
#include <QFile>

class JSONOperations
{
public:
    JSONOperations();

    static QJsonDocument * InitGlobalConfigJSON(QFile * GlobalConfigFile);
    static QJsonDocument * LoadJSON(QFile * JSONFile);
    static void SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile);
};

#endif // JSONOPERATIONS_H
