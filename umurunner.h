#ifndef UMURUNNER_H
#define UMURUNNER_H

#include <QTableView>
#include <QProcess>
#include <QMessageBox>

class RunnerParams;
class UMURunner
{
public:
    bool Run();
    bool Cleanup();

    UMURunner(QTableView * LibraryTableView, QTableView * PackagesTableView, QString ProtonPath);
    static bool RunWithUMU(QString ProtonPath, QString WinePrefix, QString ExePath, QStringList ExeArgs = {}, QString WorkDirPath = "", QString GAMEID = "0", QString DllOverrides = "");

private:
    RunnerParams * RunnerParams;
    bool ProcessFileSystemSubComponents();
    bool ProcessOtherSubComponents();
    bool RemoveSubComponents();
    bool InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString);
};

#endif // UMURUNNER_H
