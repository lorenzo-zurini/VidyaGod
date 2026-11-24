#include "volumekeys.h"

#include "controllerservice.h"
#include "gameoverlay.h"   // MarkExternalOverlay: drawn above a game under gamescope

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace Volume {

std::optional<Level> ParseWpctl(const QString &Out)
{
    static const QRegularExpression Re(QStringLiteral(R"(Volume:\s*([0-9]+(?:\.[0-9]+)?))"));
    const auto M = Re.match(Out);
    if (!M.hasMatch()) return std::nullopt;
    Level L;
    L.Percent = std::clamp(int(std::lround(M.captured(1).toDouble() * 100)), 0, 150);
    L.Muted = Out.contains(QStringLiteral("[MUTED]"));
    return L;
}

std::optional<Level> ParsePactl(const QString &VolumeOut, const QString &MuteOut)
{
    static const QRegularExpression Re(QStringLiteral(R"(([0-9]+)%)"));
    const auto M = Re.match(VolumeOut);   // the first channel's: a balanced output says the same of each
    if (!M.hasMatch()) return std::nullopt;
    Level L;
    L.Percent = std::clamp(M.captured(1).toInt(), 0, 150);
    L.Muted = MuteOut.contains(QStringLiteral("yes"), Qt::CaseInsensitive);
    return L;
}

Command StepCommand(const QString &Tool, int Delta, int Now)
{
    const QString Amount = QString::number(std::abs(Delta)) + QStringLiteral("%");
    if (Tool == QLatin1String("wpctl"))   // -l 1.0: never past 100%
        return { Tool, { "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", Amount + (Delta < 0 ? "-" : "+") } };
    //pactl has no limit: a step up that would pass 100% (by the level last read) goes to 100% exactly, and one from at or
    //above it does nothing — never lowering a level set higher elsewhere. Otherwise relative (steps of a held key, each
    //sent before the last one's level is read back, add up).
    if (Delta > 0 && Now >= 100) return { QString(), {} };
    if (Delta > 0 && Now >= 0 && Now + Delta > 100) return { Tool, { "set-sink-volume", "@DEFAULT_SINK@", "100%" } };
    return { Tool, { "set-sink-volume", "@DEFAULT_SINK@", (Delta < 0 ? "-" : "+") + Amount } };
}

Command MuteCommand(const QString &Tool)
{
    if (Tool == QLatin1String("wpctl")) return { Tool, { "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle" } };
    return { Tool, { "set-sink-mute", "@DEFAULT_SINK@", "toggle" } };
}

std::vector<Command> ReadCommands(const QString &Tool)
{
    if (Tool == QLatin1String("wpctl")) return { { Tool, { "get-volume", "@DEFAULT_AUDIO_SINK@" } } };
    return { { Tool, { "get-sink-volume", "@DEFAULT_SINK@" } }, { Tool, { "get-sink-mute", "@DEFAULT_SINK@" } } };
}

} // namespace Volume

// ---------------------------------------------------------------------------

VolumeToast::VolumeToast(QWidget *Parent)
    : QWidget(Parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("volumeToast"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    Hide = new QTimer(this);
    Hide->setSingleShot(true);
    Hide->setInterval(ShowMs);
    connect(Hide, &QTimer::timeout, this, &QWidget::hide);
    MarkExternalOverlay(this);   // above a game, taking no input (gamescope); nothing elsewhere
}

void VolumeToast::present(Volume::Level L)
{
    Shown = L;
    const QScreen *S = QGuiApplication::primaryScreen();
    const QRect A = S ? S->geometry() : QRect(0, 0, 1280, 800);
    const int W = std::max(260, A.width() / 4), H = std::max(56, A.height() / 12);
    setGeometry(A.x() + (A.width() - W) / 2, A.y() + A.height() - H - A.height() / 10, W, H);
    show();
    raise();
    update();
    Hide->start();
}

void VolumeToast::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const QRectF R = rect();
    QPainterPath Box;
    Box.addRoundedRect(R.adjusted(1, 1, -1, -1), R.height() / 4, R.height() / 4);
    P.fillPath(Box, QColor(16, 18, 22, 225));
    const double Pad = R.height() * 0.28;
    const QRectF Label(Pad, 0, R.height() * 1.6, R.height());
    QFont F = font();
    F.setPixelSize(int(R.height() * 0.32));
    F.setBold(true);
    P.setFont(F);
    P.setPen(QColor(235, 238, 242));
    const QString Text = Shown.Percent < 0 ? QStringLiteral("—") : Shown.Muted ? QStringLiteral("Mute") : QString::number(Shown.Percent) + "%";
    P.drawText(Label, Qt::AlignVCenter | Qt::AlignLeft, Text);
    const QRectF Track(Label.right() + Pad / 2, R.height() / 2 - R.height() * 0.08, R.width() - Label.right() - Pad * 1.5, R.height() * 0.16);
    P.setPen(Qt::NoPen);
    P.setBrush(QColor(255, 255, 255, 50));
    P.drawRoundedRect(Track, Track.height() / 2, Track.height() / 2);
    if (Shown.Percent > 0 && !Shown.Muted)
    {
        QRectF Fill = Track;
        Fill.setWidth(Track.width() * std::min(100, Shown.Percent) / 100.0);
        P.setBrush(QColor(102, 192, 244));
        P.drawRoundedRect(Fill, Fill.height() / 2, Fill.height() / 2);
    }
}

