#ifndef RUNNER_H
#define RUNNER_H

#include "nlohmann/json.hpp"

#include <QString>
#include <QDir>
#include <QList>
#include <QTableView>
#include <QScreen>
#include <QMessageBox>

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
    QString UMUID = "0";
    QString ParentPackage;
    QString ExePath;
    QStringList ExeArgs;
    QString DllOverrides;
    QMap<QString, QString> Paths;
    QMap<QString, QString> WindowsPaths;
    QMap<QString, QString> SystemVariables;
    int subgame;
    int component;
    QList<int> Recipe;
    nlohmann::ordered_json SubComponentsArray;
    QString UnionFSString;

    Runner(QDir * PackageDir, nlohmann::ordered_json * MANIFESTJSON, nlohmann::ordered_json * GlobalConfigJSON, int subgame = 0, int component = 0);

    bool BuildRuntime();
    bool InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString);
    bool ProcessFileSystemSubComponents();
    bool ProcessOtherSubComponents();
    bool Cleanup();
    bool RemoveSubComponents();
    bool RegAdd(nlohmann::ordered_json SubComponentJSON);
    bool Run(QString OverrideExePath = "", QStringList OverrideExeArgs = QStringList({}));

    // static bool RunWithUMU(QString ProtonPath, QString WinePrefix, QString ExePath, QStringList ExeArgs = {}, QString WorkDirPath = "", QString GAMEID = "0", QString DllOverrides = "");
    static QStringList StringListReplaceVariables(QStringList OriginalStringList, QMap<QString, QString> VariableValues);
};

#endif // RUNNER_H
