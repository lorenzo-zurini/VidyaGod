#ifndef FILESYSTEMOPERATIONS_H
#define FILESYSTEMOPERATIONS_H

#include <QDir>
#include <iostream>
#include <QDirIterator>
#include <QProcess>

#include "nlohmann/json.hpp"

class RunnerParams;

class FSOps
{
public:
    FSOps();

    static QString SubPath(QString Parent, QString SubDir);
    static bool CreateDirectories (QMap<QString, QString>);
    static bool CheckPackageValid(QDir * PackageDir);
    static bool CheckCaseConflicts(QString RuntimePath);

    static bool MountZipFileLayer(nlohmann::ordered_json SubComponentJSON, int LayerNumber, QString TempPath, QString PackageFilesPath, QString * UnionFSString, QString PackageName);



    static bool ConfigWrite(QString Key, QString Value, QString FilePath);
};

#endif // FILESYSTEMOPERATIONS_H
