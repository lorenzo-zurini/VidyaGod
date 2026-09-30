#include "pkgcanvaspanel.h"

#include "commonutils.h"   // LogWarn

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <cfloat>

namespace {

//Qt key → ImGui key, for everything a shortcut or a text field can use; printable text arrives separately.
ImGuiKey QtKeyToImGui(int K)
{
    if (K >= Qt::Key_A && K <= Qt::Key_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (K - Qt::Key_A));
    if (K >= Qt::Key_0 && K <= Qt::Key_9) return static_cast<ImGuiKey>(ImGuiKey_0 + (K - Qt::Key_0));
    if (K >= Qt::Key_F1 && K <= Qt::Key_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (K - Qt::Key_F1));
    switch (K)
    {
    case Qt::Key_Space: return ImGuiKey_Space;
    case Qt::Key_Return: return ImGuiKey_Enter;
    case Qt::Key_Enter: return ImGuiKey_KeypadEnter;
    case Qt::Key_Escape: return ImGuiKey_Escape;
    case Qt::Key_Tab: case Qt::Key_Backtab: return ImGuiKey_Tab;
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
    case Qt::Key_Minus: return ImGuiKey_Minus;
    case Qt::Key_Equal: case Qt::Key_Plus: return ImGuiKey_Equal;
    case Qt::Key_Comma: return ImGuiKey_Comma;
    case Qt::Key_Period: return ImGuiKey_Period;
    case Qt::Key_Slash: return ImGuiKey_Slash;
    case Qt::Key_Semicolon: return ImGuiKey_Semicolon;
    case Qt::Key_Apostrophe: return ImGuiKey_Apostrophe;
    case Qt::Key_BracketLeft: return ImGuiKey_LeftBracket;
    case Qt::Key_BracketRight: return ImGuiKey_RightBracket;
    case Qt::Key_Backslash: return ImGuiKey_Backslash;
    case Qt::Key_QuoteLeft: return ImGuiKey_GraveAccent;
    case Qt::Key_Shift: return ImGuiKey_LeftShift;
    case Qt::Key_Control: return ImGuiKey_LeftCtrl;
    case Qt::Key_Alt: return ImGuiKey_LeftAlt;
    case Qt::Key_Meta: return ImGuiKey_LeftSuper;
    default: return ImGuiKey_None;
    }
}

void FeedModifiers(Qt::KeyboardModifiers M)
{
    ImGuiIO &Io = ImGui::GetIO();
    Io.AddKeyEvent(ImGuiMod_Ctrl, M.testFlag(Qt::ControlModifier));
    Io.AddKeyEvent(ImGuiMod_Shift, M.testFlag(Qt::ShiftModifier));
    Io.AddKeyEvent(ImGuiMod_Alt, M.testFlag(Qt::AltModifier));
    Io.AddKeyEvent(ImGuiMod_Super, M.testFlag(Qt::MetaModifier));
}

int ButtonOf(Qt::MouseButton B) { return B == Qt::LeftButton ? 0 : B == Qt::RightButton ? 1 : B == Qt::MiddleButton ? 2 : -1; }

} // namespace

int PkgCanvasPanel::LivePanels = 0;

