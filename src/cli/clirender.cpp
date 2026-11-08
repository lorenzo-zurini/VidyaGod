#include "cliapp_p.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>   // GetConsoleScreenBufferInfo: the console's width
#endif

namespace CliApp::detail
{

bool IsTty(int Fd)
{
#ifdef _WIN32
    return ::_isatty(Fd);
#else
    return ::isatty(Fd);
#endif
}

// The terminal's width, for output read by a person; 0 when stdout is not a terminal (piped, captured): then
// nothing is shortened or wrapped — a script reads every value whole.
size_t g_Width = 0;

size_t TerminalWidth()
{
    if (!IsTty(1)) return 0;
    if (const char *C = std::getenv("COLUMNS")) { const int N = std::atoi(C); if (N > 20) return static_cast<size_t>(N); }
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO Info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &Info)) return static_cast<size_t>(Info.srWindow.Right - Info.srWindow.Left + 1);
#else
    struct winsize Ws {};
    if (::ioctl(1, TIOCGWINSZ, &Ws) == 0 && Ws.ws_col > 20) return Ws.ws_col;
#endif
    return 100;
}

// A CID as a table on a terminal shows it: "bafkrei…myteka", git-style — a node argument takes any unique part of one,
// that form included (commands that take a raw CID, ipfs fetch and the like, need it whole). Piped output and --json
// keep every CID whole.
bool IsCid(const std::string &S)
{
    if (S.size() < 46 || S.size() > 120 || !(S.rfind("Qm", 0) == 0 || S.rfind("baf", 0) == 0)) return false;
    return std::all_of(S.begin(), S.end(), [](unsigned char C) { return std::isalnum(C); });
}
std::string ShortCid(const std::string &S) { return IsCid(S) ? S.substr(0, 7) + "…" + S.substr(S.size() - 6) : S; }

