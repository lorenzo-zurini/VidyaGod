#ifndef PKGCANVASPANEL_H
#define PKGCANVASPANEL_H

#include "pkgcanvas.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>

class QTimer;

// ---------------------------------------------------------------------------
// PkgCanvasPanel — hosts the package canvas (Dear ImGui) in a QOpenGLWidget: owns the ImGui context, feeds it Qt's
// input, and renders ON DEMAND — a frame per input event, and continuously only while something moves (a zoom easing,
// a drag, a caret blinking, a conversion's progress). An idle editor costs nothing.
// ---------------------------------------------------------------------------
class PkgCanvasPanel : public QOpenGLWidget
{
    Q_OBJECT
public:
    PkgCanvasPanel(PkgDoc::Document *doc, QWidget *parent = nullptr);
    ~PkgCanvasPanel() override;

    PkgCanvas *canvas() const { return m_canvas; }
    void requestFrame();                            // the document or host state changed: draw it

protected:
    void initializeGL() override;
    void paintGL() override;

    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    bool event(QEvent *e) override;                  // Tab reaches the canvas (not focus navigation)

private:
    static int LivePanels;                           // ImGui's context is global: exactly one canvas may own it
    PkgCanvas *m_canvas;
    bool m_imguiReady = false;
    bool m_inFrame = false;                          // a modal's nested loop still delivers paint events
    int m_extraFrames = 0;                           // frames still owed after input (imgui settles over two)
    QTimer *m_tick = nullptr;                        // drives frames while the canvas wants them
    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
};

#endif // PKGCANVASPANEL_H
