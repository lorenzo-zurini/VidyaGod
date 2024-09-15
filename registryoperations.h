#ifndef REGISTRYOPERATIONS_H
#define REGISTRYOPERATIONS_H

#include <QJsonObject>

class RunnerParams;
class RegOps
{
public:
    RegOps();
    static bool RegAdd(QJsonObject SubComponentJSON, QString PrefixPath, QString ProtonPath);
};

#endif // REGISTRYOPERATIONS_H
