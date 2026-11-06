#include "pkginsight.h"

#include "packageeditormodel.h"
#include "pkgactions.h"
#include "pkggraph.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

using json = nlohmann::ordered_json;

namespace {
QColor TypeQColor(const std::string &Type)
{
    int R, G, B;
    PkgGraph::TypeColour(Type, R, G, B);
    return QColor(std::min(255, R + 70), std::min(255, G + 70), std::min(255, B + 70));
}
}

PkgInsight::PkgInsight(PackageEditorModel *M, QTabWidget *Tabs) : QObject(Tabs), Model(M)
{
    Soon = new QTimer(this);
    Soon->setSingleShot(true);
    connect(Soon, &QTimer::timeout, this, &PkgInsight::recompute);

    // ---- what the game sees ----
    SeesPage = new QWidget(Tabs);
    auto *SL = new QVBoxLayout(SeesPage);
    SL->setContentsMargins(4, 4, 4, 4);
    auto *SeesIntro = new QLabel("Every file the selected node gives the game when it runs. A later layer wins: what it covers is listed under it.", SeesPage);
    SeesIntro->setWordWrap(true);
    SeesFilter = new QLineEdit(SeesPage);
    SeesFilter->setPlaceholderText("Filter files");
    SeesFilter->setClearButtonEnabled(true);
    Sees = new QTreeWidget(SeesPage);
    Sees->setHeaderLabels({"File", "From"});
    Sees->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    SeesNotes = new QLabel(SeesPage);
    SeesNotes->setWordWrap(true);
    SL->addWidget(SeesIntro);
    SL->addWidget(SeesFilter);
    SL->addWidget(Sees, 1);
    SL->addWidget(SeesNotes);
    connect(SeesFilter, &QLineEdit::textChanged, this, [this](const QString &T) {
        for (int I = 0; I < Sees->topLevelItemCount(); ++I)
        {
            QTreeWidgetItem *Place = Sees->topLevelItem(I);
            int Shown = 0;
            for (int J = 0; J < Place->childCount(); ++J)
            {
                const bool Hit = T.isEmpty() || Place->child(J)->text(0).contains(T, Qt::CaseInsensitive);
                Place->child(J)->setHidden(!Hit);
                Shown += Hit;
            }
            Place->setHidden(Shown == 0);
        }
    });

    // ---- the fold, in order ----
    FoldPage = new QWidget(Tabs);
    auto *FL = new QVBoxLayout(FoldPage);
    FL->setContentsMargins(4, 4, 4, 4);
    auto *FoldIntro = new QLabel("The selected node's layers in the order they are applied, top to bottom - "
                                 "a contained node's layers where it is contained. Later wins.", FoldPage);
    FoldIntro->setWordWrap(true);
    Fold = new QTreeWidget(FoldPage);
    Fold->setHeaderLabels({"#", "Layer", "In node"});
    Fold->setRootIsDecorated(false);
    Fold->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    FL->addWidget(FoldIntro);
    FL->addWidget(Fold, 1);

    // ---- the shelf card ----
    CardPage = new QWidget(Tabs);
    auto *CL = new QVBoxLayout(CardPage);
    CL->setContentsMargins(8, 8, 8, 8);
    auto *CardIntro = new QLabel("How the selected version shows in the Library.", CardPage);
    Cover = new QLabel(CardPage);
    Cover->setAlignment(Qt::AlignCenter);
    Cover->setMinimumHeight(160);
    Cover->setStyleSheet("QLabel { background: #1b1d22; border-radius: 6px; color: #888; }");
    CardTitle = new QLabel(CardPage);
    CardTitle->setStyleSheet("QLabel { font-size: 15pt; font-weight: bold; }");
    CardTitle->setWordWrap(true);
    CardVersions = new QLabel(CardPage);
    CardVersions->setWordWrap(true);
    CardVersions->setTextFormat(Qt::RichText);
    CL->addWidget(CardIntro);
    CL->addWidget(Cover);
    CL->addWidget(CardTitle);
    CL->addWidget(CardVersions);
    CL->addStretch(1);

    Tabs->addTab(SeesPage, "Game files");
    Tabs->addTab(FoldPage, "Fold order");
    Tabs->addTab(CardPage, "Card");
    connect(Tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });
}

void PkgInsight::showNode(const QString &H) { Handle = H; refresh(); }
void PkgInsight::refresh() { Soon->start(250); }   // edits arrive in bursts; zips are read once they settle

