#include "jsonoperations.h"

#include <QSaveFile>
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
//Returns true on success, false if the file cannot be written.
//
//ATOMIC. This writes GlobalConfig.JSON — PackageSources, PackageCids, friends, module toggles, every persisted
//CustomVar — and every node .json in a bundle, and it used to truncate in place. A crash, a kill or ENOSPC part
//way through left the user with a half-written config or a half-written package and no second copy. QSaveFile
//writes a sibling temp and renames on commit, so the file on disk is always either the old one or the new one.
bool JSONOps::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
{
    QSaveFile Out(JSONFile->fileName());
    if (!Out.open(QFile::WriteOnly))
    {
        LogErr("JSONOperations", "could not open for writing: " + JSONFile->fileName().toStdString());
        return false;
    }
    {
        QTextStream OutFileStream(&Out);
        OutFileStream << QString::fromStdString((*JSONDocument).dump(4));
    }
    //commit() performs the rename AND reports a write that failed on flush (a full disk fails HERE, not at
    //open) — the case a plain QFile reported as success.
    if (!Out.commit())
    {
        LogErr("JSONOperations", "could not save " + JSONFile->fileName().toStdString()
                                 + ": " + Out.errorString().toStdString());
        return false;
    }
    LogOut("JSONOperations", "Saved " + JSONFile->fileName().toStdString());
    return true;
}

