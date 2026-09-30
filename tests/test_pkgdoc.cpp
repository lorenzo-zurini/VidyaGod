// PkgDoc — the package editor's document. Saving is a generation-6 re-mint: a node's file is named by the CID of its
// bytes, so an edit renames it, and every node that names it (here and in other packages) is re-minted in turn.

#include "vgtest.h"
#include "pkgdoc.h"
#include "cid.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

fs::path Fresh(const char *Tag)
{
    fs::path D = fs::temp_directory_path() / (std::string("vg_pkgdoc_") + Tag);
    fs::remove_all(D);
    fs::create_directories(D);
    return D;
}

//A frozen node on disk, as a published package holds it: canonical bytes named by their CID.
std::string Freeze(const fs::path &Dir, const json &N)
{
    const std::string C = Cid::OfNode(N);
    std::ofstream(Dir / (C + ".json"), std::ios::binary) << Cid::Canonical(N);
    return C;
}

std::string Slurp(const fs::path &P) { std::ifstream I(P, std::ios::binary); std::ostringstream S; S << I.rdbuf(); return S.str(); }

//Every node file in Dir is canonical bytes named by their own CID — the invariant a save must leave behind.
bool AllFrozen(const fs::path &Dir)
{
    for (const auto &E : fs::directory_iterator(Dir))
    {
        if (E.path().extension() != ".json" || !PkgDoc::LooksLikeCid(E.path().stem().string())) continue;
        const std::string B = Slurp(E.path());
        if (Cid::OfBytes(B) != E.path().stem().string()) return false;
    }
    return true;
}

int CountCidFiles(const fs::path &Dir)
{
    int N = 0;
    for (const auto &E : fs::directory_iterator(Dir))
        if (E.path().extension() == ".json" && PkgDoc::LooksLikeCid(E.path().stem().string())) ++N;
    return N;
}

json Zip(const std::string &Label, const std::string &File) { return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"ZIP", File}} })}}; }
json Over(const std::string &Label, const std::string &Ref) { return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"NODE", Ref}}, json{{"ENV", {{"A", "1"}}}} })}}; }

} // namespace

// A published package loads with each node named by its file's CID, so references between them resolve: the bug the
// old editor had (keying by a stored "CID" field gen-6 nodes do not carry) left every wire dangling and dropped nodes
// that share a LABEL. Teeth: key the handle by LABEL.
TEST(pkgdoc_a_published_package_loads_named_by_its_files)
{
    const fs::path D = Fresh("load");
    const std::string A = Freeze(D, Zip("Soundtrack", "a.zip"));
    const std::string B = Freeze(D, Zip("Soundtrack", "b.zip"));             // the same LABEL, a different node
    const std::string C = Freeze(D, Over("Game", A));
    std::ofstream(D / "notes.json") << R"(["not", "a", "node"])";
    PkgDoc::Document Doc;
    const auto Odd = Doc.Load(D);
    CHECK(Odd.empty());
    CHECK_EQ(Doc.Count(), 3);
    CHECK(Doc.IndexOf(A) >= 0);
    CHECK(Doc.IndexOf(B) >= 0);
    CHECK(Doc.IndexOf(C) >= 0);
    CHECK(!Doc.Dirty());
    CHECK(fs::exists(D / "notes.json"));
}

