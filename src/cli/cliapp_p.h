#ifndef CLIAPP_P_H
#define CLIAPP_P_H

// What the CLI front end's files share (cliapp.cpp and clirender.cpp, the renderer of results for a person): the
// style, and the renderer's entry points. Not for anyone else.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace CliApp::detail
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

bool IsTty(int Fd);
// The terminal's width for output a person reads; 0 when stdout is not a terminal (piped, captured: nothing shortened).
extern size_t g_Width;
size_t TerminalWidth();
// A result as text: an object as fields, a list of like objects as a table, others as an outline.
void Value(std::ostringstream &O, const json &V, const Style &St, size_t Indent);
void Fields(std::ostringstream &O, const json &Obj, const Style &St, size_t Indent);
}

#endif // CLIAPP_P_H
