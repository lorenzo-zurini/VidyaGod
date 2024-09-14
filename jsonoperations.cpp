#include "jsonoperations.h"

JSONOps::JSONOps() {}

QJsonDocument * JSONOps::InitGlobalConfigJSON(QFile * GlobalConfigFile)
{
    if (!GlobalConfigFile->exists())
    {
        qDebug() << QTime::currentTime().toString() << " [OUT] Config flie not deteced. Creating... ";
        QFile("DefaultConfig.JSON").copy("GlobalConfig.JSON");
    }
    return JSONOps::LoadJSON(GlobalConfigFile);
}

QJsonDocument * JSONOps::LoadJSON(QFile * JSONFile)
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

void JSONOps::SaveJSON(QJsonDocument * JSONDocument, QFile * JSONFile)
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

QJsonArray JSONOps::GetSubComponents(QString MetaDataPath, QList<int> Recipe)
{
    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    QJsonDocument * MANIFESTJSON = JSONOps::LoadJSON(new QFile(QDir::cleanPath(MetaDataPath + QDir::separator() +"MANIFEST.json")));
    QJsonArray SubComponentsArray;
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].toArray().count(); i++)
    {
        if (Recipe.contains(i))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"].toArray()[i].toObject()["SUBCOMPONENTS"].toArray().count(); j++)
            {
                SubComponentsArray.append((*MANIFESTJSON)["COMPONENTS"].toArray()[i].toObject()["SUBCOMPONENTS"].toArray()[j].toObject());
            }
        }
    }

    delete MANIFESTJSON;
    return SubComponentsArray;
}