void PkgInsight::recompute()
{
    const PkgDoc::Document &D = Model->doc();
    const std::string H = Handle.toStdString();
    const bool Have = !H.empty() && D.IndexOf(H) >= 0;
    const auto Labels = D.Labels();
    //Another package's node a fold contains is walked too, as the library holds it, by its own name.
    const PkgActions::OtherFn Other = [this](const std::string &C) {
        const Node *N = Model->libraryNode(C);
        if (!N) return PkgActions::OtherNode{};
        const auto X = Model->externalInfo(C);
        return PkgActions::OtherNode{&*N->Json, N->BundleDir, X.Label.empty() ? C.substr(0, 12) : X.Label + "  (" + X.Package + ")"};
    };
    auto Label = [&](const std::string &R) {
        const auto It = Labels.find(R);
        return It != Labels.end() ? It->second : D.IndexOf(R) < 0 ? Other(R).Label : std::string();
    };
    auto Find = [&](const std::string &R) -> const json * { const int I = D.IndexOf(R); return I >= 0 ? &D.Node(I) : Other(R).Node; };
    const auto *Tabs = qobject_cast<QTabWidget *>(parent());
    QWidget *Shown = Tabs ? Tabs->currentWidget() : nullptr;

    if (Shown == SeesPage)
    {
        Sees->clear();
        SeesNotes->clear();
        if (!Have || !Model->packageDir()) { SeesNotes->setText("Select a node on the canvas."); return; }
        std::vector<std::string> Notes;
        const auto Files = PkgActions::GameSees(D, H, Model->packageDir()->path().toStdString(), &Notes, Other);
        std::map<QString, QTreeWidgetItem *> Places;
        for (const auto &F : Files)
        {
            const QString P = QString::fromStdString(F.Path);
            const int Cut = P.indexOf('/');
            const QString Place = Cut < 0 ? P : P.left(Cut), Rest = Cut < 0 ? QString() : P.mid(Cut + 1);
            QTreeWidgetItem *&Top = Places[Place];
            if (!Top)
            {
                //The place in words, its anchor beside it.
                static const std::map<QString, QString> Words = {
                    {"%GameDir%", "the game's folder"}, {"%Documents%", "Documents"}, {"%AppData%", "AppData (Roaming)"},
                    {"%LocalAppData%", "AppData (Local)"}, {"%UserProfile%", "the user's folder"}, {"%ProgramData%", "ProgramData"},
                    {"%ProgramFiles%", "Program Files"}, {"%ProgramFiles32%", "Program Files (x86)"}, {"%Windows%", "the Windows folder"},
                    {"%SysDir32%", "System32"}, {"%SysDir64%", "the 64-bit system folder"}};
                const auto W = Words.find(Place);
                Top = new QTreeWidgetItem(Sees, {W == Words.end() ? Place : W->second + "  (" + Place + ")"});
                Top->setExpanded(true);
            }
            auto *It = new QTreeWidgetItem(Top, {Rest, QString::fromStdString(F.From)});
            for (const std::string &U : F.Under)
            {
                auto *Under = new QTreeWidgetItem(It, {"covers", QString::fromStdString(U)});
                QFont Ft = Under->font(1); Ft.setStrikeOut(true); Under->setFont(1, Ft);
                Under->setForeground(0, QColor(140, 140, 150)); Under->setForeground(1, QColor(140, 140, 150));
            }
        }
        for (auto &[K, Top] : Places) Top->setText(1, QString("%1 file(s)").arg(Top->childCount()));
        QStringList N;
        for (const std::string &X : Notes) N << QString::fromStdString(X);
        SeesNotes->setText(Files.empty() && Notes.empty() ? "No files yet - bring some in (a node's actions... > bring in files...)." : N.join("\n"));
        if (!SeesFilter->text().isEmpty()) emit SeesFilter->textChanged(SeesFilter->text());
    }
    else if (Shown == FoldPage)
    {
        Fold->clear();
        if (!Have) return;
        int N = 0;
        for (const PkgGraph::FoldStep &St : PkgGraph::FoldOrder(H, Find, Label))
        {
            //A node contained twice with different TAKEs (two occurrences) says which files each one takes.
            QString In = QString::fromStdString(St.Outside ? "another package" : St.NodeLabel);
            if (!St.Take.empty()) In += "  ·  takes " + QString::fromStdString(St.Take.size() > 60 ? St.Take.substr(0, 59) + "…" : St.Take);
            auto *It = new QTreeWidgetItem(Fold, {QString::number(++N), QString(St.Depth * 4, ' ') + QString::fromStdString(St.Type + "  " + St.Title), In});
            if (!St.Take.empty()) It->setToolTip(2, QString::fromStdString(St.Take));
            It->setForeground(1, TypeQColor(St.Type));
        }
        if (!N) new QTreeWidgetItem(Fold, {"", "no layers yet"});
    }
    else if (Shown == CardPage)
    {
        std::vector<std::string> All;
        for (int I = 0; I < D.Count(); ++I) All.push_back(D.Handle(I));
        std::string Root = Have ? H : std::string();
        for (int I = 0; I < D.Count() && Root.empty(); ++I) if (D.Node(I).is_object() && D.Node(I).contains("VARIANT")) Root = D.Handle(I);
        const PkgGraph::ShelfCard C = PkgGraph::CardFor(Root, All, Find);
        CardTitle->setText(C.Title.empty() ? QString("(no tile yet)") : QString::fromStdString(C.Title));
        QPixmap Px;
        if (!C.Cover.empty() && Model->packageDir()) Px.load(QDir(Model->packageDir()->path()).filePath(QString::fromStdString(C.Cover)));
        if (Px.isNull()) { Cover->setPixmap(QPixmap()); Cover->setText(C.Cover.empty() ? "no cover yet" : "cover not found"); }
        else Cover->setPixmap(Px.scaled(320, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        QStringList V;
        for (const auto &[Name, Rec] : C.Versions) V << QString::fromStdString(Name).toHtmlEscaped() + (Rec ? " <b>(recommended)</b>" : "");
        CardVersions->setText(V.isEmpty() ? "No version on the shelf yet - name one: a node's Properties > Variant."
                                          : "Versions: " + V.join(", "));
    }
}
