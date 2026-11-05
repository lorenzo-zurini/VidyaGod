// `VidyaGod app …` (the rest), `library …`, `keys …`, `util …` — the app's own switches (networking, card size, the
// tray, start on login), adding and removing packages, the housekeeping it does on its own (healing references,
// reconciling what friends share), a Library card's Play and Edit, the names keys are bound by, and the small
// helpers everything else uses.
#include "command.h"
#include "cmdutil.h"
#include "appmodel.h"
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
        { "AppModel::cardPixelWidth", "AppModel::setCardPixelWidth", "LibraryTab::onCardSizeChanged", "CatalogTab::onCardSizeChanged",
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
        "app import-runner", "install a runner (its build, through the download queue)", "", { Pos("runner", "the runner's node") },
        { "app import-runner geproton_10_34" }, { "AppModel::importRunner" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            Model(C).importRunner(QString::fromStdString(NodeOf(*Index(C), Command::Str(A, "runner"))->Key()));
            return json{{"started", true}, {"follow", "VidyaGod events follow --signal download"}};
        }, {} });

    Command::Register({
        "app remove-package", "remove a package from the Library (its managed files go)", "",
        { Pos("uid", "the package's UID"), Flag("yes", "yes, remove it") }, { "app remove-package 123456 --yes" }, { "AppModel::removePackage" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this removes the package's files: add --yes");
            Model(C).removePackage(QString::fromStdString(Command::Str(A, "uid")));
            return json{{"removed", Command::Str(A, "uid")}};
        }, {} });

    Command::Register({
        "app save", "write the app's config to disk now", "", {}, { "app save" }, { "AppModel::save", "JSONOps::SaveJSON" }, Needs::Model, false,
        [](Command::Context &C, const json &) { Model(C).save(); return json{{"saved", true}}; }, {} });

    Command::Register({
        "app housekeeping", "what the app does on its own, now: heal references, reconcile friends' shares, push node settings",
        "--heal re-adds orphaned content references (once per run); --reconcile re-reads what friends share and lands it; "
        "--reshare registers this machine's shares with the node again; --push hands the node the seed levels, the LAN "
        "roster and the presence deny list; --covers re-checks every missing cover (a sweep).",
        { Flag("heal", "heal orphaned references"), Flag("reconcile", "reconcile friends' shares"), Flag("reshare", "re-register shares"),
          Flag("push", "push node settings"), Flag("covers", "sweep missing covers") },
        { "app housekeeping --heal", "app housekeeping --reconcile --push" },
        { "AppModel::healOrphansIfAny", "AppModel::reconcileReceivedLibraries", "AppModel::enqueueReceivedShares", "AppModel::applyFriendLibrarySnapshot",
          "AppModel::onFriendBlockLanded", "AppModel::reRegisterShares", "AppModel::pushSeedLevels", "AppModel::pushLanRoster", "AppModel::pushPresenceDeny",
          "CoverCache::instance", "CoverCache::sweepNow", "CoverCache::missCount", "CoverCache::onTransferFinished", "AppModel::notifyCoversReady",
          "LibraryTab::refreshCovers", "CatalogTab::refreshCovers" , "AsyncWork::Run" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            AppModel &M = Model(C);
            json Out = json::object();
            if (Command::Bool(A, "heal", false)) { M.healOrphansIfAny(); Out["healing"] = true; }
            if (Command::Bool(A, "reconcile", false)) { M.reconcileReceivedLibraries(); Out["reconciling"] = true; }
            if (Command::Bool(A, "reshare", false)) { M.reRegisterShares(); Out["reshared"] = true; }
            if (Command::Bool(A, "push", false)) { M.pushSeedLevels(); M.pushLanRoster(); M.pushPresenceDeny(); Out["pushed"] = true; }
            if (Command::Bool(A, "covers", false)) { CoverCache::instance()->sweepNow(); Out["covers_missing"] = CoverCache::instance()->missCount(); }
            if (Out.empty()) throw Command::BadArgs("give --heal, --reconcile, --reshare, --push or --covers");
            return Out;
        }, {} });

    Command::Register({
        "app covers", "where a cover image is found (or fetched from), as a card draws it",
        "For a tile's COVER (--cover JSON) in a package folder: the file it names and its CID, and where the cover cache "
        "finds it now (\"\" while it is being fetched).", { Opt("cover", "the COVER (JSON)", Type::Json, true), Opt("package", "the package folder") },
        { "app covers --cover '{\"FILE\":\"cover.jpg\",\"SOURCE\":\"Qm…\"}' --package 749" },
        { "CoverCache::Locate", "CoverCache::resolve", "CoverCache::setAssetsRoot", "CoverCache::setOnlineProbe", "CoverCache::onNetworkOnline" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            QString File, Cid;
            CoverCache::Locate(A["cover"], File, Cid);
            const QString Dir = A.contains("package") ? QString::fromStdString(PackageDir(C, Command::Str(A, "package")).string()) : QString();
            return json{{"file", File.toStdString()}, {"cid", Cid.toStdString()}, {"found", CoverCache::instance()->resolve(A["cover"], Dir).toStdString()}};
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
        { Flag("tray", "start in the tray") }, { "app startup" }, { "MainWindow::startup", "MainWindow::model" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            Win(C).startup(Command::Bool(A, "tray", false));
            return json{{"visible", Win(C).isVisible()}, {"model", Win(C).model() == C.Model}};
        }, {} });

    Command::Register({
        "friends stop-receiving", "stop receiving a friend's libraries (their stubs go, installs stay)", "",
        { Pos("friend", "the friend's peer ID") }, { "friends stop-receiving 12D3KooW…" },
        { "AppModel::stopReceivingFromFriend", "AppModel::dropReceivedStubs" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Model(C).stopReceivingFromFriend(QString::fromStdString(Command::Str(A, "friend")));
            return json{{"stopped", Command::Str(A, "friend")}};
        }, {} });

    // ---------------- library ----------------
    Command::Register({
        "library play", "a Library card's Play: the launch window for its game",
        "Opens the launch window (versions, options, instances) as clicking the card does; drive it with `ui` and press "
        "its Play with `ui click`.", { Pos("card", "the card (its key or title)") }, { "library play 'Age of Empires II'" },
        { "LibraryGameCard::play", "MainWindow::libraryCard", "LibraryTab::view", "PreLaunchWindow::packagePath", "PreLaunchWindow::ReloadAndRebuild",
          "VariantPicker::setEntries", "VariantPicker::currentId", "VariantPicker::checkedIds", "LaunchThread::run" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            LibraryGameCard *Card = Win(C).libraryCard(QString::fromStdString(Command::Str(A, "card")));
            if (!Card) throw Command::BadArgs("no single Library card '" + Command::Str(A, "card") + "'");
            Card->play();
            return json{{"opened", Card->GameTitle.toStdString()}, {"follow", "VidyaGod ui dialogs"}};
        }, {} });

    Command::Register({
        "library edit", "a Library card's Edit: the package editor on its package", "", { Pos("card", "the card (its key or title)") },
        { "library edit 'Age of Empires II'" }, { "LibraryGameCard::edit", "LibraryGameCard::InitializeClassVariables" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            LibraryGameCard *Card = Win(C).libraryCard(QString::fromStdString(Command::Str(A, "card")));
            if (!Card) throw Command::BadArgs("no single Library card '" + Command::Str(A, "card") + "'");
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
        { "keys names", "keys names --key Ctrl+F5", "keys names --pad-button 0" },
        { "KeyNames::All", "KeyNames::IsValue", "KeyCapture::NameOf", "KeyCapture::NameOfWheel", "KeyCapture::NameOfPadButton", "KeyCapture::NameOfPadAxis" },
        Needs::Config, false,
        [](Command::Context &, const json &A) {
            json Out = json::object();
            if (A.contains("check")) Out["valid"] = KeyNames::IsValue(Command::Str(A, "check"));
            if (A.contains("key"))
            {
                const QKeySequence S(QString::fromStdString(Command::Str(A, "key")));
                if (S.isEmpty()) throw Command::BadArgs("not a key");
                const QKeyEvent E(QEvent::KeyPress, S[0].key(), S[0].keyboardModifiers());
                Out["key"] = KeyCapture::NameOf(E);
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
