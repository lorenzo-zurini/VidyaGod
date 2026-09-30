// PkgDoc — the package editor's document. Saving is a generation-6 re-mint: a node's file is named by the CID of its
// bytes, so an edit renames it, and every node that names it (here and in other packages) is re-minted in turn.

#include "vgtest.h"
#include "pkgdoc.h"
#include "cid.h"

#include <filesystem>
#include <fstream>
#include <set>
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

//References in every node file under Root that name no file under Root.
int Dangling(const fs::path &Root)
{
    std::set<std::string> Have;
    std::vector<json> All;
    for (const auto &E : fs::recursive_directory_iterator(Root))
        if (E.path().extension() == ".json" && PkgDoc::LooksLikeCid(E.path().stem().string()))
        {
            Have.insert(E.path().stem().string());
            All.push_back(json::parse(Slurp(E.path()), nullptr, false));
        }
    int N = 0;
    for (const json &J : All) PkgDoc::ForEachRef(J, [&](const std::string &R) { if (!Have.count(R)) ++N; });
    return N;
}

std::set<std::string> Files(const fs::path &Dir)
{
    std::set<std::string> Out;
    for (const auto &E : fs::directory_iterator(Dir)) Out.insert(E.path().filename().string());
    return Out;
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

// ---- what a save must never do (each found by an adversarial review of the first version) ------------------------

// A node here contains a node in another package that contains a node here: editing the inner one renames the
// other package's node, so the outer one here must be named after it — one graph across packages, not "this
// package, then the others". Teeth: mint this package first (the outer node names the other package's old file).
TEST(pkgdoc_a_loop_through_another_package_is_followed)
{
    const fs::path Lib = Fresh("loop"), A = Lib / "a", B = Lib / "b";
    for (const fs::path &P : {A, B}) fs::create_directories(P);
    const std::string X = Freeze(A, Zip("x", "x.zip"));
    const std::string E = Freeze(B, Over("e", X));
    Freeze(A, Over("p", E));
    PkgDoc::Document Doc;
    Doc.Load(A);
    json N = Doc.Node(Doc.IndexOf(X)); N["LAYERS"][0]["ZIP"] = "x2.zip"; Doc.Replace(Doc.IndexOf(X), N);
    const PkgDoc::SaveReport R = Doc.Save(A, Lib, "");
    CHECK(R.Ok);
    CHECK_EQ(Dangling(Lib), 0);
    CHECK(AllFrozen(A) && AllFrozen(B));
}

// Two nodes identical byte for byte would be one file: the save refuses and writes nothing (it used to merge their
// handles, and every later save flipped other packages between the two). Teeth: allow the twin.
TEST(pkgdoc_two_identical_nodes_are_refused)
{
    const fs::path D = Fresh("twins");
    PkgDoc::Document Doc;
    Doc.Load(D);
    Doc.Add(Zip("X copy", "x.zip"));
    Doc.Add(Zip("X copy", "x.zip"));
    const PkgDoc::SaveReport R = Doc.Save(D, "", "");
    CHECK(!R.Ok);
    CHECK(R.Error.find("same node") != std::string::npos);
    CHECK(Files(D).empty());
    CHECK(Doc.Dirty());
}

// A library it cannot read in full is a library it cannot follow renames through: nothing is saved, nothing removed.
// Teeth: skip what cannot be read (the unreadable package keeps naming a removed file).
TEST(pkgdoc_an_unreadable_package_stops_the_save)
{
    const fs::path Lib = Fresh("unreadable"), P = Lib / "p", O = Lib / "o";
    for (const fs::path &Q : {P, O}) fs::create_directories(Q);
    const std::string X = Freeze(P, Zip("x", "x.zip"));
    Freeze(O, Over("uses x", X));
    fs::permissions(O, fs::perms::none);
    const bool Locked = [&] { std::error_code Ec; fs::directory_iterator It(O, Ec); return (bool)Ec; }();
    PkgDoc::Document Doc;
    Doc.Load(P);
    json N = Doc.Node(0); N["LAYERS"][0]["ZIP"] = "x2.zip"; Doc.Replace(0, N);
    const PkgDoc::SaveReport R = Doc.Save(P, Lib, "");
    fs::permissions(O, fs::perms::owner_all);
    if (!Locked) return;                                  // running as root: the folder cannot be made unreadable
    CHECK(!R.Ok);
    CHECK(fs::exists(P / (X + ".json")));
    CHECK_EQ((int)Files(P).size(), 1);
    CHECK_EQ(Dangling(Lib), 0);
}

// A write that fails partway (this package's folder is read-only) undoes the ones before it — the other package's
// re-minted node — so no half-saved state is left to load as duplicates. Teeth: keep what was written.
TEST(pkgdoc_a_failed_write_undoes_the_save)
{
    const fs::path Lib = Fresh("rollback"), P = Lib / "p", O = Lib / "o";
    for (const fs::path &Q : {P, O}) fs::create_directories(Q);
    const std::string X = Freeze(P, Zip("x", "x.zip"));
    Freeze(O, Over("uses x", X));
    const auto BeforeP = Files(P), BeforeO = Files(O);
    PkgDoc::Document Doc;
    Doc.Load(P);
    json N = Doc.Node(0); N["LAYERS"][0]["ZIP"] = "x2.zip"; Doc.Replace(0, N);
    fs::permissions(P, fs::perms::owner_read | fs::perms::owner_exec);
    const PkgDoc::SaveReport R = Doc.Save(P, Lib, "");
    fs::permissions(P, fs::perms::owner_all);
    if (R.Ok) return;                                     // root writes anyway: nothing to test
    CHECK(Files(P) == BeforeP);
    CHECK(Files(O) == BeforeO);
    CHECK(Doc.Dirty());
}

// One save renames X to what Y was and Y to something new; another package keeps copies of both and a node naming
// X. Its copy of old-Y is now its copy of new-X: it must stay. Teeth: spare only files this save wrote.
TEST(pkgdoc_a_file_a_save_ends_up_with_is_never_removed)
{
    const fs::path Lib = Fresh("chain"), P = Lib / "p", O = Lib / "o";
    for (const fs::path &Q : {P, O}) fs::create_directories(Q);
    const json Jx = Zip("n", "x.zip"), Jy = Zip("n", "y.zip");
    const std::string X = Freeze(P, Jx), Y = Freeze(P, Jy);
    Freeze(O, Jx); Freeze(O, Jy);
    Freeze(O, Over("uses x", X));
    PkgDoc::Document Doc;
    Doc.Load(P);
    Doc.Replace(Doc.IndexOf(Y), Zip("n", "z.zip"));
    Doc.Replace(Doc.IndexOf(X), Jy);
    CHECK(Doc.Save(P, Lib, "").Ok);
    CHECK(fs::exists(O / (Y + ".json")));
    CHECK_EQ(Dangling(O), 0);                             // the other package stands on its own
}

// Deleting a node another package names: when that package keeps its own copy, ours goes (it used to come back on
// every reload); an instance naming it is a user too, so with no other copy the file stays and the save says so.
TEST(pkgdoc_a_deleted_node_goes_unless_it_is_the_last_copy_something_names)
{
    const fs::path Lib = Fresh("deleted"), P = Lib / "p", O = Lib / "o";
    for (const fs::path &Q : {P, O}) fs::create_directories(Q);
    const std::string X = Freeze(P, Zip("x", "x.zip"));
    Freeze(O, Zip("x", "x.zip"));
    Freeze(O, Over("uses x", X));
    PkgDoc::Document Doc;
    Doc.Load(P);
    Doc.Remove(0);
    CHECK(Doc.Save(P, Lib, "").Ok);
    PkgDoc::Document Again;
    Again.Load(P);
    CHECK_EQ(Again.Count(), 0);

    const fs::path P2 = Fresh("graft"), Users = Fresh("graft_users");
    fs::create_directories(Users / "1" / "I");
    const std::string G = Freeze(P2, Zip("graft", "g.zip"));
    std::ofstream(Users / "1" / "I" / "instance.json") << json{{"GRAFTS", {{"t", json::array({G})}}}}.dump();
    PkgDoc::Document Doc2;
    Doc2.Load(P2);
    Doc2.Remove(0);
    const PkgDoc::SaveReport R = Doc2.Save(P2, "", Users);
    CHECK(R.Ok);
    CHECK(fs::exists(P2 / (G + ".json")));
    CHECK_EQ((int)R.Kept.size(), 1);
}

// A friend's package is theirs: a save into it is refused.
TEST(pkgdoc_a_friends_package_is_never_saved)
{
    const fs::path D = Fresh("friend") / "_friend_bob" / "game";
    fs::create_directories(D);
    Freeze(D, Zip("x", "x.zip"));
    PkgDoc::Document Doc;
    Doc.Load(D);
    json N = Doc.Node(0); N["LABEL"] = "mine now"; Doc.Replace(0, N);
    CHECK(!Doc.Save(D, "", "").Ok);
    CHECK_EQ((int)Files(D).size(), 1);
}

// Another package's copy of a node here was edited in place (its bytes no longer hash to its name). A save that does
// not rename that node leaves the file alone, and its hash never renames the node here. Teeth: re-derive every
// library file the save looks at (the node here is renamed to the other package's content, and the next save
// overwrites that package's file).
TEST(pkgdoc_a_misnamed_copy_elsewhere_never_renames_a_node_here)
{
    const fs::path Lib = Fresh("misnamed"), A = Lib / "a", B = Lib / "b";
    for (const fs::path &P : {A, B}) fs::create_directories(P);
    const std::string X = Freeze(A, Zip("x", "x.zip"));
    Freeze(A, Over("p", X));
    std::ofstream(B / (X + ".json"), std::ios::binary) << Cid::Canonical(Zip("x", "edited-in-place.zip"));
    const std::string TheirBytes = Slurp(B / (X + ".json"));
    PkgDoc::Document Doc;
    Doc.Load(A);
    const int P = Doc.IndexOf(X) == 0 ? 1 : 0;
    json N = Doc.Node(P); N["LABEL"] = "p renamed"; Doc.Replace(P, N);
    const PkgDoc::SaveReport R = Doc.Save(A, Lib, "");
    CHECK(R.Ok);
    CHECK(Doc.IndexOf(X) >= 0);                           // the node here keeps its name
    CHECK_EQ(Slurp(B / (X + ".json")), TheirBytes);       // theirs is untouched
    CHECK_EQ(R.Cascaded, 0);
    CHECK(AllFrozen(A));
    CHECK_EQ(Dangling(A), 0);
}

// A save with no edits changes nothing anywhere — not even a misnamed file elsewhere that names a node here.
TEST(pkgdoc_a_save_without_edits_writes_nothing_anywhere)
{
    const fs::path Lib = Fresh("noop"), A = Lib / "a", B = Lib / "b";
    for (const fs::path &P : {A, B}) fs::create_directories(P);
    const std::string X = Freeze(A, Zip("x", "x.zip"));
    const std::string Bogus = "bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    std::ofstream(B / (Bogus + ".json"), std::ios::binary) << Cid::Canonical(Over("theirs", X));
    PkgDoc::Document Doc;
    Doc.Load(A);
    const PkgDoc::SaveReport R = Doc.Save(A, Lib, "");
    CHECK(R.Ok);
    CHECK_EQ(R.Written, 0);
    CHECK_EQ(R.Removed, 0);
    CHECK(fs::exists(B / (Bogus + ".json")));
}

// A package added from outside the library is itself one of the roots a save walks: its own files are never taken
// for another package's copies (which let a deleted node an instance names be removed). Teeth: gather the package
// being saved when it is a root.
TEST(pkgdoc_a_package_that_is_a_root_is_not_its_own_copy)
{
    const fs::path P = Fresh("localpkg"), Lib = Fresh("locallib"), Users = Fresh("local_users");
    fs::create_directories(Users / "1" / "I");
    const std::string G = Freeze(P, Zip("graft", "g.zip"));
    std::ofstream(Users / "1" / "I" / "instance.json") << json{{"GRAFTS", {{"t", json::array({G})}}}}.dump();
    PkgDoc::Document Doc;
    Doc.Load(P);
    Doc.Remove(0);
    const PkgDoc::SaveReport R = Doc.Save(P, std::vector<fs::path>{Lib, P}, Users);
    CHECK(R.Ok);
    CHECK(fs::exists(P / (G + ".json")));
    CHECK_EQ((int)R.Kept.size(), 1);
    CHECK_EQ(R.Cascaded, 0);
}
