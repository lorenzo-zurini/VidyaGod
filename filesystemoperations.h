#ifndef FILESYSTEMOPERATIONS_H
#define FILESYSTEMOPERATIONS_H

#include <QtLogging>

#include <QDir>

class FileSystemOperations
{
public:
    FileSystemOperations();

    static bool CheckPackageValid(QDir * PackageDir);
};

#endif // FILESYSTEMOPERATIONS_H
