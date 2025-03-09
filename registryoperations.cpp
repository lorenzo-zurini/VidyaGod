#include "registryoperations.h"
#include "umurunner.h"

RegOps::RegOps() {}

bool RegOps::RegAdd(nlohmann::ordered_json SubComponentJSON, QString PrefixPath, QString ProtonPath)
{
    QString REGPATH = QString::fromStdString(SubComponentJSON["REGPATH"]);

    if (SubComponentJSON.contains("KEYVALUES"))
    {
        for (auto Item : SubComponentJSON["KEYVALUES"].items())
        {
            QStringList CommandArgs;
            CommandArgs.append("add");      CommandArgs.append(REGPATH);
            CommandArgs.append("/f");
            CommandArgs.append("/v");       CommandArgs.append(QString::fromStdString(Item.key()));
            CommandArgs.append("/t");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["TYPE"]));

            if (SubComponentJSON["KEYVALUES"][Item.key()].contains("VALUE"))
            {
                CommandArgs.append("/d");       CommandArgs.append(QString::fromStdString(SubComponentJSON["KEYVALUES"][Item.key()]["VALUE"]));
            }

            if(SubComponentJSON["ARCHITECTURE"] == "32")
            {
                CommandArgs.append("/reg:32");
            }

            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] EXECUTING REGISTRY STRING: " << CommandArgs;
            UMURunner::RunWithUMU(ProtonPath, PrefixPath, "reg", CommandArgs);
        }
    }
    else
    {
        QStringList CommandArgs;
        CommandArgs.append("add");      CommandArgs.append(REGPATH);
        CommandArgs.append("/f");

        if(SubComponentJSON["ARCHITECTURE"] == "32")
        {
            CommandArgs.append("/reg:32");
        }

        qDebug().noquote() << QTime::currentTime().toString() << "REG STRING: " << CommandArgs;
        UMURunner::RunWithUMU(ProtonPath, PrefixPath, "reg", CommandArgs);
    }
    return true;
}
