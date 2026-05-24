#ifndef FILESYSTEMOPERATIONS_H
#define FILESYSTEMOPERATIONS_H

#include <QDir>
#include <iostream>
#include <QDirIterator>
#include <QProcess>

#include "nlohmann/json.hpp"

class FSOps
{
public:
    FSOps();

    static QString SubPath(QString Parent, QString SubDir);
    static bool CreateDirectories (QMap<QString, QString>);
    static bool CheckPackageValid(QDir * PackageDir);
    static bool CheckCaseConflicts(QString RuntimePath);
};

#endif // FILESYSTEMOPERATIONS_H
