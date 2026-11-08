#include "guiformat.h"   // PlainLabel
#include "installed.h"
#include "theme.h"   // the named colours
#include "shelf.h"
#include "prelaunchwindow.h"
#include "fold.h"   // a row's options and entries are its resolution's
#include "apppaths.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "launchsession.h"     // ResolveTakingBack — what the game changed, read back before the values show
#include "launchresolver.h"     // ResolveChainIds / ResolveChainTail / kNativeTerminalId (runner daisy-chain UI)
#include "jsonoperations.h"
#include "ipfswrapper.h"     // LanPeers/SetLanExcluded/LanLaunchVars — the Virtual LAN panel
#include "variantpicker.h"     // NaturalLess — deterministic version-aware variant ordering
#include "instancestore.h"     // the instance picker
#include "asyncwork.h"        // the size under the cover, counted off the GUI thread

#include <set>
#include <algorithm>

#include <QBrush>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFont>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QGridLayout>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include "keycapture.h"
#include <QPainterPath>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QPointer>

#include "prelaunchwindow_p.h"

void PreLaunchWindow::FillInstances()
{
    if (!InstanceCombo) return;
    QSignalBlocker B(InstanceCombo);
    InstanceCombo->clear();
    std::vector<std::string> Names = PackageUID.empty() ? std::vector<std::string>{}
                                                        : InstanceStore::List(*GlobalConfigJSON, PackageUID);
    //A game never played has no instance yet: its default one is created by the first launch.
    if (std::find(Names.begin(), Names.end(), InstanceName) == Names.end() && !InstanceName.empty()) Names.push_back(InstanceName);
    for (const std::string& N : Names)
        InstanceCombo->addItem(N == InstanceStore::DefaultInstance ? QString("Default") : QString::fromStdString(N),
                               QString::fromStdString(N));
    InstanceCombo->setCurrentIndex(std::max(0, InstanceCombo->findData(QString::fromStdString(InstanceName))));
}

void PreLaunchWindow::onInstanceChanged()
{
    const std::string Picked = InstanceCombo->currentData().toString().toStdString();
    if (Picked.empty() || Picked == InstanceName) return;
    InstanceName = Picked;
    //Everything the instance holds: its runner chain, its add-ons, its option values.
    RebuildRunnerChain();
    RebuildModuleTree();
    RebuildCustomVarPickers();
    RefreshGraftEntryRows();
    RefreshInstanceInfo();
}

void PreLaunchWindow::RefreshInstanceInfo()
{
    if (!InstanceInfo) return;
    const QString Last = QString::fromStdString(InstanceStore::LastRun(*GlobalConfigJSON, PackageUID, InstanceName));
    const QString When = PlayedWhen(Last);
    if (When.isEmpty()) { InstanceInfo->setText("Never played"); return; }
    InstanceInfo->setText("Last played " + When);
    InstanceInfo->setToolTip(QDateTime::fromString(Last, Qt::ISODate).toLocalTime().toString(Qt::TextDate));
}

namespace {
//A name for a new instance, or "" when cancelled; an invalid or taken name is refused and asked again.
std::string AskInstanceName(QWidget* Parent, const QString& Title, const QString& Suggest,
                            const nlohmann::ordered_json& Cfg, const std::string& Uid)
{
    QString Name = Suggest;
    for (;;)
    {
        bool Ok = false;
        Name = QInputDialog::getText(Parent, Title, "Name:", QLineEdit::Normal, Name, &Ok).trimmed();
        if (!Ok || Name.isEmpty()) return {};
        const std::string N = Name.toStdString();
        const auto Have = InstanceStore::List(Cfg, Uid);
        if (!InstanceStore::ValidName(N))
            QMessageBox::warning(Parent, Title, "An instance name is one folder name: no slashes, up to 64 characters.");
        else if (std::find(Have.begin(), Have.end(), N) != Have.end())
            QMessageBox::warning(Parent, Title, "There is already an instance called \"" + Name + "\".");
        else return N;
    }
}
}

void PreLaunchWindow::NewInstance()
{
    const std::string N = AskInstanceName(this, "New instance", "New instance", *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    if (!InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)) { QMessageBox::warning(this, "New instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

void PreLaunchWindow::DuplicateInstance()
{
    const QString Shown = InstanceCombo->currentText();
    const std::string N = AskInstanceName(this, "Duplicate instance", Shown + " copy", *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    const bool Exists = std::find(Have.begin(), Have.end(), InstanceName) != Have.end();
    //An instance never launched has nothing on disk to copy: the duplicate starts fresh, like it.
    if (!(Exists ? InstanceStore::Clone(*GlobalConfigJSON, PackageUID, InstanceName, N, &Err)
                 : InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)))
    { QMessageBox::warning(this, "Duplicate instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

void PreLaunchWindow::RenameInstance()
{
    const std::string N = AskInstanceName(this, "Rename instance", InstanceCombo->currentText(), *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    const bool Exists = std::find(Have.begin(), Have.end(), InstanceName) != Have.end();
    if (!(Exists ? InstanceStore::Rename(*GlobalConfigJSON, PackageUID, InstanceName, N, &Err)
                 : InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)))
    { QMessageBox::warning(this, "Rename instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RefreshInstanceInfo();
}

void PreLaunchWindow::DeleteInstance()
{
    const QString Shown = InstanceCombo->currentText();
    QMessageBox Ask(QMessageBox::Warning, "Delete instance",
                    "Delete the instance \"" + Shown + "\"?\n\nIts saves and settings are deleted with it. This cannot be undone.",
                    QMessageBox::Cancel, this);
    QPushButton* Del = Ask.addButton("Delete", QMessageBox::DestructiveRole);
    Ask.setDefaultButton(QMessageBox::Cancel);
    Ask.exec();
    if (Ask.clickedButton() != Del) return;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    if (std::find(Have.begin(), Have.end(), InstanceName) != Have.end())
    {
        std::string Err;
        if (!InstanceStore::Delete(*GlobalConfigJSON, PackageUID, InstanceName, &Err))
        { QMessageBox::warning(this, "Delete instance", QString::fromStdString(Err)); return; }
    }
    InstanceName = InstanceStore::ResolveActive(*GlobalConfigJSON, PackageUID);
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

// ---- tabs -----------------------------------------------------------------------------------------------------------
