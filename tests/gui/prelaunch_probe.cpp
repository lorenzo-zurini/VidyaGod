// Offscreen probe: builds the real PreLaunchWindow for a given launchable node group and prints every
// CustomVar row it renders (label + control kind + whether it collects a value). Diagnostic tool for
// "why is knob X not exposed in the prelaunch window" questions. Run with QT_QPA_PLATFORM=offscreen.
//   prelaunch_probe <nodeId> [<nodeId>...]

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>

#include <nlohmann/json.hpp>

#include "prelaunchwindow.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "jsonoperations.h"
#include "apppaths.h"

#include <iostream>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 2) { std::cerr << "usage: prelaunch_probe <nodeId> [<nodeId>...]\n"; return 2; }

    nlohmann::ordered_json Cfg;
    const std::string CfgPath = (AppPaths::DataRoot() / "GlobalConfig.JSON").string();
    { QFile f(QString::fromStdString(CfgPath)); JSONOps::LoadJSON(&f, &Cfg); }

    NodeIndex Index = PackageCatalog::BuildCatalogIndex(Cfg);

    std::vector<std::string> Group;
    for (int i = 1; i < argc; ++i) Group.emplace_back(argv[i]);
    for (const auto &g : Group)
        if (!Index.Find(g)) { std::cerr << "node not found: " << g << "\n"; return 1; }

    PreLaunchWindow W(&Cfg, &Index, Group);
    W.resize(900, 700);
    W.show();
    QApplication::processEvents();
    QApplication::processEvents();

    int rows = 0;
    for (QGroupBox *Box : W.findChildren<QGroupBox *>())
    {
        bool printedBox = false;
        for (QWidget *Row : Box->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        {
            QLabel *Lbl = Row->findChild<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
            if (!Lbl) continue;
            QWidget *Field = nullptr;
            for (QWidget *c : Row->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
                if (c != Lbl) { Field = c; break; }
            if (!Field) continue;
            if (!printedBox) { std::cout << "[" << Box->title().toStdString() << "]\n"; printedBox = true; }
            const QString CVKey = Field->property("CVKey").toString();
            std::string kind = Field->metaObject()->className();
            std::string extra;
            if (auto *L2 = qobject_cast<QLabel *>(Field)) extra = " text=\"" + L2->text().toStdString() + "\"";
            std::cout << "   " << Lbl->text().toStdString() << "  <" << kind << ">"
                      << (CVKey.isEmpty() ? "  [NOT collected]" : ("  [collects " + CVKey.toStdString() + "]"))
                      << extra << "\n";
            ++rows;
        }
    }
    std::cout << "TOTAL rendered CustomVar rows: " << rows << "\n";
    W.grab().save("/tmp/prelaunch_probe.png");
    std::cout << "screenshot: /tmp/prelaunch_probe.png\n";
    return 0;
}
