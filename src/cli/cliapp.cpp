#include "cliapp.h"

#include <fstream>
#include "apppaths.h"
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#ifndef VIDYAGOD_VERSION
#define VIDYAGOD_VERSION "dev"
#endif

namespace CliApp
{

namespace
{

using json = nlohmann::ordered_json;

enum Exit : std::uint8_t { Done = 0, Failed = 1, Usage = 2, Unreachable = 3 };

struct Style
{
    bool On = false;
    std::string B(const std::string &S) const { return On ? "\033[1m" + S + "\033[0m" : S; }      // bold
    std::string Dim(const std::string &S) const { return On ? "\033[2m" + S + "\033[0m" : S; }
    std::string Cyan(const std::string &S) const { return On ? "\033[36m" + S + "\033[0m" : S; }
    std::string Green(const std::string &S) const { return On ? "\033[32m" + S + "\033[0m" : S; }
    std::string Red(const std::string &S) const { return On ? "\033[31m" + S + "\033[0m" : S; }
    std::string Yellow(const std::string &S) const { return On ? "\033[33m" + S + "\033[0m" : S; }
};

struct Globals
{
    std::string DataDir, PackageDir, RuntimeDir, UserDataDir;   // the last three: per-launch path overrides (AppPaths)
    bool Json = false, NoColour = false, Verbose = false, Help = false;
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

bool IsTty(int Fd)
{
#ifdef _WIN32
    return ::_isatty(Fd);
#else
    return ::isatty(Fd);
#endif
}

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

std::string Nearest(const std::string &W, const std::vector<std::string> &Of)
{
    std::string Best;
    size_t D = std::string::npos;
    for (const std::string &C : Of)
        if (const size_t X = Distance(W, C); X < D) { D = X; Best = C; }
    return D <= std::max<size_t>(2, W.size() / 3) ? Best : std::string();
}

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
    for (const auto &A : S.Args) if (A.Positional) U += " " + Placeholder(A);
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

void PrintRootHelp(const Style &St)
{
    std::ostream &O = std::cout;
    O << St.B("VidyaGod") << " " << St.Dim(VIDYAGOD_VERSION) << " — play, package and share games\n\n";
    O << St.B("Usage:") << " VidyaGod <group> <command> [<arguments>…] [--options]\n";
    O << "       VidyaGod                 (no arguments) opens the app\n\n";
    O << St.B("Groups:") << "\n";
    size_t W = 0;
    for (const std::string &G : GroupNames()) W = std::max(W, G.size());
    for (const std::string &G : GroupNames())
    {
        const auto It = Command::Groups().find(G);
        O << "  " << St.Cyan(G) << std::string(W - G.size() + 3, ' ')
          << (It == Command::Groups().end() ? std::string() : It->second) << "\n";
    }
    O << "\n" << St.B("Global options:") << "\n"
      << "  --data-dir <dir>   the data dir to work on (default ~/.VidyaGod, or the portable one)\n"
      << "  --json             print the exact result as JSON\n"
      << "  --no-color         plain output (also: NO_COLOR set, or not a terminal)\n"
      << "  --wait <ms>        keep running this long after the call, for what it started\n"
      << "  --package-dir <d>  a launch's package folder, instead of the node's own (also --runtime-dir, --userdata-dir)\n"
      << "  -v, --verbose      the engine's log on stderr\n"
      << "  -h, --help         help for whatever precedes it\n\n";
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
    O << "\n" << St.Dim("Runs against: ") << (S.Requires == Command::Needs::Ui ? "the app's windows"
                                              : S.Requires == Command::Needs::Model ? "the app's model" : "the config")
      << (S.Node ? St.Dim(" · needs the IPFS node") : "") << "\n";
    if (!S.Covers.empty()) O << St.Dim("Drives: " + Join(S.Covers, 0, S.Covers.size(), ", ")) << "\n";
}

// Positionals in order, --name value / --name=value, --flag / --no-flag; every value typed by its Arg.
json ParseArgs(const Command::Spec &S, const std::vector<std::string> &Rest)
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
            Out[A->Name] = Typed(*A, Val);
            continue;
        }
        if (NextPos >= Pos.size()) throw Command::BadArgs("unexpected argument '" + W + "'");
        Out[Pos[NextPos]->Name] = Typed(*Pos[NextPos], W);
        ++NextPos;
    }
    if (NextPos < Pos.size()) throw Command::BadArgs("missing " + Placeholder(*Pos[NextPos]));
    for (const auto &A : S.Args)
        if (!A.Positional && A.Required && !Out.contains(A.Name)) throw Command::BadArgs("missing --" + A.Name);
    return Out;
}

