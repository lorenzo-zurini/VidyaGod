#include "jsonoperations.h"

JSONOps::JSONOps() {}

bool JSONOps::LoadJSON(QFile * JSONFile, nlohmann::ordered_json * JSONDocument)
{
    std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] Parsing JSON " << JSONFile->fileName().toStdString() << std::endl;

    if (!JSONFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[ERR] File " << JSONFile->fileName().toStdString() << " does not exist." << std::endl;
        return 1; //fail
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] File " << JSONFile->fileName().toStdString() << " opened for reading successfully!" << std::endl;
        QByteArray JSONFileData = JSONFile->readAll();
        if (nlohmann::ordered_json::accept(JSONFileData))
        {
            std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] File " << JSONFile->fileName().toStdString() << " appears valid, parsing." << std::endl;
            (*JSONDocument) = nlohmann::ordered_json::parse(JSONFileData);
            std::cout << JSONDocument->dump() << std::endl;
            JSONFile->close();
            std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] Parse done! " << std::endl;
            return 0; //success
        }
        else
        {
            std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[ERR] Invalid JSON! " << std::endl;
            return 1; //fail
        }
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[ERR] Could not open file for reading! " << std::endl;
        return 1; //fail
    }
}

bool JSONOps::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] File opened for writing successfully!" << std::endl;
        std::cout << QTime::currentTime().toString().toStdString()  << " JSONOperations: " << "[OUT] Saved " << JSONFile->fileName().toStdString() << std::endl;
        QTextStream OutFileStream(JSONFile);
        OutFileStream << QString::fromStdString((*JSONDocument).dump(4));
        JSONFile->close();
        return true;
    }
    else
    {
        //qDebug().noquote() << QTime::currentTime()  << "JSONOperations:" << "[ERR] Could not open file for writing!";
        return false;
    }
}

nlohmann::ordered_json JSONOps::GetSubComponents(QString MetaDataPath, QList<int> Recipe)
{
    //BUILD AN ARRAY CONTAINING ALL SUBCOMPONENTS, IN ORDER, FILTERED BY RECIPE.
    nlohmann::ordered_json * MANIFESTJSON = new nlohmann::ordered_json;

    if(JSONOps::LoadJSON(new QFile(QDir::cleanPath(MetaDataPath + QDir::separator() + "MANIFEST.json")), MANIFESTJSON))
    {
        //ADD ERROR HANDLING HERE!
    }

    nlohmann::ordered_json SubComponentsArray;
    for (int i = 0; i < (*MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        if (Recipe.contains(i))
        {
            for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                SubComponentsArray.push_back((*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]);
                //qDebug().noquote() << QTime::currentTime()  << "JSONOperations:" << " [OUT] Added COMPONENT" << i << "SUBCOMPONENT" << j;
            }
        }
    }

    delete MANIFESTJSON;
    //qDebug().noquote() << QTime::currentTime()  << "JSONOperations:" << "[OUT] Completed SubComponentsArray:"<< QString::fromStdString(SubComponentsArray.dump(4));
    return SubComponentsArray;
}

nlohmann::ordered_json JSONOps::ReplaceVariables(nlohmann::ordered_json OriginalArray, QMap<QString, QString> VariableValues)
{
    QString JSONString(QString::fromStdString(OriginalArray.dump()));

    // Iterate using a const iterator (Qt 6.2+ compatible)
    for (auto it = VariableValues.constBegin(); it != VariableValues.constEnd(); ++it)
    {
        const QString &Variable = it.key();
        const QString &Value = it.value();

        JSONString.replace("%" + Variable + "%", Value);
    }

    return nlohmann::ordered_json::parse(JSONString.toUtf8());
}
