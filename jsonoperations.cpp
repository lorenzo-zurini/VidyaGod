#include "jsonoperations.h"

JSONOperations::JSONOperations() {}

QJsonDocument * JSONOperations::LoadJSON(QFile * JSONFile)
{
    qDebug() << QTime::currentTime().toString() << " [OUT] Parsing JSON " << JSONFile->fileName();
    //ADD VALID JSON CHECK HERE

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