std::string Cell(const json &V)
{
    if (V.is_string()) return V.get<std::string>();
    if (V.is_boolean()) return V.get<bool>() ? "yes" : "no";
    if (V.is_null()) return "—";
    if (V.is_array() && V.empty()) return "—";
    if (V.is_array() && std::all_of(V.begin(), V.end(), [](const json &X) { return X.is_string() || X.is_number(); }))
    {
        std::string S;
        for (const auto &X : V) S += (S.empty() ? "" : ", ") + (X.is_string() ? X.get<std::string>() : X.dump());
        return S;
    }
    return V.dump();
}

// Width of a cell as shown (UTF-8: count code points, not bytes).
size_t Shown(const std::string &S)
{
    size_t N = 0;
    for (unsigned char C : S) if ((C & 0xC0) != 0x80) ++N;
    return N;
}

std::string Clip(const std::string &S, size_t Max)
{
    if (Shown(S) <= Max) return S;
    std::string Out;
    size_t N = 0;
    for (size_t I = 0; I < S.size(); ++I)
    {
        if ((static_cast<unsigned char>(S[I]) & 0xC0) != 0x80 && ++N > Max - 1) break;
        Out += S[I];
    }
    return Out + "…";
}

void Fields(std::ostringstream &O, const json &Obj, const Style &St, size_t Indent);

void Table(std::ostringstream &O, const json &Rows, const Style &St, size_t Indent)
{
    std::vector<std::string> Cols;
    for (const auto &R : Rows)
        for (const auto &[K, V] : R.items())
            if (std::find(Cols.begin(), Cols.end(), K) == Cols.end()) Cols.push_back(K);
    std::vector<size_t> W(Cols.size());
    for (size_t C = 0; C < Cols.size(); ++C)
    {
        W[C] = Shown(Cols[C]);
        for (const auto &R : Rows) W[C] = std::max(W[C], std::min<size_t>(60, Shown(Cell(R.value(Cols[C], json())))));
    }
    const std::string Pad(Indent, ' ');
    O << Pad;
    for (size_t C = 0; C < Cols.size(); ++C) O << St.B(Cols[C]) << (C + 1 < Cols.size() ? std::string(W[C] - Shown(Cols[C]) + 2, ' ') : "");
    O << "\n";
    for (const auto &R : Rows)
    {
        O << Pad;
        for (size_t C = 0; C < Cols.size(); ++C)
        {
            const std::string V = Clip(Cell(R.value(Cols[C], json())), 60);
            O << V << (C + 1 < Cols.size() ? std::string(W[C] - Shown(V) + 2, ' ') : "");
        }
        O << "\n";
    }
    O << Pad << St.Dim(std::to_string(Rows.size()) + (Rows.size() == 1 ? " row" : " rows")) << "\n";
}

void Value(std::ostringstream &O, const json &V, const Style &St, size_t Indent)
{
    if (V.is_array() && !V.empty() && std::all_of(V.begin(), V.end(), [](const json &X) { return X.is_object(); }))
        Table(O, V, St, Indent);
    else if (V.is_object() && !V.empty())
        Fields(O, V, St, Indent);
    else if (V.is_array() && !V.empty() && std::any_of(V.begin(), V.end(), [](const json &X) { return X.is_structured(); }))
        for (const auto &X : V) { Value(O, X, St, Indent); }
    else if (V.is_array())
    {
        if (V.empty()) { O << std::string(Indent, ' ') << St.Dim("(none)") << "\n"; return; }
        for (const auto &X : V) O << std::string(Indent, ' ') << Cell(X) << "\n";
    }
    else O << std::string(Indent, ' ') << Cell(V) << "\n";
}

