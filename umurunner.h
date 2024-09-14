#ifndef UMURUNNER_H
#define UMURUNNER_H

#include <QTableView>
#include <QProcess>
#include <QMessageBox>

class RunnerParams;
class UMURunner
{
public:
    UMURunner(QTableView * LibraryTableView, QTableView * PackagesTableView, QString ProtonPath);
    bool Run();
    static bool RunWithUMU(QString ProtonPath, QString WorkDirPath, QString WinePrefix, QString GAMEID, QString ExePath, QStringList ExeArgs);

private:
    RunnerParams * RunnerParams;
    bool Cleanup();
    bool ProcessSubComponents();
    bool RemoveSubComponents();
    bool InitializeUMUPrefix(QString PrefixPath, QString ProtonPath, QString * UnionFSString);
};

#endif // UMURUNNER_H
