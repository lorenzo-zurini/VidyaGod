// `VidyaGod app …` (the rest), `library …`, `keys …`, `util …` — the app's own switches (networking, card size, the
// tray, start on login), adding and removing packages, the housekeeping it does on its own (healing references,
// reconciling what friends share), a Library card's Play and Edit, the names keys are bound by, and the small
// helpers everything else uses.
#include <filesystem>
#include <QTextDocument>
#include <QEventLoop>
#include "command.h"
#include "cmdutil.h"
#include "appmodel.h"
#include "friendsharing.h"
#include "autostart.h"
#include "commonutils.h"
#include "covercache.h"
#include "guiformat.h"
#include "keycapture.h"
#include "keynames.h"
#include "libraryview.h"
#include "mainwindow.h"
#include "procenv.h"
#include "traycontroller.h"
#include "variantpicker.h"

#include <QKeyEvent>
#include <algorithm>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

AppModel &Model(Command::Context &C)
{
    if (!C.Model) throw std::runtime_error("no app model");
    return *C.Model;
}

MainWindow &Win(Command::Context &C)
{
    auto *W = qobject_cast<MainWindow *>(C.Window);
    if (!W) throw std::runtime_error("no main window");
    return *W;
}

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("library", "a Library card's own buttons: Play (the launch window) and Edit (the package editor)");
    Command::RegisterGroup("keys", "the names keys, mouse buttons and pad controls are bound by");
    Command::RegisterGroup("util", "the small helpers: sizes in words, name patterns, natural sort, running a system tool");

    Command::Register({
        "app networking", "networking on or off (the node, friends, the Catalog) — or what it is",
        "Settings → Networking: on starts the node and the network tabs, off stops them.",
        { Opt("set", "on or off", Type::String, false, { "on", "off" }) }, { "app networking", "app networking --set off" },
        { "AppModel::networkingEnabled", "AppModel::setNetworkingEnabled", "AppModel::markNodeReady" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("set")) Model(C).setNetworkingEnabled(Command::Str(A, "set") == "on");
            return json{{"networking", Model(C).networkingEnabled()}};
        }, {} });

    Command::Register({
        "app card-size", "the Library and Catalog cards' width in pixels, or set it", "",
        { Opt("set", "the width", Type::Int) }, { "app card-size", "app card-size --set 220" },
        { "AppModel::cardPixelWidth", "AppModel::setCardPixelWidth", "CoverCache::setDisplaySize", "LibraryTab::onCardSizeChanged", "CatalogTab::onCardSizeChanged", "SyncSizeButtons",
          "LibraryView::layoutCards", "LibraryView::prescaleCovers" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("set")) Model(C).setCardPixelWidth(static_cast<int>(Command::Int(A, "set")));
            return json{{"width", Model(C).cardPixelWidth()}};
        }, {} });

    Command::Register({
        "app import", "add every package found under a folder to the Library (in place, as local packages)", "",
        { Pos("dir", "the folder") }, { "app import ~/Games/packages" }, { "AppModel::importPackagesFromDir", "FSOps::CheckPackageValid" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto [Added, Skipped] = Model(C).importPackagesFromDir(QString::fromStdString(Command::Str(A, "dir")));
            return json{{"added", Added}, {"skipped", Skipped}};
        }, {} });

    Command::Register({
        "app add-game", "add a game by its launchable's CID (fetched from the network into the Library)",
        "What pasting a game's CID does: its node DAG and content come in as a package of the Library.", { Pos("cid", "the launchable's CID") },
        { "app add-game bafkrei…" }, { "AppModel::addGameByCid" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            Model(C).addGameByCid(QString::fromStdString(Command::Str(A, "cid")));
            return json{{"started", true}, {"follow", "VidyaGod events wait --signal gameAdd"}};
        }, {} });

    Command::Register({
        "app import-runner", "install a runner (its build, through the download queue)",
        "The app's download pump fetches it (resume, progress, cancel, as a game's download): the app must be up on the "
        "data dir.", { Pos("runner", "the runner's node") },
        { "app import-runner geproton_10_34" }, { "AppModel::importRunner" }, Needs::App, true,
        [](Command::Context &C, const json &A) {
            Model(C).importRunner(QString::fromStdString(NodeOf(*Index(C), Command::Str(A, "runner"))->Key()));
            return json{{"started", true}, {"follow", "VidyaGod events follow --signal download"}};
        }, {} });

    Command::Register({
        "app remove-package", "remove a package from the Library (its managed files go)",
        "As Settings → Installed Packages' Remove: a package added from outside LIBRARY is forgotten, its folder untouched; "
        "a source's package loses its content files (its nodes and cover stay, back in the Catalog). A package of LIBRARY "
        "itself is not removed from here — its content can be shared with other packages and may have no source.",
        { Pos("package", "the package's UID or folder"), Flag("yes", "yes, remove it") }, { "app remove-package 123456 --yes" },
        { "AppModel::removePackage", "PackageCatalog::PackageDirsWithUid" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this removes the package's files: add --yes");
            const std::string Q = Command::Str(A, "package");
            std::filesystem::path Dir = Q;
            if (std::error_code Ec; !std::filesystem::is_directory(Dir, Ec))
            {
                const auto Dirs = PackageCatalog::PackageDirsWithUid(*Index(C), Q);
                if (Dirs.empty()) throw Command::BadArgs("no package has UID '" + Q + "' (VidyaGod packages installed lists them)");
                if (Dirs.size() > 1)
                {
                    std::string L;
                    for (const auto &D : Dirs) L += "\n  " + D.string();
                    throw Command::BadArgs("UID '" + Q + "' names " + std::to_string(Dirs.size()) + " packages; name the folder:" + L);
                }
                Dir = Dirs.front();
            }
            bool Async = false;
            if (const QString Why = Model(C).removePackage(QString::fromStdString(Dir.string()), &Async); !Why.isEmpty())
                throw std::runtime_error(Why.toStdString());
            //A dehydrate runs on a worker: here (no app to keep it going) the call waits for it, or the process would
            //end in its middle.
            if (Async && !C.Live)
            {
                QEventLoop Wait;
                QObject::connect(&Model(C), &AppModel::catalogChanged, &Wait, &QEventLoop::quit);
                Wait.exec();
                return json{{"removed", Dir.string()}};
            }
            return Async ? json{{"removing", Dir.string()}, {"follow", "VidyaGod events wait --signal catalogChanged"}} : json{{"removed", Dir.string()}};
        }, {} });

    Command::Register({
        "app save", "write the app's config to disk now", "", {}, { "app save" }, { "AppModel::save", "JSONOps::SaveConfig", "JSONOps::SaveJSON" }, Needs::Model, false,
        [](Command::Context &C, const json &) { Save(C); return json{{"saved", true}}; }, {} });

    Command::Register({
        "app housekeeping", "what the app does on its own, now: heal references, reconcile friends' shares, push node settings",
        "--heal re-adds orphaned content references (once per run); --reconcile re-reads what friends share and lands it; "
        "--reshare registers this machine's shares with the node again; --push hands the node the seed levels, the LAN "
        "roster and the presence deny list; --covers re-checks every missing cover (a sweep).",
        { Flag("heal", "heal orphaned references"), Flag("reconcile", "reconcile friends' shares"), Flag("reshare", "re-register shares"),
          Flag("push", "push node settings"), Flag("covers", "sweep missing covers") },
        { "app housekeeping --heal", "app housekeeping --reconcile --push" },
        { "AppModel::healOrphansIfAny", "FriendSharing::reconcileReceivedLibraries", "FriendSharing::enqueueReceivedShares", "FriendSharing::applyFriendLibrarySnapshot",
          "FriendSharing::onFriendBlockLanded", "FriendSharing::reRegisterShares", "AppModel::pushSeedLevels", "FriendSharing::pushLanRoster", "FriendSharing::pushPresenceDeny",
          "CoverCache::instance", "CoverCache::sweepNow", "CoverCache::prefetch", "CoverCache::missCount", "CoverCache::onTransferFinished", "AppModel::notifyCoversReady",
          "LibraryTab::refreshCovers", "CatalogTab::refreshCovers" , "AsyncWork::Run", "AsyncWork::Job::Live", "AsyncWork::Job::Post" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            AppModel &M = Model(C);
            json Out = json::object();
            if (Command::Bool(A, "heal", false)) { M.healOrphansIfAny(); Out["healing"] = true; }
            if (Command::Bool(A, "reconcile", false)) { M.friends().reconcileReceivedLibraries(); Out["reconciling"] = true; }
            if (Command::Bool(A, "reshare", false)) { M.friends().reRegisterShares(); Out["reshared"] = true; }
            if (Command::Bool(A, "push", false)) { M.pushSeedLevels(); M.friends().pushLanRoster(); M.friends().pushPresenceDeny(); Out["pushed"] = true; }
            if (Command::Bool(A, "covers", false)) { CoverCache::instance()->sweepNow(); Out["covers_missing"] = CoverCache::instance()->missCount(); }
            if (Out.empty()) throw Command::BadArgs("give --heal, --reconcile, --reshare, --push or --covers");
            return Out;
        }, {} });

    Command::Register({
        "app covers", "where a cover image is found (or fetched from), as a card draws it",
        "For a tile's COVER (--cover JSON) in a package folder: the file it names and its CID, and where the cover cache "
        "finds it now (\"\" while it is being fetched). --decode (in the running app) takes its image as a card does: "
        "\"decoding\" the first time, as the decode runs off the GUI thread, and its size once it has landed — the size a "
        "card keeps it at (the widest card's, 2:3), not the file's.",
        { WithSample(Opt("cover", "the COVER (JSON)", Type::Json, true), "{\"PATH\":\"cover.jpg\"} or \"cover.png\""), Opt("package", "the package folder"), Flag("decode", "take the decoded image") },
        { "app covers --cover '{\"PATH\":\"cover.jpg\",\"SOURCE\":{\"TYPE\":\"ipfs\",\"CID\":\"Qm…\"}}' --package 749",
          "app covers --cover '\"cover.png\"' --package 749 --decode" },
        { "CoverCache::Locate", "CoverCache::resolve", "CoverCache::setAssetsRoot", "CoverCache::setOnlineProbe", "CoverCache::onNetworkOnline",
          "CoverCache::take", "CoverCache::decodeCount" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            QString File, Cid;
            CoverCache::Locate(A["cover"], File, Cid);
            const QString Dir = A.contains("package") ? QString::fromStdString(PackageDir(C, Command::Str(A, "package")).string()) : QString();
            const QString Found = CoverCache::instance()->resolve(A["cover"], Dir);
            json Out{{"file", File.toStdString()}, {"cid", Cid.toStdString()}, {"found", Found.toStdString()}};
            if (Command::Bool(A, "decode", false))
            {
                if (!C.Window) throw std::runtime_error("--decode needs the running app (an image is made on its GUI thread)");
                if (Found.isEmpty()) throw std::runtime_error("the cover is not on disk yet");
                const QPixmap P = CoverCache::instance()->take(Found);
                Out["image"] = P.isNull() ? json("decoding") : json(std::to_string(P.width()) + "x" + std::to_string(P.height()));
                Out["decodes"] = CoverCache::instance()->decodeCount();
            }
            return Out;
        }, {} });

    Command::Register({
        "app window", "the main window: bring it forward (from the tray too), or hide it to the tray",
        "--show is what starting VidyaGod again does while it runs: the window comes back from the tray (or from behind "
        "others) instead of a second app. --hide puts it in the tray, as closing it does with Close to tray on.",
        { Flag("show", "bring the window forward"), Flag("hide", "hide it to the tray") }, { "app window --show", "app window" },
        { "MainWindow::restoreFromTray", "MainWindow::hideToTray" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            if (Command::Bool(A, "show", false) && Command::Bool(A, "hide", false)) throw Command::BadArgs("--show or --hide, not both");
            if (Command::Bool(A, "show", false)) Win(C).restoreFromTray();
            if (Command::Bool(A, "hide", false)) Win(C).hideToTray();
            return json{{"visible", Win(C).isVisible()}, {"minimized", Win(C).isMinimized()}};
        }, {} });

    Command::Register({
        "app tray", "the tray: whether there is one, and what closing, minimising and starting do", "",
        { Opt("hidden", "remember the window hidden (on) or shown (off)", Type::String, false, { "on", "off" }), Flag("notify", "show the 'still running' note") },
        { "app tray" },
        { "MainWindow::tray", "TrayController::available", "TrayController::closeToTray", "TrayController::minimizeToTray", "TrayController::startInTray",
          "TrayController::shouldStartHidden", "TrayController::rememberHidden", "TrayController::notifyHidden" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            TrayController *T = Win(C).tray();
            if (!T) throw std::runtime_error("no tray controller");
            if (A.contains("hidden")) T->rememberHidden(Command::Str(A, "hidden") == "on");
            if (Command::Bool(A, "notify", false)) T->notifyHidden();
            return json{{"available", T->available()}, {"close_to_tray", T->closeToTray()}, {"minimize_to_tray", T->minimizeToTray()},
                        {"start_in_tray", T->startInTray()}, {"starts_hidden", T->shouldStartHidden()}};
        }, {} });

    Command::Register({
        "app autostart", "start on login: whether it is set, or set it", "",
        { Opt("set", "on or off", Type::String, false, { "on", "off" }) }, { "app autostart", "app autostart --set on" },
        { "AutoStart::IsEnabled", "AutoStart::SetEnabled" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (A.contains("set"))
            {
                QString Err;
                if (!AutoStart::SetEnabled(Command::Str(A, "set") == "on", &Err)) throw std::runtime_error(Err.toStdString());
            }
            return json{{"autostart", AutoStart::IsEnabled()}};
        }, {} });

    Command::Register({
        "app startup", "show the main window as start-up does (or bring it up in the tray)", "",
        { Flag("tray", "start in the tray") }, { "app startup" }, { "MainWindow::startup", "MainWindow::model", "NodeService::start" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            Win(C).startup(Command::Bool(A, "tray", false));
            return json{{"visible", Win(C).isVisible()}, {"model", Win(C).model() == C.Model}};
        }, {} });

    Command::Register({
        "friends stop-receiving", "stop receiving a friend's libraries (their stubs go, installs stay)", "",
        { Pos("friend", "the friend's peer ID") }, { "friends stop-receiving 12D3KooW…" },
        { "FriendSharing::stopReceivingFromFriend", "FriendSharing::dropReceivedStubs" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Model(C).friends().stopReceivingFromFriend(QString::fromStdString(Command::Str(A, "friend")));
            return json{{"stopped", Command::Str(A, "friend")}};
        }, {} });

    // ---------------- library ----------------
    Command::Register({
        "library play", "a Library card's Play: the launch window for its game",
        "Opens the launch window (versions, options, instances) as clicking the card does; drive it with `ui` and press "
        "its Play with `ui click`.", { Pos("card", "the card (its key or title)") }, { "library play 749", "library play 'Age of Empires II - The Age of Kings'" },
        { "LibraryGameCard::play", "MainWindow::libraryCard", "LibraryTab::view", "PreLaunchWindow::packagePath", "PreLaunchWindow::ReloadAndRebuild",
          "VariantPicker::setEntries", "VariantPicker::currentId", "VariantPicker::checkedIds", "LaunchThread::run" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            LibraryGameCard *Card = Win(C).libraryCard(QString::fromStdString(Command::Str(A, "card")));
            if (!Card) throw Command::BadArgs("no single Library card '" + Command::Str(A, "card") + "'"
                                              + Win(C).cardMiss(false, QString::fromStdString(Command::Str(A, "card"))).toStdString());
            Card->play();
            return json{{"opened", Card->GameTitle.toStdString()}, {"follow", "VidyaGod ui dialogs"}};
        }, {} });

    Command::Register({
        "library details", "what a Library card's hover card says: series, year, size, last played, golden since",
        "The facts a card does not print on itself, as hovering it shows them.", { Pos("card", "the card (its key or title)") },
        { "library details 749" }, { "LibraryGameCard::HoverText", "LibraryGameCard::lastPlayed", "InstanceStore::LastRun", "PlayedWhen" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            LibraryGameCard *Card = Win(C).libraryCard(QString::fromStdString(Command::Str(A, "card")));
            if (!Card) throw Command::BadArgs("no single Library card '" + Command::Str(A, "card") + "'"
                                              + Win(C).cardMiss(false, QString::fromStdString(Command::Str(A, "card"))).toStdString());
            json Lines = json::array();   // the hover card's lines, as text
            for (const QString &L : Card->HoverText().split("<br>"))
            {
                QTextDocument Doc;
                Doc.setHtml(L);
                Lines.push_back(Doc.toPlainText().toStdString());
            }
            return json{{"title", Card->GameTitle.toStdString()}, {"last_played", Card->lastPlayed().toStdString()},
                        {"bytes", Card->Bytes}, {"golden", Card->Golden}, {"hover", Lines}};
        }, {} });

    Command::Register({
        "library edit", "a Library card's Edit: the package editor on its package", "", { Pos("card", "the card (its key or title)") },
        { "library edit 749" }, { "LibraryGameCard::edit", "LibraryGameCard::InitializeClassVariables" }, Needs::App, false,
        [](Command::Context &C, const json &A) {
            LibraryGameCard *Card = Win(C).libraryCard(QString::fromStdString(Command::Str(A, "card")));
            if (!Card) throw Command::BadArgs("no single Library card '" + Command::Str(A, "card") + "'"
                                              + Win(C).cardMiss(false, QString::fromStdString(Command::Str(A, "card"))).toStdString());
            Card->edit();
            return json{{"editing", Card->GameTitle.toStdString()}};
        }, {} });

    // ---------------- keys ----------------
    Command::Register({
        "keys names", "every name a key option can hold, and what a key, button, wheel or pad control is called",
        "--check NAME says whether a key option can hold it; --key SEQ, --button left|right|middle, --wheel ±N, "
        "--pad-button N, --pad-axis N±: the name a press of it gets.",
        { Opt("check", "a name"), Opt("key", "a key (Ctrl+A, F1, …)"), Opt("button", "a mouse button"), Opt("wheel", "a wheel step", Type::Int),
          Opt("pad-button", "an SDL pad button", Type::Int), Opt("pad-axis", "an SDL pad axis", Type::Int), Flag("negative", "(--pad-axis) its negative side") },
        { "keys names", "keys names --key F5", "keys names --key Ctrl+F5", "keys names --pad-button 0" },
        { "KeyNames::All", "KeyNames::IsValue", "KeyCapture::NameOf", "KeyCapture::NameOfWheel", "KeyCapture::NameOfPadButton", "KeyCapture::NameOfPadAxis" },
        Needs::Config, false,
        [](Command::Context &, const json &A) {
            json Out = json::object();
            //A key option holds one key: the rebinder is press-a-key, and a modifier held with it is not part of the name.
            const std::string Combo = "a key option holds one key, not a combination (the rebinder records the key a "
                                      "press of it gives, without Ctrl, Alt, Shift or Meta)";
            if (A.contains("check"))
            {
                const std::string N = Command::Str(A, "check");
                Out["valid"] = KeyNames::IsValue(N);
                if (!Out["valid"].get<bool>() && N.find('+') != std::string::npos && N.size() > 1) Out["why"] = Combo;
            }
            if (A.contains("key"))
            {
                const QKeySequence S(QString::fromStdString(Command::Str(A, "key")));
                if (S.isEmpty()) throw Command::BadArgs("not a key");
                const QKeyEvent E(QEvent::KeyPress, S[0].key(), S[0].keyboardModifiers());
                Out["key"] = KeyCapture::NameOf(E);
                if (S[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))
                    Out["note"] = Combo + ": " + Command::Str(A, "key") + " is bound as " + Out["key"].get<std::string>();
            }
            if (A.contains("button"))
            {
                const std::string B = Command::Str(A, "button");
                Out["button"] = KeyCapture::NameOf(B == "right" ? Qt::RightButton : B == "middle" ? Qt::MiddleButton : Qt::LeftButton);
            }
            if (A.contains("wheel")) Out["wheel"] = KeyCapture::NameOfWheel(static_cast<int>(Command::Int(A, "wheel")));
            if (A.contains("pad-button")) Out["pad_button"] = KeyCapture::NameOfPadButton(static_cast<int>(Command::Int(A, "pad-button")));
            if (A.contains("pad-axis")) Out["pad_axis"] = KeyCapture::NameOfPadAxis(static_cast<int>(Command::Int(A, "pad-axis")), Command::Bool(A, "negative", false));
            if (Out.empty()) Out["names"] = KeyNames::All();
            return Out;
        }, {} });

    // ---------------- util ----------------
    Command::Register({
        "util bytes", "a size in words, as the app shows sizes", "", { Pos("bytes", "the size", Type::Int) }, { "util bytes 3221225472" },
        { "HumanBytes", "HumanBytesQ" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            return json{{"text", HumanBytes(Command::Int(A, "bytes"))}, {"qt", HumanBytesQ(Command::Int(A, "bytes")).toStdString()}};
        }, {} });

    Command::Register({
        "util match", "whether a name matches a pattern (* and ?, case-insensitive), as KEEP and TAKE patterns do", "",
        { Pos("pattern", "the pattern"), Pos("name", "the name") }, { "util match '*.sav' SLOT1.SAV" }, { "WildcardMatch", "HasWildcard" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            return json{{"pattern", HasWildcard(Command::Str(A, "pattern"))}, {"matches", WildcardMatch(Command::Str(A, "pattern"), Command::Str(A, "name"))}};
        }, {} });

    Command::Register({
        "util sort", "names in natural order (file2 before file10), as lists in the app are sorted", "", { Pos("names", "the names (a,b,c)") },
        { "util sort 'Patch 10,Patch 2,Patch 1b'" }, { "NaturalLess" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            auto V = List(A, "names");
            std::sort(V.begin(), V.end(), [](const std::string &X, const std::string &Y) { return NaturalLess(QString::fromStdString(X), QString::fromStdString(Y)); });
            return json(V);
        }, {} });

    Command::Register({
        "util run", "run a system tool the way the app does (its environment, not the AppImage's)",
        "Its output goes to the log, as for the app's own runs (-v shows it); prints its exit code.",
        { Pos("program", "the program"), Opt("args", "its arguments (a,b)") }, { "util run -v ldconfig --args -p" },
        { "RunCommand", "SystemToolEnv" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            return json{{"exit_code", RunCommand(Command::Str(A, "program"), List(A, "args"), SystemToolEnv())}};
        }, {} });
}
