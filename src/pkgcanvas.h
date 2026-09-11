#ifndef PKGCANVAS_H
#define PKGCANVAS_H

#include "pkggraph.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PkgCanvasState;

// ---------------------------------------------------------------------------
// PkgCanvas — the blueprint surface for a package bundle: every imgui/imnodes call, with NO GL and NO QWidget
// in it. The host (PkgCanvasPanel, a QOpenGLWidget) owns the ImGui context and the render backend and calls
// frame() between ImGui::NewFrame() and ImGui::Render(); a test does the same headlessly with a synthetic
// mouse. So wiring, node creation, payload editing and deletion are all exercised by tests — which is the one
// thing the old tab-and-form editor could never be, because its logic lived inside QWidget constructors.
//
// The canvas edits the SAME `{"NODES":[…]}` document the model persists one-file-per-node. There is no second
// representation: a wire dragged here is a PARENTS entry on disk, and a file changed on disk reappears here on
// reload. Nothing is captured by index in a callback — immediate mode redraws from the document every frame,
// so the stale-index problem that forced the old editor to rebuild its entire widget tree cannot arise.
// ---------------------------------------------------------------------------
class PkgCanvas : public QObject
{
    Q_OBJECT

public:
    using SaveFn     = std::function<void()>;                                  // persist the document
    using KnownIdsFn = std::function<std::vector<std::string>()>;              // ids across the catalog (parent picker)
    using ActionFn   = std::function<void(const std::string &NodeId, const std::string &Action)>;

    //`layout` is the canvas-position sidecar (NODE_ID -> [x,y]); see PkgGraph::Build. Optional: pass nullptr and
    //the canvas auto-lays-out every frame and persists nothing, which is what the headless tests want.
    PkgCanvas(nlohmann::ordered_json *doc, SaveFn save, QObject *parent = nullptr,
              nlohmann::ordered_json *layout = nullptr);
    ~PkgCanvas() override;

    void initContexts();      // right after ImGui::CreateContext()
    void shutdownContexts();  // right before ImGui::DestroyContext()
    void frame();             // one frame, between NewFrame() and Render()

    void setKnownIds(KnownIdsFn fn);
    //The document changed behind the canvas's back (a reload, an action's writeback) — drop the cached graph.
    void invalidateGraph();
    //Per-node problems, keyed by NODE_ID — drawn ON the node that is wrong, not in a separate report.
    void setIssues(const std::vector<std::pair<std::string, std::string>> &issues);
    //Node actions the host performs (file dialogs, zip/dir/delta conversion, capture, test launch): the canvas
    //only renders the buttons and reports the click. It does no IO and knows nothing about the engine.
    void setActionHandler(ActionFn fn);
    //Facts the canvas cannot know without touching disk, per NODE_ID — e.g. "deflate" makes the re-store button
    //appear. The host probes off-thread and pushes them in.
    void setNodeHints(const std::string &nodeId, const std::vector<std::string> &hints);

    // ---- long-running actions ----------------------------------------------
    // A node running a heavy action swaps its buttons for a progress bar + Cancel and goes READ-ONLY, so an edit
    // cannot race a zip that is rewriting the very file the fields describe. This state is canvas-local, keyed by
    // NODE_ID: it never reaches the document, and it survives the immediate-mode redraw.
    void beginAction(const std::string &nodeId, const QString &what, bool cancellable);
    void setProgress(const std::string &nodeId, float fraction, const QString &detail = {});
    void endAction(const std::string &nodeId);
    bool isBusy(const std::string &nodeId) const;

    // ---- programmatic access (the host, and the tests) ----------------------
    int  addNode(const std::string &type, float x = 60.0f, float y = 60.0f);   // returns the new node's index
    bool removeNode(int index);
    bool connect(int parentIndex, int childIndex);                             // adds a PARENTS entry
    //Wire an OUT-OF-BUNDLE parent (a runner, MediaStack_MS, asiloader) by id. Every package in the library
    //depends on at least one, so a canvas that can only wire within the bundle cannot author a real package.
    bool connectExternal(const std::string &parentId, int childIndex);
    //Show a chip for an external id that nothing references yet, so there is something to drag a wire from.
    void offerExternal(const std::string &parentId);
    bool disconnect(int parentIndex, int childIndex);
    //Renames a node and re-points every reference to it. Refuses a name another node already owns.
    //Returns false if the rename was rejected.
    bool renameNode(int index, const std::string &newId);
    int  nodeCount() const;
    int  indexOf(const std::string &nodeId) const;
    PkgGraph::Graph graph() const;
    int  selectedNode() const;
    void selectNode(int index);

signals:
    void documentChanged();                       // the canvas mutated the document (already saved)
    void nodeAction(const QString &nodeId, const QString &action);
    void cancelRequested(const QString &nodeId);  // Cancel pressed on a running action

private:
    void drawToolbar();
    void drawNode(int index, PkgGraph::Graph &g);
    void drawEnvelope(nlohmann::ordered_json &node);
    void drawPayload(nlohmann::ordered_json &node, int index);
    void drawActions(nlohmann::ordered_json &node, int index, const PkgGraph::Graph &g);
    void drawField(nlohmann::ordered_json &node, const PkgGraph::Field &f, int index);
    void drawRegEdits(nlohmann::ordered_json &node, int index);
    void syncLinks(const PkgGraph::Graph &g);
    void flushPositions(PkgGraph::Graph &g);
    std::unique_ptr<PkgCanvasState> m_s;
};

#endif // PKGCANVAS_H
