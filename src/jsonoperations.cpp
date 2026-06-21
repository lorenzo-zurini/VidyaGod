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
            //std::cout << JSONDocument->dump(4) << std::endl;
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

