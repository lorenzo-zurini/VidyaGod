#include <algorithm>
#include <cctype>
#include "apppaths.h"   // ConfigReadOnly
#include "cmdutil.h"
#include "appmodel.h"
#include "jsonoperations.h"
#include "packageeditor.h"
#include "packageeditormodel.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <sstream>

namespace CmdUtil
{

void Save(Command::Context &C)
{
    if (C.Model) { C.Model->save(); return; }
    if (AppPaths::ConfigReadOnly())
        throw std::runtime_error("this call runs beside the app (it carries a path override), so it cannot change the "
                                 "config: run it without the override, or quit the app");
    if (!JSONOps::SaveJSON(C.Config, new QFile(C.AppDataDir->filePath("GlobalConfig.JSON"))))
        throw std::runtime_error("could not save the config");
}

std::shared_ptr<const NodeIndex> Index(Command::Context &C)
{
    if (C.Snapshot) return C.Snapshot;   // a Background command: the snapshot taken for it on the app's thread
    if (C.Model) return C.Model->catalogSnapshot();
    return PackageCatalog::FreshCatalogIndex(*C.Config);
}

const Node *NodeOf(const NodeIndex &Idx, const std::string &Id)
{
    if (const Node *N = Idx.Find(Id)) return N;
    //A CID as a table shows it ("bafkrei…myteka": its start and end) or any unique start of one — a handle the same.
    const auto Dots = Id.find("…");
    const std::string Head = Dots == std::string::npos ? Id : Id.substr(0, Dots);
    const std::string Tail = Dots == std::string::npos ? std::string() : Id.substr(Dots + std::string("…").size());
    const auto Is = [&](const std::string &K) {
        return K.size() > Head.size() + Tail.size() && K.compare(0, Head.size(), Head) == 0
               && (Tail.empty() || K.compare(K.size() - Tail.size(), Tail.size(), Tail) == 0);
    };
    if (Head.size() >= 6 && std::all_of(Head.begin(), Head.end(), [](unsigned char C) { return std::isalnum(C); }))
    {
        std::vector<const Node *> Hits;
        for (const auto &[K, N] : Idx.Nodes)
            if (Is(K) || (!N.Handle.empty() && Is(N.Handle))) Hits.push_back(&N);
        if (Hits.size() == 1) return Hits.front();
        if (Hits.size() > 1)
        {
            std::string L;
            for (size_t I = 0; I < Hits.size() && I < 8; ++I) L += "\n  " + Hits[I]->Key() + "  " + Hits[I]->NodeId;
            throw Command::BadArgs("'" + Id + "' is the start of " + std::to_string(Hits.size()) + " nodes' CIDs; give more of it:" + L);
        }
    }
    throw Command::BadArgs("no node '" + Id + "' (a CID or any unique part of its start, a working handle, or a LABEL — VidyaGod catalog shelf lists the games)");
}

std::filesystem::path PackageDir(Command::Context &C, const std::string &Q, bool Received)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    if (fs::is_directory(Q, Ec)) return fs::weakly_canonical(Q, Ec);
    std::vector<fs::path> All;
    std::vector<std::string> Roots = { PackageCatalog::LibraryRootDir(*C.Config) };
    if (Received) Roots.push_back(PackageCatalog::CatalogRootDir(*C.Config));
    for (const std::string &R : Roots)
        for (const auto &Lib : fs::directory_iterator(R, Ec))
        {
            if (!Lib.is_directory(Ec)) continue;
            for (const auto &Pkg : fs::directory_iterator(Lib.path(), Ec))
                if (Pkg.is_directory(Ec) && Pkg.path().filename().string()[0] != '.') All.push_back(Pkg.path());
        }
    const QString W = QString::fromStdString(Q);
    std::vector<fs::path> Exact, ByUid, Part;
    for (const fs::path &D : All)
    {
        const QString N = QString::fromStdString(D.filename().string());
        if (N == W) Exact.push_back(D);
        else if (N.startsWith("[" + W + "]")) ByUid.push_back(D);
        else if (N.contains(W, Qt::CaseInsensitive)) Part.push_back(D);
    }
    for (std::vector<fs::path> *V : { &Exact, &ByUid, &Part })
    {
        if (V->size() == 1) return V->front();
        if (V->size() > 1)
        {
            std::string Names;
            for (size_t I = 0; I < V->size() && I < 6; ++I) Names += (I ? ", " : "") + (*V)[I].parent_path().filename().string() + "/" + (*V)[I].filename().string();
            throw Command::BadArgs("'" + Q + "' names " + std::to_string(V->size()) + " packages: " + Names);
        }
    }
    throw Command::BadArgs("no package '" + Q + "' (a folder path, its name, its UID, or part of its title)");
}

std::vector<std::string> List(const json &Args, const char *Key)
{
    std::vector<std::string> Out;
    if (!Args.contains(Key)) return Out;
    const json &V = Args[Key];
    if (V.is_array()) { for (const auto &X : V) if (X.is_string()) Out.push_back(X.get<std::string>()); return Out; }
    if (!V.is_string()) throw Command::BadArgs(std::string("--") + Key + " is a list: a,b,c");
    std::stringstream In(V.get<std::string>());
    for (std::string X; std::getline(In, X, ',');) if (!X.empty()) Out.push_back(X);
    return Out;
}

double Number(const std::string &V, const std::string &What)
{
    size_t N = 0;
    double D = 0;
    try { D = std::stod(V, &N); } catch (...) { N = 0; }
    if (V.empty() || N != V.size()) throw Command::BadArgs(What + " takes a number, not '" + V + "'");
    return D;
}

bool Point(const json &Args, const char *Key, double &X, double &Y)
{
    if (!Args.contains(Key)) return false;
    const auto P = List(Args, Key);
    const std::string What = std::string("--") + Key;
    if (P.size() != 2) throw Command::BadArgs(What + " takes x,y (two numbers, a comma between)");
    X = Number(P[0], What);
    Y = Number(P[1], What);
    return true;
}

std::string Canon(const std::filesystem::path &P)
{
    std::error_code Ec;
    return std::filesystem::weakly_canonical(P, Ec).string();
}

std::map<std::string, std::unique_ptr<PackageEditorModel>> &BuildSessions()
{
    static std::map<std::string, std::unique_ptr<PackageEditorModel>> K;   // the app's sessions, across calls
    return K;
}

BuildSession OpenBuild(Command::Context &C, const std::string &Pkg)
{
    const std::string P = Canon(PackageDir(C, Pkg, false));
    if (PackageEditor *E = PackageEditor::live(); E && E->model() && Canon(E->bundleDir().toStdString()) == P)
        return {nullptr, E->model(), "editor"};
    auto &K = BuildSessions();
    if (const auto It = K.find(P); It != K.end()) return {nullptr, It->second.get(), "session"};
    auto M = std::make_unique<PackageEditorModel>(C.Config, nullptr);
    M->initPackage(QString::fromStdString(P), nullptr);
    if (C.Live) { PackageEditorModel *Raw = M.get(); K[P] = std::move(M); return {nullptr, Raw, "session"}; }
    BuildSession S{std::move(M), nullptr, "this call"};
    S.M = S.Own.get();
    return S;
}

} // namespace CmdUtil
