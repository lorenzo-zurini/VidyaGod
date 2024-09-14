#include "registryoperations.h"
#include "umurunner.h"

RegOps::RegOps() {}

bool RegOps::RegAdd(QJsonObject SubComponentJSON, QString DefPrefixPath, QString ProtonPath)
{
    QString REGPATH = SubComponentJSON["REGPATH"].toString();

    if (SubComponentJSON.keys().contains("KEYVALUES"))
    {
        for (auto Key : SubComponentJSON["KEYVALUES"].toObject().keys())
        {
            QStringList CommandArgs;
            CommandArgs.append("add");      CommandArgs.append(REGPATH);
            CommandArgs.append("/f");
            CommandArgs.append("/v");       CommandArgs.append(Key);
            CommandArgs.append("/t");       CommandArgs.append(SubComponentJSON["KEYVALUES"].toObject()[Key].toObject()["TYPE"].toString());

            if (SubComponentJSON["KEYVALUES"].toObject()[Key].toObject().keys().contains("VALUE"))
            {
                CommandArgs.append("/d");       CommandArgs.append(SubComponentJSON["KEYVALUES"].toObject()[Key].toObject()["VALUE"].toString());
            }

            if(SubComponentJSON["ARCHITECTURE"].toString() == "32")
            {
                CommandArgs.append("/reg:32");
            }

            qDebug().noquote() << QTime::currentTime().toString() << "REG STRING: " << CommandArgs;
            UMURunner::RunWithUMU(ProtonPath, DefPrefixPath, DefPrefixPath, "0", "reg", CommandArgs);
        }
    }
    else
    {
        QStringList CommandArgs;
        CommandArgs.append("add");      CommandArgs.append(REGPATH);
        CommandArgs.append("/f");

        if(SubComponentJSON["ARCHITECTURE"].toString() == "32")
        {
            CommandArgs.append("/reg:32");
        }

        qDebug().noquote() << QTime::currentTime().toString() << "REG STRING: " << CommandArgs;
        UMURunner::RunWithUMU(ProtonPath, DefPrefixPath, DefPrefixPath, "0", "reg", CommandArgs);
    }
    return true;
}
