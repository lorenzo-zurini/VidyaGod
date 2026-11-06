#include "pathspage.h"
#include "sessions.h"
#include "apppaths.h"          // AppPaths::DataRoot — default TEMP/LIBRARY display roots
#include "appmodel.h"          // AppModel — config + appDataDir + save
#include "asyncwork.h"
#include "instancestore.h"     // Root — USERDATA
#include "commonutils.h"       // HumanBytes
#include "launchresolver.h"    // SessionRoot
#include "packagecatalog.h"    // the roots; ClearIndexCache
#include "parallel.h"
#include "vfsmount.h"          // SweepDeadSessions

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QDesktopServices>
#include <QGridLayout>
#include <QPointer>
#include <QUrl>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

PathsPage::PathsPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    // (Temporary / runtime root is no longer configurable: ephemeral TEMP always lives in the system temp
    //  (/tmp, %TEMP%) — pure throwaway mount points + unmapped-writes scratch. Durable state is what a package's
    //  DeclarePersist targets map into the instance dir; if something writes a lot, map it, don't relocate TEMP.)

    // Library folder — Settings.Paths.LibraryRoot (empty = default). Imported packages are hydrated here,
    // one subfolder per repo.
    const QString defaultLibRoot = QDir::cleanPath(QString::fromStdString((AppPaths::DataRoot() / "LIBRARY").string()));
    QWidget * libRow = new QWidget(this);
    QHBoxLayout * ll = new QHBoxLayout(libRow); ll->setContentsMargins(0,0,0,0);
    libRow->setLayout(ll);
    QLineEdit * libEdit = new QLineEdit(libRow);
    libEdit->setPlaceholderText(defaultLibRoot + "  (default)");
    {
        auto & S = (*Model.config())["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("LibraryRoot") && S["Paths"]["LibraryRoot"].is_string())
            libEdit->setText(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"])));
    }
    auto writeLibRoot = [this,libEdit]{
        auto & S = (*Model.config())["Settings"];
        QString t = libEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("LibraryRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["LibraryRoot"] = t.toStdString();
        }
        Model.save();
    };
    connect(libEdit, &QLineEdit::editingFinished, this, writeLibRoot);
    ll->addWidget(libEdit, 1);
    QPushButton * libBrowse = new QPushButton("Browse…", libRow);
    connect(libBrowse, &QPushButton::clicked, this, [this,libEdit,writeLibRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select library folder");
        if (!d.isEmpty()) { libEdit->setText(d); writeLibRoot(); }
    });
    ll->addWidget(libBrowse);
    form->addRow("Library folder:", libRow);

    // AppData dir (read-only, informational — determined at launch, not settable here). Disabled so it's visibly
    // non-editable, but kept text-selectable so the path can still be copied.
    QLineEdit * appDataLbl = new QLineEdit(Model.appDataDir()->path(), this);
    appDataLbl->setReadOnly(true);
    appDataLbl->setStyleSheet("QLineEdit{color:palette(disabled-text);background:palette(window);}");
    form->addRow("App data directory:", appDataLbl);

    QLabel * note = new QLabel("Path changes apply to future launches.", this);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);

    buildSpace(pl);
    buildCleanUp(pl);
    pl->addStretch(1);
}