PkgCanvasPanel::PkgCanvasPanel(PkgDoc::Document *doc, QWidget *parent)
    : QOpenGLWidget(parent), m_canvas(new PkgCanvas(doc, this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_tick = new QTimer(this);
    m_tick->setSingleShot(true);
    connect(m_tick, &QTimer::timeout, this, [this] { update(); });
}

PkgCanvasPanel::~PkgCanvasPanel()
{
    if (m_imguiReady)
    {
        --LivePanels;
        makeCurrent();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        doneCurrent();
    }
}

void PkgCanvasPanel::requestFrame() { m_extraFrames = 4; update(); }   // imgui spreads one input burst over frames

//Dear ImGui keeps ONE global current context. A second panel would overwrite it and whichever destructor ran first
//would destroy the other's; the editor is reachable from two places (the library and the pre-launch window), so one
//canvas at a time is enforced here as well as by PackageEditor::OpenFor.
void PkgCanvasPanel::initializeGL()
{
    if (LivePanels > 0)
    {
        LogWarn("PkgCanvasPanel", "A package editor canvas is already open - this one stays blank (Dear ImGui has one global context).");
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &Io = ImGui::GetIO();
    Io.IniFilename = nullptr;                         // never scribble imgui.ini into a package
    Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    PkgCanvas::InstallFontsAndStyle();
    ImGui_ImplOpenGL3_Init("#version 330");           // the 3.3 core context the app requests
    ++LivePanels;
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_imguiReady = true;
    m_extraFrames = 3;
}

void PkgCanvasPanel::paintGL()
{
    if (!m_imguiReady || width() <= 0 || height() <= 0 || m_inFrame) return;
    m_inFrame = true;
    struct FrameGuard { bool &F; ~FrameGuard() { F = false; } } Guard{m_inFrame};
    ImGuiIO &Io = ImGui::GetIO();
    const qreal Dpr = devicePixelRatioF();
    Io.DisplaySize = ImVec2((float)width(), (float)height());
    Io.DisplayFramebufferScale = ImVec2((float)Dpr, (float)Dpr);
    const qint64 Now = m_clock.nsecsElapsed();
    Io.DeltaTime = qBound(1e-4f, (float)(Now - m_lastNs) / 1e9f, 0.25f);
    m_lastNs = Now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    m_canvas->frame();
    ImGui::Render();
    glViewport(0, 0, (int)(width() * Dpr), (int)(height() * Dpr));
    glClearColor(0.094f, 0.098f, 0.114f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    //Keep drawing while something moves; otherwise a couple more frames (imgui settles hover/popups over two), then idle.
    if (m_canvas->wantsFrames()) m_tick->start(16);
    else if (m_extraFrames > 0) { --m_extraFrames; m_tick->start(16); }
}

// ---- Qt -> ImGui input -----------------------------------------------------------------------------------------

void PkgCanvasPanel::mouseMoveEvent(QMouseEvent *E)
{
    if (!m_imguiReady) return;
    ImGui::GetIO().AddMousePosEvent((float)E->position().x(), (float)E->position().y());
    requestFrame();
}

void PkgCanvasPanel::mousePressEvent(QMouseEvent *E)
{
    setFocus();
    const int B = ButtonOf(E->button());
    if (!m_imguiReady || B < 0) return;
    ImGui::GetIO().AddMousePosEvent((float)E->position().x(), (float)E->position().y());
    ImGui::GetIO().AddMouseButtonEvent(B, true);
    requestFrame();
}

void PkgCanvasPanel::mouseReleaseEvent(QMouseEvent *E)
{
    const int B = ButtonOf(E->button());
    if (!m_imguiReady || B < 0) return;
    ImGui::GetIO().AddMouseButtonEvent(B, false);
    requestFrame();
}

//Qt turns a second click into a double-click event instead of a press: imgui detects double-clicks itself from two
//presses, so it is handed a press.
void PkgCanvasPanel::mouseDoubleClickEvent(QMouseEvent *E) { mousePressEvent(E); }

void PkgCanvasPanel::wheelEvent(QWheelEvent *E)
{
    if (m_imguiReady)
        ImGui::GetIO().AddMouseWheelEvent((float)E->angleDelta().x() / 120.0f, (float)E->angleDelta().y() / 120.0f);
    E->accept();
    requestFrame();
}

void PkgCanvasPanel::keyPressEvent(QKeyEvent *E)
{
    if (!m_imguiReady) { QOpenGLWidget::keyPressEvent(E); return; }
    ImGuiIO &Io = ImGui::GetIO();
    FeedModifiers(E->modifiers());
    const ImGuiKey K = QtKeyToImGui(E->key());
    if (K != ImGuiKey_None) Io.AddKeyEvent(K, true);
    const QString T = E->text();
    if (!T.isEmpty() && T.at(0).isPrint() && !(E->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
        Io.AddInputCharactersUTF8(T.toUtf8().constData());
    requestFrame();
}

void PkgCanvasPanel::keyReleaseEvent(QKeyEvent *E)
{
    if (!m_imguiReady) { QOpenGLWidget::keyReleaseEvent(E); return; }
    FeedModifiers(E->modifiers());
    const ImGuiKey K = QtKeyToImGui(E->key());
    if (K != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(K, false);
    requestFrame();
}

void PkgCanvasPanel::leaveEvent(QEvent *E)
{
    if (m_imguiReady) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // nothing on the canvas is hovered now
    QOpenGLWidget::leaveEvent(E);
    requestFrame();
}

void PkgCanvasPanel::focusInEvent(QFocusEvent *E)
{
    if (m_imguiReady) ImGui::GetIO().AddFocusEvent(true);
    QOpenGLWidget::focusInEvent(E);
    requestFrame();
}

void PkgCanvasPanel::focusOutEvent(QFocusEvent *E)
{
    if (m_imguiReady) ImGui::GetIO().AddFocusEvent(false);   // releases held keys: no stuck Ctrl after Alt-Tab
    QOpenGLWidget::focusOutEvent(E);
    requestFrame();
}

bool PkgCanvasPanel::event(QEvent *E)
{
    //Tab and Shift+Tab move between the canvas's own fields; Qt would otherwise take them for focus navigation.
    if (E->type() == QEvent::KeyPress)
    {
        auto *K = static_cast<QKeyEvent *>(E);
        if (K->key() == Qt::Key_Tab || K->key() == Qt::Key_Backtab) { keyPressEvent(K); return true; }
    }
    return QOpenGLWidget::event(E);
}
