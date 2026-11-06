#include "boxlaunch.h"
#include "registrywrapper.h"

#ifdef _WIN32
#include "boxprefix.h"
#include "commonutils.h"   // Log*
#include "platform/platform.h"
#include "persistlayer.h"    // CapturePersistFiles — a leftover box's kept files
#include "registrylayer.h"   // CapturePersistRegKeys — a leftover box's kept keys
#include "sandboxlayer.h"
#include "varsubst.h"

#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace BoxLaunch {

#ifndef _WIN32

bool Prepare(struct ContainerParams &) { return true; }
bool Harvest(struct ContainerParams &, RegistryWrapper &, bool &HaveSession) { HaveSession = false; return true; }
void Discard(struct ContainerParams &) {}

#else

namespace {

namespace fs = std::filesystem;

//The launch's sandbox options as Execute builds them (FromParams, the recorded mounts, the working folder): the box's
//name comes from the working folder, so every step reaches the box the game runs in.
SandboxLayer::Options BoxOptions(const struct ContainerParams &CP)
{
    SandboxLayer::Options O = SandboxLayer::FromParams(CP);
    for (const auto &[Spec, Mnt] : CP.SandboxMounts) O.Mounts.push_back({Spec, Mnt});
    O.WorkDir = CP.WorkDirPathComplete.string();
    return O;
}

//The runner's DRIVES with their %variables% resolved, as GuestToLayout lays guest paths out.
nlohmann::ordered_json ResolvedDrives(struct ContainerParams &CP)
{
    nlohmann::ordered_json D = nlohmann::ordered_json::object();
    if (!CP.Drives.is_object()) return D;
    const std::map<std::string, std::string> Vars = CP.GetVariablesMap();
    for (const auto &[Drive, V] : CP.Drives.items())
    {
        std::string R = V.is_string() ? V.get<std::string>() : std::string();
        VarSubst::StringVariableSubstitution(R, Vars);
        D[Drive] = R;
    }
    return D;
}

//The drive Windows is on (where a box helper writes its files), "C:" when unknown.
std::string SystemDrive(const std::map<std::string, std::string> &Host)
{
    const auto Win = Host.find("Windows");
    return (Win != Host.end() && Win->second.size() >= 2) ? Win->second.substr(0, 2) : std::string("C:");
}

void Report(const char *Where, const std::vector<std::string> &Errors, const std::string &What)
{
    for (size_t I = 0; I < Errors.size() && I < 10; ++I) LogErr(Where, What + ": " + Errors[I]);
    if (Errors.size() > 10) LogErr(Where, What + ": … and " + std::to_string(Errors.size() - 10) + " more");
}

//The box emptied by Sandboxie (its registry hive unloaded first), then whatever it left removed — never through a link.
bool Empty(const SandboxLayer::Options &O, std::vector<std::string> &Errors)
{
    if (!SandboxLayer::EmptyBox(O)) LogWarn("BoxLaunch", "Sandboxie did not empty the game's box " + O.BoxRoot + "; removing what is left.");
    return Platform::RemoveTree(O.BoxRoot, Errors);
}

//What Materialise gave the box (its box paths, one per line), kept at the box root, beside what Sandboxie keeps there
//and outside every path a program in the box sees. Its presence also marks a box no Discard has emptied yet.
fs::path PlacedFile(const SandboxLayer::Options &O) { return fs::path(O.BoxRoot) / "vidyagod-placed.txt"; }

bool ReadPlaced(const SandboxLayer::Options &O, std::vector<std::string> &Placed)
{
    std::ifstream In(PlacedFile(O), std::ios::binary);
    if (!In) return false;
    for (std::string L; std::getline(In, L); )
    {
        if (!L.empty() && L.back() == '\r') L.pop_back();
        if (!L.empty()) Placed.push_back(L);
    }
    return true;
}

//The box's kept files back into the runtime, its kept keys into Session (read by reg.exe in the box). The box is
//stopped first: what the game left running writes nothing more. False when anything kept was NOT brought back: the box
//is then the only copy of it and must not be emptied.
bool TakeKeeps(struct ContainerParams &CP, const SandboxLayer::Options &O, const std::vector<std::string> &Placed,
               RegistryWrapper &Session, const char *Where)
{
    bool Ok = true;
    SandboxLayer::TerminateBox(O);
    const std::map<std::string, std::string> Host = Platform::HostFolders();
    std::vector<std::string> Errors;
    if (!BoxPrefix::SyncKeepsBack(CP.RuntimePath, CP.ContentRoot, ResolvedDrives(CP), Host, O.BoxRoot,
                                  CP.KeepDirs, CP.KeepFiles, Placed, Errors))
    { Report(Where, Errors, "a kept file was NOT saved"); Ok = false; }

    //A key the box does not have (reg.exe exits 1) stays as the store has it. Each is read whole or not at all: a file
    //reg.exe wrote that this reader cannot follow to its end saves nothing of that key (half a key would replace the
    //stored one). Anything else reg.exe answers is a key NOT saved.
    const std::string Drive = SystemDrive(Host);
    int N = 0;
    for (std::string Key : CP.KeepRegKeys)
    {
        std::replace(Key.begin(), Key.end(), '/', '\\');
        const std::string Real = Drive + "\\__vidyagod_regkeep_" + std::to_string(N++) + ".reg";
        const fs::path File = fs::path(O.BoxRoot) / BoxPrefix::BoxRelForReal(Real, Host);
        std::error_code Ec;
        fs::remove(File, Ec);
        const int Rc = SandboxLayer::RunInBox(O, "reg.exe", {"export", QString::fromStdString(Key), QString::fromStdString(Real), "/y"}, 60000);
        //Exit 1 is reg.exe's answer to a missing key AND to every other failure, and Start.exe's when it could not run the
        //program at all. It counts as "absent" only when reg.exe demonstrably runs in the box (a key every profile has
        //answers 0) and asking for this key answers 1; anything else is a key NOT saved.
        if (Rc == 1)
        {
            const int Runs = SandboxLayer::RunInBox(O, "reg.exe", {"query", "HKCU\\Software"}, 60000);
            const int Has  = Runs == 0 ? SandboxLayer::RunInBox(O, "reg.exe", {"query", QString::fromStdString(Key)}, 60000) : -1;
            if (Runs == 0 && Has == 1) { LogOut(Where, "Key absent in the box (kept as stored): " + Key); continue; }
            LogErr(Where, "reg.exe could not export the kept key " + Key + " out of the game's box (exit code 1; "
                   + (Runs != 0 ? std::string("reg.exe does not run in the box") : "the key is there") + ") — this session's changes to it are NOT saved.");
            Ok = false;
            continue;
        }
        if (Rc != 0 || !fs::is_regular_file(File, Ec))
        {
            LogErr(Where, "reg.exe could not read the kept key " + Key + " out of the game's box ("
                   + (Rc < 0 ? std::string("it did not run or finish") : Rc != 0 ? "exit code " + std::to_string(Rc) : std::string("no file"))
                   + ") — this session's changes to it are NOT saved.");
            Ok = false;
            continue;
        }
        std::ifstream In(File, std::ios::binary);
        std::stringstream Bytes; Bytes << In.rdbuf();
        RegistryWrapper One;
        std::string Err;
        if (!BoxPrefix::FromRegFile(BoxPrefix::Utf16LeToUtf8(Bytes.str()), One, Err))
        { LogErr(Where, "the kept key " + Key + " could not be read out of the box (" + Err + ") — NOT saved."); Ok = false; continue; }
        if (!Session.MergeKeyFrom(One, Key)) LogOut(Where, "Key absent in the box (kept as stored): " + Key);
    }
    SandboxLayer::TerminateBox(O);
    return Ok;
}

} // namespace

