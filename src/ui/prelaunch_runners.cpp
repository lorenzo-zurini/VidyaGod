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

namespace {
//A runner's id as a person reads it: "geproton_11_3" → "Geproton 11.3", "native-passthrough_exec" → "Native". The id
//stays in the tooltip; the data names runners by their node label, which is an identifier, not a title.
QString RunnerDisplayName(const std::string& Id)
{
    if (Id == LaunchResolver::kNativeTerminalId) return "Native (built-in)";
    std::string S = Id;
    if (S.size() > 5 && S.compare(S.size() - 5, 5, "_exec") == 0) S.resize(S.size() - 5);
    if (S.rfind("native-passthrough", 0) == 0) return "Native";
    for (size_t I = 1; I + 1 < S.size(); ++I)
        if (S[I] == '_') S[I] = (std::isdigit((unsigned char)S[I - 1]) && std::isdigit((unsigned char)S[I + 1])) ? '.' : ' ';
    if (!S.empty()) S[0] = (char)std::toupper((unsigned char)S[0]);
    return QString::fromStdString(S);
}
}

std::string PreLaunchWindow::ChainStepInput(int Step) const
{
    if (Step <= 0)
    {
        const Node* L = CurrentLaunch();
        if (!L) return ManifestModel::MachinePlatform();
        //The entry may be one a ticked graft adds: read it off the row's resolution with the grafts applied.
        if (!Entrypoint.empty() && !L->Entries.contains(Entrypoint))
        {
            const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), L->Key(), {}, {}, CollectGrafts());
            if (P.Exec.contains(Entrypoint)) return P.Exec[Entrypoint].value("HOST", L->HostPlatform);
        }
        return L->HostFor(Entrypoint);
    }
    if (Step - 1 >= (int)CurrentChain.size()) return ManifestModel::MachinePlatform();
    const std::string& Prev = CurrentChain[Step - 1];
    if (Prev == LaunchResolver::kNativeTerminalId) return ManifestModel::MachinePlatform();
    const Node* R = Index ? Index->Find(Prev) : nullptr;
    return R ? R->HostPlatform : ManifestModel::MachinePlatform();
}

bool PreLaunchWindow::ChainIdIsTerminal(const std::string& Id) const
{
    if (Id == LaunchResolver::kNativeTerminalId) return true;
    const Node* R = Index ? Index->Find(Id) : nullptr;
    if (!R || R->HostPlatform != ManifestModel::MachinePlatform()) return false;
    for (const auto& G : R->GuestPlatform) if (G == ManifestModel::MachinePlatform()) return true;
    return false;
}

void PreLaunchWindow::RebuildRunnerChain()
{
    // Resolve the DEFAULT chain for this variant (honours a persisted RUNNER_CHAIN), then render the per-step combos.
    CurrentChain.clear();
    const Node* L = CurrentLaunch();
    if (L)
    {
        ContainerParams Cp{std::filesystem::path(BundleDir)};
        Cp.NodeIdx = Index; Cp.LaunchNodeId = LaunchNodeId; Cp.Entrypoint = Entrypoint; Cp.PackageUID = PackageUID;
        Cp.InstanceName = InstanceName;
        Cp.Platform = ChainStepInput(0);
        CurrentChain = LaunchResolver::ResolveChainIds(*Index, *L, Cp, *GlobalConfigJSON);
    }
    RenderChainCombos();
}

