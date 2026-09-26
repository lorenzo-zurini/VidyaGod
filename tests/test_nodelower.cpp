// NodeLower — the schema FRONT-END (generation 6). It checks a node's vocabulary and lowers each typed layer into the
// engine's ops, so it is the single place the on-disk format meets the engine. The lowering is the ONLY code that
// reads most payload keys, so anything it forgets is invisible everywhere else — the node parses, validates and
// launches, and one layer just quietly never happens. So these tests assert the MAPPING, field by field, per type,
// and that hostile, type-confused input is refused rather than thrown on (a node file arrives from a peer).

#include "vgtest.h"
#include "nodelower.h"
#include "manifestmodel.h"

using ordered_json = nlohmann::ordered_json;

namespace {

ordered_json Node1(const ordered_json &Layer) { return { {"LABEL", "n"}, {"LAYERS", ordered_json::array({ Layer })} }; }
ordered_json Lower(const ordered_json &Layer) { return NodeLower::LowerNode(Node1(Layer), "n"); }
bool Refused(const ordered_json &Layer) { return !NodeLower::CheckNode(Node1(Layer), "n").empty(); }

} // namespace

TEST(lower_content_maps_the_type_key_and_carries_placement)
{
    const char *Kinds[][2] = { {"ZIP", "VFSZipLayer"}, {"DELTA", "VFSDeltaLayer"}, {"FILE", "VFSFileLayer"} };
    for (const auto &K : Kinds)
    {
        const ordered_json L = Lower({ {K[0], "p.bin"}, {"TARGET", "FILES/C:/%PackageUID%"}, {"SOURCE", "Qm9"}, {"SIZE", 12},
                                       {"SUBMOUNTS", ordered_json::array({"a/x.dll:C:/g/x.dll"})}, {"WHEN", "%m%==1"} });
        if (L.empty()) { CHECK(!L.empty()); continue; }
        CHECK_EQ(L.size(), (size_t)1);
        CHECK_EQ(L[0]["TYPE"].get<std::string>(), std::string(K[1]));
        CHECK_EQ(L[0]["PATH"].get<std::string>(), std::string("p.bin"));
        CHECK_EQ(L[0]["TARGET"].get<std::string>(), std::string("C:/%PackageUID%"));   // the FILES namespace is the address, not the path
        CHECK_EQ(L[0]["SOURCE"]["TYPE"].get<std::string>(), std::string("ipfs"));
        CHECK_EQ(L[0]["SOURCE"]["CID"].get<std::string>(), std::string("Qm9"));
        CHECK_EQ(L[0]["SOURCE"]["SIZE"].get<int>(), 12);
        CHECK_EQ(L[0]["SUBMOUNTS"][0].get<std::string>(), std::string("a/x.dll:C:/g/x.dll"));
        CHECK_EQ(L[0]["WHEN"].get<std::string>(), std::string("%m%==1"));
    }
    //A DIR is a host directory: it lowers with its placement, and has no SOURCE to fetch.
    const ordered_json D = Lower({ {"DIR", "%DefaultPfxDir%"}, {"TARGET", "FILES/pfx"} });
    CHECK(D.size() == 1 && D[0]["TYPE"] == "VFSDirLayer" && D[0]["TARGET"] == "pfx");
    CHECK(Refused({ {"DIR", "d"}, {"SOURCE", "Qm9"} }));
    //No TARGET is the mount root: no TARGET key, not an empty one.
    CHECK(!Lower({ {"ZIP", "p.zip"} })[0].contains("TARGET"));
    CHECK(!Lower({ {"ZIP", "p.zip"}, {"TARGET", "FILES"} })[0].contains("TARGET"));
}

