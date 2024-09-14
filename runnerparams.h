#ifndef RUNNERPARAMS_H
#define RUNNERPARAMS_H

#include "dboperations.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"

#include <QString>
#include <QList>
#include <QDir>
#include <QTableView>
#include <QJsonArray>

class RunnerParams

{
public:
    RunnerParams(QTableView * LibraryTableView, QTableView * PackagesTableView, QString ProtonPath);
    QString GameName;
    QString UMUID;
    QString ParentPackage;
    QString ExePath;
    QList<int> Recipe;
    QStringList ExeArgs;
    QMap<QString, QString> Paths;
    QJsonArray SubComponentsArray;
    QString UnionFSString;
};

#endif // RUNNERPARAMS_H
