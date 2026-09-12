// Times the canvas on the REAL Minecraft bundle (2775 nodes / 39k links) — the graph the user reports as
// "opens after half an hour, then completely unresponsive". Measurement, not speculation: it separates the
// one-time open cost (graph build) from the per-frame cost (drawing every node and every wire, every frame).
//
// Skipped when the bundle is not present, so it never fails on a machine without that library.

#include "pkgcanvas.h"
#include "pkggraph.h"

#include "imgui.h"
#include "imnodes.h"

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>

#include <fstream>

using json = nlohmann::ordered_json;

class CanvasPerfTest : public QObject
{
    Q_OBJECT
    json Doc, Layout;
    PkgCanvas *Canvas = nullptr;

    static QString bundlePath()
    {
        return QDir::homePath() + "/.VidyaGod/LIBRARY/VidyaGod/[320][v1.0] Minecraft";
    }

private slots:
    void initTestCase()
    {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char *px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        QVERIFY(io.Fonts->IsBuilt());
    }
    void cleanupTestCase() { ImGui::DestroyContext(); }

    void theBiggestGraphInTheLibraryIsUsable()
    {
        const QString Dir = bundlePath();
        if (!QDir(Dir).exists()) QSKIP("Minecraft bundle not present on this machine");

        QElapsedTimer T; T.start();
        Doc = json{{"NODES", json::array()}};
        const QStringList Files = QDir(Dir).entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
        for (const QString &F : Files)
        {
            std::ifstream In((Dir + "/" + F).toStdString());
            json J;
            try { In >> J; } catch (...) { continue; }
            if (J.is_object() && J.contains("NODE_ID")) Doc["NODES"].push_back(std::move(J));
            else if (J.is_array()) for (auto &N : J) if (N.is_object() && N.contains("NODE_ID")) Doc["NODES"].push_back(N);
        }
        const qint64 LoadMs = T.restart();
        const int N = (int)Doc["NODES"].size();
        qInfo() << "nodes:" << N << " load:" << LoadMs << "ms";

        Layout = json::object();
        Canvas = new PkgCanvas(&Doc, []{}, nullptr, &Layout);
        Canvas->initContexts();

        const PkgGraph::Graph G = PkgGraph::Build(Doc["NODES"], &Layout);
        qInfo() << "links:" << (int)G.Links.size() << " externals:" << (int)G.Externals.size()
                << " build+layout:" << T.restart() << "ms";

        auto Frame = [&] {
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1400, 900);
            io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(400, 300);
            ImGui::NewFrame();
            Canvas->frame();
            ImGui::Render();
        };

        T.restart();
        Frame();
        const qint64 FirstMs = T.restart();
        for (int I = 0; I < 5; ++I) Frame();
        const qint64 SteadyMs = T.elapsed() / 5;
        qInfo() << "first frame:" << FirstMs << "ms   steady frame:" << SteadyMs << "ms";

        Canvas->shutdownContexts();
        delete Canvas; Canvas = nullptr;

        // 100 ms/frame is 10 fps — already bad, and well short of the multi-minute stall being chased. These
        // are the numbers that say whether a fix worked, so they are asserted, not just printed.
        QVERIFY2(SteadyMs < 100, qPrintable(QString("steady frame %1 ms — the canvas is unusable").arg(SteadyMs)));
    }
};

QTEST_MAIN(CanvasPerfTest)
#include "test_canvasperf.moc"
