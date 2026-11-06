#include "command.h"
#include "appmodel.h"     // networkingEnabled — a Node command while the node starts
#include "commonutils.h"   // LogRecorder — what a failing call logged
#include "ipfswrapper.h"   // Available — a Node command on an app with networking off

#include <algorithm>
#include <set>
#include <vector>

#include <cstdio>
#include <cstdlib>

namespace Command
{

Registrar::Registrar(void (*F)()) noexcept
{
    try { F(); }
    catch (const std::exception &E) { std::fprintf(stderr, "VidyaGod: command registration failed: %s\n", E.what()); std::abort(); }
    catch (...) { std::fprintf(stderr, "VidyaGod: command registration failed\n"); std::abort(); }
}

static std::map<std::string, Spec> &Registry()
{
    static std::map<std::string, Spec> R;   // function-local: registrars in other TUs run before or after, never against an unconstructed map
    return R;
}

void Register(Spec S)
{
    auto &R = Registry();
    if (R.count(S.Name)) throw std::logic_error("command registered twice: " + S.Name);
    // The CLI's own options are read wherever they appear: a command's option of the same name would never reach it.
    static const std::set<std::string> Global = { "json", "no-color", "no-colour", "data-dir", "wait", "verbose", "help",
                                                  "package-dir", "runtime-dir", "userdata-dir" };
    for (const Arg &A : S.Args)
        if (!A.Positional && Global.count(A.Name)) throw std::logic_error(S.Name + ": --" + A.Name + " is a global option");
    const std::string Name = S.Name;
    R.emplace(Name, std::move(S));
}

static std::map<std::string, std::string> &GroupRegistry()
{
    static std::map<std::string, std::string> G;
    return G;
}

void RegisterGroup(const std::string &Name, const std::string &Summary) { GroupRegistry()[Name] = Summary; }

const std::map<std::string, Spec> &All() { return Registry(); }
const std::map<std::string, std::string> &Groups() { return GroupRegistry(); }

const Spec *Find(const std::string &Name)
{
    const auto It = Registry().find(Name);
    return It == Registry().end() ? nullptr : &It->second;
}

static const char *NeedsName(Needs N)
{
    switch (N) { case Needs::Config: return "config"; case Needs::Model: return "model"; case Needs::Ui: return "ui"; case Needs::App: return "app"; }
    return "?";
}

bool WorksOffline(const std::string &Name)
{
    return Name == "ipfs status" || Name == "launch run" || Name == "launch build";
}

namespace { bool g_VerboseOutput = false; }
void SetVerboseOutput(bool On) { g_VerboseOutput = On; }
bool VerboseOutput() { return g_VerboseOutput; }

nlohmann::ordered_json Run(const std::string &Name, const nlohmann::ordered_json &Args, Context &Ctx)
{
    const Spec *S = Find(Name);
    if (!S) return {{"ok", false}, {"error", "no command '" + Name + "' ('VidyaGod help' lists them)"}, {"usage", true}};
    if ((S->Requires != Needs::Config && !Ctx.Model) || ((S->Requires == Needs::Ui || S->Requires == Needs::App) && !Ctx.Window))
        return {{"ok", false}, {"error", "'" + Name + "' needs the " + NeedsName(S->Requires) + ", which this run has not brought up"}};
    if (!Args.is_object()) return {{"ok", false}, {"error", "arguments must be a JSON object"}, {"usage", true}};
    //The running app with networking off has no node: say so at once instead of waiting on it.
    if (S->Node && Ctx.Live && !IpfsWrapper::Available() && !WorksOffline(Name))
    {
        if (Ctx.Model && Ctx.Model->networkingEnabled())   // on, and the node coming up (it starts after the window)
            return {{"ok", false}, {"error", "'" + Name + "' needs the network, and the app's node is still starting: run it again in a moment"}};
        return {{"ok", false}, {"error", "'" + Name + "' needs the network, and networking is off in the app: turn it on "
                                         "(VidyaGod app networking --set on), then run it again"}};
    }
    for (const Arg &A : S->Args)   // a JSON value of the wrong shape says what it should be, not what the parser met
    {
        if (A.Kind != Type::Json || A.Sample.empty() || !Args.contains(A.Name)) continue;
        const nlohmann::ordered_json &V = Args[A.Name];
        const char Want = A.Sample.find(" or ") != std::string::npos ? 0 : A.Sample.front();   // "x or y": either shape
        if ((Want == '{' && !V.is_object()) || (Want == '[' && !V.is_array()))
            return {{"ok", false}, {"error", "--" + A.Name + " takes a JSON " + (Want == '{' ? "object" : "array") + ", like "
                                             + A.Sample + " — this is " + (V.is_object() ? "an object" : V.is_array() ? "an array"
                                             : V.is_string() ? "a string" : V.is_number() ? "a number" : V.is_boolean() ? "a boolean" : "null")},
                    {"usage", true}};
    }
    //What went wrong is in the error itself: the ERR lines this call logged (an edit that did not apply, a mount
    //helper's own output) follow the command's message, instead of "the log says why".
    LogRecorder Said({});
    const auto Why = [&Said](std::string Msg) {
        std::vector<std::string> Lines;
        for (const auto &E : Said.Take())
            if (E.Level == LogLevel::ERR && std::find(Lines.begin(), Lines.end(), E.Message) == Lines.end() && E.Message != Msg) Lines.push_back(E.Message);
        const size_t Cap = 12;
        for (size_t I = 0; I < Lines.size() && I < Cap; ++I) Msg += "\n  " + Lines[I];
        if (Lines.size() > Cap) Msg += "\n  … and " + std::to_string(Lines.size() - Cap) + " more (-v shows the log)";
        return Msg;
    };
    try
    {
        return {{"ok", true}, {"result", S->Run(Ctx, Args)}};
    }
    catch (const nlohmann::ordered_json::exception &E)
    {
        //The parser's "[json.exception.type_error.302] type must be string, but is number": somewhere inside a JSON
        //value something has another type than the command reads. Say so, with the samples of what it takes.
        std::string Msg = E.what();
        if (const auto Close = Msg.find("] "); Msg.rfind("[json.exception", 0) == 0 && Close != std::string::npos) Msg = Msg.substr(Close + 2);
        //Only a JSON value the caller gave is the caller's mistake; one read from disk (a corrupt node file) is a failure.
        std::string Like;
        bool Given = false;
        for (const Arg &A : S->Args)
            if (A.Kind == Type::Json && Args.contains(A.Name))
            {
                Given = true;
                if (!A.Sample.empty()) Like += (Like.empty() ? "" : "; ") + ("--" + A.Name + " like " + A.Sample);
            }
        if (!Given) return {{"ok", false}, {"error", Why(E.what())}};
        return {{"ok", false}, {"error", Why("a value is not what '" + Name + "' reads (" + Msg + ")" + (Like.empty() ? "" : ": " + Like))}, {"usage", true}};
    }
    catch (const BadArgs &E)
    {
        return {{"ok", false}, {"error", E.what()}, {"usage", true}};
    }
    catch (const std::exception &E)
    {
        return {{"ok", false}, {"error", Why(E.what())}};
    }
}

nlohmann::ordered_json List()
{
    nlohmann::ordered_json Out = nlohmann::ordered_json::array();
    static const char *TypeNames[] = { "string", "bool", "int", "json" };
    for (const auto &[N, S] : Registry())
    {
        nlohmann::ordered_json A = nlohmann::ordered_json::array();
        for (const Arg &X : S.Args)
            A.push_back({{"name", X.Name}, {"type", TypeNames[static_cast<int>(X.Kind)]}, {"help", X.Help},
                         {"positional", X.Positional}, {"required", X.Required}, {"choices", X.Choices}});
        Out.push_back({{"name", N}, {"summary", S.Summary}, {"help", S.Help}, {"args", A}, {"examples", S.Examples},
                       {"covers", S.Covers}, {"needs", NeedsName(S.Requires)}, {"node", S.Node}});
    }
    return Out;
}

static const nlohmann::ordered_json &Need(const nlohmann::ordered_json &Args, const char *Key)
{
    if (!Args.contains(Key)) throw BadArgs(std::string("missing argument '") + Key + "'");
    return Args[Key];
}

namespace
{
size_t Distance(const std::string &A, const std::string &B)
{
    std::vector<size_t> Prev(B.size() + 1), Cur(B.size() + 1);
    for (size_t J = 0; J <= B.size(); ++J) Prev[J] = J;
    for (size_t I = 1; I <= A.size(); ++I)
    {
        Cur[0] = I;
        for (size_t J = 1; J <= B.size(); ++J)
            Cur[J] = std::min({ Prev[J] + 1, Cur[J - 1] + 1, Prev[J - 1] + (A[I - 1] == B[J - 1] ? 0 : 1) });
        std::swap(Prev, Cur);
    }
    return Prev[B.size()];
}
}

std::string Nearest(const std::string &W, const std::vector<std::string> &Of)
{
    std::string Best;
    size_t D = std::string::npos;
    for (const std::string &C : Of)
        if (const size_t X = Distance(W, C); X < D) { D = X; Best = C; }
    return D <= std::max<size_t>(2, W.size() / 3) ? Best : std::string();
}

std::string Str(const nlohmann::ordered_json &Args, const char *Key)
{
    const auto &V = Need(Args, Key);
    if (!V.is_string()) throw BadArgs(std::string("argument '") + Key + "' must be a string");
    return V.get<std::string>();
}

std::string Str(const nlohmann::ordered_json &Args, const char *Key, const std::string &Default)
{
    return Args.contains(Key) ? Str(Args, Key) : Default;
}

bool Bool(const nlohmann::ordered_json &Args, const char *Key)
{
    const auto &V = Need(Args, Key);
    if (!V.is_boolean()) throw BadArgs(std::string("argument '") + Key + "' must be true or false");
    return V.get<bool>();
}

bool Bool(const nlohmann::ordered_json &Args, const char *Key, bool Default)
{
    return Args.contains(Key) ? Bool(Args, Key) : Default;
}

long long Int(const nlohmann::ordered_json &Args, const char *Key)
{
    const auto &V = Need(Args, Key);
    if (!V.is_number_integer()) throw BadArgs(std::string("argument '") + Key + "' must be an integer");
    return V.get<long long>();
}

long long Int(const nlohmann::ordered_json &Args, const char *Key, long long Default)
{
    return Args.contains(Key) ? Int(Args, Key) : Default;
}

} // namespace Command
