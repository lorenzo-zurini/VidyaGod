#ifndef FILESYSTEMOPERATIONS_H
#define FILESYSTEMOPERATIONS_H

#include <QDir>
#include <iostream>
#include <QDirIterator>
#include <QProcess>

#include "nlohmann/json.hpp"

//Static utility class for filesystem operations that don't belong to a specific container.
//Primarily used at startup and when adding packages to the library.
class FSOps
{
public:
    FSOps();

    //Returns a clean joined path: Parent/SubDir, normalized for the current OS separator.
    static QString SubPath(QString Parent, QString SubDir);

    //Creates all directories listed in the map (key = label, value = path).
    //Returns true on success.
    static bool CreateDirectories (QMap<QString, QString>);

    //Validates that PackageDir is a well-formed VidyaGod package:
    //currently checks that a METADATA subdirectory exists.
    //Returns false if the path is empty (e.g. user cancelled the file picker)
    //or if the METADATA directory is absent.
    static bool CheckPackageValid(QDir * PackageDir);

    //Walks RuntimePath recursively and warns about case-insensitive filename
    //collisions that would cause problems under Wine's case-insensitive filesystem view.
    static bool CheckCaseConflicts(QString RuntimePath);
};

#endif // FILESYSTEMOPERATIONS_H
