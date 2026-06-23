#include "nodegraphview.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

// ═════════════════════════════════════════════════════════════════════════════
// NodeGraphView — clickable left→right DAG of the bundle's nodes (launchable on the left, parents to the right).
// ═════════════════════════════════════════════════════════════════════════════
// Vertical layout (fits a left sidebar): depth grows DOWN (Col → y), branches spread RIGHT (Row → x).
namespace { constexpr int ChipW = 138, ChipH = 28, GapX = 16, GapY = 30, MargX = 12, MargY = 10; }

NodeGraphView::NodeGraphView(QWidget * parent) : QWidget(parent)
{
    setMouseTracking(true);                         // hover → pointing-hand cursor over chips
    setAutoFillBackground(true);
    QPalette Pal = palette(); Pal.setColor(QPalette::Window, QColor("#15181c")); setPalette(Pal);
}

QSize NodeGraphView::sizeHint() const { return QSize(std::max(ContentW, 1), std::max(ContentH, 1)); }

void NodeGraphView::SetCurrent(const std::string & NodeId) { Current = NodeId; update(); }

void NodeGraphView::SetGraph(const nlohmann::ordered_json & NodesArray)
{
    Nodes.clear(); Edges.clear();
    if (NodesArray.is_array())
    {
        std::map<std::string, int> Ix;
        for (const auto & N : NodesArray)
        {
            if (!N.is_object()) continue;
            GNode G;
            G.Id    = N.value("NODE_ID", std::string());
            G.Role  = N.value("ROLE", std::string("content"));
            G.Label = QString::fromStdString(G.Id);   // match the tab labels (which key off NODE_ID) for click-to-tab
            if (G.Id.empty()) continue;
            Ix[G.Id] = (int)Nodes.size();
            Nodes.push_back(std::move(G));
        }
        // Edges: a node → each of its PARENTS that lives in THIS bundle.
        for (const auto & N : NodesArray)
        {
            if (!N.is_object()) continue;
            auto It = Ix.find(N.value("NODE_ID", std::string()));
            if (It == Ix.end() || !N.contains("PARENTS") || !N["PARENTS"].is_array()) continue;
            for (const auto & P : N["PARENTS"])
            {
                auto Pit = Ix.find(P.is_string() ? std::string(P) : std::string());
                if (Pit != Ix.end()) Edges.emplace_back(It->second, Pit->second);
            }
        }
    }
    Relayout();
}

void NodeGraphView::Relayout()
{
    const int N = (int)Nodes.size();
    // Column = longest child→parent chain from a launchable (no incoming child edge) to this node, so launchables
    // land at column 0 and the deepest base content on the right. Memoized longest-path over the DAG.
    std::vector<int> Col(N, -1);
    std::function<int(int)> ColOf = [&](int I) -> int {
        if (Col[I] >= 0) return Col[I];
        int M = 0; Col[I] = 0;                       // break any accidental cycle gracefully
        for (const auto & [C, P] : Edges) if (P == I) M = std::max(M, ColOf(C) + 1);
        return Col[I] = M;
    };
    for (int I = 0; I < N; ++I) ColOf(I);

    // Row: process columns left→right; a node inherits a child's row to keep linear chains straight, else takes the
    // next free row in its column. Extra parents thus branch onto fresh rows.
    std::vector<int> Order(N); for (int I = 0; I < N; ++I) Order[I] = I;
    std::stable_sort(Order.begin(), Order.end(), [&](int A, int B){ return Col[A] < Col[B]; });
    std::vector<int> Row(N, -1);
    std::map<int, std::set<int>> Used;               // column → taken rows
    for (int I : Order)
    {
        int Want = -1;
        for (const auto & [C, P] : Edges) if (P == I && Row[C] >= 0) Want = (Want < 0 ? Row[C] : std::min(Want, Row[C]));
        if (Want < 0) Want = 0;
        while (Used[Col[I]].count(Want)) ++Want;
        Row[I] = Want; Used[Col[I]].insert(Want);
    }

    int MaxCol = 0, MaxRow = 0;
    for (int I = 0; I < N; ++I)
    {
        Nodes[I].Col = Col[I]; Nodes[I].Row = Row[I];   // Col = depth (down), Row = branch (right)
        Nodes[I].Rect = QRect(MargX + Row[I] * (ChipW + GapX), MargY + Col[I] * (ChipH + GapY), ChipW, ChipH);
        MaxCol = std::max(MaxCol, Col[I]); MaxRow = std::max(MaxRow, Row[I]);
    }
    ContentW = N ? MargX * 2 + (MaxRow + 1) * (ChipW + GapX) - GapX : 0;
    ContentH = N ? MargY * 2 + (MaxCol + 1) * (ChipH + GapY) - GapY : 0;
    setFixedSize(std::max(ContentW, 1), std::max(ContentH, 1));
    updateGeometry(); update();
}

void NodeGraphView::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing, true);

    // Edges: child (above) → parent (below), a smooth vertical cubic.
    P.setPen(QPen(QColor("#4a5258"), 1.6));
    for (const auto & [C, Par] : Edges)
    {
        const QRect &A = Nodes[C].Rect, &B = Nodes[Par].Rect;
        QPointF S(A.center().x(), A.bottom()), E(B.center().x(), B.top());
        const qreal Dy = (E.y() - S.y()) * 0.5;
        QPainterPath Path(S);
        Path.cubicTo(S.x(), S.y() + Dy, E.x(), E.y() - Dy, E.x(), E.y());
        P.drawPath(Path);
    }

    // Chips.
    for (const auto & G : Nodes)
    {
        const bool Cur = (G.Id == Current);
        QColor Fill = G.Role == "launchable" ? QColor("#264b73")
                    : G.Role == "runner"     ? QColor("#4a3a63")
                                             : QColor("#2b3138");
        P.setBrush(Fill);
        P.setPen(QPen(Cur ? QColor("#4a90d9") : QColor("#3a424a"), Cur ? 2.2 : 1.2));
        P.drawRoundedRect(G.Rect, 6, 6);
        P.setPen(Cur ? QColor("#eaf2fb") : QColor("#c6d4df"));
        QRect TextR = G.Rect.adjusted(8, 0, -8, 0);
        P.drawText(TextR, Qt::AlignVCenter | Qt::AlignLeft,
                   P.fontMetrics().elidedText(G.Label, Qt::ElideRight, TextR.width()));
    }
}

void NodeGraphView::mousePressEvent(QMouseEvent * E)
{
    for (const auto & G : Nodes)
        if (G.Rect.contains(E->pos())) { emit nodeClicked(QString::fromStdString(G.Id)); return; }
}

void NodeGraphView::mouseMoveEvent(QMouseEvent * E)
{
    bool Over = false;
    for (const auto & G : Nodes) if (G.Rect.contains(E->pos())) { Over = true; break; }
    setCursor(Over ? Qt::PointingHandCursor : Qt::ArrowCursor);
}
