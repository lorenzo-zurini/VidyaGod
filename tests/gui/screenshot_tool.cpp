// Offscreen screenshot tool: builds the real PackageEditor + section widgets against a synthetic bundle and saves
// PNGs via QWidget::grab() (raster render — no display, WM, or GL needed). Run headless with QT_QPA_PLATFORM=offscreen.
// Output dir is argv[1] (default /tmp/vg_shots). Lets a human (or me) eyeball that the de-godded editor renders.

#include <QApplication>
#include <QDir>
#include <QFile>

#include <nlohmann/json.hpp>

#include "packageeditor.h"
#include "packageeditormodel.h"
#include "nodesections.h"
#include "jsonoperations.h"
#include "mainwindow.h"

#include <QTabWidget>
#include <QRegularExpression>

using json = nlohmann::ordered_json;

static void writeNode(const QString & dir, const QString & id, const json & node)
{
    json n = node; n["NODE_ID"] = id.toStdString();
    QFile f(dir + "/" + id + ".json");
    JSONOps::SaveJSON(&n, &f);
}

static void shot(QWidget * w, int width, int height, const QString & path)
{
    w->resize(width, height);
    w->show();
    QApplication::processEvents();
    QApplication::processEvents();
    w->grab().save(path);
    qInfo("saved %s", qUtf8Printable(path));
    // Neutralize signals before the caller's scope destroys w: a focused QLineEdit would otherwise fire
    // editingFinished during ~QWidget and invoke a slot on the half-destroyed section.
    w->blockSignals(true);
    for (QObject * c : w->findChildren<QObject *>()) c->blockSignals(true);
}

int main(int argc, char ** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp/vg_shots");
    QDir().mkpath(out);

    // ---- a synthetic, content-rich bundle so the sections render populated ----
    const QString bundle = "/tmp/vg_shot_bundle";
    QDir(bundle).removeRecursively(); QDir().mkpath(bundle);

    writeNode(bundle, "aoe2", json{
        {"PARENTS", json::array({"wine-ge", "aoe2-base"})},
        {"LAYERS", json::array({
            json{{"TYPE", "DeclareLibraryItem"}, {"UID", "aoe2-hd"}, {"TITLE", "Age of Empires II"},
                 {"DEVELOPER", "Ensemble Studios"}, {"PUBLISHER", "Microsoft"}, {"RELEASEDATE", "1999"},
                 {"SERIES", "Age of Empires"}},
            json{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "AoK HD.exe"},
                 {"EXEARGS", ""}, {"WORKDIR", ""}, {"LABEL", "HD Edition"}, {"RECOMMENDED", true}},
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "game.zip"}, {"TARGET", "drive_c/aoe2"}},
            json{{"TYPE", "RegEdit"}, {"REGPATH", "HKCU\\Software\\Microsoft\\AoE2"}, {"ARCHITECTURE", "32"},
                 {"KEYVALUES", {{"Resolution", "1920x1080"}, {"Windowed", "0"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "FPS_CAP"}, {"LABEL", "FPS cap"}, {"DEFAULT", "60"},
                 {"VARTYPE", "dword"}, {"DISPLAY", true}},
            json{{"TYPE", "Persist"}, {"KEEP", "drive_c/users/steamuser/Saved Games/aoe2"}}})}});

    writeNode(bundle, "wine-ge", json{
        {"LAYERS", json::array({
            json{{"TYPE", "DeclareRunner"}, {"HOST", "linux64"}, {"GUEST", json::array({"win32", "win64"})},
                 {"EXECUTABLE", "%RunnerMount%/proton"}, {"CONTENT_ROOT", "pfx/drive_c/%PackageUID%"},
                 {"PREFIX_GENERATE", true}, {"ARGS", json::array({"waitforexitandrun", "%Content%"})},
                 {"ENV", {{"PROTON_LOG", "%PROTON_LOG%"}}}},
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "proton.zip"}} })}});

    const json cfg = json{{"Settings", {{"Repositories", json::array()}}}};

    // ---- the full editor on the bundle ----
    {
        PackageEditor editor(&cfg, nullptr, bundle);
        shot(&editor, 1500, 950, out + "/01_packageeditor_full.png");
    }

    // ---- individual sections (node 0 = the launchable/tile, node 1 = the runner) ----
    // Identity is now a derived badge; all the Declare*/content/edit editors live in the LAYERS section.
    PackageEditorModel model(&cfg, nullptr);
    model.initPackage(bundle, nullptr);
    { NodeIdentitySection  s(&model, 0); shot(&s, 560, 280, out + "/02_section_identity.png"); }
    { NodeParentsSection   s(&model, 0); shot(&s, 560, 320, out + "/03_section_parents.png"); }
    { NodeSelectionSection s(&model, 0); shot(&s, 560, 320, out + "/04_section_selection.png"); }
    // node 0's LAYERS: DeclareLibraryItem (cover drop) + DeclareExec + VFS/RegEdit/CustomVar/Persist
    { NodeLayersSection    s(&model, 0); shot(&s, 700, 900, out + "/05_section_layers_game.png"); }
    // node 1's LAYERS: DeclareRunner + the runner's VFS build layer
    { NodeLayersSection    s(&model, 1); shot(&s, 700, 520, out + "/06_section_layers_runner.png"); }

    // ---- the real MainWindow against the live ~/.VidyaGod (read-only render; IPFS node is NOT started, so the
    // IPFS tab shows "unavailable" — everything else renders with the real library/catalog/settings) ----
    const QString dataDir = QDir::homePath() + "/.VidyaGod";
    if (QFile::exists(dataDir + "/GlobalConfig.JSON"))
    {
        static nlohmann::ordered_json gcfg;
        QFile cf(dataDir + "/GlobalConfig.JSON");
        JSONOps::LoadJSON(&cf, &gcfg);   // true == failure; gcfg stays {} then, still fine to render
        static QDir appDir(dataDir);
        MainWindow * mw = new MainWindow(&gcfg, &appDir);
        mw->resize(1500, 950);
        mw->show();
        QApplication::processEvents(); QApplication::processEvents();
        mw->grab().save(out + "/10_mainwindow.png");
        if (QTabWidget * tabs = mw->findChild<QTabWidget *>())
        {
            for (int i = 0; i < tabs->count(); ++i)
            {
                tabs->setCurrentIndex(i);
                QApplication::processEvents(); QApplication::processEvents();
                QString name = tabs->tabText(i).remove(QRegularExpression("[^A-Za-z0-9]"));
                mw->grab().save(out + QString("/1%1_tab_%2.png").arg(i + 1).arg(name));
                qInfo("saved tab %s", qUtf8Printable(name));
            }
        }
        // leak mw on purpose: tearing the real MainWindow down here can touch IPFS/threads; the process exits next.
    }

    return 0;
}
