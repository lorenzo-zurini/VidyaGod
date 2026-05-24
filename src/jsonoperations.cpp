#include "jsonoperations.h"
#include "commonutils.h"

JSONOps::JSONOps() {}

bool JSONOps::LoadJSON(QFile * JSONFile, nlohmann::ordered_json * JSONDocument)
{
    LogOut("JSONOperations", "Parsing JSON " + JSONFile->fileName().toStdString());

    if (!JSONFile->exists())
    {
        LogErr("JSONOperations", "File " + JSONFile->fileName().toStdString() + " does not exist.");
        return 1; //fail
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        LogSucc("JSONOperations", "File " + JSONFile->fileName().toStdString() + " opened for reading successfully!");
        QByteArray JSONFileData = JSONFile->readAll();
        if (nlohmann::ordered_json::accept(JSONFileData))
        {
            LogOut("JSONOperations", "File " + JSONFile->fileName().toStdString() + " appears valid, parsing.");
            (*JSONDocument) = nlohmann::ordered_json::parse(JSONFileData);
            std::cout << JSONDocument->dump(4) << std::endl;
            JSONFile->close();
            LogOut("JSONOperations", "Parse done!");
            return 0; //success
        }
        else
        {
            LogErr("JSONOperations", "Invalid JSON!");
            return 1; //fail
        }
    }
    else
    {
        LogErr("JSONOperations", "Could not open file for reading!");
        return 1; //fail
    }
}

bool JSONOps::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        LogOut("JSONOperations", "File opened for writing successfully!");
        LogOut("JSONOperations", "Saved " + JSONFile->fileName().toStdString());
        QTextStream OutFileStream(JSONFile);
        OutFileStream << QString::fromStdString((*JSONDocument).dump(4));
        JSONFile->close();
        return true;
    }
    else
    {
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
            }
        }
    }

    delete MANIFESTJSON;
    return SubComponentsArray;
}

nlohmann::ordered_json JSONOps::ReplaceVariables(nlohmann::ordered_json OriginalArray, QMap<QString, QString> VariableValues)
{
    QString JSONString(QString::fromStdString(OriginalArray.dump()));
    for (auto it = VariableValues.constBegin(); it != VariableValues.constEnd(); ++it)
    {
        const QString &Variable = it.key();
        const QString &Value = it.value();

        JSONString.replace("%" + Variable + "%", Value);
    }

    return nlohmann::ordered_json::parse(JSONString.toUtf8());
}
