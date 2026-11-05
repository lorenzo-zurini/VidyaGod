#include "command.h"

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
    switch (N) { case Needs::Config: return "config"; case Needs::Model: return "model"; case Needs::Ui: return "ui"; }
    return "?";
}

nlohmann::ordered_json Run(const std::string &Name, const nlohmann::ordered_json &Args, Context &Ctx)
{
    const Spec *S = Find(Name);
    if (!S) return {{"ok", false}, {"error", "no command '" + Name + "' ('VidyaGod help' lists them)"}, {"usage", true}};
    if ((S->Requires != Needs::Config && !Ctx.Model) || (S->Requires == Needs::Ui && !Ctx.Window))
        return {{"ok", false}, {"error", "'" + Name + "' needs the " + NeedsName(S->Requires) + ", which this run has not brought up"}};
    if (!Args.is_object()) return {{"ok", false}, {"error", "arguments must be a JSON object"}, {"usage", true}};
    try
    {
        return {{"ok", true}, {"result", S->Run(Ctx, Args)}};
    }
    catch (const BadArgs &E)
    {
        return {{"ok", false}, {"error", E.what()}, {"usage", true}};
    }
    catch (const std::exception &E)
    {
        return {{"ok", false}, {"error", E.what()}};
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
                         {"positional", X.Positional}, {"required", X.Positional || X.Required}, {"choices", X.Choices}});
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
