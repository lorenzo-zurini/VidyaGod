// Times the canvas on a graph the SHAPE of the biggest bundle in the library (2775 nodes, a 904-layer chain
// with heavy fan-in) — the graph reported as "opens after half an hour, then completely unresponsive".
// Measurement, not speculation: it separates the one-time open cost from the per-frame cost of drawing every
// node and every wire.
//
// The graph is SYNTHETIC on purpose. Keying it to a real bundle made this a QSKIP on every machine but one,
// so a green ctest said nothing about the regression it exists to catch.

#include "pkgcanvas.h"
#include "pkggraph.h"

#include "imgui.h"
#include "imnodes.h"

#include <QtTest>
#include <QElapsedTimer>

#include <fstream>

using json = nlohmann::ordered_json;

class CanvasPerfTest : public QObject
{
    Q_OBJECT
    json Doc, Layout;
    PkgCanvas *Canvas = nullptr;


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

    // A SYNTHETIC graph of the same shape as the real one, so this runs in CI and on any machine. Keying the
    // only guard against the 3753 ms regression to a bundle that exists on one developer's disk made ctest
    // green mean nothing: ~/.VidyaGod is documented as user-deletable, and CI has never had it.
    static json syntheticMinecraftShaped(int chainLen, int fanPerLink)
    {
        // The real bundle: 2775 nodes, 904 layers deep, widest layer 15, heavy fan-in per link.
        json Nodes = json::array();
        for (int I = 0; I < chainLen; ++I)
        {
            json N;
            N["LABEL"] = "c" + std::to_string(I);
            N["TYPE"]    = "Content";
            N["FORM"]    = "delta";
            N["PATH"]    = "d" + std::to_string(I) + ".vgdelta";
            json P = json::array();
            if (I > 0) P.push_back("c" + std::to_string(I - 1));
            for (int K = 0; K < fanPerLink; ++K)
            {
                const int Src = (I * 7 + K * 13) % std::max(1, chainLen);
                if (Src < I) P.push_back("c" + std::to_string(Src));
            }
            N["PARENTS"] = P;
            Nodes.push_back(std::move(N));
        }
        return json{{"NODES", std::move(Nodes)}};
    }

    void aMinecraftShapedGraphIsUsable()
    {
        QElapsedTimer T; T.start();
        Doc = syntheticMinecraftShaped(2775, 14);   // ~= the real 2775 nodes / 39k links
        const qint64 LoadMs = T.restart();
        const int N = (int)Doc["NODES"].size();
        qInfo() << "nodes:" << N << " build:" << LoadMs << "ms";

        Layout = json::object();
        Canvas = new PkgCanvas(&Doc, []{}, nullptr, &Layout);
        Canvas->initContexts();
        Canvas->setMiniMap(false);   // culling is unconditional now; this measures the canvas without the overview

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

        // The measured steady frame is ~0-15 ms with culling and ~450 ms without, so the threshold sits an
        // order of magnitude below the broken case and well above the working one. 100 ms would have let a
        // 6x regression through while still reading like a guard.
        QVERIFY2(SteadyMs < 50, qPrintable(QString("steady frame %1 ms — the canvas is regressing").arg(SteadyMs)));
    }
};

QTEST_MAIN(CanvasPerfTest)
#include "test_canvasperf.moc"
