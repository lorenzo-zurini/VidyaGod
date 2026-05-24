#include "jsonoperations.h"
#include "commonutils.h"

JSONOps::JSONOps() {}

//Parses the file at JSONFile into JSONDocument.
//Uses nlohmann::ordered_json::accept() for a cheap validity pre-check before the full
//parse, which avoids throwing exceptions on malformed input.
//Returns 0 on success, 1 on failure — intentionally matching shell exit-code convention
//so callers can test with `if (LoadJSON(...))` to detect failure.
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

        //Pre-validate before parsing to avoid exceptions on corrupt/empty files.
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

//Writes JSONDocument to JSONFile with 4-space pretty-printing.
//Truncates the file if it already exists so stale content is never left behind.
//Returns true on success, false if the file cannot be opened.
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

//LEGACY: Builds a SubComponentsArray by loading MANIFEST.json from MetaDataPath and
//collecting SUBCOMPONENTS from each COMPONENT whose integer index appears in Recipe.
//This integer-index approach has been replaced by the string ComponentID recipe in
//ContainerWrapper::BuildSubComponentsArray — use that instead for new code.
//NOTE: Allocates MANIFESTJSON on the heap and deletes it before returning.
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
        //Only include components whose integer index is in the Recipe list.
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

//LEGACY: Performs %VARIABLE% substitution across the entire JSON by serializing it to a
//QString, doing string replacements for every key in VariableValues, then re-parsing.
//Simple but requires a full serialize/deserialize round-trip.
//Superseded by ContainerWrapper::StringVariableSubstitution which operates per-string.
nlohmann::ordered_json JSONOps::ReplaceVariables(nlohmann::ordered_json OriginalArray, QMap<QString, QString> VariableValues)
{
    QString JSONString(QString::fromStdString(OriginalArray.dump()));

    //Iterate every variable and replace all occurrences of %KEY% with its value.
    for (auto it = VariableValues.constBegin(); it != VariableValues.constEnd(); ++it)
    {
        const QString &Variable = it.key();
        const QString &Value = it.value();

        JSONString.replace("%" + Variable + "%", Value);
    }

    return nlohmann::ordered_json::parse(JSONString.toUtf8());
}
