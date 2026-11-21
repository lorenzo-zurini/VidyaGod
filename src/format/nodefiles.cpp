#include "nodefiles.h"

#include "cid.h"
#include "contentfile.h"
#include "manifestmodel.h"   // IsNodeObject
#include "nodegraph.h"       // ParseBounded

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace NodeFiles
{

namespace fs = std::filesystem;

std::string SafeName(const std::string &In)
{
    std::string O;
    for (char c : In) O.push_back((std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'
                                   || c == ' ' || c == '[' || c == ']' || c == '(' || c == ')') ? c : '_');
    if (O.size() > 120) O.resize(120);
    if (O.find_first_not_of('.') == std::string::npos) O = std::string(O.size() ? O.size() : 1, '_');   // "", ".", ".."
    //Names Windows cannot hold as given — the same on every platform, so every machine has the same name: a trailing
    //'.' or ' ' is silently dropped there (a folder "nick - .." is created as "nick -", and every path through the
    //name then misses it), and a device name (CON, NUL, COM1, …, with any extension) is no file at all.
    for (size_t I = O.size(); I > 0 && (O[I - 1] == '.' || O[I - 1] == ' '); --I) O[I - 1] = '_';
    std::string Stem = O.substr(0, O.find('.'));
    while (!Stem.empty() && Stem.back() == ' ') Stem.pop_back();
    for (char &C : Stem) C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
    static const std::set<std::string> Devices = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    if (Devices.count(Stem)) O = "_" + O;
    return O;
}

std::map<std::string, std::string> Names(const std::map<std::string, nlohmann::ordered_json> &Package)
{
    ContentFile::ClaimMap Content;
    for (const auto &[Cid, Node] : Package) ContentFile::ClaimsOfNode(Node, Content);
    std::map<std::string, std::string> Own;                                   // CID → its own name
    std::map<std::string, std::vector<std::string>, ContentFile::NameLess> ByName;   // a name, without case → its CIDs
    for (const auto &[Cid, Node] : Package)
    {
        const std::string Label = Node.is_object() && Node.contains("LABEL") && Node["LABEL"].is_string()
                                      ? Node["LABEL"].get<std::string>() : std::string();
        Own[Cid] = (Label.empty() ? std::string("node") : SafeName(Label)) + ".json";
        ByName[Own[Cid]].push_back(Cid);
    }
    std::map<std::string, std::string> Out;
    for (const auto &[Name, Cids] : ByName)
    {
        const bool Bump = Cids.size() > 1 || Content.count(Name);
        for (const std::string &C : Cids) Out[C] = Bump ? ContentFile::Bumped(Own[C], C) : Own[C];
    }
    return Out;
}

std::map<std::string, File> In(const fs::path &Dir, std::vector<fs::path> *Twins)
{
    std::map<std::string, File> Out;
    std::error_code Ec;
    std::vector<fs::path> Paths;
    for (const auto &E : fs::directory_iterator(Dir, Ec))
    {
        std::error_code Fe;
        if (E.is_regular_file(Fe) && E.path().extension() == ".json") Paths.push_back(E.path());
    }
    std::sort(Paths.begin(), Paths.end());   // the same file wins a twin on every machine
    for (const fs::path &P : Paths)
    {
        std::error_code Fe;
        if (fs::file_size(P, Fe) > (8u << 20) || Fe) continue;       // never a node: a node is one 256 KiB block
        std::ifstream F(P, std::ios::binary);
        std::ostringstream S;
        S << F.rdbuf();
        const std::string Bytes = S.str();
        nlohmann::ordered_json J = NodeGraph::ParseBounded(Bytes);
        if (J.is_discarded() || !ManifestModel::IsNodeObject(J)) continue;
        const std::string Cid = Cid::OfNode(J);
        if (Cid.empty()) continue;                                    // over one block: not a node any machine holds
        if (Out.count(Cid)) { if (Twins) Twins->push_back(P); continue; }
        const bool Frozen = !J.contains("CID") && !J.contains("POS") && Bytes == Cid::Canonical(J);
        Out.emplace(Cid, File{ P, std::move(J), Frozen });
    }
    return Out;
}

std::string ParkedName(const std::string &Cid) { return ".renaming-" + Cid + ".json"; }

bool Normalize(const fs::path &Dir, std::vector<std::pair<fs::path, fs::path>> *Moved, std::string *Error)
{
    std::vector<fs::path> Twins;
    const std::map<std::string, File> Files = In(Dir, &Twins);
    std::map<std::string, nlohmann::ordered_json> Package;                  // the frozen nodes: what is published
    for (const auto &[C, F] : Files) if (F.Frozen) Package.emplace(C, F.Node);
    const std::map<std::string, std::string> Want = Names(Package);
    bool Ok = true;
    auto Fail = [&](const std::string &Why) { Ok = false; if (Error && Error->empty()) *Error = Why; };
    //Twins first (each the same bytes as a file that stays), so a twin never holds a name another node takes. What
    //referred to a twin is pointed at its node's file where that file ends up, below.
    std::vector<std::pair<fs::path, std::string>> Gone;                      // twin → its node's CID
    for (const fs::path &T : Twins)
    {
        std::error_code Ec;
        nlohmann::ordered_json J;
        if (!NodeGraph::ReadTreeJsonBounded(T, J)) continue;
        const std::string C = Cid::OfNode(J);
        if (!Files.count(C) || !fs::remove(T, Ec)) continue;
        Gone.push_back({ T, C });
    }
    //Then the renames, through a name no node file has, so that two files trading names never overwrite each other.
    //That name ends in .json: a rename that stops half-way (a crash) leaves a node file that is read, and named,
    //next time — never one nothing finds.
    struct Step { std::string Cid; fs::path From, Tmp, To; };
    std::vector<Step> Steps;
    std::map<std::string, fs::path> Final;                                     // CID → where its file ends up
    for (const auto &[C, F] : Files)
    {
        Final[C] = F.Path;
        if (!F.Frozen) continue;
        const fs::path To = Dir / Want.at(C);
        if (F.Path.filename() == To.filename()) continue;
        Steps.push_back({ C, F.Path, Dir / ParkedName(C), To });
    }
    std::vector<Step> Parked;
    for (const Step &S : Steps)
    {
        std::error_code Ec;
        fs::rename(S.From, S.Tmp, Ec);
        if (Ec) { Fail("cannot rename " + S.From.string() + ": " + Ec.message()); continue; }
        Parked.push_back(S);
    }
    for (const Step &S : Parked)
    {
        std::error_code Ec;
        //A name taken by a file that is no node file of the package (a stray copy, a hand-made file): that node keeps
        //its old name — a rename never replaces a file.
        const bool Taken = fs::exists(S.To, Ec) || Ec;
        if (!Taken) fs::rename(S.Tmp, S.To, Ec);
        if (Taken || Ec)
        {
            std::error_code Back;
            fs::rename(S.Tmp, S.From, Back);
            const fs::path Now = Back ? S.Tmp : S.From;                        // where the node is: found next time either way
            if (Moved && Now != S.From) Moved->push_back({ S.From, Now });
            Final[S.Cid] = Now;
            Fail(Taken ? S.To.string() + " is taken by a file that is not this node: " + Now.filename().string() + " keeps its name"
                       : "cannot rename " + S.From.string() + " to " + S.To.filename().string() + ": " + Ec.message());
            continue;
        }
        Final[S.Cid] = S.To;
        if (Moved) Moved->push_back({ S.From, S.To });
    }
    if (Moved) for (const auto &[T, C] : Gone) Moved->push_back({ T, Final[C] });
    return Ok;
}

}
