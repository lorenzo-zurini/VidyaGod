#ifndef CLIAPP_H
#define CLIAPP_H

#include <nlohmann/json.hpp>
#include <QString>
#include <functional>
#include <string>

class QDir;

// ---------------------------------------------------------------------------
// CliApp — VidyaGod as a command-line program: git-style subcommands over the Command registry.
//
//   VidyaGod <group> <command> [<positional>…] [--option value…]
//   VidyaGod help [<group> [<command>]]   ·   VidyaGod <…> --help   ·   VidyaGod commands   ·   VidyaGod version
//   VidyaGod completion bash|zsh|fish     a completion script (completes groups, commands and options)
//
// Global options, anywhere on the line: --data-dir <dir>, --json (the exact result), --no-color, --wait <ms> (keep
// the event loop running after the call, for what it started), -v/--verbose (the engine's log on stderr).
// Exit codes: 0 done · 1 the command failed · 2 a usage error · 3 the app could not be reached or brought up.
// ---------------------------------------------------------------------------
namespace CliApp
{

struct Hooks
{
    std::function<QString(const std::string &Override)> DataDir;          // the data dir (portable, --data-dir)
    std::function<bool(nlohmann::ordered_json *, QDir *)> InitConfig;     // load (or seed) GlobalConfig.JSON
};

// True when the command line asks for the CLI app — a subcommand, help, version — rather than a --flag mode or the GUI.
bool Wants(int argc, char **argv);
int Main(int argc, char **argv, const Hooks &H);

// The generic rendering of a result for people: an array of objects as a table, an object as aligned fields.
std::string RenderGeneric(const nlohmann::ordered_json &Result, bool Colour);
// The width output is laid out for (a terminal's columns; 0: not a terminal — nothing shortened). Set per call from
// stdout; a test sets it to render as a terminal would.
void SetOutputWidth(size_t Columns);

} // namespace CliApp

#endif // CLIAPP_H