bool Prepare(struct ContainerParams &CP)
{
    CP.BoxPrepared = false;
    const SandboxLayer::Options O = BoxOptions(CP);
    if (O.BoxRoot.empty()) return true;
    SandboxLayer::PrepareBox(O);
    SandboxLayer::TerminateBox(O);   // nothing from an earlier launch still runs in it, holding its files

    //A box that was never emptied is a launch that did not finish (VidyaGod, or the machine, stopped mid-game): what the
    //game saved that session is only there. It goes to the instance first — kept files, kept keys — and only when all
    //of it is stored is the box emptied. Otherwise the launch is refused: the box keeps it for the next try.
    if (std::vector<std::string> Left; ReadPlaced(O, Left))
    {
        LogWarn("BoxLaunch::Prepare", "The game's box still holds the session of a launch that did not finish; its kept "
                "files and keys are saved first (" + O.BoxRoot + ").");
        RegistryWrapper Session;
        bool Saved = TakeKeeps(CP, O, Left, Session, "BoxLaunch::Prepare");
        if (!CP.KeepFiles.empty() && !PersistLayer::CapturePersistFiles(CP)) Saved = false;
        if (!CP.KeepRegKeys.empty() && !RegistryLayer::CapturePersistRegKeys(CP, &Session)) Saved = false;
        if (!Saved)
        {
            LogErr("BoxLaunch::Prepare", "What the unfinished session kept could not all be stored (see above): the game is "
                   "NOT started, and its box is left as it is (" + O.BoxRoot + ").");
            return false;
        }
    }
    std::vector<std::string> Errors;
    if (!Empty(O, Errors))
    {
        Report("BoxLaunch::Prepare", Errors, "the game's box could not be emptied (the game would see what an earlier launch left)");
        return false;
    }
    std::error_code Ec;
    fs::create_directories(O.BoxRoot, Ec);
    if (Ec) { LogErr("BoxLaunch::Prepare", "cannot create the game's box at " + O.BoxRoot + ": " + Ec.message()); return false; }

    //What the package places outside the game folder. A file that could not be placed is said, and the game still runs
    //(as a malformed registry edit does under Wine): it is the package's, not the user's.
    const std::map<std::string, std::string> Host = Platform::HostFolders();
    const nlohmann::ordered_json Drives = ResolvedDrives(CP);
    std::vector<std::string> Placed;
    if (!BoxPrefix::Materialise(CP.RuntimePath, CP.ContentRoot, Drives, Host, O.BoxRoot, Errors, &Placed))
        Report("BoxLaunch::Prepare", Errors, "not placed in the game's box");
    //...but what the USER keeps there must all be in the box, or the game finds no save, writes its defaults, and the
    //capture after it stores them over the user's: a kept file the box did not get (or got partly) stops the launch.
    if (std::vector<std::string> Missing = BoxPrefix::KeptNotInBox(CP.RuntimePath, CP.ContentRoot, Drives, Host, O.BoxRoot, CP.KeepDirs, CP.KeepFiles); !Missing.empty())
    {
        Report("BoxLaunch::Prepare", Missing, "a kept file did not reach the game's box");
        LogErr("BoxLaunch::Prepare", "The game is NOT started: it would not see the user's kept files above, and what it "
               "wrote instead would be stored over them.");
        return false;
    }
    LogOut("BoxLaunch::Prepare", "Placed " + std::to_string(Placed.size()) + " file(s) the package puts outside the game "
           "folder into its box (" + O.BoxRoot + ").");

    //The registry the box starts with: base edits, the kept keys, override edits — a Wine prefix's order.
    RegistryWrapper RW;
    if (!RW.ApplyRegEdits(CP.SubComponentsArray, /*WantOverride=*/false))
        LogErr("BoxLaunch::Prepare", "One or more BASE RegEdits were malformed and NOT applied (see above).");
    const fs::path Store = CP.UserDataPath / "REGKEYS";
    bool SeededKept = false;
    if (!CP.KeepRegKeys.empty() && fs::exists(Store, Ec))
    {
        RegistryWrapper Durable;
        //The game would run on defaults and the capture after it would store them over the user's keys: not started.
        if (!Durable.LoadPrefix(Store))
        {
            LogErr("BoxLaunch::Prepare", "could not load the kept registry keys at " + Store.string() + " — the game is NOT "
                   "started (it would run on defaults, and those would be stored over the user's keys).");
            return false;
        }
        for (const std::string &Key : CP.KeepRegKeys)
            if (RW.MergeKeyFrom(Durable, Key)) { LogOut("BoxLaunch::Prepare", "Seeded kept key " + Key); SeededKept = true; }
    }
    if (!RW.ApplyRegEdits(CP.SubComponentsArray, /*WantOverride=*/true))
        LogErr("BoxLaunch::Prepare", "One or more OVERRIDE RegEdits were malformed and NOT applied (see above).");
    size_t Skipped = 0;
    const std::string Text = BoxPrefix::ToRegFile(RW, &Skipped);
    if (Skipped) LogWarn("BoxLaunch::Prepare", std::to_string(Skipped) + " registry key(s)/value(s) cannot go into a box "
                         "(HKU\\.Default, or a value type a .reg file cannot carry) — not imported.");
    if (Text.find("\r\n[") != std::string::npos)
    {
        //The file goes INTO the box, at a real path the box shows its own copy of: reg.exe there could not open one
        //written beside the box ("Error opening the file", measured), and a box's own files it always sees.
        const std::string FileReal = SystemDrive(Host) + "\\__vidyagod_reg_import.reg";
        const fs::path File = fs::path(O.BoxRoot) / BoxPrefix::BoxRelForReal(FileReal, Host);
        fs::create_directories(File.parent_path(), Ec);
        std::ofstream Out(File, std::ios::binary | std::ios::trunc);
        Out << BoxPrefix::Utf8ToUtf16Le(Text);
        Out.close();
        //Through cmd, so what reg.exe says lands in a file (in the box, at OutReal) that a failure can quote.
        const std::string OutReal = SystemDrive(Host) + "\\__vidyagod_reg_import.txt";
        const int Rc = !Out ? -1 : SandboxLayer::RunInBox(O, "cmd.exe", {"/c", "reg", "import", QString::fromStdString(FileReal), ">",
                                                                          QString::fromStdString(OutReal), "2>&1"}, 60000);
        if (Rc != 0)
        {
            std::ifstream In(fs::path(O.BoxRoot) / BoxPrefix::BoxRelForReal(OutReal, Host), std::ios::binary);
            std::stringstream Said; Said << In.rdbuf();
            std::string Text = Said.str();
            while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r')) Text.pop_back();
            LogErr("BoxLaunch::Prepare", "the package's registry was NOT imported into the game's box (reg import "
                   + (!Out ? std::string("not run: the file could not be written") : Rc < 0 ? std::string("did not finish")
                                                                                     : "exit code " + std::to_string(Rc)) + ": "
                   + (Text.empty() ? std::string("it said nothing") : Text) + "; the file: " + File.string() + ").");
            //The import also carried the user's kept keys: run without them and the defaults would be stored over them.
            if (SeededKept)
            {
                LogErr("BoxLaunch::Prepare", "The import held the user's kept registry keys — the game is NOT started.");
                return false;
            }
        }
        else LogOut("BoxLaunch::Prepare", "Imported the package's registry into the game's box.");
    }

    //Last, once the box is as the game will find it: what it was given, which also marks it as this launch's — from
    //here a crash leaves a box the next Prepare saves. Without it the game does not start (its saves would go nowhere).
    {
        std::ofstream Out(PlacedFile(O), std::ios::binary | std::ios::trunc);
        for (const std::string &P : Placed) Out << P << '\n';
        Out.close();
        if (!Out) { LogErr("BoxLaunch::Prepare", "cannot write " + PlacedFile(O).string() + " — the game is NOT started."); return false; }
    }
    CP.BoxPrepared = true;
    return true;
}

