// Offscreen probe: builds the real PreLaunchWindow for a given launchable node group and prints every
// CustomVar row it renders (label + control kind + whether it collects a value). Diagnostic tool for
// "why is knob X not exposed in the prelaunch window" questions. Run with QT_QPA_PLATFORM=offscreen.
//   prelaunch_probe <nodeId> [<nodeId>...]

#include <QApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>

#include <nlohmann/json.hpp>

#include "prelaunchwindow.h"
#include <QStyleFactory>
#include <QTabWidget>
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "jsonoperations.h"
#include "apppaths.h"

#include <iostream>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    //The app's own look (main.cpp): Fusion over the desktop palette, DejaVu Sans 10.
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setFont(QFont("DejaVu Sans", 10));
    if (argc < 2) { std::cerr << "usage: prelaunch_probe <nodeId> [<nodeId>...]\n"; return 2; }

    nlohmann::ordered_json Cfg;
    const std::string CfgPath = (AppPaths::DataRoot() / "GlobalConfig.JSON").string();
    { QFile f(QString::fromStdString(CfgPath)); JSONOps::LoadJSON(&f, &Cfg); }

    NodeIndex Index = PackageCatalog::BuildCatalogIndex(Cfg);

    std::vector<std::string> Group;
    for (int i = 1; i < argc; ++i) Group.emplace_back(argv[i]);
    for (const auto &g : Group)
        if (!Index.Find(g)) { std::cerr << "node not found: " << g << "\n"; return 1; }

    PreLaunchWindow W(&Cfg, &Index, Group, qEnvironmentVariable("PRELAUNCH_FACE").toStdString());   // PRELAUNCH_FACE: the card
    //PRELAUNCH_SHOT=<png> [PRELAUNCH_SIZE=WxH]: just the window as it opens (its own size unless given), settled.
    if (qEnvironmentVariableIsSet("PRELAUNCH_SHOT"))
    {
        W.show();
        if (qEnvironmentVariableIsSet("PRELAUNCH_SIZE"))
        {
            const QStringList WH = qEnvironmentVariable("PRELAUNCH_SIZE").split('x');
            if (WH.size() == 2) W.resize(WH[0].toInt(), WH[1].toInt());
        }
        //PRELAUNCH_TAB=<title prefix>: show that tab; PRELAUNCH_EXPAND=1: open every section of its trees.
        if (auto *T = W.findChild<QTabWidget*>("prelaunchTabs"); T && qEnvironmentVariableIsSet("PRELAUNCH_TAB"))
            for (int i = 0; i < T->count(); ++i)
                if (T->tabText(i).startsWith(qEnvironmentVariable("PRELAUNCH_TAB"))) T->setCurrentIndex(i);
        if (qEnvironmentVariableIsSet("PRELAUNCH_EXPAND"))
            for (QTreeWidget *Tr : W.findChildren<QTreeWidget*>()) Tr->expandAll();
        for (int i = 0; i < 30; ++i) QApplication::processEvents();
        const QString Shot = qEnvironmentVariable("PRELAUNCH_SHOT");
        W.grab().save(Shot);
        std::cout << "screenshot: " << Shot.toStdString() << " (" << W.width() << "x" << W.height() << ")\n";
        return 0;
    }
    // Geometry probe: the cover must own the lion's share of the left column (regression: it went comically tiny).
    W.resize(1100, 750);
    W.show();
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    if (QLabel * Cov = W.findChild<QLabel*>("CoverLabel"))
        std::cout << "[geometry] window=" << W.width() << "x" << W.height()
                  << " cover=" << Cov->width() << "x" << Cov->height() << "\n";
    // Simulate a POPULATED LAN panel (offscreen has no node → it stays hidden otherwise), then run refresh-like
    // relayout cycles and watch for a cover-size ratchet (the "comically tiny cover" regression).
    for (QGroupBox * G : W.findChildren<QGroupBox*>())
        if (G->title() == "Virtual LAN")
        {
            G->setVisible(true);
            if (auto * GL = qobject_cast<QVBoxLayout*>(G->layout()))
            {
                GL->addWidget(new QCheckBox("Friend A   ● direct · 34 ms", G));
                GL->addWidget(new QCheckBox("Friend B   ● relayed", G));
            }
        }
    for (int Cycle = 0; Cycle < 6; ++Cycle)
    {
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        if (QLabel * Cov = W.findChild<QLabel*>("CoverLabel"))
            std::cout << "[geometry] cycle " << Cycle << " cover=" << Cov->width() << "x" << Cov->height()
                  << " pixmap=" << Cov->pixmap().width() << "x" << Cov->pixmap().height() << "\n";
    }
    W.resize(900, 700);
    W.show();
    QApplication::processEvents();
    QApplication::processEvents();

    int rows = 0;
    for (QGroupBox *Box : W.findChildren<QGroupBox *>())
    {
        bool printedBox = false;
        auto *Grid = qobject_cast<QGridLayout *>(Box->layout());
        for (int r = 0; Grid && r < Grid->rowCount(); ++r)
        {
            QLayoutItem *LI = Grid->itemAtPosition(r, 0), *FI = Grid->itemAtPosition(r, 1);
            QLabel *Lbl = LI ? qobject_cast<QLabel *>(LI->widget()) : nullptr;
            QWidget *Field = FI ? FI->widget() : nullptr;
            if (!Lbl || !Field) continue;
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
