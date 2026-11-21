// NodeFiles — a package's node files are named by their labels (MetaPackageFormat chapter 02 §2.5).

#include "vgtest.h"
#include "nodefiles.h"
#include "cid.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

fs::path Fresh(const char *Tag)
{
    fs::path D = fs::temp_directory_path() / (std::string("vg_nodefiles_") + Tag);
    fs::remove_all(D);
    fs::create_directories(D);
    return D;
}

json Node(const std::string &Label, const std::string &Dir)
{
    json N = json{{"LAYERS", json::array({ json{{"DIR", Dir}} })}};
    if (!Label.empty()) N["LABEL"] = Label;
    return N;
}

std::string Tail(const std::string &C) { return C.substr(C.size() - 8); }

std::string Slurp(const fs::path &P) { std::ifstream I(P, std::ios::binary); std::ostringstream S; S << I.rdbuf(); return S.str(); }

} // namespace

//A node's file is its label made a safe name; a node without one is "node". Teeth: drop the "node" default (".json"),
//or SafeName (a '/' in a label leaves the folder).
TEST(nodefiles_a_node_file_is_named_by_its_label)
{
    const json A = Node("Cave Story 1.0.0.6", "a"), B = Node("", "b"), C = Node("a/b:c", "c");
    const auto Names = NodeFiles::Names({ {Cid::OfNode(A), A}, {Cid::OfNode(B), B}, {Cid::OfNode(C), C} });
    CHECK_EQ(Names.at(Cid::OfNode(A)), std::string("Cave Story 1.0.0.6.json"));
    CHECK_EQ(Names.at(Cid::OfNode(B)), std::string("node.json"));
    CHECK_EQ(Names.at(Cid::OfNode(C)), std::string("a_b_c.json"));
    CHECK_EQ(NodeFiles::SafeName(std::string(500, 'x')).size(), (size_t)120);
    CHECK_EQ(NodeFiles::SafeName("CON"), std::string("_CON"));
    CHECK_EQ(NodeFiles::SafeName(".."), std::string("__"));
}

//Two nodes claiming one name (without case), or a node claiming a content file's name, are EACH named by their CID's
//tail — never one plain and one bumped, which would make the name depend on which came first. Teeth: compare names
//with case (Play/play stay apart, one file on Windows); bump only the second; ignore content claims.
TEST(nodefiles_a_shared_name_is_bumped_for_every_claimant)
{
    const json P1 = Node("Play", "1"), P2 = Node("play", "2"), Solo = Node("Solo", "s");
    json Settings = Node("settings", "x");
    json Shipper = json{{"LABEL", "shipper"}, {"LAYERS", json::array({ json{{"FILE", "settings.json"}, {"SOURCE", "bafkreisettingsfile"}} })}};
    const std::string C1 = Cid::OfNode(P1), C2 = Cid::OfNode(P2), CS = Cid::OfNode(Settings);
    const auto Names = NodeFiles::Names({ {C1, P1}, {C2, P2}, {Cid::OfNode(Solo), Solo}, {CS, Settings}, {Cid::OfNode(Shipper), Shipper} });
    CHECK_EQ(Names.at(C1), "Play [" + Tail(C1) + "].json");
    CHECK_EQ(Names.at(C2), "play [" + Tail(C2) + "].json");
    CHECK_EQ(Names.at(Cid::OfNode(Solo)), std::string("Solo.json"));
    CHECK_EQ(Names.at(CS), "settings [" + Tail(CS) + "].json");   // the content file keeps settings.json
}