void PreLaunchWindow::RenderChainCombos()
{
    static const QString NoRunnerMsg = "No installed runner chain — download a compatible runner from the Catalog.";

    // Tear down the previous step rows.
    ChainCombos.clear();
    QLayoutItem* Item;
    while ((Item = ChainLayout->takeAt(0)) != nullptr)
    {
        if (QWidget* W = Item->widget()) W->deleteLater();
        delete Item;
    }

    const std::string Machine = ManifestModel::MachinePlatform();
    const Node* L0 = CurrentLaunch();
    for (int i = 0; i < (int)CurrentChain.size(); ++i)
    {
        const std::string Input = ChainStepInput(i);
        if (i > 0)
        {
            QLabel* Sep = new QLabel("›", ChainContainer);
            Sep->setProperty("role", "dim");
            ChainLayout->addWidget(Sep);
        }
        QComboBox* Combo = new QComboBox(ChainContainer);
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        {
            QSignalBlocker B(Combo);
            for (const Node* R : Installed::CandidateRunners(*Index, Input, L0 ? L0->BundleDir : std::filesystem::path()))
            {
                Combo->addItem(RunnerDisplayName(R->NodeId), QString::fromStdString(R->Key()));   // keyed by the index key
                Combo->setItemData(Combo->count() - 1, QString::fromStdString(R->NodeId), Qt::ToolTipRole);
            }
            // The native terminal step also offers the built-in passthrough (used when no native runner is authored).
            if (Input == Machine && Combo->findData(QString::fromStdString(LaunchResolver::kNativeTerminalId)) < 0)
                Combo->addItem("Native (built-in)", QString::fromStdString(LaunchResolver::kNativeTerminalId));
            int Sel = Combo->findData(QString::fromStdString(CurrentChain[i]));
            if (Sel < 0 && Combo->count() > 0)
            {
                // The resolved id isn't an installed candidate (e.g. synthesized terminal absent here) — show it anyway
                // so the chain is faithful and selectable.
                Combo->addItem(RunnerDisplayName(CurrentChain[i]), QString::fromStdString(CurrentChain[i]));
                Sel = Combo->count() - 1;
            }
            if (Sel >= 0) Combo->setCurrentIndex(Sel);
        }
        Combo->setToolTip(QString("Runs %1 programs").arg(QString::fromStdString(Input)));
        Combo->setAccessibleName(i == 0 ? QString("Runner") : i == 1 ? QString("Runner's runner") : QString("Runner %1").arg(i + 1));
        const int Step = i;
        connect(Combo, &QComboBox::currentIndexChanged, this, [this, Step](int){ onChainStepChanged(Step); });
        ChainLayout->addWidget(Combo);
        ChainCombos.push_back(Combo);
    }

    // Validity: a chain is runnable when it ends on the machine platform (the native terminal). Say so only when not.
    const bool Reaches = !CurrentChain.empty() && ChainIdIsTerminal(CurrentChain.back());
    if (ChainHint)
    {
        if (CurrentChain.empty())
            ChainHint->setText(QString("⚠ Nothing installed runs this on %1 — download a runner from the Catalog.").arg(QString::fromStdString(Machine)));
        else if (!Reaches)
            ChainHint->setText(QString("⚠ This chain does not reach %1.").arg(QString::fromStdString(Machine)));
        ChainHint->setVisible(!Reaches);
    }
    if (LaunchButton) LaunchButton->setEnabled(Reaches);
    if (StatusLabel)
    {
        if (!Reaches) StatusLabel->setText(NoRunnerMsg);
        else if (StatusLabel->text() == NoRunnerMsg) StatusLabel->clear();
    }
}

void PreLaunchWindow::onChainStepChanged(int Step)
{
    if (Step < 0 || Step >= (int)ChainCombos.size()) return;
    const std::string Chosen = ChainCombos[Step]->currentData().toString().toStdString();
    if (Chosen.empty()) return;

    // Adopt the choice as the new step, drop everything downstream, then BFS-re-resolve the tail from its HOST.
    CurrentChain.resize(Step + 1);
    CurrentChain[Step] = Chosen;
    if (!ChainIdIsTerminal(Chosen))
    {
        const Node* R = Index ? Index->Find(Chosen) : nullptr;
        const std::string Host = R ? R->HostPlatform : ManifestModel::MachinePlatform();
        const Node* L = CurrentLaunch();
        std::vector<std::string> Tail = L ? LaunchResolver::ResolveChainTail(*Index, Host, *L)
                                          : std::vector<std::string>{};
        for (const std::string& Id : Tail) CurrentChain.push_back(Id);
    }
    RenderChainCombos();
    RebuildCustomVarPickers();
}
