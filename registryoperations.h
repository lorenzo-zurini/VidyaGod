#ifndef REGISTRYOPERATIONS_H
#define REGISTRYOPERATIONS_H

#include "nlohmann/json.hpp"
#include <QString>
#include <QTime>

class RunnerParams;
class RegOps
{
public:
    RegOps();
    static bool RegAdd(nlohmann::ordered_json SubComponentJSON, QString PrefixPath, QString ProtonPath);
};

#endif // REGISTRYOPERATIONS_H
