#include "pkgactions.h"
#include "noderefs.h"

#include "pkgcanvas.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "authoringsessionwindow.h"
#include "asyncwork.h"
#include "vgdelta.h"
#include "bytesource.h"
#include "commonutils.h"

#include <QDir>
#include <QImageReader>
#include <QCoreApplication>
#include <QFileDialog>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#include <iterator>
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <zip.h>
#include <filesystem>
#include <fstream>
#include <functional>

using json = nlohmann::ordered_json;
#include "pkgactions_p.h"

std::vector<PkgActions::SeenFile> PkgActions::GameSees(const PkgDoc::Document & D, const std::string & Handle,
                                                       const std::filesystem::path & Bundle, std::vector<std::string> * Notes,
                                                       const OtherFn & Other)
{
    const auto Labels = D.Labels();
    std::map<std::string, OtherNode> Others;             // asked once each
    auto Outside = [&](const std::string & H) -> const OtherNode & {
        auto It = Others.find(H);
        if (It == Others.end()) It = Others.emplace(H, Other ? Other(H) : OtherNode{}).first;
        return It->second;
    };
    auto Label = [&](const std::string & H) {
        const auto It = Labels.find(H);
        return It != Labels.end() ? It->second : D.IndexOf(H) < 0 ? Outside(H).Label : std::string();
    };
    auto Find = [&](const std::string & H) -> const json * { const int I = D.IndexOf(H); return I >= 0 ? &D.Node(I) : Outside(H).Node; };
    std::map<std::string, SeenFile> Seen;                 // by path, case-folded: a Windows game sees one file per name
    auto Add = [&](const std::string & Path, const std::string & From) {
        std::string Key = Path;
        for (char & C : Key) C = (char)std::tolower((unsigned char)C);
        auto It = Seen.find(Key);
        if (It == Seen.end()) { Seen[Key] = SeenFile{Path, From, {}}; return; }
        It->second.Under.push_back(It->second.From);       // later wins: what was there is now beneath
        It->second.From = From;
        It->second.Path = Path;
    };
    auto Dir = [&](const std::string & H) { return (D.IndexOf(H) >= 0 ? Bundle : Outside(H).Bundle).string(); };
    //Where the game sees a file of an item: through the item's view (its TAKEs, then their placements), else under
    //its placed target. A root target is the game's folder.
    auto Where = [](const Fold::Item & I, const std::string & Entry) -> std::optional<std::string> {
        std::string P;
        if (I.View.is_null()) P = I.Target.empty() ? Entry : I.Target + "/" + Entry;
        else
        {
            const auto A = Fold::ThroughView(I.View, "FILES/" + (I.Own.empty() ? Entry : I.Own + "/" + Entry));
            if (!A) return std::nullopt;
            P = A->size() > 6 ? A->substr(6) : std::string();
        }
        return P.empty() || P[0] == '%' ? (P.empty() ? std::string("%GameDir%") : P) : "%GameDir%/" + P;
    };
    const Fold::Plan P = PkgGraph::FoldOf(Handle, Find, Dir);
    for (const Fold::Item & I : P.Seq)
    {
        if (I.Kind == "EDIT" || I.Payload.empty()) continue;
        const std::string & Name = I.Payload;
        const std::string NL = Label(I.From);
        const std::string From = (NL.empty() ? I.From.substr(0, 12) : NL) + " > " + Name;
        const std::filesystem::path Folder = I.Dir;
        const auto Put = [&](const std::string & Entry) { if (const auto S = Where(I, Entry)) Add(*S, From); };
        if (I.Kind == "ZIP")
        {
            int Err = 0;
            zip_t * Za = zip_open((Folder / Name).string().c_str(), ZIP_RDONLY, &Err);
            if (!Za) { if (Notes) Notes->push_back(Name + " is not in the package folder"); continue; }
            for (zip_int64_t E = 0, Num = zip_get_num_entries(Za, 0); E < Num; ++E)
                if (const char * Nm = zip_get_name(Za, (zip_uint64_t)E, 0); Nm && *Nm && Nm[std::strlen(Nm) - 1] != '/') Put(Nm);
            zip_close(Za);
        }
        else if (I.Kind == "DIR")
        {
            std::error_code Ec;
            const std::filesystem::path Dd = Folder / Name;
            for (auto It = std::filesystem::recursive_directory_iterator(Dd, Ec); !Ec && It != std::filesystem::recursive_directory_iterator(); ++It)
                if (It->is_regular_file(Ec)) Put(std::filesystem::relative(It->path(), Dd, Ec).generic_string());
        }
        else if (I.Kind == "FILE") Put(std::filesystem::path(Name).filename().generic_string());
        else if (I.Kind == "DELTA" && Notes) Notes->push_back("+ what the patch " + Name + " rebuilds");
    }
    if (Notes)
        for (const Fold::Plan::Step & S : P.Steps)
            if (!S.Ref.empty()) Notes->push_back("+ the files of \"" + (Label(S.Ref).empty() ? S.Ref : Label(S.Ref)) + "\" (another package)");
    std::vector<SeenFile> Out;
    for (auto & [K, F] : Seen) { (void)K; Out.push_back(std::move(F)); }
    std::sort(Out.begin(), Out.end(), [](const SeenFile & A, const SeenFile & B) { return A.Path < B.Path; });
    return Out;
}

PkgActions::VersionDiff PkgActions::Compare(const PkgDoc::Document & D, const std::string & A, const std::string & B,
                                            const std::filesystem::path & Bundle)
{
    VersionDiff R;
    const auto Labels = D.Labels();
    auto Label = [&](const std::string & H) { const auto It = Labels.find(H); return It == Labels.end() ? std::string() : It->second; };
    auto Find = [&](const std::string & H) -> const json * { const int I = D.IndexOf(H); return I < 0 ? nullptr : &D.Node(I); };
    //Layers as a multiset of "TYPE name": what one folds that the other does not.
    std::map<std::string, int> La, Lb;
    for (const auto & S : PkgGraph::FoldOrder(A, Find, Label)) ++La[S.Type + "  " + S.Title];
    for (const auto & S : PkgGraph::FoldOrder(B, Find, Label)) ++Lb[S.Type + "  " + S.Title];
    for (const auto & [K, N] : La) for (int I = Lb.count(K) ? Lb[K] : 0; I < N; ++I) R.LayersOnlyA.push_back(K);
    for (const auto & [K, N] : Lb) for (int I = La.count(K) ? La[K] : 0; I < N; ++I) R.LayersOnlyB.push_back(K);
    std::map<std::string, SeenFile> Fa, Fb;
    auto Key = [](std::string P) { for (char & C : P) C = (char)std::tolower((unsigned char)C); return P; };
    for (auto & F : GameSees(D, A, Bundle)) Fa[Key(F.Path)] = F;
    for (auto & F : GameSees(D, B, Bundle)) Fb[Key(F.Path)] = F;
    for (const auto & [K, F] : Fa)
    {
        const auto It = Fb.find(K);
        if (It == Fb.end()) R.FilesOnlyA.push_back(F.Path);
        else if (It->second.From != F.From) R.FilesDiffer.push_back(F.Path + "  (" + F.From + "  vs  " + It->second.From + ")");
    }
    for (const auto & [K, F] : Fb) if (!Fa.count(K)) R.FilesOnlyB.push_back(F.Path);
    return R;
}