void Fields(std::ostringstream &O, const json &Obj, const Style &St, size_t Indent)
{
    size_t W = 0;
    for (const auto &[K, V] : Obj.items()) if (!V.is_structured() || V.empty()) W = std::max(W, Shown(K));
    for (const auto &[K, V] : Obj.items())
    {
        if (V.is_structured() && !V.empty() && !(V.is_array() && Cell(V).size() < 80 && !V.front().is_structured()))
        {
            O << std::string(Indent, ' ') << St.B(K) << "\n";
            Value(O, V, St, Indent + 2);
        }
        else
            O << std::string(Indent, ' ') << St.B(K) << std::string(W > Shown(K) ? W - Shown(K) + 2 : 2, ' ') << Cell(V) << "\n";
    }
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
        for (const char *X : { "--json", "--no-color", "--data-dir", "--wait", "--package-dir", "--runtime-dir", "--userdata-dir", "--verbose", "--help" }) Offer(X);
    }
    for (const auto &C : Out) std::cout << C << "\n";
}

// Wait until the node is up (a headless run that needs it) — or give up after a while, the command reports what is missing.
void AwaitNode()
{
    for (int I = 0; I < 30 && IpfsWrapper::Available() && !IpfsWrapper::DaemonRunning(); ++I)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

int Report(const json &Reply, const Command::Spec *S, const Globals &G, const Style &St, const Style &ErrSt)
{
    if (Reply.value("ok", false))
    {
        const json &R = Reply.contains("result") ? Reply["result"] : json();
        if (G.Json) std::cout << R.dump(2) << "\n";
        else if (S && S->Render) std::cout << S->Render(R, St.On);
        else std::cout << RenderGeneric(R, St.On);
        return Done;
    }
    std::cerr << ErrSt.Red("error:") << " " << Reply.value("error", std::string("failed")) << "\n";
    if (Reply.value("usage", false) && S)
        std::cerr << "\n" << ErrSt.B("Usage:") << " " << UsageLine(*S, ErrSt) << "\n" << "Run " << ErrSt.B("VidyaGod help " + S->Name)
                  << " for more.\n";
    return Reply.value("usage", false) ? Usage : Failed;
}

} // namespace

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
    VgSetLogVerbose(G.Verbose ? 1 : 0);   // and the node's own trace

    if (Tokens.empty() || Tokens.front() == "help")
    {
        const std::vector<std::string> T(Tokens.empty() ? Tokens.begin() : Tokens.begin() + 1, Tokens.end());
        if (T.empty()) { PrintRootHelp(St); return Done; }
        if (const Command::Spec *S = Command::Find(Join(T, 0, T.size()))) { PrintCommandHelp(*S, St); return Done; }
        if (GroupNames().count(T.front()) && T.size() == 1) { PrintGroupHelp(T.front(), St); return Done; }
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
    if (G.Help) { PrintCommandHelp(*S, St); return Done; }
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
    if (!Overrides && CommandServer::Send(DataPath.toStdString(), S->Name, Args, Reply)) return Report(Reply, S, G, St, ErrSt);

    std::string Holder;
    if (!Platform::AcquireSingleInstanceLock((AppDataDir.absolutePath() + "/vidyagod.lock").toStdString(), &Holder))
    {
        std::cerr << ErrSt.Red("error:") << " VidyaGod is running on " << DataPath.toStdString()
                  << (Holder.empty() ? "" : " (" + Holder + ")")
                  << " but does not answer commands — it predates them. Quit it, or update it.\n";
        return Unreachable;
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
    if (S->Node) { IpfsWrapper::StartNode((AppDataDir.absolutePath() + "/IPFS").toStdString()); AwaitNode(); }
    Command::Context Ctx;
    Ctx.Config = &Config;
    Ctx.AppDataDir = &AppDataDir;
    if (S->Requires == Command::Needs::Config)
    {
        Reply = Command::Run(S->Name, Args, Ctx);
        return Report(Reply, S, G, St, ErrSt);
    }
    // The model (and, for UI commands, the windows) of the app, brought up offscreen for this call.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    int Argc = 1;
    char Name[] = "VidyaGod";
    char *Argv[] = { Name, nullptr };
    QApplication App(Argc, Argv);
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
    Reply = Command::Run(S->Name, Args, Ctx);
    if (G.WaitMs > 0)
    {
        QTimer::singleShot(static_cast<int>(G.WaitMs), &App, &QCoreApplication::quit);
        QApplication::exec();
    }
    QCoreApplication::processEvents();
    return Report(Reply, S, G, St, ErrSt);
}

} // namespace CliApp
