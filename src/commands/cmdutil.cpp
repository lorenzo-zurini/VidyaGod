#include "cmdutil.h"
#include "appmodel.h"
#include "jsonoperations.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <sstream>

namespace CmdUtil
{

void Save(Command::Context &C)
{
    if (C.Model) { C.Model->save(); return; }
    if (!JSONOps::SaveJSON(C.Config, new QFile(C.AppDataDir->filePath("GlobalConfig.JSON"))))
        throw std::runtime_error("could not save the config");
}

std::shared_ptr<const NodeIndex> Index(Command::Context &C)
{
    if (C.Model) return C.Model->catalogSnapshot();
    return PackageCatalog::FreshCatalogIndex(*C.Config);
}

const Node *NodeOf(const NodeIndex &Idx, const std::string &Id)
{
    if (const Node *N = Idx.Find(Id)) return N;
    throw Command::BadArgs("no node '" + Id + "' (a CID, a working handle, or a LABEL — VidyaGod catalog shelf lists the games)");
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

} // namespace CmdUtil