//Normalize names a folder's frozen node files by the rule: two files that trade names do not overwrite each other, a
//second file of one node goes (its move recorded, so references follow), and a draft is left as it is. Teeth: rename
//in one step (the swap overwrites one node); keep the twin; rename a draft.
TEST(nodefiles_normalize_names_a_folder_by_the_rule)
{
    const fs::path D = Fresh("normalize");
    const json A = Node("alpha", "a"), B = Node("beta", "b");
    const std::string CA = Cid::OfNode(A), CB = Cid::OfNode(B);
    std::ofstream(D / "beta.json", std::ios::binary) << Cid::Canonical(A);   // swapped
    std::ofstream(D / "alpha.json", std::ios::binary) << Cid::Canonical(B);
    std::ofstream(D / (CA + ".json"), std::ios::binary) << Cid::Canonical(A);  // a twin of alpha, named by its CID
    std::ofstream(D / "draft.json") << json{{"CID", "h1"}, {"LABEL", "alpha"}, {"LAYERS", json::array()}}.dump(4);   // a draft: not published, takes no part
    std::vector<std::pair<fs::path, fs::path>> Moved;
    std::string Err;
    CHECK(NodeFiles::Normalize(D, &Moved, &Err));
    CHECK_EQ(Slurp(D / "alpha.json"), Cid::Canonical(A));
    CHECK_EQ(Slurp(D / "beta.json"), Cid::Canonical(B));
    CHECK(fs::exists(D / "draft.json"));
    CHECK(!fs::exists(D / (CA + ".json")) && !fs::exists(D / (CB + ".json")));
    int Files = 0;
    for (const auto &E : fs::directory_iterator(D)) { (void)E; ++Files; }
    CHECK_EQ(Files, 3);
    CHECK_EQ(Moved.size(), (size_t)3);                                         // the twin and the two renames
    for (const auto &[From, To] : Moved) CHECK(fs::exists(To));                // a twin's move ends where its node's file is
    const auto In = NodeFiles::In(D);
    CHECK(In.at(CA).Frozen && In.at(CB).Frozen);
    CHECK(!In.at(Cid::OfNode(json{{"LABEL", "alpha"}, {"LAYERS", json::array()}})).Frozen);
    //Run again: nothing to do.
    Moved.clear();
    CHECK(NodeFiles::Normalize(D, &Moved, &Err));
    CHECK(Moved.empty());
    fs::remove_all(D);
}

//A name taken by a file that is not a node of the package is never overwritten: that node keeps its old name, and the
//failure is said. Teeth: rename over whatever is there.
TEST(nodefiles_normalize_never_overwrites_another_file)
{
    const fs::path D = Fresh("taken");
    const json A = Node("alpha", "a");
    const std::string CA = Cid::OfNode(A);
    std::ofstream(D / (CA + ".json"), std::ios::binary) << Cid::Canonical(A);
    std::ofstream(D / "alpha.json") << "not json at all";
    std::vector<std::pair<fs::path, fs::path>> Moved;
    std::string Err;
    CHECK(!NodeFiles::Normalize(D, &Moved, &Err));
    CHECK(!Err.empty());
    CHECK_EQ(Slurp(D / "alpha.json"), std::string("not json at all"));
    CHECK_EQ(Slurp(D / (CA + ".json")), Cid::Canonical(A));
    CHECK(Moved.empty());
    fs::remove_all(D);
}

//A rename that stops half-way (a crash) leaves the node under its temporary name: a .json file, so the next pass finds
//it and names it. Teeth: park under a name In() does not read (the node is lost to every later pass).
TEST(nodefiles_a_half_done_rename_is_finished_next_time)
{
    const fs::path D = Fresh("halfdone");
    const json A = Node("alpha", "a");
    const std::string CA = Cid::OfNode(A);
    std::ofstream(D / NodeFiles::ParkedName(CA), std::ios::binary) << Cid::Canonical(A);   // where a crash left it
    CHECK(NodeFiles::In(D).count(CA) == 1);
    std::vector<std::pair<fs::path, fs::path>> Moved;
    std::string Err;
    CHECK(NodeFiles::Normalize(D, &Moved, &Err));
    CHECK_EQ(Slurp(D / "alpha.json"), Cid::Canonical(A));
    fs::remove_all(D);
}
