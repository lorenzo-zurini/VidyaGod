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

    //Whether PackageDir is a VidyaGod package: it holds at least one node file. False for an empty path (a cancelled
    //file picker) or a folder with no node in it.
    static bool CheckPackageValid(QDir * PackageDir);
};

#endif // FILESYSTEMOPERATIONS_H
