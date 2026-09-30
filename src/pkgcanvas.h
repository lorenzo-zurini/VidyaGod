#ifndef PKGCANVAS_H
#define PKGCANVAS_H

#include "pkgdoc.h"
#include "pkggraph.h"

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct PkgCanvasState;
struct ImFont;

// ---------------------------------------------------------------------------
// PkgCanvas — the package editor's node canvas: every Dear ImGui call of the editor, no GL and no QWidget, so the
// whole surface runs headless under test.
//
// Model: a PkgDoc::Document (nodes by handle, positions, undo). The canvas never holds a second copy of the package:
// it draws the document, and every edit is a Document call.
//
// View: a camera (the world point at the canvas's top-left) and a zoom. Zoom is REAL, not a transform of the pixels
// drawn at 1:1: each node is an ImGui child window placed at its screen position, sized, styled and typeset at the
// zoomed size (the editor's fonts are vector fonts rasterised at any size), so text stays crisp, clicks land where
// things are drawn and popups open where they belong at every zoom. Below ~40% nodes are drawn as boxes with their
// names (level of detail), which is what an overview needs and what keeps a 2775-node package fluid.
//
// Wires are drawn beneath the nodes from the positions the canvas knows for EVERY node, so a wire is visible whenever
// any part of it is on screen, whatever is culled. Other packages' nodes a package names are chips, labelled with
// their package and name, placed by the same layout as everything else.
// ---------------------------------------------------------------------------
class PkgCanvas : public QObject
{
    Q_OBJECT

public:
    //What the editor knows about a node this package does not own (another package's node, named by CID).
    struct External { std::string Label, Package, PackageDir; };
    using ExternalFn = std::function<External(const std::string &Cid)>;
    //Every node the library offers for wiring in, {cid, label, package} — the "link another package's node" picker.
    struct Offer { std::string Cid, Label, Package; };
    using OffersFn = std::function<std::vector<Offer>()>;

    explicit PkgCanvas(PkgDoc::Document *doc, QObject *parent = nullptr);
    ~PkgCanvas() override;

    //Once per ImGui context: the editor's fonts (Noto Sans / Bold / Mono, embedded) and its style.
    static void InstallFontsAndStyle();
    static ImFont *BoldFont();
    static ImFont *MonoFont();

    void frame();             // one frame, between NewFrame() and Render()
    bool wantsFrames() const; // something is animating or being edited: keep rendering (else render on input only)

    // ---- host facts -------------------------------------------------------------------------------------------
    void setExternalLookup(ExternalFn fn);
    void setOffers(OffersFn fn);
    void setIssues(const std::map<std::string, std::vector<std::string>> &issuesByHandle);
    void setNodeHints(const std::string &handle, const std::vector<std::string> &hints);   // host facts ("deflate")
    void beginAction(const std::string &handle, const QString &what, bool cancellable);
    void setProgress(const std::string &handle, float fraction, const QString &detail = {});
    void endAction(const std::string &handle);
    bool isBusy(const std::string &handle) const;
    //The document was replaced or reloaded wholesale: forget per-node view state that no longer applies.
    void documentReset();
    //Handles renamed by a save (PkgDoc::Document::TakeRenames): folds, sizes, selection follow the nodes.
    void applyRenames(const std::map<std::string, std::string> &renames);

    // ---- selection & navigation ------------------------------------------------------------------------------
    std::string selectedHandle() const;                   // the one selected node ("" for none or several)
    std::set<std::string> selection() const;
    void select(const std::string &handle, bool frame = false);
    void frameAll();
    void frameSelection();
    void tidyLayout();                                    // forget this machine's positions: back to the layout
    void setExpanded(const std::string &handle, bool open);   // open every row of a node (or fold them all)

    // ---- view -------------------------------------------------------------------------------------------------
    float zoom() const;
    void  setZoom(float z);                               // about the centre of the view
    void  setCamera(float x, float y);                    // the world point at the canvas's top-left
    void  cameraPos(float &x, float &y) const;
    bool  miniMap() const;
    void  setMiniMap(bool on);

    // ---- document shortcuts (also reachable from the canvas's own menus) --------------------------------------
    int  addNode(const std::string &layerType, float worldX, float worldY);   // returns its index
    bool removeNodes(const std::set<std::string> &handles);
    void undo();
    void redo();

    // ---- introspection (tests, the host) ---------------------------------------------------------------------
    //Where things were drawn LAST frame, in screen pixels; false/empty when not drawn.
    bool nodeRect(const std::string &handle, float &x0, float &y0, float &x1, float &y1) const;
    bool portPos(const std::string &handle, bool out, float &x, float &y) const;
    bool canvasRect(float &x0, float &y0, float &x1, float &y1) const;
    int  visibleNodes() const;                            // nodes drawn last frame (full or as boxes)
    int  visibleWires() const;                            // wires drawn last frame
    int  externalCount() const;
    std::string externalLabel(const std::string &cid) const;   // the text its chip shows
    //World position and size the canvas uses for a node or external (placed, laid out or measured).
    bool worldRect(const std::string &handle, float &x, float &y, float &w, float &h) const;

signals:
    void selectionChanged(const QString &handle);         // "" when nothing / several are selected
    void documentEdited();                                // the canvas edited the document (and committed a step)
    void saveRequested();                                 // ctrl+S
    void nodeAction(const QString &handle, const QString &action);
    void cancelRequested(const QString &handle);
    void openPackageRequested(const QString &packageDir); // an external chip's "open its package"
    void statusMessage(const QString &text);              // a one-line note for the host's status bar

private:
    std::unique_ptr<PkgCanvasState> m_s;
};

#endif // PKGCANVAS_H
