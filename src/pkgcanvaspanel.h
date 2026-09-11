#ifndef PKGCANVASPANEL_H
#define PKGCANVASPANEL_H

#include "pkgcanvas.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>

class QTimer;

// The GL host for the package canvas: a QOpenGLWidget that owns the Dear ImGui context + OpenGL3 backend,
// bridges Qt input into ImGui, and asks the canvas to draw itself each frame. Nothing here decides anything —
// every editing behaviour lives in PkgCanvas, which is testable headlessly. This is only the plumbing that
// puts it on screen.
class PkgCanvasPanel : public QOpenGLWidget
{
    Q_OBJECT

public:
    PkgCanvasPanel(nlohmann::ordered_json *doc, PkgCanvas::SaveFn save, QWidget *parent = nullptr,
                   nlohmann::ordered_json *layout = nullptr);   // layout sidecar — see PkgGraph::Build
    ~PkgCanvasPanel() override;

    PkgCanvas *canvas() const { return m_canvas; }

protected:
    void initializeGL() override;
    void paintGL() override;

    // Qt → ImGui input bridge.
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;

private:
    static int LivePanels;   // ImGui's context is global: exactly one canvas may own it
    PkgCanvas *m_canvas;
    bool m_imguiReady = false;
    bool m_inFrame = false;   // re-entrancy guard: a modal's nested loop still delivers paint events
    QTimer *m_repaint = nullptr;   // ~60fps so edits are live
    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
};

#endif // PKGCANVASPANEL_H