// Editing a node renames it, re-mints the node that contains it, and leaves every file canonical and named by its
// bytes; the replaced files are gone and the document's handles are the new names. Teeth: write back under the old
// name (the old SaveNodes) — the file no longer hashes to its name.
TEST(pkgdoc_saving_an_edit_re_mints_it_and_its_referrers)
{
    const fs::path D = Fresh("edit");
    const std::string A = Freeze(D, Zip("Base", "a.zip"));
    const std::string C = Freeze(D, Over("Game", A));
    PkgDoc::Document Doc;
    Doc.Load(D);
    json N = Doc.Node(Doc.IndexOf(A));
    N["LAYERS"][0]["ZIP"] = "a2.zip";
    Doc.Replace(Doc.IndexOf(A), N);
    CHECK(Doc.Dirty());
    const PkgDoc::SaveReport R = Doc.Save(D, "", "");
    CHECK(R.Ok);
    CHECK(!Doc.Dirty());
    CHECK(R.Renamed.count(A));
    CHECK(R.Renamed.count(C));                                               // the container re-minted too
    CHECK(!fs::exists(D / (A + ".json")));
    CHECK(!fs::exists(D / (C + ".json")));
    CHECK_EQ(CountCidFiles(D), 2);
    CHECK(AllFrozen(D));
    const int Ci = Doc.IndexOf(R.Renamed.at(C));
    CHECK(Ci >= 0);
    CHECK_EQ(Doc.Node(Ci)["LAYERS"][0]["NODE"].get<std::string>(), R.Renamed.at(A));
    const auto Moves = Doc.TakeRenames();
    CHECK_EQ(Moves.size(), (size_t)2);
    CHECK(Doc.TakeRenames().empty());                                        // consumed
    //Saving again without an edit writes nothing.
    const PkgDoc::SaveReport R2 = Doc.Save(D, "", "");
    CHECK(R2.Ok);
    CHECK_EQ(R2.Written, 0);
    CHECK_EQ(R2.Removed, 0);
}

// Another package names the edited node: it is re-minted, and so is what names IT (two levels, two packages), and a
// copy of the edited node another package keeps is renamed with it. Instances remembering an old name follow.
// Teeth: skip the cascade — the other package keeps naming a file that no longer exists.
TEST(pkgdoc_a_save_follows_renames_through_the_library_and_instances)
{
    const fs::path Lib = Fresh("lib");
    const fs::path Fonts = Lib / "fonts", Game = Lib / "game", Mod = Lib / "mod", Friend = Lib / "_friend_x";
    for (const fs::path &P : {Fonts, Game, Mod, Friend}) fs::create_directories(P);
    const std::string F = Freeze(Fonts, Zip("Fonts", "f.zip"));
    Freeze(Mod, Zip("Fonts", "f.zip"));                                      // a copy of the same node
    const std::string G = Freeze(Game, Over("Game", F));
    const std::string M = Freeze(Mod, Over("Mod", G));                       // names the game, which names the fonts
    const std::string Fr = Freeze(Friend, Over("Theirs", F));                // a friend's landed copy: never ours
    const fs::path Users = Fresh("users");
    fs::create_directories(Users / "749" / "DefaultInstance");
    std::ofstream(Users / "749" / "DefaultInstance" / "instance.json")
        << json{{"GRAFTS", {{"802", json::array({M, "other"})}}}, {"VARIABLES", {{"x", "1"}}}}.dump();

    PkgDoc::Document Doc;
    Doc.Load(Fonts);
    json N = Doc.Node(0); N["LAYERS"][0]["ZIP"] = "f2.zip"; Doc.Replace(0, N);
    const PkgDoc::SaveReport R = Doc.Save(Fonts, Lib, Users);
    CHECK(R.Ok);
    auto NewName = [&](const std::string &Old) { const auto It = R.Renamed.find(Old); return It == R.Renamed.end() ? std::string("missing") : It->second; };
    const std::string F2 = NewName(F), G2 = NewName(G), M2 = NewName(M);
    CHECK(G2 != "missing" && M2 != "missing");                               // the referrers were re-minted
    CHECK(fs::exists(Mod / (F2 + ".json")));                                 // the copy, renamed
    CHECK(!fs::exists(Mod / (F + ".json")));
    CHECK(fs::exists(Game / (G2 + ".json")) && !fs::exists(Game / (G + ".json")));
    CHECK(fs::exists(Mod / (M2 + ".json")) && !fs::exists(Mod / (M + ".json")));
    CHECK(fs::exists(Friend / (Fr + ".json")));                              // untouched
    CHECK(AllFrozen(Fonts) && AllFrozen(Game) && AllFrozen(Mod));
    const json GameNode = json::parse(Slurp(Game / (G2 + ".json")), nullptr, false);
    CHECK(GameNode.is_object() && GameNode["LAYERS"][0]["NODE"] == F2);
    const json Inst = json::parse(Slurp(Users / "749" / "DefaultInstance" / "instance.json"), nullptr, false);
    CHECK_EQ(Inst["GRAFTS"]["802"][0].get<std::string>(), M2);
    CHECK_EQ(Inst["GRAFTS"]["802"][1].get<std::string>(), std::string("other"));
    CHECK_EQ(R.InstancesUpdated, 1);
}