namespace {
std::uintmax_t FolderBytes(const std::filesystem::path & Dir)
{
    std::uintmax_t N = 0;
    std::error_code Ec;
    for (auto It = std::filesystem::recursive_directory_iterator(Dir, std::filesystem::directory_options::skip_permission_denied, Ec);
         !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
        if (std::error_code E2; It->is_regular_file(E2) && !It->is_symlink(E2)) N += It->file_size(E2);
    return N;
}
QLabel * Heading(const QString & T, QWidget * Parent)
{
    auto * L = new QLabel(T, Parent);
    L->setStyleSheet("font-weight:600; margin-top:14px;");
    return L;
}
} // namespace

// What each part of the data dir takes on disk, and the space left on the library's drive: counted off the GUI thread
// (the library is hundreds of GB of files), filled in when it lands.
void PathsPage::buildSpace(QVBoxLayout * pl)
{
    pl->addWidget(Heading("Space used", this));
    auto * Grid = new QGridLayout();
    Grid->setHorizontalSpacing(16);
    pl->addLayout(Grid);
    const auto & Cfg = *Model.config();
    const std::filesystem::path Data = AppPaths::DataRoot();
    const std::vector<std::pair<QString, std::filesystem::path>> Parts = {
        { "Library (installed games)", PackageCatalog::LibraryRootDir(Cfg) },
        { "Received catalog (friends' packages)", PackageCatalog::CatalogRootDir(Cfg) },
        { "Saves and settings (USERDATA)", InstanceStore::Root(Cfg) },
        { "IPFS store", Data / "IPFS" },
        { "Covers", PackageCatalog::AssetsRootDir(Cfg) },
        { "Caches", Data / "CACHE" },
        { "Game sessions (running or left over)", Sessions::Root() },
    };
    auto Sizes = std::make_shared<std::vector<std::uintmax_t>>(Parts.size(), 0);
    auto Free = std::make_shared<std::filesystem::space_info>();
    std::vector<QLabel *> Labels;
    int Row = 0;
    for (const auto & [Name, Path] : Parts)
    {
        Grid->addWidget(new QLabel(Name, this), Row, 0);
        auto * Size = new QLabel("…", this);
        Size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        Grid->addWidget(Size, Row, 1);
        auto * Where = new QLabel(QString::fromStdString(Path.string()), this);
        Where->setStyleSheet("color:#8f98a0; font-size:9pt;");
        Where->setTextInteractionFlags(Qt::TextSelectableByMouse);
        Grid->addWidget(Where, Row, 2);
        auto * Open = new QPushButton("Open", this);
        Open->setAccessibleName("Open " + Name);
        const QString P = QString::fromStdString(Path.string());
        connect(Open, &QPushButton::clicked, this, [P]{ QDesktopServices::openUrl(QUrl::fromLocalFile(P)); });
        Grid->addWidget(Open, Row, 3);
        Labels.push_back(Size);
        ++Row;
    }
    Grid->setColumnStretch(2, 1);
    auto * FreeLabel = new QLabel(this);
    FreeLabel->setStyleSheet("color:#8f98a0;");
    FreeLabel->setAccessibleName("Free space");
    pl->addWidget(FreeLabel);
    std::vector<std::filesystem::path> Paths;
    for (const auto & P : Parts) Paths.push_back(P.second);
    const std::filesystem::path Lib = PackageCatalog::LibraryRootDir(Cfg);
    QPointer<QLabel> Fl(FreeLabel);
    std::vector<QPointer<QLabel>> Ls(Labels.begin(), Labels.end());
    AsyncWork::Run(this, [Paths, Sizes, Free, Lib]{
            //Each part split into the folders two levels down (LIBRARY/<library>/<package>), walked on every core: one
            //thread walking the library alone took minutes.
            struct Piece { std::size_t Part; std::filesystem::path Dir; };
            std::vector<Piece> Pieces;
            for (std::size_t I = 0; I < Paths.size(); ++I)
            {
                std::error_code Ec;
                for (const auto & A : std::filesystem::directory_iterator(Paths[I], Ec))
                {
                    std::error_code E2;
                    if (A.is_symlink(E2)) continue;
                    if (A.is_regular_file(E2)) { (*Sizes)[I] += A.file_size(E2); continue; }
                    if (!A.is_directory(E2)) continue;
                    for (const auto & B : std::filesystem::directory_iterator(A.path(), E2))
                    {
                        std::error_code E3;
                        if (B.is_symlink(E3)) continue;
                        if (B.is_regular_file(E3)) (*Sizes)[I] += B.file_size(E3);
                        else if (B.is_directory(E3)) Pieces.push_back({ I, B.path() });
                    }
                }
            }
            std::vector<std::uintmax_t> Bytes(Pieces.size(), 0);
            Parallel::ForWith<Parallel::NoState>(Pieces.size(), [&](Parallel::NoState &, std::size_t K) { Bytes[K] = FolderBytes(Pieces[K].Dir); });
            for (std::size_t K = 0; K < Pieces.size(); ++K) (*Sizes)[Pieces[K].Part] += Bytes[K];
            std::error_code Ec;
            *Free = std::filesystem::space(Lib, Ec);
        },
        [Ls, Fl, Sizes, Free]{
            for (size_t I = 0; I < Ls.size(); ++I) if (Ls[I]) Ls[I]->setText(QString::fromStdString(HumanBytes(static_cast<long long>((*Sizes)[I]))));
            if (Fl && Free->capacity > 0)
                Fl->setText(QString("%1 free of %2 on the library's drive").arg(QString::fromStdString(HumanBytes(static_cast<long long>(Free->available))),
                                                                               QString::fromStdString(HumanBytes(static_cast<long long>(Free->capacity)))));
        });
}

// What can go without losing anything: sessions whose game is gone (what start-up clears too) and the index cache
// (made again on the next start). A package's unused files are the editor's to show — it knows what its nodes use.
void PathsPage::buildCleanUp(QVBoxLayout * pl)
{
    pl->addWidget(Heading("Clean up", this));
    const auto Action = [this, pl](const QString & Button, const QString & What, std::function<QString()> Do) {
        auto * Row = new QHBoxLayout();
        auto * B = new QPushButton(Button, this);
        auto * L = new QLabel(What, this);
        L->setWordWrap(true);
        L->setStyleSheet("color:#8f98a0;");
        Row->addWidget(B);
        Row->addWidget(L, 1);
        pl->addLayout(Row);
        connect(B, &QPushButton::clicked, this, [B, L, Do]{
            B->setEnabled(false);
            auto Said = std::make_shared<QString>();
            QPointer<QPushButton> Pb(B);
            QPointer<QLabel> Pl(L);
            AsyncWork::Run(B, [Do, Said]{ *Said = Do(); }, [Pb, Pl, Said]{ if (Pb) Pb->setEnabled(true); if (Pl) Pl->setText(*Said); });
        });
    };
    Action("Clear left-over sessions", "Game sessions whose game is no longer running (a crash, a power cut) — what VidyaGod "
                                       "clears at start-up anyway. A running game's session is never touched.",
           []{
               const int N = VfsMount::SweepDeadSessions(Sessions::Root());
               return N == 0 ? QString("Nothing was left over.") : QString("Cleared %1 left-over session%2.").arg(N).arg(N == 1 ? "" : "s");
           });
    Action("Clear the index cache", "The library's index, kept so VidyaGod starts fast — made again on the next start.",
           []{ return PackageCatalog::ClearIndexCache() ? QString("Cleared; the next start indexes the library afresh.") : QString("There was no cache."); });
    auto * Hint = new QLabel("A package's files that none of its nodes use: open it in the Package Editor and validate it.", this);
    Hint->setWordWrap(true);
    Hint->setStyleSheet("color:#8f98a0; font-size:9pt; margin-top:6px;");
    pl->addWidget(Hint);
}
