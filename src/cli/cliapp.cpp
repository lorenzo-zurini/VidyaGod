#include "cliapp.h"
#include "cliapp_p.h"   // Style, the renderer (clirender.cpp)

#include <fstream>
#include "apppaths.h"
#include "theme.h"
#include "command.h"
#include "commandserver.h"
#include "appmodel.h"
#include "mainwindow.h"
#include "ipfswrapper.h"
#include "vgipfsapi.h"
#include "commonutils.h"
#include "platform/platform.h"
#include "a11ynames.h"
#include "eventlog.h"

#include <QApplication>
#include <QDir>
#include <QTimer>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <cctype>
#include <chrono>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>   // GetConsoleScreenBufferInfo: the console's width
#endif

#ifndef VIDYAGOD_VERSION
#define VIDYAGOD_VERSION "dev"
#endif

namespace CliApp
{

using namespace detail;

namespace
{

struct Globals
{
    std::string DataDir, PackageDir, RuntimeDir, UserDataDir;   // the last three: per-launch path overrides (AppPaths)
    bool Json = false, NoColour = false, Verbose = false, Help = false;
    bool Online = false;   // --online: start the node for this call though Settings → Networking is off
    bool NoWait = false;   // --no-wait: fail at once when another command holds the data dir, instead of waiting
    bool Offline = false;  // --offline: no node for this call, though Settings → Networking is on
    bool Beside = false;   // --bypass-single-instance-lock: run in this process beside whatever holds the data dir, on a
                           // config it never writes (no lock taken, nothing sent to the app)
    long long WaitMs = 0;
};

// The global options a CLI-app line may carry anywhere; the value-taking ones consume the next word.
bool TakeGlobal(const std::vector<std::string> &A, size_t &I, Globals &G)
{
    const std::string &W = A[I];
    auto Value = [&](std::string &Out) {
        if (I + 1 >= A.size()) throw Command::BadArgs(W + " needs a value");
        Out = A[++I];
        return true;
    };
    if (W == "--json") return G.Json = true;
    if (W == "--no-color" || W == "--no-colour") return G.NoColour = true;
    if (W == "-v" || W == "--verbose") return G.Verbose = true;
    if (W == "-h" || W == "--help") return G.Help = true;
    if (W == "--online") return G.Online = true;
    if (W == "--no-wait") return G.NoWait = true;
    if (W == "--offline") return G.Offline = true;
    if (W == "--bypass-single-instance-lock") return G.Beside = true;
    if (W == "--data-dir") return Value(G.DataDir);
    if (W.rfind("--data-dir=", 0) == 0) { G.DataDir = W.substr(11); return true; }
    if (W == "--package-dir") return Value(G.PackageDir);
    if (W == "--runtime-dir") return Value(G.RuntimeDir);
    if (W == "--userdata-dir") return Value(G.UserDataDir);
    if (W == "--wait" || W.rfind("--wait=", 0) == 0)
    {
        std::string V;
        if (W == "--wait") Value(V); else V = W.substr(7);
        try { G.WaitMs = std::stoll(V); } catch (...) { throw Command::BadArgs("--wait takes milliseconds, not '" + V + "'"); }
        return true;
    }
    return false;
}


using Command::Nearest;

std::vector<std::string> Words(const std::string &S)
{
    std::vector<std::string> Out;
    std::istringstream In(S);
    for (std::string W; In >> W;) Out.push_back(W);
    return Out;
}

std::string Join(const std::vector<std::string> &V, size_t From, size_t To, const std::string &Sep = " ")
{
    std::string Out;
    for (size_t I = From; I < To && I < V.size(); ++I) Out += (I > From ? Sep : "") + V[I];
    return Out;
}

// The commands in a group ("" = every command), by name.
std::vector<const Command::Spec *> InGroup(const std::string &Group)
{
    std::vector<const Command::Spec *> Out;
    for (const auto &[N, S] : Command::All())
        if (Group.empty() || N.rfind(Group + " ", 0) == 0) Out.push_back(&S);
    return Out;
}

std::set<std::string> GroupNames()
{
    std::set<std::string> Out;
    for (const auto &[N, S] : Command::All()) Out.insert(Words(N).front());
    for (const auto &[G, Sum] : Command::Groups()) Out.insert(G);
    return Out;
}

std::string Placeholder(const Command::Arg &A) { return "<" + A.Name + ">"; }

std::string UsageLine(const Command::Spec &S, const Style &St)
{
    std::string U = St.B("VidyaGod " + S.Name);
    for (const auto &A : S.Args) if (A.Positional) U += A.Required ? " " + Placeholder(A) : " [" + Placeholder(A) + "]";
    for (const auto &A : S.Args)
        if (!A.Positional)
        {
            const std::string O = "--" + A.Name + (A.Kind == Command::Type::Bool ? "" : " " + Placeholder(A));
            U += " " + (A.Required ? O : "[" + O + "]");
        }
    return U;
}

void Wrap(std::ostream &Out, const std::string &Text, size_t Indent, size_t Width = 100)
{
    std::istringstream Lines(Text);
    for (std::string Line; std::getline(Lines, Line);)
    {
        std::string Cur;
        for (const std::string &W : Words(Line))
        {
            if (!Cur.empty() && Indent + Cur.size() + 1 + W.size() > Width) { Out << std::string(Indent, ' ') << Cur << "\n"; Cur.clear(); }
            Cur += (Cur.empty() ? "" : " ") + W;
        }
        Out << std::string(Indent, ' ') << Cur << "\n";
    }
}

// The root help's first steps: real command lines (the example check parses them too).
const std::vector<std::pair<std::string, std::string>> &StartWith()
{
    static const std::vector<std::pair<std::string, std::string>> L = {
        { "catalog shelf", "the games here, one line each" },
        { "launch plan --tile 749", "what launching a game would do, without running it" },
        { "ui tree", "the open app's windows, every control and its state" },
    };
    return L;
}

void PrintRootHelp(const Style &St)
{
    std::ostream &O = std::cout;
    O << St.B("VidyaGod") << " " << St.Dim(VIDYAGOD_VERSION) << " — play, package and share games\n\n";
    O << St.B("Usage:") << " VidyaGod <group> <command> [<arguments>…] [--options]\n";
    O << "       VidyaGod                 (no arguments) opens the app\n\n";
    // The groups by what they are for; a group not placed here is listed under "More" (so none is ever hidden).
    static const std::vector<std::pair<const char *, std::vector<std::string>>> Sections = {
        { "Play",                 { "library", "catalog", "launch", "instances", "runners", "deps", "grafts" } },
        { "Share",                { "friends", "received", "updates", "sources", "downloads", "transfers", "queue", "ipfs" } },
        { "Make packages",        { "packages", "build", "editor", "canvas", "author", "edits", "registry", "vfs" } },
        { "The format",           { "node", "fold", "vars", "graph", "cid", "content" } },
        { "The app",              { "app", "ui", "events", "keys", "util" } },
    };
    size_t W = 0;
    for (const std::string &G : GroupNames()) W = std::max(W, G.size());
    std::set<std::string> Placed;
    const auto GroupLine = [&](const std::string &G) {
        const auto It = Command::Groups().find(G);
        O << "  " << St.Cyan(G) << std::string(W - G.size() + 3, ' ')
          << (It == Command::Groups().end() ? std::string() : It->second) << "\n";
        Placed.insert(G);
    };
    for (const auto &[Title, Gs] : Sections)
    {
        O << St.B(std::string(Title) + ":") << "\n";
        for (const std::string &G : Gs) if (GroupNames().count(G)) GroupLine(G);
        O << "\n";
    }
    bool More = false;
    for (const std::string &G : GroupNames())
        if (!Placed.count(G)) { if (!More) O << St.B("More:") << "\n"; More = true; GroupLine(G); }
    if (More) O << "\n";
    O << St.B("Start with:") << "\n";
    for (const auto &[Line, What] : StartWith())
        O << "  VidyaGod " << Line << std::string(Line.size() < 30 ? 30 - Line.size() : 1, ' ') << St.Dim(What) << "\n";
    O << "  VidyaGod commands" << std::string(22, ' ') << St.Dim("every command, one line each (--json: all of their help)") << "\n";
    O << "\n" << St.B("Global options:") << "\n"
      << "  --data-dir <dir>   the data dir to work on (default ~/.VidyaGod, or the portable one)\n"
      << "  --json             print the exact result as JSON\n"
      << "  --no-color         plain output (also: NO_COLOR set, or not a terminal)\n"
      << "  --wait <ms>        keep running this long after the call, for what it started\n"
      << "  --online           go online for this call though networking is off (Settings → Networking)\n"
      << "  --no-wait          fail at once if another command is using the data dir (at a terminal it waits for it)\n"
      << "  --offline          no network for this call, though networking is on\n"
      << "  --bypass-single-instance-lock  run here, beside the app or a command on the data dir, never writing the config\n"
      << "  --package-dir <d>  a launch's package folder, instead of the node's own (also --runtime-dir, --userdata-dir)\n"
      << "  -v, --verbose      the engine's log on stderr\n"
      << "  -h, --help         help for whatever precedes it\n\n";
    O << "Also: " << St.B("VidyaGod version") << ", " << St.B("VidyaGod completion bash|zsh|fish") << " (tab completion).\n";
    O << "With the app open on the data dir, commands run inside it, on its live state; otherwise in this process.\n";
    O << "Run " << St.B("VidyaGod help <group>") << " for a group's commands, " << St.B("VidyaGod help <group> <command>")
      << " for one.\n";
}

void PrintGroupHelp(const std::string &G, const Style &St)
{
    std::ostream &O = std::cout;
    const auto It = Command::Groups().find(G);
    O << St.B("VidyaGod " + G) << (It == Command::Groups().end() ? "" : " — " + It->second) << "\n\n";
    O << St.B("Usage:") << " VidyaGod " << G << " <command> [<arguments>…] [--options]\n\n" << St.B("Commands:") << "\n";
    const auto Cmds = InGroup(G);
    size_t W = 0;
    for (const auto *S : Cmds) W = std::max(W, S->Name.size() - G.size() - 1);
    for (const auto *S : Cmds)
    {
        const std::string Rest = S->Name.substr(G.size() + 1);
        O << "  " << St.Cyan(Rest) << std::string(W - Rest.size() + 3, ' ') << S->Summary << "\n";
    }
    O << "\nRun " << St.B("VidyaGod help " + G + " <command>") << " for one command.\n";
}

void PrintCommandHelp(const Command::Spec &S, const Style &St)
{
    std::ostream &O = std::cout;
    O << St.B("VidyaGod " + S.Name) << " — " << S.Summary << "\n\n";
    O << St.B("Usage:") << " " << UsageLine(S, St) << "\n";
    if (!S.Help.empty()) { O << "\n"; Wrap(O, S.Help, 2); }
    std::vector<const Command::Arg *> Pos, Opt;
    for (const auto &A : S.Args) (A.Positional ? Pos : Opt).push_back(&A);
    size_t W = 0;
    for (const auto *A : Pos) W = std::max(W, Placeholder(*A).size());
    for (const auto *A : Opt) W = std::max(W, A->Name.size() + 2 + (A->Kind == Command::Type::Bool ? 0 : Placeholder(*A).size() + 1));
    auto Row = [&](const std::string &Left, const Command::Arg &A) {
        std::string H = A.Help;
        if (!A.Choices.empty()) H += " (one of: " + Join(A.Choices, 0, A.Choices.size(), ", ") + ")";
        if (!A.Sample.empty()) H += ", like " + A.Sample;
        if (!A.Positional && A.Required) H += " [required]";
        O << "  " << St.Cyan(Left) << std::string(W - Left.size() + 3, ' ') << H << "\n";
    };
    if (!Pos.empty()) { O << "\n" << St.B("Arguments:") << "\n"; for (const auto *A : Pos) Row(Placeholder(*A), *A); }
    if (!Opt.empty())
    {
        O << "\n" << St.B("Options:") << "\n";
        for (const auto *A : Opt) Row("--" + A->Name + (A->Kind == Command::Type::Bool ? "" : " " + Placeholder(*A)), *A);
    }
    if (std::any_of(S.Args.begin(), S.Args.end(), [](const Command::Arg &A) { return A.Kind == Command::Type::Json; }))
        O << "\n" << St.Dim("A JSON value is given as the JSON itself, as @file (that file's JSON), or as - (read from stdin).") << "\n";
    if (!S.Examples.empty())
    {
        O << "\n" << St.B("Examples:") << "\n";
        for (const auto &E : S.Examples) O << "  " << St.Dim("$") << " VidyaGod " << E << "\n";
    }
    O << "\n" << St.Dim("Runs against: ") << (S.Requires == Command::Needs::App ? "the running app (start VidyaGod on the data dir first)"
                                              : S.Requires == Command::Needs::Ui ? "the app's windows"
                                              : S.Requires == Command::Needs::Model ? "the app's model" : "the config")
      << (S.Node ? St.Dim(" · needs the IPFS node") : "") << "\n";
    if (!S.Covers.empty()) O << St.Dim("Drives: " + Join(S.Covers, 0, S.Covers.size(), ", ")) << "\n";
}

// Positionals in order, --name value / --name=value, --flag / --no-flag; every value typed by its Arg.
// A line split as a shell splits it: words, 'single' and "double" quotes, backslash escapes outside single quotes.
std::vector<std::string> ShellWords(const std::string &Line)
{
    std::vector<std::string> Out;
    std::string W;
    bool In = false;
    char Quote = 0;
    for (size_t I = 0; I < Line.size(); ++I)
    {
        const char C = Line[I];
        if (Quote == '\'') { if (C == '\'') Quote = 0; else W += C; continue; }
        if (C == '\\' && I + 1 < Line.size() && Quote != '\'') { W += Line[++I]; In = true; continue; }
        if (Quote == '"') { if (C == '"') Quote = 0; else W += C; continue; }
        if (C == '\'' || C == '"') { Quote = C; In = true; continue; }
        if (C == ' ' || C == '\t') { if (In) Out.push_back(W); W.clear(); In = false; continue; }
        W += C; In = true;
    }
    if (In) Out.push_back(W);
    return Out;
}

// ReadInputs false (checking an example): a JSON @file or - is taken as given, nothing is read.
json ParseArgs(const Command::Spec &S, const std::vector<std::string> &Rest, bool ReadInputs = true)
{
    json Out = json::object();
    size_t NextPos = 0;
    std::vector<const Command::Arg *> Pos;
    for (const auto &A : S.Args) if (A.Positional) Pos.push_back(&A);
    auto Typed = [&](const Command::Arg &A, const std::string &V) -> json {
        switch (A.Kind)
        {
        case Command::Type::Int:
        {
            size_t N = 0;
            long long X = 0;
            try { X = std::stoll(V, &N); } catch (...) { N = 0; }
            if (V.empty() || N != V.size()) throw Command::BadArgs(Placeholder(A) + " must be a whole number, not '" + V + "'");
            return X;
        }
        case Command::Type::Bool:
            if (V == "true" || V == "on" || V == "yes" || V == "1") return true;
            if (V == "false" || V == "off" || V == "no" || V == "0") return false;
            throw Command::BadArgs(Placeholder(A) + " must be on or off, not '" + V + "'");
        case Command::Type::Json:
        {
            // @path: the JSON is that file's (a node, a list of targets); - is stdin.
            std::string Text = V;
            if (!ReadInputs && ((V.size() > 1 && V[0] == '@') || V == "-")) return V;
            if (V.size() > 1 && V[0] == '@')
            {
                std::ifstream In(V.substr(1), std::ios::binary);
                if (!In) throw Command::BadArgs(Placeholder(A) + ": cannot read " + V.substr(1));
                Text.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
            }
            else if (V == "-") Text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
            json J = json::parse(Text, nullptr, false);
            if (J.is_discarded()) throw Command::BadArgs(Placeholder(A) + " must be JSON");
            return J;
        }
        case Command::Type::String:
            if (!A.Choices.empty() && std::find(A.Choices.begin(), A.Choices.end(), V) == A.Choices.end())
            {
                const std::string Near = Nearest(V, A.Choices);
                throw Command::BadArgs(Placeholder(A) + " cannot be '" + V + "' (one of: " + Join(A.Choices, 0, A.Choices.size(), ", ")
                                       + ")" + (Near.empty() ? "" : " — did you mean '" + Near + "'?"));
            }
            return V;
        }
        return V;
    };
    for (size_t I = 0; I < Rest.size(); ++I)
    {
        const std::string &W = Rest[I];
        if (W.size() > 2 && W.rfind("--", 0) == 0)
        {
            std::string Key = W.substr(2), Val;
            bool HasVal = false;
            if (const size_t Eq = Key.find('='); Eq != std::string::npos) { Val = Key.substr(Eq + 1); Key.resize(Eq); HasVal = true; }
            bool Negated = false;
            const Command::Arg *A = nullptr;
            for (const auto &X : S.Args) if (!X.Positional && X.Name == Key) A = &X;
            if (!A && Key.rfind("no-", 0) == 0)
                for (const auto &X : S.Args)
                    if (!X.Positional && X.Kind == Command::Type::Bool && X.Name == Key.substr(3)) { A = &X; Negated = true; }
            if (!A)
            {
                std::vector<std::string> Names;
                for (const auto &X : S.Args) if (!X.Positional) Names.push_back(X.Name);
                const std::string Near = Nearest(Key, Names);
                throw Command::BadArgs("unknown option --" + Key + (Near.empty() ? "" : " — did you mean --" + Near + "?"));
            }
            if (A->Kind == Command::Type::Bool && !HasVal) { Out[A->Name] = !Negated; continue; }
            if (!HasVal)
            {
                if (I + 1 >= Rest.size()) throw Command::BadArgs("--" + Key + " needs " + Placeholder(*A));
                Val = Rest[++I];
            }
            //A text option given again is a list of what it was given (--base a --base b): a value may hold any
            //character — a Steam depot's file name has commas in it.
            if (A->Kind == Command::Type::String && Out.contains(A->Name))
            {
                if (!Out[A->Name].is_array()) Out[A->Name] = json::array({ Out[A->Name] });
                Out[A->Name].push_back(Typed(*A, Val));
            }
            else Out[A->Name] = Typed(*A, Val);
            continue;
        }
        if (NextPos >= Pos.size()) throw Command::BadArgs("unexpected argument '" + W + "'");
        Out[Pos[NextPos]->Name] = Typed(*Pos[NextPos], W);
        ++NextPos;
    }
    if (NextPos < Pos.size() && Pos[NextPos]->Required) throw Command::BadArgs("missing " + Placeholder(*Pos[NextPos]));
    for (const auto &A : S.Args)
        if (!A.Positional && A.Required && !Out.contains(A.Name)) throw Command::BadArgs("missing --" + A.Name);
    return Out;
}

// Every command's examples, read as the command line reads them: each must name a command and parse against it.
// Returns the ones that do not, with why.
json CheckExamples()
{
    json Bad = json::array();
    std::vector<std::pair<std::string, std::string>> Lines;   // (whose, the line)
    for (const auto &[Name, S] : Command::All())
        for (const std::string &Ex : S.Examples) Lines.emplace_back(Name, Ex);
    for (const auto &[Line, What] : StartWith()) Lines.emplace_back("help", Line);
    for (const auto &[Name, Ex] : Lines)
    {
        std::vector<std::string> A = ShellWords(Ex), Tokens;
        if (!A.empty() && A.front() == "VidyaGod") A.erase(A.begin());
        Globals G;
        try
        {
            for (size_t I = 0; I < A.size(); ++I)
                if (!TakeGlobal(A, I, G)) Tokens.push_back(A[I]);
            const Command::Spec *X = nullptr;
            size_t Used = 0;
            for (size_t N = Tokens.size(); N > 0 && !X; --N)
                if ((X = Command::Find(Join(Tokens, 0, N)))) Used = N;
            if (!X) throw Command::BadArgs("names no command");
            ParseArgs(*X, { Tokens.begin() + static_cast<long>(Used), Tokens.end() }, false);
        }
        catch (const Command::BadArgs &E) { Bad.push_back({{"command", Name}, {"example", Ex}, {"error", E.what()}}); }
    }
    return Bad;
}


void PrintCompletion(const std::string &Shell, const Style &St)
{
    if (Shell == "bash")
        std::cout << "# VidyaGod bash completion — eval \"$(VidyaGod completion bash)\"\n"
                     "_vidyagod() {\n"
                     "  local IFS=$'\\n'\n"
                     "  COMPREPLY=( $(VidyaGod __complete \"${COMP_WORDS[@]:1:COMP_CWORD}\" 2>/dev/null) )\n"
                     "}\n"
                     "complete -o default -F _vidyagod VidyaGod\n";
    else if (Shell == "zsh")
        std::cout << "#compdef VidyaGod\n# VidyaGod zsh completion — eval \"$(VidyaGod completion zsh)\"\n"
                     "_vidyagod() {\n"
                     "  local -a c; c=(\"${(@f)$(VidyaGod __complete \"${words[@]:1:$((CURRENT-1))}\" 2>/dev/null)}\")\n"
                     "  compadd -a c\n"
                     "}\n"
                     "compdef _vidyagod VidyaGod\n";
    else if (Shell == "fish")
        std::cout << "# VidyaGod fish completion — VidyaGod completion fish | source\n"
                     "complete -c VidyaGod -f -a '(VidyaGod __complete (commandline -opc)[2..] (commandline -ct) 2>/dev/null)'\n";
    else
        throw Command::BadArgs("completion is for bash, zsh or fish, not '" + Shell + "'");
    (void)St;
}

// The candidates for the last word, given the words before it: groups, then commands, then options.
void PrintCandidates(const std::vector<std::string> &W)
{
    const std::string Cur = W.empty() ? std::string() : W.back();
    const std::vector<std::string> Before(W.begin(), W.empty() ? W.begin() : W.end() - 1);
    std::set<std::string> Out;
    auto Offer = [&](const std::string &C) { if (C.rfind(Cur, 0) == 0) Out.insert(C); };
    if (Before.empty())
    {
        for (const std::string &G : GroupNames()) Offer(G);
        for (const char *X : { "help", "commands", "version", "completion" }) Offer(X);
    }
    else if (Before.front() == "help" || Before.front() == "completion")
    {
        if (Before.front() == "completion") for (const char *X : { "bash", "zsh", "fish" }) Offer(X);
        else if (Before.size() == 1) for (const std::string &G : GroupNames()) Offer(G);
        else for (const auto *S : InGroup(Before[1])) Offer(Words(S->Name)[1]);
    }
    else
    {
        const std::string Prefix = Join(Before, 0, Before.size());
        const Command::Spec *S = nullptr;
        for (size_t N = Before.size(); N > 0 && !S; --N) S = Command::Find(Join(Before, 0, N));
        if (S)
        {
            for (const auto &A : S->Args)
                if (!A.Positional) { Offer("--" + A.Name); if (A.Kind == Command::Type::Bool) Offer("--no-" + A.Name); }
            for (const auto &A : S->Args) for (const auto &C : A.Choices) Offer(C);
        }
        else
            for (const auto &[N, Spec] : Command::All())
            {
                const auto NW = Words(N);
                if (NW.size() > Before.size() && Join(NW, 0, Before.size()) == Prefix) Offer(NW[Before.size()]);
            }
        for (const char *X : { "--json", "--no-color", "--data-dir", "--wait", "--package-dir", "--runtime-dir", "--userdata-dir", "--verbose", "--help", "--online", "--no-wait", "--offline", "--bypass-single-instance-lock" }) Offer(X);
    }
    for (const auto &C : Out) std::cout << C << "\n";
}

// Wait until the node is up (a headless run that needs it) — or give up after a while, the command reports what is missing.
void AwaitNode()
{
    for (int I = 0; I < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++I)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

// A long call says it is still at work: after a second, one line on stderr — the command and how long it has run —
// redrawn in place and wiped before anything is printed. Only on a terminal, and never with -v (the log is the
// progress there).
class Ticker
{
public:
    void Start(const std::string &What)
    {
        if (!IsTty(2) || Running) return;
        Running = true;
        Thread = std::thread([this, What] {
            static const char *Spin[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
            const auto Since = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> Lk(Mu);
            for (int I = 0; !Cv.wait_for(Lk, std::chrono::milliseconds(120), [this] { return !Running; }); ++I)
            {
                const auto Secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - Since).count();
                if (Secs < 1) continue;
                std::cerr << "\r\033[K\033[2m" << Spin[I % 10] << " " << What << " · " << Secs << " s\033[0m" << std::flush;
                Drawn = true;
            }
            if (Drawn) std::cerr << "\r\033[K" << std::flush;
        });
    }
    void Stop()
    {
        if (!Running) return;
        { std::lock_guard<std::mutex> Lk(Mu); Running = false; }
        Cv.notify_all();
        if (Thread.joinable()) Thread.join();
    }
    ~Ticker() { Stop(); }
private:
    std::thread Thread;
    std::mutex Mu;
    std::condition_variable Cv;
    bool Running = false, Drawn = false;
};
Ticker g_Ticker;

int Report(const json &Reply, const Command::Spec *S, const Globals &G, const Style &St, const Style &ErrSt)
{
    g_Ticker.Stop();
    if (Reply.value("ok", false))
    {
        const json &R = Reply.contains("result") ? Reply["result"] : json();
        g_Width = G.Json ? 0 : TerminalWidth();
        if (G.Json) std::cout << R.dump(2) << "\n";
        else if (S && S->Render) std::cout << S->Render(R, St.On);
        else std::cout << RenderGeneric(R, St.On);
        return Done;
    }
    if (Reply.contains("result"))   // a check that did not pass: what it found, then that it failed
    {
        const json &R = Reply["result"];
        g_Width = G.Json ? 0 : TerminalWidth();
        if (G.Json) std::cout << R.dump(2) << "\n";
        else if (S && S->Render) std::cout << S->Render(R, St.On);
        else std::cout << RenderGeneric(R, St.On);
        std::cout.flush();
    }
    std::cerr << ErrSt.Red("error:") << " " << Reply.value("error", std::string("failed")) << "\n";
    if (Reply.value("usage", false) && S)
        std::cerr << "\n" << ErrSt.B("Usage:") << " " << UsageLine(*S, ErrSt) << "\n" << "Run " << ErrSt.B("VidyaGod help " + S->Name)
                  << " for more.\n";
    return Reply.value("usage", false) ? Usage : Failed;
}

} // namespace

void SetOutputWidth(size_t Columns) { g_Width = Columns; }

std::string RenderGeneric(const nlohmann::ordered_json &Result, bool Colour)
{
    Style St; St.On = Colour;
    std::ostringstream O;
    if (Result.is_null()) return St.Green("done") + "\n";
    Value(O, Result, St, 0);
    return O.str();
}

bool Wants(int argc, char **argv)
{
    std::vector<std::string> A(argv + 1, argv + argc);
    Globals G;
    for (size_t I = 0; I < A.size(); ++I)
    {
        try { if (TakeGlobal(A, I, G)) { if (G.Help) return true; continue; } }
        catch (...) { return true; }   // a malformed global: the CLI app says what is wrong with it
        return A[I].empty() || A[I][0] != '-' || A[I] == "--version";
    }
    return false;
}

namespace
{
// An in-process call's last step: its result is printed and everything it wrote is on disk, so the process ends here,
// without freeing an index of hundreds of thousands of values one by one (a fifth of a short call) — as the app's own
// Quit does. Not while the IPFS node is open: its store is closed by the exit hook, on the normal way out.
int Finish(int Rc)
{
    if (IpfsWrapper::Available()) return Rc;
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(Rc);
}
} // namespace

int Main(int argc, char **argv, const Hooks &H)
{
    Globals G;
    std::vector<std::string> Tokens;
    Style St, ErrSt;
    ErrSt.On = !std::getenv("NO_COLOR");
    try
    {
        const std::vector<std::string> A(argv + 1, argv + argc);
        for (size_t I = 0; I < A.size(); ++I)
            if (!TakeGlobal(A, I, G)) Tokens.push_back(A[I]);
    }
    catch (const Command::BadArgs &E)
    {
        std::cerr << ErrSt.Red("error:") << " " << E.what() << "\n";
        return Usage;
    }
    St.On = !G.NoColour && !std::getenv("NO_COLOR") && IsTty(1) && !G.Json;
    ErrSt.On = ErrSt.On && !G.NoColour && IsTty(2);
    SetLogEcho(G.Verbose);
    SetLogMillis(G.Verbose);
    Command::SetVerboseOutput(G.Verbose);
    VgSetLogVerbose(G.Verbose ? 1 : 0);   // and the node's own trace

    //With --json, help is the commands' own entries (as `commands --json` lists them): one command's, a group's, all.
    const auto Entries = [](const std::string &Prefix) {
        json Out = json::array();
        for (const auto &E : Command::List())
            if (Prefix.empty() || E["name"].get<std::string>() == Prefix || E["name"].get<std::string>().rfind(Prefix + " ", 0) == 0) Out.push_back(E);
        return Out;
    };
    if (Tokens.empty() || Tokens.front() == "help")
    {
        const std::vector<std::string> T(Tokens.empty() ? Tokens.begin() : Tokens.begin() + 1, Tokens.end());
        if (T.empty()) { if (G.Json) std::cout << Entries("").dump(2) << "\n"; else PrintRootHelp(St); return Done; }
        if (const Command::Spec *S = Command::Find(Join(T, 0, T.size())))
        {
            if (G.Json) std::cout << Entries(S->Name).at(0).dump(2) << "\n"; else PrintCommandHelp(*S, St);
            return Done;
        }
        if (GroupNames().count(T.front()) && T.size() == 1)
        {
            if (G.Json) std::cout << Entries(T.front()).dump(2) << "\n"; else PrintGroupHelp(T.front(), St);
            return Done;
        }
        std::cerr << ErrSt.Red("error:") << " no help for '" << Join(T, 0, T.size()) << "'\n";
        return Usage;
    }
    if (Tokens.front() == "version" || Tokens.front() == "--version")
    {
        std::cout << "VidyaGod " << VIDYAGOD_VERSION << "\n";
        return Done;
    }
    if (Tokens.front() == "__complete") { PrintCandidates({ Tokens.begin() + 1, Tokens.end() }); return Done; }
    if (Tokens.front() == "completion")
    {
        try { PrintCompletion(Tokens.size() > 1 ? Tokens[1] : std::string(), St); return Done; }
        catch (const Command::BadArgs &E) { std::cerr << ErrSt.Red("error:") << " " << E.what() << "\n"; return Usage; }
    }
    if (Tokens.front() == "commands")
    {
        if (Tokens.size() > 1 && Tokens[1] == "--check")   // every example in the help parses (a test runs this)
        {
            const json Bad = CheckExamples();
            if (G.Json) std::cout << Bad.dump(2) << "\n";
            else if (Bad.empty()) std::cout << "every example parses\n";
            else std::cout << RenderGeneric(Bad, St.On);
            return Bad.empty() ? Done : Failed;
        }
        if (G.Json) { std::cout << Command::List().dump(2) << "\n"; return Done; }
        json Rows = json::array();
        for (const auto &[N, S] : Command::All()) Rows.push_back({{"command", N}, {"summary", S.Summary}});
        std::cout << RenderGeneric(Rows, St.On);
        return Done;
    }

    // The command: the longest run of leading words that names one.
    const Command::Spec *S = nullptr;
    size_t Used = 0;
    for (size_t N = Tokens.size(); N > 0 && !S; --N)
    {
        S = Command::Find(Join(Tokens, 0, N));
        if (S) Used = N;
    }
    if (!S)
    {
        const std::string &G0 = Tokens.front();
        if (GroupNames().count(G0))
        {
            if (Tokens.size() == 1) { PrintGroupHelp(G0, St); return G.Help ? Done : Usage; }
            std::vector<std::string> Names;
            for (const auto *X : InGroup(G0)) Names.push_back(Words(X->Name)[1]);
            const std::string Near = Nearest(Tokens[1], Names);
            std::cerr << ErrSt.Red("error:") << " '" << G0 << "' has no command '" << Tokens[1] << "'"
                      << (Near.empty() ? "" : " — did you mean '" + G0 + " " + Near + "'?") << "\n"
                      << "Run " << ErrSt.B("VidyaGod help " + G0) << " for its commands.\n";
            return Usage;
        }
        const std::set<std::string> Names = GroupNames();
        const std::vector<std::string> Gs(Names.begin(), Names.end());
        const std::string Near = Nearest(G0, Gs);
        std::cerr << ErrSt.Red("error:") << " no command '" << G0 << "'" << (Near.empty() ? "" : " — did you mean '" + Near + "'?")
                  << "\nRun " << ErrSt.B("VidyaGod help") << " for everything it can do.\n";
        return Usage;
    }
    if (G.Help) { if (G.Json) std::cout << Entries(S->Name).at(0).dump(2) << "\n"; else PrintCommandHelp(*S, St); return Done; }
    json Args;
    try { Args = ParseArgs(*S, { Tokens.begin() + static_cast<long>(Used), Tokens.end() }); }
    catch (const Command::BadArgs &E)
    {
        return Report({{"ok", false}, {"error", E.what()}, {"usage", true}}, S, G, St, ErrSt);
    }

    // Where it runs: inside the app, when one is up on this data dir; else here.
    const QString DataPath = H.DataDir(G.DataDir);
    QDir AppDataDir(DataPath);
    AppDataDir.mkpath(".");
    json Reply;
    if (S->Name == "events follow")
    {
        // Ask the app for what is new, over and over, printing each batch; the app has to be up to have events.
        json A = Args;
        const auto Start = std::chrono::steady_clock::now();
        const long long For = Args.value("for", 0LL);
        bool First = !A.contains("since");
        for (;;)
        {
            json R;
            if (!CommandServer::Send(DataPath.toStdString(), "events list", First ? json{{"limit", 1}} : A, R))
            {
                std::cerr << ErrSt.Red("error:") << " no app is running on " << DataPath.toStdString() << " — nothing to follow\n";
                return Unreachable;
            }
            if (!R.value("ok", false)) return Report(R, S, G, St, ErrSt);
            const json &Res = R["result"];
            if (First) { A["since"] = Res.value("next", 0ULL); A.erase("for"); First = false; }
            else
            {
                if (!Res["events"].empty())
                {
                    if (G.Json) for (const auto &E : Res["events"]) std::cout << E.dump() << "\n";
                    else std::cout << S->Render(Res["events"], St.On);
                    std::cout.flush();
                }
                A["since"] = Res.value("next", A.value("since", 0ULL));
            }
            if (For > 0 && std::chrono::steady_clock::now() - Start > std::chrono::seconds(For)) return Done;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    // A path override is this process's: a call carrying one runs here, never in the app (whose launches keep theirs).
    const bool Overrides = !G.PackageDir.empty() || !G.RuntimeDir.empty() || !G.UserDataDir.empty();
    if (!G.Verbose) g_Ticker.Start("VidyaGod " + S->Name);
    if (!Overrides && !G.Beside && CommandServer::Send(DataPath.toStdString(), S->Name, Args, Reply)) return Report(Reply, S, G, St, ErrSt);

    //Beside whatever holds the data dir, only what changes nothing it keeps: a half-done change there (a source's
    //folder gone, the config still naming it) is worse than none.
    if (G.Beside && !Command::RunsBeside(S->Name))
    {
        std::cerr << ErrSt.Red("error:") << " '" << S->Name << "' changes the data dir (or needs the app or the node), so it "
                  << "does not run beside them (--bypass-single-instance-lock): run it without that flag — in the app, "
                  << "if one is up on " << DataPath.toStdString() << "\n";
        return Unreachable;
    }
    if (S->Requires == Command::Needs::App)   // what only the running app holds: nothing to bring up here
    {
        std::cerr << ErrSt.Red("error:") << " '" << S->Name << "' acts on the running app, and none is up on "
                  << DataPath.toStdString() << ": start VidyaGod on this data dir (off-screen is fine), then run it\n";
        return Unreachable;
    }
    std::string Holder;
    const std::string LockFile = (AppDataDir.absolutePath() + "/vidyagod.lock").toStdString();
    if (G.Beside) AppPaths::SetConfigReadOnly(true);
    else if (!Platform::AcquireSingleInstanceLock(LockFile, &Holder))
    {
        json Ping;
        const bool AppAnswers = CommandServer::Send(DataPath.toStdString(), "util bytes", json{{"bytes", 1}}, Ping, 3000);
        if (Overrides && AppAnswers)
        {
            //The app holds the data dir and a path override is this process's alone: a call that only reads (where the
            //paths are, what a launch would be) runs here beside it, on a config it never writes. Anything that writes
            //— the disk, a game's saves — needs the app gone: two writers there is what the lock is for.
            if (S->Name != "app paths" && S->Name != "launch plan")
            {
                std::cerr << ErrSt.Red("error:") << " '" << S->Name << "' with a path override runs in a process of its own, "
                          << "and the app holds " << DataPath.toStdString() << ": quit the app, or run it without the override "
                          << "(beside the app, only app paths and launch plan take one)\n";
                return Unreachable;
            }
            AppPaths::SetConfigReadOnly(true);
        }
        else
        {
            //Who holds it: another command-line call (wait for it, as two calls at once would race on the config) or
            //an app that does not answer (still starting — or from before commands: no use waiting long).
            const auto IsCommandRun = [](const std::string &Who) {
                const auto Colon = Who.find(": ");
                if (Who.empty() || Colon == std::string::npos) return true;   // not known: as a command (wait)
                const std::vector<std::string> W = Words(Who.substr(Colon + 2));
                for (size_t I = 1; I < W.size(); ++I)
                {
                    if (W[I] == "--data-dir" || W[I] == "--wait" || W[I] == "--package-dir" || W[I] == "--runtime-dir"
                        || W[I] == "--userdata-dir" || W[I] == "--log") { ++I; continue; }
                    if (W[I] == "--json" || W[I] == "--no-color" || W[I] == "-v" || W[I] == "--verbose" || W[I] == "--online"
                        || W[I] == "--no-wait" || W[I] == "--tray" || W[I] == "--offline" || W[I] == "--bypass-single-instance-lock"
                        || W[I].rfind("--data-dir=", 0) == 0) continue;   // the app's own start options (--tray: autostart)
                    return true;   // a command, or one of the old flag modes
                }
                return false;      // the app itself
            };
            const bool ByCommand = IsCommandRun(Holder);
            const std::string Who = Holder.empty() ? std::string() : " (" + Holder + ")";
            //Waiting is for a person at a terminal; a script gets the answer at once, as it always did (and can retry).
            if (G.NoWait || !IsTty(2))
            {
                std::cerr << ErrSt.Red("error:") << (ByCommand ? " another VidyaGod command is using " : " VidyaGod is running on ")
                          << DataPath.toStdString() << Who << (ByCommand ? ": run this one when it ends" : " but does not answer commands")
                          << "\n";
                return Unreachable;
            }
            g_Ticker.Stop();
            std::cerr << ErrSt.Dim(std::string("waiting for ") + (ByCommand ? "another VidyaGod command" : "the app") + " on "
                                   + DataPath.toStdString() + Who + " (--no-wait: fail instead)…") << "\n";
            const auto Since = std::chrono::steady_clock::now();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (Platform::AcquireSingleInstanceLock(LockFile, &Holder)) break;
                if (!Overrides && CommandServer::Send(DataPath.toStdString(), S->Name, Args, Reply, -1)) return Report(Reply, S, G, St, ErrSt);
                if (!ByCommand && std::chrono::steady_clock::now() - Since > std::chrono::seconds(180))
                {
                    std::cerr << ErrSt.Red("error:") << " VidyaGod is running on " << DataPath.toStdString() << Who
                              << " but has not answered commands for 3 minutes — still indexing a large library, or a build "
                                 "from before commands. Try again, or quit it.\n";
                    return Unreachable;
                }
            }
        }
    }
    if (!G.PackageDir.empty()) AppPaths::SetPackagePathOverride(G.PackageDir);
    if (!G.RuntimeDir.empty()) AppPaths::SetRuntimePathOverride(G.RuntimeDir);
    if (!G.UserDataDir.empty()) AppPaths::SetUserDataPathOverride(G.UserDataDir);
    nlohmann::ordered_json Config;
    if (!H.InitConfig(&Config, &AppDataDir))
    {
        std::cerr << ErrSt.Red("error:") << " cannot read the config in " << DataPath.toStdString() << "\n";
        return Unreachable;
    }
    if (S->Node)
    {
        //Networking off (Settings → Networking) means off here too: nothing goes online unless this call says so.
        //The node's status is what it is — off — without starting it.
        //Beside the app, the node's store is the app's: this process never opens it. --offline: not for this call.
        const auto &Ipfs = Config["Settings"]["IPFS"];
        const bool Networking = Ipfs.is_object() && Ipfs.value("Enabled", false);
        const bool Up = !G.Offline && !G.Beside && (Networking || G.Online);
        if (!Up && !Command::WorksOffline(S->Name))
        {
            std::cerr << ErrSt.Red("error:") << " '" << S->Name << "' needs the network, and "
                      << (G.Beside ? "a call run beside the app (--bypass-single-instance-lock) never opens the node the app holds: run it without that, in the app"
                          : G.Offline ? "this call is --offline"
                          : "networking is off on " + DataPath.toStdString() + ": add --online to go online for this call, or turn it on (VidyaGod app networking --set on)")
                      << "\n";
            return Unreachable;
        }
        //Networking off: a command that works without the node (a launch of what is installed) runs without it.
        if (Up) { IpfsWrapper::StartNode((AppDataDir.absolutePath() + "/IPFS").toStdString()); AwaitNode(); }
    }
    Command::Context Ctx;
    Ctx.Config = &Config;
    Ctx.AppDataDir = &AppDataDir;
    if (S->Requires == Command::Needs::Config)
    {
        Reply = Command::Run(S->Name, Args, Ctx);
        return Finish(Report(Reply, S, G, St, ErrSt));
    }
    // The model (and, for UI commands, the windows) of the app, brought up offscreen for this call.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    int Argc = 1;
    char Name[] = "VidyaGod";
    char *Argv[] = { Name, nullptr };
    QApplication App(Argc, Argv);
    Theme::Apply(App);          // the app's look: a screenshot here is what the running app shows
    A11yNames::Install(&App);   // the names ui commands address controls by — the same the running app gives them
    std::unique_ptr<MainWindow> Window;
    std::unique_ptr<AppModel> Model;
    if (S->Requires == Command::Needs::Ui)
    {
        Window = std::make_unique<MainWindow>(&Config, &AppDataDir);
        Window->show();   // offscreen: laid out and "visible" as on a screen, painted nowhere
        Ctx.Model = Window->model();
        Ctx.Window = Window.get();
    }
    else
    {
        Model = std::make_unique<AppModel>(&Config, &AppDataDir);
        Ctx.Model = Model.get();
        EventLog::Attach(Model.get(), "AppModel");
        EventLog::Attach(FriendsManager::instance(), "FriendsManager");
        EventLog::Attach(IpfsManager::instance(), "IpfsManager");
    }
    QCoreApplication::processEvents();
    //An action that opens a dialog: nothing here will answer it, so the dialog is the result and the call ends.
    Ctx.Early = [&](const json &R) {
        const int Rc = Report({{"ok", true}, {"result", R}}, S, G, St, ErrSt);
        //From inside the dialog's event loop: no unwinding (the windows and the action are still on the stack) — the
        //node's store is closed first if it is open, then the process ends.
        if (IpfsWrapper::Available()) IpfsWrapper::StopNode();
        std::fflush(nullptr);
        std::_Exit(Rc);
    };
    Reply = Command::Run(S->Name, Args, Ctx);
    if (G.WaitMs > 0)
    {
        QTimer::singleShot(static_cast<int>(G.WaitMs), &App, &QCoreApplication::quit);
        QApplication::exec();
    }
    QCoreApplication::processEvents();
    return Finish(Report(Reply, S, G, St, ErrSt));
}

} // namespace CliApp
