#ifndef LAUNCHSESSION_H
#define LAUNCHSESSION_H

#include "launchparams.h"

#include <nlohmann/json.hpp>

// LaunchSession — what a launch does around the plan (LaunchResolver): the steps that read or write the user's state.
namespace LaunchSession
{

//A launch's resolve that first takes back what the game changed in its own files (OptionReadBack): a quiet look-ahead
//resolve says where the options are written and where the user's saved copies are, the values the files hold become
//the instance's (Adopt), and the launch is then resolved for real from its input, on them (a value can gate a layer).
//The look-ahead logs nothing into a running launch's console or verdict. Nothing is read while the instance is running
//(its saved copies are not yet what the session has written), nor with a path override (--userdata-dir: a plain
//resolve). Reresolve false: the look-ahead only — the launch window updating the instance before it shows the values.
[[nodiscard]] bool ResolveTakingBack(ContainerParams &CP, const nlohmann::ordered_json &Config, bool Reresolve = true);

} // namespace LaunchSession

#endif // LAUNCHSESSION_H