std::string Cell(const json &V, bool Short = false)
{
    if (V.is_string())
    {
        const std::string &T = V.get_ref<const std::string &>();
        return T.empty() ? "—" : Short ? ShortCid(T) : T;   // an empty value, not a gap
    }
    if (V.is_boolean()) return V.get<bool>() ? "yes" : "no";
    if (V.is_null()) return "—";
    if ((V.is_array() || V.is_object()) && V.empty()) return "—";
    if (V.is_array() && std::all_of(V.begin(), V.end(), [](const json &X) { return X.is_string() || X.is_number(); }))
    {
        std::string S;
        for (const auto &X : V) S += (S.empty() ? "" : ", ") + (X.is_string() ? (Short ? ShortCid(X.get<std::string>()) : X.get<std::string>()) : Cell(X));
        return S;
    }
    if (V.is_number_float())   // geometry and ratios: three places say it, seventeen are noise
    {
        std::ostringstream O;
        O << std::setprecision(4) << V.get<double>();
        return O.str();
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

// The first Max code points of S (a byte offset into it).
size_t Prefix(const std::string &S, size_t Max)
{
    size_t N = 0, I = 0;
    for (; I < S.size(); ++I)
        if ((static_cast<unsigned char>(S[I]) & 0xC0) != 0x80 && N++ == Max) break;
    return I;
}

std::string Clip(const std::string &S, size_t Max)
{
    if (Shown(S) <= Max) return S;
    return S.substr(0, Prefix(S, Max - 1)) + "…";
}

// A path too long for its column keeps both ends — the folder it is in and its file name: ".../LIBRARY/…/game.zip".
std::string ClipMiddle(const std::string &S, size_t Max)
{
    const size_t N = Shown(S);
    if (N <= Max || Max < 9) return Clip(S, Max);
    const size_t Tail = (Max - 1) * 2 / 3, Head = Max - 1 - Tail;
    size_t From = Prefix(S, N - Tail);
    return S.substr(0, Prefix(S, Head)) + "…" + S.substr(From);
}

bool PathLike(const std::string &S) { return S.find('/') != std::string::npos && S.find(' ') == std::string::npos ? true : (S.size() > 1 && (S[0] == '/' || S[0] == '~')); }

// S in lines of at most Max code points, broken at spaces where it can be (a word longer than a line is cut).
std::vector<std::string> Wrap(const std::string &S, size_t Max)
{
    std::vector<std::string> Out;
    std::string Rest = S;
    while (Shown(Rest) > Max)
    {
        size_t Cut = Prefix(Rest, Max);
        const size_t Sp = Rest.rfind(' ', Cut);
        if (Sp != std::string::npos && Sp > 0 && Shown(Rest.substr(0, Sp)) >= Max / 3) Cut = Sp;
        Out.push_back(Rest.substr(0, Cut));
        Rest = Rest.substr(Cut);
        if (!Rest.empty() && Rest[0] == ' ') Rest.erase(0, 1);
    }
    Out.push_back(Rest);
    return Out;
}

// A cell's lines in a column of width Max: a path shortened in the middle, any other text wrapped.
std::vector<std::string> Fit(const std::string &S, size_t Max)
{
    if (Max == 0 || Shown(S) <= Max) return { S };
    if (PathLike(S)) return { ClipMiddle(S, Max) };
    return Wrap(S, Max);
}

void Fields(std::ostringstream &O, const json &Obj, const Style &St, size_t Indent);
void Value(std::ostringstream &O, const json &V, const Style &St, size_t Indent);

// Whether a list of objects reads as a table: its rows mostly share their keys and hold short plain values. A list of
// unlike objects (a node's layers: a ZIP, a REG, a VARS) is an outline instead — a table of it is mostly "—".
bool Tabular(const json &Rows)
{
    std::vector<std::string> Cols;
    size_t Filled = 0, Long = 0;
    for (const auto &R : Rows)
        for (const auto &[K, V] : R.items())
        {
            if (std::find(Cols.begin(), Cols.end(), K) == Cols.end()) Cols.push_back(K);
            if (!V.is_null() && !(V.is_structured() && V.empty()) && !(V.is_string() && V.get_ref<const std::string &>().empty())) ++Filled;
            const bool Plain = V.is_array() && std::all_of(V.begin(), V.end(), [](const json &X) { return !X.is_structured(); });
            if (V.is_structured() && !Plain && Cell(V).size() > 40) ++Long;
        }
    if (Cols.empty() || Cols.size() > 12) return false;
    const size_t Cells = Cols.size() * Rows.size();
    return Long * 4 <= Rows.size() && Filled * 2 >= Cells;
}

void Table(std::ostringstream &O, const json &Rows, const Style &St, size_t Indent)
{
    std::vector<std::string> Cols;
    for (const auto &R : Rows)
        for (const auto &[K, V] : R.items())
            if (std::find(Cols.begin(), Cols.end(), K) == Cols.end()) Cols.push_back(K);
    std::vector<std::vector<std::string>> Text(Rows.size(), std::vector<std::string>(Cols.size()));
    for (size_t R = 0; R < Rows.size(); ++R)
        for (size_t C = 0; C < Cols.size(); ++C) Text[R][C] = Cell(Rows[R].value(Cols[C], json()), g_Width != 0);   // short CIDs on a terminal only
    //A column of text saying in every row what an earlier one says is shown once (a row's title that is also its
    //summary). Only text, over two rows or more: two flags or counts that agree are two facts, not one.
    for (size_t C = Cols.size(); C-- > 1;)
        for (size_t E = 0; E < C; ++E)
        {
            bool Same = Rows.size() >= 2;
            for (size_t R = 0; R < Rows.size() && Same; ++R)
            {
                const json &V = Rows[R].value(Cols[C], json());
                Same = V.is_string() && V.get_ref<const std::string &>().size() > 3 && Text[R][C] == Text[R][E];
            }
            if (!Same) continue;
            Cols.erase(Cols.begin() + static_cast<long>(C));
            for (auto &Row : Text) Row.erase(Row.begin() + static_cast<long>(C));
            break;
        }
    std::vector<size_t> W(Cols.size());
    for (size_t C = 0; C < Cols.size(); ++C)
    {
        W[C] = Shown(Cols[C]);
        for (size_t R = 0; R < Rows.size(); ++R) W[C] = std::max(W[C], Shown(Text[R][C]));
    }
    //On a terminal the table fits it: the widest columns give way (to no less than 12) and their cells wrap — a
    //message whole, over lines — or, a path, keep both ends. Piped, every cell is printed whole.
    if (g_Width)
    {
        const size_t Gaps = Indent + (Cols.size() ? 2 * (Cols.size() - 1) : 0);
        const size_t Avail = g_Width > Gaps + 12 ? g_Width - Gaps : 12;
        //The widest columns are cut to one width, the largest that fits: no column is starved while another keeps 90.
        const auto Capped = [&W](size_t Cap) { size_t Sum = 0; for (size_t X : W) Sum += std::min(X, Cap); return Sum; };
        if (Capped(SIZE_MAX) > Avail)
        {
            size_t Lo = 12, Hi = *std::max_element(W.begin(), W.end());
            while (Lo < Hi) { const size_t Mid = (Lo + Hi + 1) / 2; if (Capped(Mid) <= Avail) Lo = Mid; else Hi = Mid - 1; }
            for (size_t &X : W) X = std::min(X, Lo);
        }
    }
    const std::string Pad(Indent, ' ');
    const auto Line = [&](const std::vector<std::vector<std::string>> &Cells, bool Head) {
        size_t Height = 1;
        for (const auto &L : Cells) Height = std::max(Height, L.size());
        for (size_t H = 0; H < Height; ++H)
        {
            std::string Row = Pad;
            for (size_t C = 0; C < Cols.size(); ++C)
            {
                const std::string V = H < Cells[C].size() ? Cells[C][H] : std::string();
                const bool Last = C + 1 == Cols.size();
                Row += Head ? St.B(V) : V;
                if (!Last) Row += std::string(W[C] > Shown(V) ? W[C] - Shown(V) + 2 : 2, ' ');
            }
            while (!Row.empty() && Row.back() == ' ') Row.pop_back();
            O << Row << "\n";
        }
    };
    std::vector<std::vector<std::string>> Head(Cols.size());
    for (size_t C = 0; C < Cols.size(); ++C) Head[C] = { g_Width ? Clip(Cols[C], W[C]) : Cols[C] };
    Line(Head, true);
    for (size_t R = 0; R < Rows.size(); ++R)
    {
        std::vector<std::vector<std::string>> Cells(Cols.size());
        for (size_t C = 0; C < Cols.size(); ++C) Cells[C] = g_Width ? Fit(Text[R][C], W[C]) : std::vector<std::string>{ Text[R][C] };
        Line(Cells, false);
    }
    O << Pad << St.Dim(std::to_string(Rows.size()) + (Rows.size() == 1 ? " row" : " rows")) << "\n";
}

// A list of unlike objects: each one an indented block under a "-".
void Outline(std::ostringstream &O, const json &Items, const Style &St, size_t Indent)
{
    for (const auto &X : Items)
    {
        std::ostringstream One;
        Value(One, X, St, Indent + 2);
        std::string T = One.str();
        if (T.size() >= Indent + 2) T.replace(Indent, 2, "- ");
        O << T;
    }
}

void Value(std::ostringstream &O, const json &V, const Style &St, size_t Indent)
{
    if (V.is_array() && !V.empty() && std::all_of(V.begin(), V.end(), [](const json &X) { return X.is_object() && !X.empty(); }))
    {
        if (Tabular(V)) Table(O, V, St, Indent);
        else Outline(O, V, St, Indent);
    }
    else if (V.is_object() && !V.empty())
        Fields(O, V, St, Indent);
    else if (V.is_array() && !V.empty() && std::any_of(V.begin(), V.end(), [](const json &X) { return X.is_structured(); }))
        Outline(O, V, St, Indent);
    else if (V.is_array())
    {
        if (V.empty()) { O << std::string(Indent, ' ') << St.Dim("(none)") << "\n"; return; }
        for (const auto &X : V) O << std::string(Indent, ' ') << Cell(X) << "\n";
    }
    else O << std::string(Indent, ' ') << Cell(V) << "\n";
}

void Fields(std::ostringstream &O, const json &Obj, const Style &St, size_t Indent)
{
    //A value nests under its key when it is a block of its own; a short plain list stays on the key's line.
    const auto Nests = [](const json &V) {
        return V.is_structured() && !V.empty() && !(V.is_array() && Cell(V).size() < 80 && !V.front().is_structured());
    };
    size_t W = 0;
    for (const auto &[K, V] : Obj.items()) if (!Nests(V)) W = std::max(W, Shown(K));
    for (const auto &[K, V] : Obj.items())
    {
        if (Nests(V))
        {
            O << std::string(Indent, ' ') << St.B(K) << "\n";
            Value(O, V, St, Indent + 2);
            continue;
        }
        //A long value wraps under itself on a terminal (a message is read whole; a path stays one line, to copy).
        const size_t At = Indent + std::max(W, Shown(K)) + 2;
        const std::string Text = Cell(V);
        std::vector<std::string> Lines = { Text };
        if (g_Width && g_Width > At + 20 && Shown(Text) > g_Width - At && !PathLike(Text)) Lines = Wrap(Text, g_Width - At);
        O << std::string(Indent, ' ') << St.B(K) << std::string(W > Shown(K) ? W - Shown(K) + 2 : 2, ' ') << Lines[0] << "\n";
        for (size_t I = 1; I < Lines.size(); ++I) O << std::string(At, ' ') << Lines[I] << "\n";
    }
}

} // namespace CliApp::detail
