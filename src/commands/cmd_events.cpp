// `VidyaGod events …` — what happens in the app, as it happens: every signal its own objects emit (a download finished,
// a friend's library arrived, the catalog was rebuilt), with its arguments, in order (EventLog).
#include "command.h"
#include "eventlog.h"
#include "uifind.h"

#include <QWidget>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

namespace
{
using json = nlohmann::ordered_json;

std::string EventsText(const json &R, bool Colour)
{
    std::string Out;
    const json &Evs = R.is_object() && R.contains("events") ? R["events"] : R;
    for (const auto &E : Evs)
    {
        std::string Args;
        for (const auto &[K, V] : E["args"].items()) Args += (Args.empty() ? "" : "  ") + K + "=" + (V.is_string() ? V.get<std::string>() : V.dump());
        const std::string When = E.value("time", std::string()), Obj = E.value("object", std::string()), Sig = E.value("signal", std::string());
        Out += (Colour ? "\033[2m" + When + "\033[0m" : When) + "  " + (Colour ? "\033[36m" + Obj + "\033[0m" : Obj) + "."
               + (Colour ? "\033[1m" + Sig + "\033[0m" : Sig) + (Args.empty() ? "" : "  " + Args) + "\n";
    }
    if (Evs.empty() && !(R.is_object() && R.contains("next"))) Out = Colour ? "\033[2m(no events)\033[0m\n" : "(no events)\n";
    return Out;
}
} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    Command::RegisterGroup("events", "what happens in the app as it happens: every signal, with its arguments");

    Command::Register({
        "events list", "the events so far (the last 5000), oldest first",
        "Every signal the app's objects emitted — the model, the IPFS model, the download manager, the friend service, "
        "each tab and view — with its arguments and a sequence number. Filter by object or signal (a part of the name).",
        { Command::Opt("since", "only events after this sequence number", Type::Int),
          Command::Opt("object", "only from objects whose name contains this"),
          Command::Opt("signal", "only signals whose name contains this"),
          Command::Opt("limit", "at most this many (default 1000)", Type::Int) },
        { "events list", "events list --signal download --json", "events list --object AppModel --since 120" },
        { "{signal}", "EventLog::Since", "EventLog::Latest" }, Needs::Model, false,
        [](Command::Context &, const json &A) {
            return json{{"events", EventLog::Since(static_cast<std::uint64_t>(Command::Int(A, "since", 0)), Command::Str(A, "object", ""),
                                                   Command::Str(A, "signal", ""), static_cast<std::size_t>(Command::Int(A, "limit", 1000)))},
                        {"next", EventLog::Latest()}};
        }, &EventsText });

    Command::Register({
        "events wait", "wait until a signal is emitted, then print it",
        "Block until the app emits a matching signal (after this call starts), up to --timeout seconds — how a script "
        "waits for what it started: `downloads start …` then `events wait --signal downloadFinished`.",
        { Command::Opt("signal", "the signal (a part of its name)", Type::String, true),
          Command::Opt("object", "only from objects whose name contains this"),
          Command::Opt("timeout", "give up after this many seconds (default 60)", Type::Int) },
        { "events wait --signal catalogChanged", "events wait --signal friendLibrary --timeout 120" },
        { "EventLog::Since" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            const std::uint64_t From = EventLog::Latest();
            const std::string Sig = Command::Str(A, "signal"), Obj = Command::Str(A, "object", "");
            const qint64 Budget = Command::Int(A, "timeout", 60) * 1000;
            QElapsedTimer T;
            T.start();
            while (T.elapsed() < Budget)
            {
                const json Hit = EventLog::Since(From, Obj, Sig, 1);
                if (!Hit.empty()) return json{{"events", Hit}, {"next", Hit.back()["seq"]}};
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                QThread::msleep(20);
            }
            throw std::runtime_error("no '" + Sig + "' within " + std::to_string(Budget / 1000) + " s");
        }, &EventsText });

    Command::Register({
        "events follow", "print events as they happen, until interrupted",
        "Stream the app's signals live (Ctrl-C to stop, or --for seconds). Needs the app open on the data dir: a "
        "command run on its own has no app to watch.",
        { Command::Opt("object", "only from objects whose name contains this"),
          Command::Opt("signal", "only signals whose name contains this"),
          Command::Opt("for", "stop after this many seconds", Type::Int),
          Command::Opt("since", "start after this sequence number (default: now)", Type::Int) },
        { "events follow", "events follow --signal download", "events follow --object FriendsManager --for 30" },
        { "EventLog::Since" }, Needs::App, false,
        // The CLI calls this over and over with "since": each call returns what is new (CliApp's follow loop).
        [](Command::Context &, const json &A) {
            return json{{"events", EventLog::Since(static_cast<std::uint64_t>(Command::Int(A, "since", 0)), Command::Str(A, "object", ""),
                                                   Command::Str(A, "signal", ""), 1000)},
                        {"next", EventLog::Latest()}};
        }, &EventsText });

    Command::Register({
        "events attach", "record the signals of a window or control opened later",
        "The app records its models, managers and main window from the start; a dialog or panel opened since (the "
        "launch window, the package editor) is added here — it and everything inside it.",
        { Command::Pos("control", "the window or control (its name, as ui tree shows it)"),
          Command::Flag("only", "just that object, not what is inside it") },
        { "events attach PreLaunchWindow", "events attach PackageEditor" },
        { "EventLog::Attach", "EventLog::AttachTree" }, Needs::App, false,
        [](Command::Context &, const json &A) {
            QWidget *W = UiFind::Named(Command::Str(A, "control"));
            if (Command::Bool(A, "only", false)) EventLog::Attach(W); else EventLog::AttachTree(W);
            return json{{"recording", W->objectName().isEmpty() ? W->metaObject()->className() : W->objectName().toStdString()}};
        }, {} });
}