// ---------------------------------------------------------------------------

VolumeKeys &VolumeKeys::instance()
{
    static VolumeKeys *V = new VolumeKeys();   // for the app's life
    return *V;
}

VolumeKeys::VolumeKeys()
{
    Repeat = new QTimer(this);
    connect(Repeat, &QTimer::timeout, this, &VolumeKeys::repeat);
    connect(this, &VolumeKeys::changed, this, [this](Volume::Level L) {
        if (!Toast) Toast = new VolumeToast();
        Toast->present(L);
    });
}

void VolumeKeys::follow()
{
    connect(&ControllerService::instance(), &ControllerService::uiEvent, this, &VolumeKeys::onUiEvent, Qt::UniqueConnection);
}

QString VolumeKeys::tool() const
{
    static const QString T = !QStandardPaths::findExecutable(QStringLiteral("wpctl")).isEmpty() ? QStringLiteral("wpctl")
                           : !QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty() ? QStringLiteral("pactl") : QString();
    return T;
}

void VolumeKeys::onUiEvent(const QString &Event, double Value)
{
    const int Delta = Event == QLatin1String("ui_volume_up") ? StepPercent : Event == QLatin1String("ui_volume_down") ? -StepPercent : 0;
    if (Event == QLatin1String("ui_volume_mute")) { if (Value > 0.5) toggleMute(); return; }
    if (!Delta) return;
    if (Value < 0.5) { if (Held == Delta) { Held = 0; Repeat->stop(); } return; }
    if (Held == Delta) return;   // the key's own repeat: ours paces it
    Held = Delta;
    Repeats = 0;
    step(Delta);
    Repeat->start(400);          // held: again after a moment, then steadily
}

void VolumeKeys::repeat()
{
    //A release that never came (the controller service gone mid-hold) must not step for ever: ~7 s of holding is the
    //whole range.
    if (!Held || ++Repeats > MaxRepeats) { Held = 0; Repeat->stop(); return; }
    step(Held);
    Repeat->setInterval(120);
}

void VolumeKeys::step(int Delta)
{
    if (tool().isEmpty()) return;
    run(Volume::StepCommand(tool(), Delta, Current.Percent), /*ThenRead=*/true);
}

void VolumeKeys::toggleMute()
{
    if (tool().isEmpty()) return;
    run(Volume::MuteCommand(tool()), /*ThenRead=*/true);
}

void VolumeKeys::run(const Volume::Command &C, bool ThenRead)
{
    if (C.first.isEmpty()) return;   // nothing to do (already at the top)
    auto *P = new QProcess(this);
    connect(P, &QProcess::finished, this, [this, P, ThenRead] { P->deleteLater(); if (ThenRead) refresh(); });
    connect(P, &QProcess::errorOccurred, P, &QObject::deleteLater);
    P->start(C.first, C.second);
}

void VolumeKeys::refresh()
{
    if (tool().isEmpty()) return;
    readNext(Volume::ReadCommands(tool()), {});
}

void VolumeKeys::readNext(std::vector<Volume::Command> Left, QStringList Outs)
{
    if (Left.empty())
    {
        const auto L = tool() == QLatin1String("wpctl") ? Volume::ParseWpctl(Outs.value(0)) : Volume::ParsePactl(Outs.value(0), Outs.value(1));
        if (L) { Current = *L; emit changed(Current); }
        return;
    }
    const Volume::Command C = Left.front();
    Left.erase(Left.begin());
    auto *P = new QProcess(this);
    connect(P, &QProcess::finished, this, [this, P, Left, Outs]() mutable {
        Outs << QString::fromUtf8(P->readAllStandardOutput());
        P->deleteLater();
        readNext(std::move(Left), std::move(Outs));
    });
    connect(P, &QProcess::errorOccurred, P, &QObject::deleteLater);
    P->start(C.first, C.second);
}
