#include "jsonoperations.h"
#include "apppaths.h"

#include <QSaveFile>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <map>
#include <optional>
#include <thread>
#include "commonutils.h"

JSONOps::JSONOps() {}

//Parses the file at JSONFile into JSONDocument.
//Uses nlohmann::ordered_json::accept() for a cheap validity pre-check before the full
//parse, which avoids throwing exceptions on malformed input.
//Returns 0 on success, 1 on failure — intentionally matching shell exit-code convention
//so callers can test with `if (LoadJSON(...))` to detect failure.
bool JSONOps::LoadJSON(QFile * JSONFile, nlohmann::ordered_json * JSONDocument)
{
    LogOut("JSONOperations", "Parsing JSON " + JSONFile->fileName().toStdString());

    if (!JSONFile->exists())
    {
        LogErr("JSONOperations", "File " + JSONFile->fileName().toStdString() + " does not exist.");
        return 1; //fail
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        LogSucc("JSONOperations", "File " + JSONFile->fileName().toStdString() + " opened for reading successfully!");
        QByteArray JSONFileData = JSONFile->readAll();

        //Pre-validate before parsing to avoid exceptions on corrupt/empty files.
        if (nlohmann::ordered_json::accept(JSONFileData))
        {
            LogOut("JSONOperations", "File " + JSONFile->fileName().toStdString() + " appears valid, parsing.");
            (*JSONDocument) = nlohmann::ordered_json::parse(JSONFileData);
            //std::cout << JSONDocument->dump(4) << std::endl;
            JSONFile->close();
            LogOut("JSONOperations", "Parse done!");
            return 0; //success
        }
        else
        {
            LogErr("JSONOperations", "Invalid JSON!");
            return 1; //fail
        }
    }
    else
    {
        LogErr("JSONOperations", "Could not open file for reading!");
        return 1; //fail
    }
}

//The config's writes, 4-space pretty-printed.
//
//ATOMIC. This writes GlobalConfig.JSON — PackageSources, peers, module toggles, every persisted
//CustomVar — and it used to truncate in place. A crash, a kill or ENOSPC part
//way through left the user with a half-written config or a half-written package and no second copy. QSaveFile
//writes a sibling temp and renames on commit, so the file on disk is always either the old one or the new one.
namespace {
//Every config write, in the order its text was taken (the config's own order: the text is taken on the thread that
//changes it). Gen numbers a text when taken; Committed is the newest written, per file (a newer text of one data
//folder's config says nothing of another's). WriteMu orders the writes themselves.
std::mutex                  WriteMu;
std::atomic<unsigned long long> Gen{0};
std::map<std::filesystem::path, unsigned long long> Committed;   // under WriteMu
std::atomic<long long>      Writes{0};
std::mutex                  QueueMu;
std::condition_variable     QueueCv;
struct PendingWrite { std::string Text; std::filesystem::path File; unsigned long long N = 0; };
std::map<std::filesystem::path, PendingWrite> Waiting;   // per file, the newest text not written yet (an older one is dropped)
bool                        WorkerBusy = false;
bool                        Hold = false;

bool WriteText(const std::string &Text, const std::filesystem::path &File, unsigned long long N)
{
    std::lock_guard<std::mutex> Lk(WriteMu);
    unsigned long long &Newest = Committed[File];
    if (N <= Newest) return true;                    // a newer text of this file is on disk already
    QSaveFile Out(QString::fromStdString(File.string()));
    if (!Out.open(QFile::WriteOnly))
    {
        LogErr("JSONOperations", "could not open for writing: " + File.string());
        return false;
    }
    Out.write(Text.data(), static_cast<qint64>(Text.size()));
    //commit() performs the rename AND reports a write that failed on flush (a full disk fails HERE, not at open).
    if (!Out.commit())
    {
        LogErr("JSONOperations", "could not save " + File.string() + ": " + Out.errorString().toStdString());
        return false;
    }
    Newest = N;
    ++Writes;
    LogOut("JSONOperations", "Saved " + File.string());
    return true;
}

void WorkerLoop()
{
    for (;;)
    {
        PendingWrite W;
        {
            std::unique_lock<std::mutex> Lk(QueueMu);
            QueueCv.wait(Lk, [] { return !Hold && !Waiting.empty(); });
            W = std::move(Waiting.begin()->second);
            Waiting.erase(Waiting.begin());
        }
        WriteText(W.Text, W.File, W.N);
        {
            std::lock_guard<std::mutex> Lk(QueueMu);
            if (Waiting.empty()) { WorkerBusy = false; QueueCv.notify_all(); return; }
        }
    }
}
}

bool JSONOps::SaveConfig(const nlohmann::ordered_json &Config, const std::filesystem::path &DataDir)
{
    const std::filesystem::path File = DataDir / "GlobalConfig.JSON";
    if (AppPaths::ConfigReadOnly())
    {
        LogWarn("JSONOperations", "not saving " + File.string() + ": this call runs beside the app that holds it (read-only)");
        return false;
    }
    //Dumped straight from the config: a deep copy of it, then a round trip through UTF-16, cost more than the dump.
    const std::string Text = Config.dump(4);
    return WriteText(Text, File, ++Gen);
}

bool JSONOps::SaveConfigAsync(const nlohmann::ordered_json &Config, const std::filesystem::path &DataDir)
{
    const std::filesystem::path File = DataDir / "GlobalConfig.JSON";
    if (AppPaths::ConfigReadOnly())
    {
        LogWarn("JSONOperations", "not saving " + File.string() + ": this call runs beside the app that holds it (read-only)");
        return false;
    }
    PendingWrite W{ Config.dump(4), File, 0 };
    W.N = ++Gen;
    std::lock_guard<std::mutex> Lk(QueueMu);
    Waiting[File] = std::move(W);
    if (!WorkerBusy) { WorkerBusy = true; std::thread(WorkerLoop).detach(); }
    else QueueCv.notify_all();
    return true;
}

void JSONOps::FlushConfigWrites()
{
    std::unique_lock<std::mutex> Lk(QueueMu);
    Hold = false;
    QueueCv.notify_all();
    QueueCv.wait(Lk, [] { return !WorkerBusy; });
}

long long JSONOps::DebugConfigWrites() { return Writes.load(); }

void JSONOps::DebugHoldConfigWrites(bool H)
{
    std::lock_guard<std::mutex> Lk(QueueMu);
    Hold = H;
    QueueCv.notify_all();
}

