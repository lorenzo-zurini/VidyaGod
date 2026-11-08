#ifndef LAUNCHCOMMAND_H
#define LAUNCHCOMMAND_H

#include "launchparams.h"   // ContainerParams

#include <QProcessEnvironment>
#include <QStringList>

#include <string>
#include <vector>

//The command a launch runs, composed from its resolved parameters alone: the boundary runner's EXECUTABLE + ARGS
//(or the content itself for a native chain), a cross-namespace chain's inner target, each outer same-namespace
//wrapper around it, and the environment — Base, then every outer wrapper's ENV/REMOVE_ENV, the boundary's last (the
//content-proximate one wins), WINEDLLOVERRIDES for a prefix runner with an operator's own entries after the package's.
//No process, no sandbox, no disk: what ContainerWrapper::Execute then runs, inside the sandbox.
//
//OverrideExe (tooling / install steps) swaps just the target: content-bearing ARGS are dropped, the override follows
//the launcher verb, wrappers are not applied.
struct LaunchCommand
{
    std::string Program;
    QStringList Arguments;
    QProcessEnvironment Env;
    std::vector<std::string> Wrappers;   // the outer wrappers applied, innermost first

    //Base: the environment the process starts from (the system's, with the sandbox's private home under a launch).
    static LaunchCommand Compose(ContainerParams CP, QProcessEnvironment Base, const std::string &OverrideExe = {},
                                 const std::vector<std::string> &OverrideArgs = {});
};

#endif // LAUNCHCOMMAND_H
