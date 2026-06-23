#ifndef NODEGRAPHVIEW_H
#define NODEGRAPHVIEW_H

#include <QWidget>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// NodeGraphView — a compact, clickable DAG of a bundle's nodes. Laid out left→right: launchables on the left, their
// PARENTS chain flowing right, deepest base content on the right. A linear dependency chain stays on one row; extra
// parents branch onto new rows (so it reads "linear with branches"). Clicking a node chip emits nodeClicked(NODE_ID),
// which the editor uses to jump to that node's tab. Refreshed via SetGraph() on every edit.
// ---------------------------------------------------------------------------
class NodeGraphView : public QWidget
{
    Q_OBJECT
public:
    explicit NodeGraphView(QWidget * parent = nullptr);
    void SetGraph(const nlohmann::ordered_json & NodesArray);   // the working { NODES:[...] } array
    void SetCurrent(const std::string & NodeId);                // highlight the open node
    QSize sizeHint() const override;

signals:
    void nodeClicked(const QString & NodeId);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

private:
    struct GNode { std::string Id; QString Label; std::string Role; int Col = 0, Row = 0; QRect Rect; };
    std::vector<GNode> Nodes;
    std::vector<std::pair<int,int>> Edges;   // (child index, parent index)
    std::string Current;
    int ContentW = 0, ContentH = 0;
    void Relayout();
};

#endif // NODEGRAPHVIEW_H
