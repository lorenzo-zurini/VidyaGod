#include "jsonoperations.h"

JSONOperations::JSONOperations() {}

QJsonDocument * JSONOperations::InitGlobalConfigJSON(QFile * GlobalConfigFile)
{
    if (!GlobalConfigFile->exists())
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Config flie not deteced. Creating... ";
        QFile("DefaultConfig.JSON").copy("GlobalConfig.JSON");
    }
    return JSONOperations::LoadJSON(GlobalConfigFile);
}

QJsonDocument * JSONOperations::LoadJSON(QFile * JSONFile)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Parsing JSON " << JSONFile->fileName();

    if (!JSONFile->exists())
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] File " << JSONFile->fileName() << " does not exist.";
        return nullptr;
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] File " << JSONFile->fileName() << " opened for reading successfully!";
        QJsonDocument * JSONDocument = new QJsonDocument(QJsonDocument::fromJson(JSONFile->readAll()));
        JSONFile->close();
        qDebug() << QTime::currentTime().toString() << " [OUT] Parse done!";

        if (JSONDocument->isNull() || JSONDocument->isEmpty())
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Null or empty JSON.";
            delete JSONDocument;
            return nullptr;
        }

        return JSONDocument;
    }
    else
    {
        qDebug() << QTime::currentTime().toString() << " [ERR] Could not open file for reading!";
        return nullptr;
    }
}

void JSONOperations::SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] File opened for writing successfully!";
        qDebug() << QTime::currentTime().toString() << " [OUT] Saved " << JSONFile->fileName();
        QTextStream OutFileStream(JSONFile);
        OutFileStream << *JSONDocument->toJson();
        JSONFile->close();
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open file for writing!";
    }
}
