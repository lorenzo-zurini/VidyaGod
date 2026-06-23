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
        {"ROLE", "launchable"}, {"GROUP", "aoe2"}, {"LABEL", "HD Edition"}, {"RECOMMENDED", true},
        {"UID", "aoe2-hd"},
        {"PARENTS", json::array({"wine-ge", "aoe2-base"})},
        {"META", {{"TITLE", "Age of Empires II"}, {"DEVELOPER", "Ensemble Studios"},
                  {"PUBLISHER", "Microsoft"}, {"RELEASEDATE", "1999"}, {"SERIES", "Age of Empires"}}},
        {"EXEC", {{"CONTENTPATH", "AoK HD.exe"}, {"EXEARGS", ""}, {"WORKDIR", ""}}},
        {"LAYERS", json::array({
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "game.zip"}, {"TARGET", "drive_c/aoe2"}},
            json{{"TYPE", "RegEdit"}, {"REGPATH", "HKCU\\Software\\Microsoft\\AoE2"}, {"ARCHITECTURE", "32"},
                 {"KEYVALUES", {{"Resolution", "1920x1080"}, {"Windowed", "0"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "FPS_CAP"}, {"LABEL", "FPS cap"}, {"DEFAULT", "60"},
                 {"VARTYPE", "dword"}, {"DISPLAY", true}},
            json{{"TYPE", "PersistDir"}, {"PATH", "drive_c/users/steamuser/Saved Games/aoe2"}}})}});

    writeNode(bundle, "wine-ge", json{
        {"ROLE", "runner"},
        {"PLATFORM", {{"HOST", "linux64"}, {"GUEST", json::array({"win32", "win64"})}}},
        {"EXEC", {{"EXECUTABLE", "%RunnerMount%/proton"}, {"CONTENT_ROOT", "pfx/drive_c/%PackageUID%"},
                  {"PREFIX_GENERATE", true}, {"ARGS", json::array({"waitforexitandrun", "%Content%"})},
                  {"ENV", {{"PROTON_LOG", "%PROTON_LOG%"}}}}},
        {"LAYERS", json::array({ json{{"TYPE", "VFSZipLayer"}, {"PATH", "proton.zip"}} })}});

    const json cfg = json{{"Settings", {{"Repositories", json::array()}}}};

    // ---- the full editor on the bundle ----
    {
        PackageEditor editor(&cfg, nullptr, bundle);
        shot(&editor, 1500, 950, out + "/01_packageeditor_full.png");
    }

    // ---- individual sections (model bound to node 0 = the launchable) ----
    PackageEditorModel model(&cfg, nullptr);
    model.initPackage(bundle, nullptr);
    { NodeIdentitySection s(&model, 0); shot(&s, 560, 280, out + "/02_section_identity.png"); }
    { NodePlatformSection s(&model, 1); shot(&s, 560, 320, out + "/03_section_platform_runner.png"); }
    { NodeExecSection     s(&model, 0); shot(&s, 620, 480, out + "/04_section_exec.png"); }
    { NodeMetaSection     s(&model, 0); shot(&s, 620, 560, out + "/05_section_meta.png"); }
    { NodeLayersSection   s(&model, 0); shot(&s, 680, 760, out + "/06_section_layers.png"); }

    return 0;
}
