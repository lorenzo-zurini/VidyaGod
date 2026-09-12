#include "pkgcanvaspanel.h"

#include "commonutils.h"   // LogWarn

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

namespace {

// Map a Qt key to an ImGuiKey for the keys editing/navigation needs; printable
// characters arrive separately as text input.
ImGuiKey qtKeyToImGui(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<ImGuiKey>(ImGuiKey_0 + (key - Qt::Key_0));
    switch (key) {
    case Qt::Key_Space: return ImGuiKey_Space;
    case Qt::Key_Return:
    case Qt::Key_Enter: return ImGuiKey_Enter;
    case Qt::Key_Escape: return ImGuiKey_Escape;
    case Qt::Key_Tab: return ImGuiKey_Tab;
    case Qt::Key_Backspace: return ImGuiKey_Backspace;
    case Qt::Key_Delete: return ImGuiKey_Delete;
    case Qt::Key_Insert: return ImGuiKey_Insert;
    case Qt::Key_Left: return ImGuiKey_LeftArrow;
    case Qt::Key_Right: return ImGuiKey_RightArrow;
    case Qt::Key_Up: return ImGuiKey_UpArrow;
    case Qt::Key_Down: return ImGuiKey_DownArrow;
    case Qt::Key_Home: return ImGuiKey_Home;
    case Qt::Key_End: return ImGuiKey_End;
    case Qt::Key_PageUp: return ImGuiKey_PageUp;
    case Qt::Key_PageDown: return ImGuiKey_PageDown;
    default: return ImGuiKey_None;
    }
}

void feedModifiers(Qt::KeyboardModifiers m)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, m.testFlag(Qt::ControlModifier));
    io.AddKeyEvent(ImGuiMod_Shift, m.testFlag(Qt::ShiftModifier));
    io.AddKeyEvent(ImGuiMod_Alt, m.testFlag(Qt::AltModifier));
    io.AddKeyEvent(ImGuiMod_Super, m.testFlag(Qt::MetaModifier));
}

} // namespace

PkgCanvasPanel::PkgCanvasPanel(nlohmann::ordered_json *doc, PkgCanvas::SaveFn save, QWidget *parent,
                               nlohmann::ordered_json *layout, PkgCanvas::SaveFn saveLayoutOnly)
    : QOpenGLWidget(parent),
      m_canvas(new PkgCanvas(doc, std::move(save), this, layout, std::move(saveLayoutOnly)))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_repaint = new QTimer(this);
    connect(m_repaint, &QTimer::timeout, this, [this] { update(); });
    m_repaint->start(16);
}

PkgCanvasPanel::~PkgCanvasPanel()
{
    if (m_imguiReady) {
        --LivePanels;
        makeCurrent();
        m_canvas->shutdownContexts();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        doneCurrent();
    }
}

//Dear ImGui keeps ONE global current context, and imnodes hangs off it. A second panel would overwrite it,
//leaving the first panel's frames pointing at a foreign context, and whichever destructor ran first would
//destroy the other's. The editor is opened non-modally from two places (the library tab and the pre-launch
//window, which "test launch" itself opens), so two panels is reachable, not theoretical. One at a time.
int PkgCanvasPanel::LivePanels = 0;

void PkgCanvasPanel::initializeGL()
{
    if (LivePanels > 0)
    {
        LogWarn("PkgCanvasPanel", "A package editor canvas is already open - this one stays blank. "
                                  "Dear ImGui has a single global context; two would corrupt each other.");
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't scribble imgui.ini into the bundle
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    m_canvas->initContexts();
    ImGui_ImplOpenGL3_Init("#version 330"); // matches the 3.3 core context
    ++LivePanels;
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_imguiReady = true;
}

void PkgCanvasPanel::paintGL()
{
    if (!m_imguiReady || width() <= 0 || height() <= 0)
        return;
    //A modal opened from an action runs a nested Qt event loop, which keeps delivering paint events. Without
    //this guard the nested paint calls ImGui::NewFrame() while a frame is already open.
    if (m_inFrame) return;
    m_inFrame = true;
    struct FrameGuard { bool &F; ~FrameGuard() { F = false; } } Guard{m_inFrame};
    ImGuiIO &io = ImGui::GetIO();
    const qreal dpr = devicePixelRatioF();
    io.DisplaySize = ImVec2(static_cast<float>(width()), static_cast<float>(height()));
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(dpr), static_cast<float>(dpr));
    const qint64 now = m_clock.nsecsElapsed();
    io.DeltaTime = qMax(1e-4f, static_cast<float>(now - m_lastNs) / 1e9f);
    m_lastNs = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    m_canvas->frame();
    ImGui::Render();

    const int fbw = static_cast<int>(width() * dpr), fbh = static_cast<int>(height() * dpr);
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---- Qt -> ImGui input bridge -------------------------------------------

void PkgCanvasPanel::mouseMoveEvent(QMouseEvent *e)
{
    if (m_imguiReady)
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(e->position().x()),
                                        static_cast<float>(e->position().y()));
}

void PkgCanvasPanel::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (!m_imguiReady)
        return;
    const int b = e->button() == Qt::LeftButton ? 0 : e->button() == Qt::RightButton ? 1 : 2;
    ImGui::GetIO().AddMouseButtonEvent(b, true);
}

void PkgCanvasPanel::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_imguiReady)
        return;
    const int b = e->button() == Qt::LeftButton ? 0 : e->button() == Qt::RightButton ? 1 : 2;
    ImGui::GetIO().AddMouseButtonEvent(b, false);
}

void PkgCanvasPanel::wheelEvent(QWheelEvent *e)
{
    if (m_imguiReady)
        ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(e->angleDelta().x()) / 120.0f,
                                          static_cast<float>(e->angleDelta().y()) / 120.0f);
    e->accept();
}

void PkgCanvasPanel::keyPressEvent(QKeyEvent *e)
{
    if (!m_imguiReady) {
        QOpenGLWidget::keyPressEvent(e);
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    feedModifiers(e->modifiers());
    const ImGuiKey k = qtKeyToImGui(e->key());
    if (k != ImGuiKey_None)
        io.AddKeyEvent(k, true);
    const QString t = e->text();
    if (!t.isEmpty() && t.at(0).isPrint())
        io.AddInputCharactersUTF8(t.toUtf8().constData());
}

void PkgCanvasPanel::keyReleaseEvent(QKeyEvent *e)
{
    if (!m_imguiReady) {
        QOpenGLWidget::keyReleaseEvent(e);
        return;
    }
    feedModifiers(e->modifiers());
    const ImGuiKey k = qtKeyToImGui(e->key());
    if (k != ImGuiKey_None)
        ImGui::GetIO().AddKeyEvent(k, false);
}