// A diamond: a node contains the edited node directly AND through another node that changes too. It is re-minted
// only once that other node has its new name, so no file is left naming a removed one. Teeth: re-mint each referrer
// as soon as it is reached (the library is read in path order, so "a/top" comes before "b/middle").
TEST(pkgdoc_a_diamond_in_the_library_is_re_minted_in_order)
{
    const fs::path Lib = Fresh("diamond");
    const fs::path Base = Lib / "base", A = Lib / "a", B = Lib / "b";
    for (const fs::path &P : {Base, A, B}) fs::create_directories(P);
    const std::string F = Freeze(Base, Zip("Fonts", "f.zip"));
    const std::string Mid = Freeze(B, Over("Middle", F));
    json Top = Over("Top", F);
    Top["LAYERS"].push_back(json{{"NODE", Mid}});
    const std::string T = Freeze(A, Top);

    PkgDoc::Document Doc;
    Doc.Load(Base);
    json N = Doc.Node(0); N["LAYERS"][0]["ZIP"] = "f2.zip"; Doc.Replace(0, N);
    const PkgDoc::SaveReport R = Doc.Save(Base, Lib, "");
    CHECK(R.Ok);
    CHECK(R.Renamed.count(Mid) && R.Renamed.count(T));
    const std::string T2 = R.Renamed.count(T) ? R.Renamed.at(T) : "", Mid2 = R.Renamed.count(Mid) ? R.Renamed.at(Mid) : "";
    const json TopNode = json::parse(Slurp(A / (T2 + ".json")), nullptr, false);
    CHECK(TopNode.is_object());
    if (TopNode.is_object())
    {
        CHECK_EQ(TopNode["LAYERS"][0]["NODE"].get<std::string>(), R.Renamed.at(F));
        CHECK_EQ(TopNode["LAYERS"][2]["NODE"].get<std::string>(), Mid2);   // the middle's NEW name
    }
    CHECK_EQ(CountCidFiles(A), 1);                                           // one version of the top, no stray
    CHECK(AllFrozen(A) && AllFrozen(B));
}

// A node made in the editor is a draft until saved; a wire to it names the draft, and the save names both.
TEST(pkgdoc_a_new_node_and_its_wire_get_cids_on_save)
{
    const fs::path D = Fresh("draft");
    PkgDoc::Document Doc;
    Doc.Load(D);
    const int A = Doc.Add(Zip("New", "n.zip"));
    const int B = Doc.Add(json{{"LABEL", "User"}, {"LAYERS", json::array()}});
    CHECK(Doc.Handle(A).rfind("draft-", 0) == 0);
    CHECK(Doc.Link(B, Doc.Handle(A), PkgDoc::Document::RefKind::Node));
    CHECK(!Doc.Link(B, Doc.Handle(A), PkgDoc::Document::RefKind::Node)); // already contained
    Doc.SetPos(Doc.Handle(A), {10, 20});
    const PkgDoc::SaveReport R = Doc.Save(D, "", "");
    CHECK(R.Ok);
    CHECK(PkgDoc::LooksLikeCid(Doc.Handle(0)) && PkgDoc::LooksLikeCid(Doc.Handle(1)));
    CHECK_EQ(Doc.Node(1)["LAYERS"][0]["NODE"].get<std::string>(), Doc.Handle(0));
    CHECK(Doc.Positions().count(Doc.Handle(0)));                             // the position moved with the name
    CHECK(AllFrozen(D));
}