TEST(lower_edit_splits_text_and_binary_ops_onto_the_target_file)
{
    const ordered_json L = Lower({ {"EDIT", ordered_json::array({
            {{"MODE", "ConfigWrite"}, {"SECTION", "D"}, {"KEY", "W"}, {"VALUE", "%ScreenWidth%"}},
            {{"MODE", "Replace"}, {"OFFSET", "0x10"}, {"EXPECT", "00"}, {"REPLACE", "01"}},
            {{"MODE", "AppendLine"}, {"VALUE", "x"}} })},
        {"TARGET", "FILES/C:/g/a.bin"}, {"WHEN", "%w%==1"} });
    CHECK_EQ(L.size(), (size_t)3);
    CHECK_EQ(L[0]["TYPE"].get<std::string>(), std::string("FileEdit"));
    CHECK_EQ(L[0]["SECTION"].get<std::string>(), std::string("D"));
    CHECK_EQ(L[0]["KEY"].get<std::string>(), std::string("W"));
    CHECK_EQ(L[1]["TYPE"].get<std::string>(), std::string("BinaryPatch"));
    CHECK_EQ(L[1]["EXPECT"].get<std::string>(), std::string("00"));
    CHECK_EQ(L[2]["TYPE"].get<std::string>(), std::string("FileEdit"));
    for (const auto &O : L)
    {
        CHECK_EQ(O["FILE"].get<std::string>(), std::string("C:/g/a.bin"));
        CHECK_EQ(O["WHEN"].get<std::string>(), std::string("%w%==1"));        // the layer's WHEN reaches every op
    }
}

TEST(lower_reg_flattens_the_hive_tree_per_architecture)
{
    const ordered_json L = Lower({ {"REG", {{"HKCU", {{"Software", {{"A", {{"k", "v"}, {"Sub", {{"k2", "v2"}}}}}, {"Empty", ordered_json::object()}}}}}}},
                                   {"ARCH", ordered_json::array({"32", "64"})} });
    int A = 0, Sub = 0, Empty = 0;
    for (const auto &R : L)
    {
        CHECK_EQ(R["TYPE"].get<std::string>(), std::string("RegEdit"));
        const std::string P = R["REGPATH"].get<std::string>();
        if (P == "HKCU\\Software\\A")          { ++A;     CHECK_EQ(R["KEYVALUES"]["k"].get<std::string>(), std::string("v")); }
        if (P == "HKCU\\Software\\A\\Sub")     { ++Sub;   CHECK_EQ(R["KEYVALUES"]["k2"].get<std::string>(), std::string("v2")); }
        if (P == "HKCU\\Software\\Empty")      { ++Empty; CHECK(R["KEYVALUES"].empty()); }   // an empty key is created
    }
    CHECK_EQ(A, 2); CHECK_EQ(Sub, 2); CHECK_EQ(Empty, 2);                  // one per architecture view
}

TEST(lower_dll_vars_and_keep)
{
    const ordered_json D = Lower({ {"DLL", {{"d3d8", "n,b"}, {"ddraw", ""}, {"gone", nullptr}}} });
    CHECK_EQ(D.size(), (size_t)2);                                         // null = a delete in the fold, not an op here
    CHECK_EQ(D[0]["DLLOVERRIDE"].get<std::string>(), std::string("d3d8=n,b"));
    CHECK_EQ(D[1]["DLLOVERRIDE"].get<std::string>(), std::string("ddraw="));   // an empty order is preserved

    const ordered_json V = Lower({ {"VARS", {{"K", {{"DEFAULT", "1"}, {"COMMENT", "c"}, {"WHEN", "%x%==1"},
                                                    {"UI", {{"LABEL", "Knob"}, {"CONTROL", "bool"}}}}}}} });
    CHECK_EQ(V[0]["TYPE"].get<std::string>(), std::string("CustomVar"));
    CHECK_EQ(V[0]["KEY"].get<std::string>(), std::string("K"));
    CHECK_EQ(V[0]["DEFAULT"].get<std::string>(), std::string("1"));
    CHECK_EQ(V[0]["WHEN"].get<std::string>(), std::string("%x%==1"));     // a declaration's WHEN gates its value
    CHECK_EQ(V[0]["UI"]["CONTROL"].get<std::string>(), std::string("bool"));

    const ordered_json K = Lower({ {"KEEP", {{"FILES/%UserProfile%/Saves/", {{"NAME", "Saves"}, {"CLOUD", false}}},
                                            {"FILES/%UserProfile%/Saves/cfg.ini", false},
                                            {"REG/HKCU/Software/Game", true}}} });
    CHECK_EQ(K.size(), (size_t)2);                                         // a KEEP false persists nothing
    CHECK_EQ(K[0]["SCOPE"].get<std::string>(), std::string("file"));
    CHECK_EQ(K[0]["PATH"].get<std::string>(), std::string("%UserProfile%/Saves/"));
    CHECK_EQ(K[0]["TARGET"].get<std::string>(), std::string("Saves"));
    CHECK(!K[0]["CLOUD"].get<bool>());
    CHECK_EQ(K[1]["SCOPE"].get<std::string>(), std::string("registry"));
    CHECK_EQ(K[1]["PATH"].get<std::string>(), std::string("HKCU\\Software\\Game"));
}

