#include "jsonoperations.h"

JSONOps::JSONOps() {}

nlohmann::ordered_json * JSONOps::InitGlobalConfigJSON(QFile * GlobalConfigFile)
{
    if (!GlobalConfigFile->exists())
    {
        //qDebug().noquote() << QTime::currentTime().toString() << "JSONOperations:" << "[OUT] Config flie not deteced. Creating... ";
        QFile("DefaultConfig.JSON").copy("GlobalConfig.JSON");
    }
    return JSONOps::LoadJSON(GlobalConfigFile);
}

nlohmann::ordered_json * JSONOps::LoadJSON(QFile * JSONFile)
{
    //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] Parsing JSON" << JSONFile->fileName();
    nlohmann::ordered_json * JSONDocument;

    if (!JSONFile->exists())
    {
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[ERR] File" << JSONFile->fileName() << "does not exist.";
        return nullptr;
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] File" << JSONFile->fileName() << "opened for reading successfully!";
        QByteArray JSONFileData = JSONFile->readAll();
        if (nlohmann::ordered_json::accept(JSONFileData))
        {
            //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] File" << JSONFile->fileName() << "appears valid, parsing.";
            JSONDocument = new nlohmann::ordered_json(nlohmann::ordered_json::parse(JSONFileData));
        }
        else
        {
            //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[ERR] Invalid JSON!";
            return nullptr;
        }

        JSONFile->close();
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] Parse done!";
        return JSONDocument;
    }
    else
    {
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[ERR] Could not open file for reading!";
        return nullptr;
    }
}

bool JSONOps::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] File opened for writing successfully!";
        //qDebug().noquote() << QTime::currentTime().toString()  << "JSONOperations:" << "[OUT] Saved" << JSONFile->fileName();
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
    nlohmann::ordered_json * MANIFESTJSON = JSONOps::LoadJSON(new QFile(QDir::cleanPath(MetaDataPath + QDir::separator() +"MANIFEST.json")));
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
    for (auto [Variable, Value] : VariableValues.asKeyValueRange())
    {
        JSONString.replace(("%" + Variable + "%"), Value);
    }
    return nlohmann::ordered_json::parse(JSONString.toUtf8());
}
