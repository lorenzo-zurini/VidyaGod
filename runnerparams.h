#ifndef RUNNERPARAMS_H
#define RUNNERPARAMS_H

#include <QString>
#include <QList>
#include <QDir>
#include <QTableView>
#include <QScreen>
#include "nlohmann/json.hpp"

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
    QMap<QString, QString> WindowsPaths;
    QMap<QString, QString> SystemVariables;
    QString DllOverrides;
    nlohmann::ordered_json SubComponentsArray;
    QString UnionFSString;

    static QStringList StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues);
};

#endif // RUNNERPARAMS_H