TEST(lower_facts_and_refs_lower_to_no_op)
{
    for (const ordered_json &L : { ordered_json{ {"NODE", "x"} }, ordered_json{ {"ANY", ordered_json::array({"x"})} },
                                   ordered_json{ {"NODE", "x"}, {"TAKE", ordered_json::array({   // distinct landings; contents merge
                                       "FILES/a/", "FILES/b/", "REG", "FILES/a/x.dll", "FILES/b/y.dll",
                                       ordered_json::array({"FILES/c/x.dll", "z.dll"}) })} },
                                   ordered_json{ {"NOT", "x"} }, ordered_json{ {"ENV", {{"A", "1"}}} },
                                   ordered_json{ {"EXEC", ordered_json::array({ {{"LABEL", "Play"}, {"HOST", "win32"}} })} } })
    {
        CHECK(!Refused(L));
        CHECK(Lower(L).empty());
    }
}

TEST(lower_entry_splits_a_game_entry_from_a_runner_entry)
{
    const ordered_json G = NodeLower::LowerEntry({ {"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "C:/g.exe"},
                                                  {"ARGS", ordered_json::array({"-w"})}, {"WORKDIR", "C:/g"} });
    CHECK_EQ(G["PLATFORM"].get<std::string>(), std::string("win32"));
    CHECK_EQ(G["CONTENTPATH"].get<std::string>(), std::string("C:/g.exe"));
    CHECK_EQ(G["EXEARGS"][0].get<std::string>(), std::string("-w"));
    CHECK_EQ(G["WORKDIR"].get<std::string>(), std::string("C:/g"));
    CHECK(!G.contains("GUEST"));
    const ordered_json R = NodeLower::LowerEntry({ {"LABEL", "run"}, {"HOST", "linux64"}, {"GUEST", ordered_json::array({"win32"})},
                                                  {"EXE", "%RunnerMount%/proton"}, {"CONTENT_ROOT", "pfx/drive_c/%PackageUID%"},
                                                  {"PREFIX_GENERATE", true}, {"GUEST_ROOTS", {{"C:", "%PrefixRoot%/drive_c"}}} });
    CHECK_EQ(R["HOST"].get<std::string>(), std::string("linux64"));
    CHECK_EQ(R["EXECUTABLE"].get<std::string>(), std::string("%RunnerMount%/proton"));
    CHECK(R["ARGS"].is_array() && R["ARGS"].empty());
    CHECK(R["PREFIX_GENERATE"].get<bool>());
    CHECK_EQ(R["GUEST_ROOTS"]["C:"].get<std::string>(), std::string("%PrefixRoot%/drive_c"));
    CHECK_EQ(R["CONTENT_ROOT"].get<std::string>(), std::string("pfx/drive_c/%PackageUID%"));
}

TEST(check_refuses_type_confused_payloads_instead_of_throwing)
{
    const std::vector<ordered_json> Hostile = {
        { {"ZIP", 5} }, { {"ZIP", "x"}, {"SOURCE", {{"CID", 5}}} }, { {"ZIP", "x"}, {"SIZE", "big"} },
        { {"ZIP", "x"}, {"SUBMOUNTS", "a:b"} }, { {"ZIP", "x"}, {"WHEN", 1} }, { {"ZIP", "x"}, {"TARGET", 7} },
        { {"NODE", 5} }, { {"NODE", "x"}, {"TAKE", "FILES"} }, { {"NODE", "x"}, {"TAKE", ordered_json::array({ ordered_json::array({"a"}) })} },
        { {"EDIT", "x"}, {"TARGET", "FILES/f"} }, { {"EDIT", ordered_json::array({ {{"MODE", 1}} })}, {"TARGET", "FILES/f"} },
        { {"REG", "x"} }, { {"REG", ordered_json::object()}, {"ARCH", "32"} }, { {"REG", ordered_json::object()}, {"ARCH", ordered_json::array({"16"})} },
        { {"VARS", {{"K", "not-an-object"}}} }, { {"VARS", {{"K", {{"DEFAULT", 3}}}}} }, { {"VARS", {{"K", {{"UI", "x"}}}}} },
        { {"ENV", {{"A", 1}}} }, { {"DLL", ordered_json::array()} },
        { {"EXEC", ordered_json::array({ {{"HOST", "win32"}} })} },                                  // no LABEL
        { {"EXEC", ordered_json::array({ {{"LABEL", "P"}, {"ARGS", ordered_json::array({1})}} })} },
        { {"EXEC", ordered_json::array({ {{"LABEL", "P"}, {"TILE", "x"}} })} },
        { {"EXEC", ordered_json::array({ {{"LABEL", "P"}, {"TILE", {{"UID", 802}}}} })} },
        { {"EXEC", ordered_json::array({ {{"LABEL", "P"}, {"PREFIX_GENERATE", "yes"}} })} },
        { {"KEEP", {{"FILES/x", "yes"}}} }, { {"ANY", ordered_json::array()} }, { {"ANY", ordered_json::array({1})} }, { {"NOT", 1} },
        { {"ZIP", "x"}, {"EXTRA", 1} },
        //two selections landing on one address (the same leaf name, a pair renamed onto a leaf, case aside)
        { {"NODE", "x"}, {"TAKE", ordered_json::array({"FILES/a/d3d8.dll", "FILES/b/D3D8.dll"})} },
        { {"NODE", "x"}, {"TAKE", ordered_json::array({"FILES/a/x.cfg", ordered_json::array({"FILES/b/y.cfg", "x.cfg"})})} },
    };
    for (const auto &L : Hostile)
    {
        CHECK(Refused(L));
        Node N;
        CHECK(ManifestModel::ParseNode(Node1(L), "f.json", "/b", N));         // indexed with the reason, never thrown
        CHECK(!N.LowerError.empty());
    }
    //And a node that is not an object with a LAYERS list is not a node at all.
    Node N;
    CHECK(!ManifestModel::ParseNode(ordered_json{{"LABEL", "x"}}, "f.json", "/b", N));
    CHECK(!ManifestModel::ParseNode(ordered_json::array(), "f.json", "/b", N));
}

// A node arrives from a peer, and its file names direct DOWNLOADS: an absolute name or a ".." would write outside the
// package (a desktop autostart entry, a shell rc). The vocabulary refuses them — a node that fails lowers to nothing
// and never enters a resolution — while a runtime path anchored at a %VARIABLE% stays legal.
TEST(check_refuses_content_and_cover_names_that_leave_the_package)
{
    for (const char *Bad : { "../x.zip", "a/../../x.zip", "/home/u/.bashrc", "\\\\evil", "C:/x.zip", "c:\\x.zip", "..", "%RunnerMount%/../../etc" })
    {
        CHECK(Refused({ {"ZIP", Bad} }));
        CHECK(Refused({ {"DIR", Bad} }));
    }
    for (const char *Ok : { "x.zip", "sub/x.zip", "%RunnerMount%/extra", "..x.zip", "a..b/c.zip" })
        CHECK(!Refused({ {"ZIP", Ok} }));
    auto Tile = [](const ordered_json &Cover) {
        return ordered_json{ {"EXEC", ordered_json::array({ { {"LABEL", "Play"}, {"TILE", { {"UID", "1"}, {"COVER", Cover} }} } })} };
    };
    CHECK(!Refused(Tile({ {"FILE", "cover.png"}, {"SOURCE", "bafkrei"} })));
    CHECK(Refused(Tile({ {"FILE", "../cover.png"} })));
    CHECK(Refused(Tile({ {"FILE", "/tmp/cover.png"} })));
    CHECK(Refused(Tile({ {"FILE", 5} })));
    CHECK(Refused(Tile({ {"FILE", "c.png"}, {"SOURCE", ordered_json::object()} })));
}
