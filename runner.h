#ifndef RUNNER_H
#define RUNNER_H

#include "nlohmann/json.hpp"

#include <QString>
#include <QDir>
#include <QList>
#include <QTableView>
#include <QScreen>

#include "dboperations.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"

class Runner
{
private:
    bool InitParams();

public:
    QDir * PackageDir;
    nlohmann::ordered_json * MANIFESTJSON;
    nlohmann::ordered_json * GlobalConfigJSON;

    QString GameName;
    QString UMUID;
    QString ParentPackage;
    QString ExePath;
    QStringList ExeArgs;
    QString DllOverrides;
    QMap<QString, QString> Paths;
    QMap<QString, QString> WindowsPaths;
    QMap<QString, QString> SystemVariables;
    int subgame;
    QList<int> Recipe;
    nlohmann::ordered_json SubComponentsArray;
    QString UnionFSString;
    //RunnerParams * RunnerParams;

    Runner(QDir * PackageDir, nlohmann::ordered_json * MANIFESTJSON, nlohmann::ordered_json * GlobalConfigJSON, int subgame = 0);
    static QStringList StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues);
};

#endif // RUNNER_H
