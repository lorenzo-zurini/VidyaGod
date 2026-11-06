#ifndef PKGCANVASPANEL_H
#define PKGCANVASPANEL_H

#include "pkgcanvas.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>

#include <map>
#include <string>

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

signals:
    //Files dragged in from the file manager and let go: on a node (its handle), or on the empty canvas ("", and the
    //world point a new node for them goes).
    void filesDropped(const QString &handle, const QStringList &paths, float worldX, float worldY);

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
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;   // files dragged back out: the drop target goes dark
    void dropEvent(QDropEvent *e) override;

private:
    static int LivePanels;                           // ImGui's context is global: exactly one canvas may own it
    PkgCanvas *m_canvas;
    bool m_imguiReady = false;
    bool m_inFrame = false;                          // a modal's nested loop still delivers paint events
    int m_extraFrames = 0;                           // frames still owed after input (imgui settles over two)
    QTimer *m_tick = nullptr;                        // drives frames while the canvas wants them
    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    //Images the canvas draws (covers on chips), uploaded once each while the GL context is current.
    struct Tex { unsigned Id = 0; float W = 0, H = 0; };
    std::map<std::string, Tex> m_textures;
};

#endif // PKGCANVASPANEL_H