bool Harvest(struct ContainerParams &CP, RegistryWrapper &Session, bool &HaveSession)
{
    HaveSession = false;
    //Only the box this launch prepared: a box left by another (a crash) is saved by the next Prepare, with its runtime
    //mounted — a teardown before or without one (a launch that failed to build) must not take or empty it.
    if (!CP.BoxPrepared) return true;
    const SandboxLayer::Options O = BoxOptions(CP);
    std::vector<std::string> Placed;
    if (!ReadPlaced(O, Placed)) { LogErr("BoxLaunch::Harvest", "the game's box lost its list " + PlacedFile(O).string()); return false; }
    HaveSession = true;
    return TakeKeeps(CP, O, Placed, Session, "BoxLaunch::Harvest");
}

void Discard(struct ContainerParams &CP)
{
    if (!CP.BoxPrepared) return;
    const SandboxLayer::Options O = BoxOptions(CP);
    SandboxLayer::TerminateBox(O);
    std::error_code Ec;
    fs::remove(PlacedFile(O), Ec);   // first: a box half emptied is no longer taken for an unfinished session
    std::vector<std::string> Errors;
    if (!Empty(O, Errors)) Report("BoxLaunch::Discard", Errors, "left in the game's box");
    CP.BoxPrepared = false;
}

#endif

} // namespace BoxLaunch
