#ifndef REGISTRYOPERATIONS_H
#define REGISTRYOPERATIONS_H

#include "nlohmann/json.hpp"
#include <QString>
#include <QTime>

class RunnerParams;
class Runner;
class RegOps
{
public:
    RegOps();
    static bool RegAdd(Runner Runner, nlohmann::ordered_json SubComponentJSON, QString PrefixPath, QString ProtonPath);
};

#endif // REGISTRYOPERATIONS_H
