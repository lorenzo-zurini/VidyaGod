// Times the canvas on a graph the SHAPE of the biggest package in the library (2775 nodes, a 904-layer chain with
// heavy fan-in: the Minecraft package, once reported as "opens after half an hour, then completely unresponsive").
// Synthetic on purpose, so it runs on every machine. It separates the one-time open (graph + layout) from the
// per-frame cost, zoomed out (every node a box) and zoomed in (the few nodes on screen drawn in full).

#include "canvasharness.h"

#include <QtTest>
#include <QElapsedTimer>

using json = nlohmann::ordered_json;

class CanvasPerfTest : public QObject
{
    Q_OBJECT
    CanvasHarness H;

    //A chain of deltas, each containing the previous one and a spread of earlier ones.
    static std::vector<std::pair<std::string, json>> MinecraftShaped(int Chain, int Fan)
    {
        std::vector<std::pair<std::string, json>> Out;
        for (int I = 0; I < Chain; ++I)
        {
            json L = json::array();
            if (I > 0) L.push_back(json{{"NODE", "n" + std::to_string(I - 1)}});
            for (int K = 0; K < Fan; ++K)
            {
                const int Src = (I * 7 + K * 13) % std::max(1, Chain);
                if (Src < I - 1) L.push_back(json{{"NODE", "n" + std::to_string(Src)}});
            }
            L.push_back(json{{"DELTA", "d" + std::to_string(I) + ".vgdelta"}, {"TARGET", "FILES/client"}});
            Out.push_back({"n" + std::to_string(I), json{{"LABEL", "version " + std::to_string(I)}, {"LAYERS", L}}});
        }
        return Out;
    }

private slots:
    void initTestCase() { CanvasHarness::InitContext(); }
    void cleanupTestCase() { CanvasHarness::DestroyContext(); }

    void aMinecraftShapedPackageIsUsable()
    {
        H.open();
        QElapsedTimer T; T.start();
        H.Doc.Reset(MinecraftShaped(2775, 14));
        H.frame();                                         // graph + layout + frame the whole package
        const qint64 OpenMs = T.restart();
        qInfo() << "open (graph, layout, first frame):" << OpenMs << "ms  wires:" << H.Canvas->visibleWires();
        QVERIFY2(OpenMs < 4000, qPrintable(QString("opening took %1 ms").arg(OpenMs)));

        auto Time = [&](const char *What, int N) {
            T.restart();
            for (int I = 0; I < N; ++I) H.frame(ImVec2(400.0f + (float)(I % 7), 300.0f));
            const double Ms = (double)T.elapsed() / N;
            qInfo() << What << Ms << "ms/frame," << H.Canvas->visibleNodes() << "nodes drawn";
            return Ms;
        };
        //Everything in view: every node is a box.
        H.Canvas->frameAll();
        const double Fit = Time("whole package (boxes):", 20);
        //Zoomed in on a node: a handful drawn in full.
        H.Canvas->select("n1400", true);
        H.Canvas->setZoom(1.0f);
        H.frames(3);
        const double Near = Time("100% (full nodes):", 30);
        QVERIFY2(Fit < 45.0, qPrintable(QString("%1 ms per frame with the whole package in view").arg(Fit)));
        QVERIFY2(Near < 20.0, qPrintable(QString("%1 ms per frame at 100%").arg(Near)));
        H.close();
    }
};

QTEST_MAIN(CanvasPerfTest)
#include "test_canvasperf.moc"
