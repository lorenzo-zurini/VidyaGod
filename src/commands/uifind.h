#ifndef UIFIND_H
#define UIFIND_H

#include <nlohmann/json.hpp>
#include <string>

class QWidget;

// The ui commands' view of the windows, shared with every command that names a control (cmd_ui.cpp defines them).
namespace UiFind
{
// A control by its name: exact objectName or accessible name first, then a unique case-insensitive part of one.
// Ambiguous or unknown is Command::BadArgs naming the candidates.
QWidget *Named(const std::string &Name, const QWidget *Under = nullptr);
// The name ui commands address W by.
std::string NameOf(const QWidget *W);
// W's exact state (what ui tree prints), Depth levels down.
nlohmann::ordered_json StateOf(const QWidget *W, int Depth, bool VisibleOnly);
}

#endif // UIFIND_H