// Two nodes containing each other cannot be named (each name includes the other's): refused, nothing written.
TEST(pkgdoc_a_reference_cycle_is_refused_and_nothing_is_written)
{
    const fs::path D = Fresh("cycle");
    PkgDoc::Document Doc;
    Doc.Load(D);
    const int A = Doc.Add(json{{"LABEL", "A"}, {"LAYERS", json::array()}});
    const int B = Doc.Add(json{{"LABEL", "B"}, {"LAYERS", json::array()}});
    Doc.Link(A, Doc.Handle(B), PkgDoc::Document::RefKind::Node);
    Doc.Link(B, Doc.Handle(A), PkgDoc::Document::RefKind::Node);
    const PkgDoc::SaveReport R = Doc.Save(D, "", "");
    CHECK(!R.Ok);
    CHECK(R.Error.find("circle") != std::string::npos);
    CHECK_EQ(CountCidFiles(D), 0);
    CHECK(Doc.Dirty());
}

// Undo and redo step whole gestures; returning to what is on disk is clean again; the history survives a save (its
// states are renamed to the new handles). Teeth: record an undo step per Replace — typing a name is many steps.
TEST(pkgdoc_undo_steps_gestures_and_survives_a_save)
{
    const fs::path D = Fresh("undo");
    const std::string A = Freeze(D, Zip("Base", "a.zip"));
    PkgDoc::Document Doc;
    Doc.Load(D);
    json N = Doc.Node(0);
    for (const char *Name : {"B", "Ba", "Bas", "Base2"}) { N["LABEL"] = Name; Doc.Replace(0, N); }
    Doc.Commit();                                                            // one gesture: typing the name
    CHECK(Doc.Dirty());
    CHECK(Doc.Undo());
    CHECK_EQ(Doc.Node(0)["LABEL"].get<std::string>(), std::string("Base"));
    CHECK(!Doc.Dirty());                                                     // back to what is on disk
    CHECK(!Doc.Undo());
    CHECK(Doc.Redo());
    CHECK_EQ(Doc.Node(0)["LABEL"].get<std::string>(), std::string("Base2"));
    //Across a save: the undo history now names the node by its new CID.
    Doc.SetPos(Doc.Handle(0), {5, 5}); Doc.Commit();
    CHECK(Doc.Save(D, "", "").Ok);
    const std::string New = Doc.Handle(0);
    CHECK(New != A);
    CHECK(Doc.Undo());                                                       // the position step
    CHECK_EQ(Doc.Handle(0), New);
    CHECK(Doc.Undo());                                                       // the rename, on the same node
    CHECK_EQ(Doc.Handle(0), New);
    CHECK_EQ(Doc.Node(0)["LABEL"].get<std::string>(), std::string("Base"));
    CHECK(Doc.Dirty());
}

// A node removed in the editor takes its references and its file with it — unless another package still contains
// it, then the file stays and the save says why. Teeth: never delete dropped files (orphans pile up).
TEST(pkgdoc_a_removed_node_goes_unless_another_package_contains_it)
{
    const fs::path Lib = Fresh("remove");
    const fs::path P = Lib / "pkg", Other = Lib / "other";
    fs::create_directories(P); fs::create_directories(Other);
    const std::string A = Freeze(P, Zip("Lone", "a.zip"));
    const std::string B = Freeze(P, Zip("Shared", "b.zip"));
    const std::string U = Freeze(P, Over("User", A));
    Freeze(Other, Over("Elsewhere", B));
    PkgDoc::Document Doc;
    Doc.Load(P);
    CHECK(Doc.Remove(Doc.IndexOf(A)));
    CHECK(Doc.Remove(Doc.IndexOf(B)));
    bool StillNamed = false;
    PkgDoc::ForEachRef(Doc.Node(Doc.IndexOf(U)), [&](const std::string &R) { if (R == A) StillNamed = true; });
    CHECK(!StillNamed);                                                      // the wire went with it
    const PkgDoc::SaveReport R = Doc.Save(P, Lib, "");
    CHECK(R.Ok);
    CHECK(!fs::exists(P / (A + ".json")));
    CHECK(fs::exists(P / (B + ".json")));
    bool Said = false;
    for (const std::string &L : R.Log) if (L.find("kept") != std::string::npos) Said = true;
    CHECK(Said);
}
