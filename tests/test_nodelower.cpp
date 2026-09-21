// NodeLower — the schema FRONT-END. It expands one flat node (TYPE + hoisted payload) into the ordered layer
// sequence the executor already consumes, so it is the single place the on-disk format meets the engine.
//
// It was entirely uncovered, and that is precisely how a dropped field survives: the lowering is the ONLY code
// that reads most payload keys, so anything it forgets is invisible everywhere else — the node still parses, the
// package still validates, the game still launches, and one layer just quietly never happens. A RegEdit entry's
// WHEN was dropped exactly this way: it round-tripped through the editor, survived every save, and applied
// unconditionally at launch.
//
// So these tests assert the MAPPING, field by field, for every TYPE.

#include "vgtest.h"
#include "nodelower.h"
#include "manifestmodel.h"

using ordered_json = nlohmann::ordered_json;

namespace {

ordered_json Lower(const ordered_json &Node, std::string *Err = nullptr)
{
    std::string E;
    ordered_json Out = NodeLower::Lower(Node, Node.value("LABEL", std::string("n")), E);
    if (Err) *Err = E;
    return Out;
}

//A REFUSAL is an empty array plus a non-empty Err — NOT an empty array alone, which is what a valid Group
//returns. Callers must distinguish by Err (ParseNode does), so the tests do too.
bool Refused(const ordered_json &Node)
{
    std::string E;
    const std::string Nm = (Node.contains("LABEL") && Node["LABEL"].is_string()) ? Node["LABEL"].get<std::string>() : std::string("n");
    const ordered_json Out = NodeLower::Lower(Node, Nm, E);
    return Out.is_array() && Out.empty() && !E.empty();
}

} // namespace

// A node file is UNTRUSTED INPUT — it arrives from a peer over IPFS, or from an author's typo — and every
// consumer downstream reads the lowered layers with nlohmann's .value(), which THROWS on a type mismatch.
// Those reads are outside any guard and BuildNodeIndex runs at STARTUP, so one such node in a fetched package
// source aborted the process before the GUI existed, on every launch, with no way to remove the source from
// inside the app. A remote, persistent denial of service.
//
// Two properties keep it dead, and both are asserted here: Lower NEVER throws, and what it emits is WELL-TYPED.
TEST(lower_refuses_type_confused_payloads_instead_of_throwing)
{
    const ordered_json Bad[] = {
        {{"LABEL","a"}, {"TYPE", 5}},                                             // the TYPE itself
        {{"LABEL","b"}, {"TYPE","DeclareExec"}, {"HOST","w"}, {"PATH","g"}, {"RECOMMENDED","yes"}},
        {{"TYPE","DeclareExec"}, {"HOST","w"}, {"PATH","g"}, {"LABEL", 5}},   // non-string LABEL → refused
        {{"LABEL","d"}, {"TYPE","DeclareExec"}, {"HOST","w"}, {"PATH","g"}, {"RUNNER", 5}},
        {{"LABEL","e"}, {"TYPE","DeclareLibraryItem"}, {"UID", 749}},
        {{"LABEL","f"}, {"TYPE","CustomVar"}, {"KEY", 5}},
        {{"LABEL","g"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH", 5}},
        {{"LABEL","h"}, {"TYPE","RegEdit"}, {"EDITS", ordered_json::array({
            ordered_json{{"OVERRIDE","true"}, {"HKCU", {{"S", {{"v","1"}}}}}}})}},
        {{"LABEL","i"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"}, {"SUBMOUNTS","a:b"}},
    };
    for (const ordered_json &N : Bad)
        CHECK(Refused(N));      // a named refusal, NOT an exception and NOT a silent empty result

    // The AUTHOR-KEYED BAGS are checked by VALUE SHAPE, not by key: their keys are arbitrary (a registry
    // value may legitimately be named "PATH"), but every reader takes their values as strings. A tile's META
    // is hoisted FLAT onto the layer, so a key whitelist could not see it at all — `"UMUID": 12345` sailed
    // through and aborted the launch.
    CHECK(Refused(ordered_json{{"LABEL","t"}, {"TYPE","DeclareLibraryItem"}, {"UID","1"},
                               {"META", {{"UMUID", 12345}}}}));
    CHECK(Refused(ordered_json{{"LABEL","e"}, {"TYPE","DeclareExec"}, {"HOST","l"},
                               {"GUEST", ordered_json::array({"w"})}, {"PATH","p"}, {"ENV", {{"K", 5}}}}));
    CHECK(Refused(ordered_json{{"LABEL","r"}, {"TYPE","RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"HKCU", {{"S", {{"v", 5}}}}}}})}}));
    // ...but a NUMBER where the schema says number is fine, and an unlisted key inside a schema sub-object is
    // simply one the table has not learned — constraining it would be the COVER mistake again.
    CHECK(!Refused(ordered_json{{"LABEL","v"}, {"TYPE","CustomVar"}, {"KEY","K"}, {"DEFAULT","1"},
                                {"UI", {{"MIN", 0}, {"MAX", 100}, {"POOL", ordered_json::array({1,2})}}}}));
    // A CHOICES entry may be the bare-string shorthand OR an object.
    CHECK(!Refused(ordered_json{{"LABEL","v"}, {"TYPE","CustomVar"}, {"KEY","K"}, {"DEFAULT","1"},
                                {"UI", {{"CHOICES", ordered_json::array({"60", "120"})}}}}));

    // COVER is deliberately unconstrained: it is dual-form (a bare filename OR a {PATH,SOURCE} object) and
    // every consumer branches on which. Constraining it rejected a real shipping node.
    CHECK(!Refused(ordered_json{{"LABEL","t"}, {"TYPE","DeclareLibraryItem"}, {"UID","1"},
                                {"COVER","cover.jpg"}}));
    CHECK(!Refused(ordered_json{{"LABEL","t"}, {"TYPE","DeclareLibraryItem"}, {"UID","1"},
                                {"COVER", {{"PATH","c.jpg"}}}}));
}

// A non-string WHEN was SILENTLY IGNORED — the layer applied unconditionally, which is the precise failure
// ("inert-looking in the file, live at launch") the whole WHEN mechanism exists to prevent. `"WHEN": true`
// is a very plausible slip, since the field reads like a boolean.
TEST(lower_refuses_a_non_string_when)
{
    CHECK(Refused(ordered_json{{"LABEL","a"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"},
                               {"WHEN", 7}}));
    CHECK(Refused(ordered_json{{"LABEL","b"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"},
                               {"WHEN", true}}));
    CHECK(!Refused(ordered_json{{"LABEL","c"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"},
                                {"WHEN", "%A% == 1"}}));
}

// ...and the same at the OTHER boundary: ParseNode reads a few fields off the RAW node (TOGGLE, PARENTS),
// outside the lowering's guarantee. `{"TOGGLE": true}` crashed the indexer the same way.
TEST(parse_node_refuses_a_type_confused_raw_field_instead_of_throwing)
{
    Node N;
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL","e"}, {"TYPE","Group"}, {"TOGGLE", true}},
                                   "f.json", "/b", N));
    CHECK(!N.LowerError.empty());          // indexed and named, not dropped and not thrown
    CHECK_EQ((int)N.Layers.size(), 0);
    CHECK_EQ(N.NodeId, std::string("e"));
}

// ---- Content -------------------------------------------------------------------------------------

TEST(lower_content_maps_form_to_layer_type_and_carries_placement)
{
    const std::pair<const char *, const char *> Forms[] = {
        {"zip", "VFSZipLayer"}, {"dir", "VFSDirLayer"}, {"file", "VFSFileLayer"}, {"delta", "VFSDeltaLayer"}};
    for (const auto &[Form, Type] : Forms)
    {
        ordered_json N{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", Form}, {"PATH", "a.zip"},
                       {"TARGET", "%PrefixRoot%/drive_c/1"}, {"SUBMOUNTS", ordered_json::array({"a:b"})},
                       {"SOURCE", ordered_json{{"TYPE", "ipfs"}, {"CID", "Qm1"}}}, {"COMMENT", "hi"}};
        const ordered_json L = Lower(N);
        CHECK_EQ((int)L.size(), 1);
        CHECK_EQ(L[0].value("TYPE", std::string()), std::string(Type));
        CHECK_EQ(L[0].value("PATH", std::string()), std::string("a.zip"));
        CHECK_EQ(L[0].value("TARGET", std::string()), std::string("%PrefixRoot%/drive_c/1"));
        CHECK_EQ(L[0].value("COMMENT", std::string()), std::string("hi"));
        CHECK(L[0].contains("SUBMOUNTS") && L[0].contains("SOURCE"));
    }
    CHECK(Refused(ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", "tarball"}}));
}

// A delta's byte-base(s) are BASE_TARGETS: ONE key, ALWAYS a list, node and layer alike. One entry is the
// ordinary cross-target delta; several are a CONCATENATION. There is no singular spelling — two keys for one
// idea is exactly how the plural came to be emitted, documented and never read by the mounter — and "" is a
// real target (the mount root) that a lone string could not tell apart from "no base declared".
TEST(lower_content_base_targets_is_one_key_always_a_list)
{
    auto Mk = [](ordered_json B) {
        return ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", "delta"}, {"PATH", "d.vgdelta"},
                            {"BASE_TARGETS", std::move(B)}};
    };
    auto Bases = [](const ordered_json &Lowered) {
        std::vector<std::string> B;
        for (const auto &E : Lowered[0].value("BASE_TARGETS", ordered_json::array())) B.push_back(E.get<std::string>());
        return B;
    };
    CHECK_EQ(Bases(Lower(Mk(ordered_json::array({"one"})))), (std::vector<std::string>{"one"}));
    CHECK_EQ(Bases(Lower(Mk(ordered_json::array({"a", "b"})))), (std::vector<std::string>{"a", "b"}));
    //A base at the ROOT is a real declaration and must survive as one.
    CHECK_EQ(Bases(Lower(Mk(ordered_json::array({""})))), (std::vector<std::string>{""}));
    //An EMPTY array used to fall through every branch and silently drop the base — a delta with nothing to
    //reconstruct against, reported by nobody.
    CHECK(Refused(Mk(ordered_json::array())));
    CHECK(Refused(Mk(7)));
    CHECK(Refused(Mk(ordered_json::array({"a", 7}))));            // a non-string entry shortens the concatenation
    //A bare string is refused rather than wrapped: the key is a LIST, said once, so there is no second shape
    //for a reader to forget. (It is the plausible slip, so the refusal names the fix.)
    CHECK(Refused(Mk("one")));
    //...and so is the singular key, rather than being ignored: BASE_TARGET reads as obviously right, and
    //dropping it silently gives a delta with no base that validates clean and audits clean.
    std::string E;
    NodeLower::Lower(ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", "delta"},
                                  {"PATH", "d.vgdelta"}, {"BASE_TARGET", "a"}}, "c", E);
    CHECK(E.find("BASE_TARGETS") != std::string::npos);           // and the message names the key to use
}

// BASE_TARGETS is a DELTA's byte-base. On any other FORM it used to be accepted and then silently discarded
// by the mounter — and the editor offers the field on every Content node, so it is a two-click mistake.
TEST(lower_content_base_targets_only_means_something_on_a_delta)
{
    auto Mk = [](const char *Form) {
        return ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", Form}, {"PATH", "a.zip"},
                            {"BASE_TARGETS", ordered_json::array({"wine"})}};
    };
    for (const char *F : {"zip", "dir", "file"})
    {
        std::string E;
        NodeLower::Lower(Mk(F), "c", E);
        CHECK(!E.empty());                                     // refused, not quietly dropped
        CHECK(E.find("BASE_TARGETS") != std::string::npos);    // and the message names the key
    }
    CHECK(!Refused(ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", "delta"},
                                {"PATH", "d.vgdelta"}, {"BASE_TARGETS", ordered_json::array({"wine"})}}));
}

// ---- RegEdit -------------------------------------------------------------------------------------

// The registry is a TREE on disk and flat REGPATH/KEYVALUES to the executor. A key holding values AND subkeys
// emits one layer per level, and an EMPTY object is "create this key, no values".
TEST(lower_regedit_flattens_the_hive_tree)
{
    ordered_json N{{"LABEL", "r"}, {"TYPE", "RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"ARCHITECTURE", ordered_json::array({"32"})},
                     {"HKLM", {{"Software", {{"App", {{"v", "1"}, {"Sub", {{"w", "2"}}}}}}}}}}})}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 2);
    CHECK_EQ(L[0].value("REGPATH", std::string()), std::string("HKLM\\Software\\App"));
    CHECK_EQ(L[0].value("KEYVALUES", ordered_json::object()).value("v", std::string()), std::string("1"));
    CHECK_EQ(L[1].value("REGPATH", std::string()), std::string("HKLM\\Software\\App\\Sub"));
    CHECK_EQ(L[0].value("ARCHITECTURE", std::string()), std::string("32"));
    //OVERRIDE must be ABSENT unless asked for: defaulting it true turns every RegEdit into a destructive
    //whole-key replace against the live prefix.
    for (const auto &E : L) CHECK(!E.contains("OVERRIDE"));
    //...and no ARCHITECTURE key at all when none was declared — a literal null throws in the executor.
    ordered_json NoArch{{"LABEL","r"}, {"TYPE","RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"HKCU", {{"S", {{"v","1"}}}}}}})}};
    CHECK(!Lower(NoArch)[0].contains("ARCHITECTURE"));

    // create-key-only
    ordered_json E{{"LABEL", "r"}, {"TYPE", "RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"HKCU", {{"Software", {{"Empty", ordered_json::object()}}}}}}})}};
    const ordered_json K = Lower(E);
    CHECK_EQ((int)K.size(), 1);
    CHECK_EQ(K[0].value("REGPATH", std::string()), std::string("HKCU\\Software\\Empty"));
    CHECK(K[0].value("KEYVALUES", ordered_json::object()).empty());
}

// ARCHITECTURE is arrayable: the SAME tree written into several views, one layer each (the executor's RegEdit
// applies to exactly one).
TEST(lower_regedit_arrayable_architecture_emits_one_layer_per_view)
{
    ordered_json N{{"LABEL", "r"}, {"TYPE", "RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"ARCHITECTURE", ordered_json::array({"32", "64"})}, {"OVERRIDE", true},
                     {"HKLM", {{"S", {{"v", "1"}}}}}}})}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 2);
    CHECK_EQ(L[0].value("ARCHITECTURE", std::string()), std::string("32"));
    CHECK_EQ(L[1].value("ARCHITECTURE", std::string()), std::string("64"));
    for (const auto &E : L) CHECK(E.value("OVERRIDE", false));
}

// THE regression. An entry's own WHEN gates THAT entry. It was skipped as a non-hive key and then never emitted
// anywhere, so a conditional registry write applied unconditionally — and a host/join pair written this way
// both fired every launch, the second overwriting the first. Inert-looking in the file, live at launch.
TEST(lower_regedit_entry_when_reaches_every_layer_it_produced)
{
    ordered_json N{{"LABEL", "r"}, {"TYPE", "RegEdit"}, {"EDITS", ordered_json::array({
        ordered_json{{"ARCHITECTURE", ordered_json::array({"32", "64"})},
                     {"WHEN", "%NETMODE% == host"},
                     {"HKCU", {{"S", {{"Hosting", "1"}, {"Deep", {{"x", "2"}}}}}}}},
        ordered_json{{"HKCU", {{"S", {{"Always", "1"}}}}}}})}};       // a second entry with NO condition
    const ordered_json L = Lower(N);
    int Gated = 0, Ungated = 0;
    for (const auto &E : L)
    {
        if (E.value("WHEN", std::string()) == "%NETMODE% == host") ++Gated;
        else if (!E.contains("WHEN")) ++Ungated;
    }
    CHECK_EQ(Gated, 4);       // 2 key paths x 2 architectures, every one carrying the condition
    CHECK_EQ(Ungated, 1);     // ...and the unconditional entry stays unconditional
}

// A node-level WHEN gates everything the node produced. Where a layer ALREADY has its own condition, BOTH must
// hold — taking either alone silently widens or narrows what the author wrote.
TEST(lower_node_when_and_entry_when_are_combined_not_replaced)
{
    ordered_json N{{"LABEL", "r"}, {"TYPE", "RegEdit"}, {"WHEN", "%A% == 1"},
                   {"EDITS", ordered_json::array({
                       ordered_json{{"WHEN", "%B% == 2"}, {"HKCU", {{"S", {{"v", "1"}}}}}},
                       ordered_json{{"HKCU", {{"T", {{"v", "1"}}}}}}})}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 2);
    const std::string Both = L[0].value("WHEN", std::string());
    CHECK(Both.find("%A% == 1") != std::string::npos);      // the node's
    CHECK(Both.find("%B% == 2") != std::string::npos);      // AND the entry's
    CHECK(Both.find("&&") != std::string::npos);
    CHECK_EQ(L[1].value("WHEN", std::string()), std::string("%A% == 1"));   // no own condition -> the node's

    //A whitespace-only own-condition composes to "(A) && (  )", which the parser REJECTS — and an unparseable
    //WHEN fails OPEN, so the node's real condition would silently evaporate. Trimmed to empty instead.
    ordered_json Blank{{"LABEL","r"}, {"TYPE","RegEdit"}, {"WHEN","%A% == 1"},
                       {"EDITS", ordered_json::array({
                           ordered_json{{"WHEN","   "}, {"HKCU", {{"S", {{"v","1"}}}}}}})}};
    CHECK_EQ(Lower(Blank)[0].value("WHEN", std::string()), std::string("%A% == 1"));
}

// Every TYPE whose layers are GATED on WHEN must propagate the node's WHEN — not just the three that happened
// to be wired first: a node that looks inert in the file and applies at launch is the worst failure this
// front-end can have.
//
// The list is the types with a real consumer for it: the VFS/edit layers (BuildSubComponentsArray's gate),
// CustomVar (its own resolver) and Persist (DerivePersistence). DeclareExec/DeclareLibraryItem/DeclareRunner
// are deliberately ABSENT — their payloads become the node's identity at index time and are never
// re-evaluated, so a WHEN there is a declaration nothing honours; ValidateNodeGraph rejects it instead.
TEST(lower_node_when_reaches_every_gated_type)
{
    const ordered_json Nodes[] = {
        {{"LABEL", "a"}, {"TYPE", "Content"}, {"FORM", "zip"}, {"PATH", "a.zip"}},
        {{"LABEL", "b"}, {"TYPE", "RegEdit"}, {"EDITS", ordered_json::array({
            ordered_json{{"HKCU", {{"S", {{"v", "1"}}}}}}})}},
        {{"LABEL", "c"}, {"TYPE", "FileEdit"}, {"FILE", "f.ini"}, {"EDITS", ordered_json::array({
            ordered_json{{"MODE", "AppendLine"}, {"VALUE", "x"}}})}},
        {{"LABEL", "d"}, {"TYPE", "BinaryPatch"}, {"FILE", "g.exe"}, {"EDITS", ordered_json::array({
            ordered_json{{"MODE", "Replace"}, {"OFFSET", "0x1"}, {"EXPECT", "01"}, {"REPLACE", "00"}}})}},
        {{"LABEL", "e"}, {"TYPE", "DllOverride"}, {"OVERRIDES", {{"d3d8", "n,b"}}}},
        {{"LABEL", "f"}, {"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", "a"}, {"TARGET", "a"}},
        {{"LABEL", "g"}, {"TYPE", "CustomVar"}, {"KEY", "K"}, {"DEFAULT", "1"}},
    };
    for (ordered_json N : Nodes)
    {
        N["WHEN"] = "%A% == 1";
        const ordered_json L = Lower(N);
        CHECK(!L.empty());
        for (const auto &E : L)
            CHECK_EQ(E.value("WHEN", std::string()), std::string("%A% == 1"));
    }
}

// The other half of the same contract: a WHEN whose consumer does not exist must be REFUSED, not accepted and
// ignored. Enforced by ValidateNodeGraph (the lowering still carries it, so the error can name it).
TEST(lower_when_on_an_identity_type_is_rejected_by_validation)
{
    for (const char *T : {"DeclareExec", "DeclareLibraryItem"})
    {
        ordered_json N{{"LABEL", "n"}, {"TYPE", T}, {"WHEN", "%A% == 1"},
                       {"HOST", "win32"}, {"PATH", "g.exe"}, {"UID", "1"}};
        NodeIndex Idx;
        Node Parsed;
        CHECK(ManifestModel::ParseNode(N, "f.json", "/b", Parsed));
        Idx.Nodes["n"] = Parsed;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        bool Found = false;
        for (const auto &E : Errors) if (E.find("never evaluated") != std::string::npos) Found = true;
        CHECK(Found);
    }
    // ...and a gated type with the same WHEN is accepted.
    NodeIndex Ok;
    Node P;
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL", "c"}, {"TYPE", "Content"}, {"FORM", "zip"},
                                                {"PATH", "a.zip"}, {"WHEN", "%A% == 1"}}, "f.json", "/b", P));
    Ok.Nodes["c"] = P;
    std::vector<std::string> E2, W2;
    ManifestModel::ValidateNodeGraph(Ok, E2, W2);
    for (const auto &E : E2) CHECK(E.find("never evaluated") == std::string::npos);
}

// ---- FileEdit / BinaryPatch / DllOverride / Persist -----------------------------------------------

// The node owns FILE and the pass; EDITS holds the per-edit fields, verbatim — a whitelist here would drop
// whatever the engine learns to read next.
TEST(lower_fileedit_and_binarypatch_batch_onto_the_node_file)
{
    ordered_json F{{"LABEL", "f"}, {"TYPE", "FileEdit"}, {"FILE", "cfg.ini"}, {"OVERRIDE", true},
                   {"EDITS", ordered_json::array({
                       ordered_json{{"MODE", "ConfigWrite"}, {"KEY", "R="}, {"VALUE", "1"}},
                       ordered_json{{"MODE", "AppendLine"}, {"VALUE", "x"}}})}};
    const ordered_json LF = Lower(F);
    CHECK_EQ((int)LF.size(), 2);
    for (const auto &L : LF)
    {
        CHECK_EQ(L.value("TYPE", std::string()), std::string("FileEdit"));
        CHECK_EQ(L.value("FILE", std::string()), std::string("cfg.ini"));
        CHECK(L.value("OVERRIDE", false));
    }
    CHECK_EQ(LF[0].value("KEY", std::string()), std::string("R="));
    //OVERRIDE is a FileEdit pass selector; it must not leak onto BinaryPatch layers, where it means nothing.
    ordered_json BOv{{"LABEL","b"}, {"TYPE","BinaryPatch"}, {"FILE","g.exe"}, {"OVERRIDE", true},
                     {"EDITS", ordered_json::array({ordered_json{{"MODE","Replace"}, {"OFFSET","0x1"},
                                                                 {"EXPECT","aa"}, {"REPLACE","bb"}}})}};
    CHECK(!Lower(BOv)[0].contains("OVERRIDE"));

    // Every BinaryPatch mode's payload field survives — the editor offers Cave and Poke, so the lowering must
    // carry PAYLOAD/CAVE/VALUE/ANCHOR/APPLY or the patch is a no-op with no diagnostic.
    ordered_json B{{"LABEL", "b"}, {"TYPE", "BinaryPatch"}, {"FILE", "g.exe"},
                   {"EDITS", ordered_json::array({
                       ordered_json{{"MODE", "Cave"}, {"OFFSET", "0x1"}, {"EXPECT", "aa"},
                                    {"PAYLOAD", "90"}, {"CAVE", "auto"}, {"COMMENT", "c"}},
                       ordered_json{{"MODE", "Poke"}, {"ANCHOR", "8b ?? 24"}, {"VALUE", "%W:u16le%"},
                                    {"APPLY", "memory"}}})}};
    const ordered_json LB = Lower(B);
    CHECK_EQ((int)LB.size(), 2);
    CHECK_EQ(LB[0].value("PAYLOAD", std::string()), std::string("90"));
    CHECK_EQ(LB[0].value("CAVE", std::string()), std::string("auto"));
    CHECK_EQ(LB[1].value("ANCHOR", std::string()), std::string("8b ?? 24"));
    CHECK_EQ(LB[1].value("VALUE", std::string()), std::string("%W:u16le%"));
    CHECK_EQ(LB[1].value("APPLY", std::string()), std::string("memory"));
    for (const auto &L : LB) CHECK_EQ(L.value("FILE", std::string()), std::string("g.exe"));
}

// An EMPTY override order means DISABLED in wine. Defaulting it to "n,b" inverts what the package asked for —
// which is how a media stack's deliberate "winegstreamer=" once became "winegstreamer=n,b".
TEST(lower_dlloverride_preserves_an_empty_order)
{
    ordered_json N{{"LABEL", "d"}, {"TYPE", "DllOverride"},
                   {"OVERRIDES", {{"d3d8", "n,b"}, {"winegstreamer", ""}}}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 2);
    bool SawEmpty = false, SawNb = false;
    for (const auto &E : L)
    {
        const std::string S = E.value("DLLOVERRIDE", std::string());
        if (S == "winegstreamer=") SawEmpty = true;
        if (S == "d3d8=n,b")       SawNb = true;
    }
    CHECK(SawEmpty);
    CHECK(SawNb);

    // package JSON arrives from peers: refuse, don't throw
    CHECK(Refused(ordered_json{{"LABEL", "d"}, {"TYPE", "DllOverride"}, {"OVERRIDES", {{"d3d8", 7}}}}));
}

// DeclarePersist is one-node = one-persist (no arrays to expand): every field is type-checked and a malformed one
// is REFUSED like every other branch, rather than lowering "successfully" to a garbage layer that silently loses
// saves at capture time.
TEST(lower_declarepersist_refuses_a_malformed_field)
{
    CHECK(Refused(ordered_json{{"LABEL","p"}, {"TYPE","DeclarePersist"}, {"SCOPE", 5}}));
    CHECK(Refused(ordered_json{{"LABEL","p"}, {"TYPE","DeclarePersist"}, {"PATH", ordered_json::array()}}));
    CHECK(Refused(ordered_json{{"LABEL","p"}, {"TYPE","DeclarePersist"}, {"TARGET", 7}}));
    CHECK(Refused(ordered_json{{"LABEL","p"}, {"TYPE","DeclarePersist"}, {"CLOUD", "yes"}}));
    // ...and a well-formed persist lowers without complaint.
    CHECK(!Refused(ordered_json{{"LABEL","p"}, {"TYPE","DeclarePersist"},
                                {"SCOPE","file"}, {"PATH","drive_c/Saves"}, {"TARGET","Saves"}, {"CLOUD",true}}));
}

TEST(lower_declarepersist_emits_one_layer_passing_the_fields_through)
{
    ordered_json N{{"LABEL", "p"}, {"TYPE", "DeclarePersist"},
                   {"SCOPE", "registry"}, {"PATH", "HKCU\\Software\\Game"}, {"TARGET", "Game"}, {"CLOUD", false}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 1);
    CHECK_EQ(L[0].value("TYPE", std::string()), std::string("DeclarePersist"));
    CHECK_EQ(L[0].value("SCOPE", std::string()), std::string("registry"));
    CHECK_EQ(L[0].value("PATH", std::string()), std::string("HKCU\\Software\\Game"));
    CHECK_EQ(L[0].value("TARGET", std::string()), std::string("Game"));
    CHECK_EQ(L[0].value("CLOUD", true), false);
}

// CustomVar's UI facet IS the pre-launch control — the label, the kind, the enum choices. Dropping it leaves a
// declared variable with no way for the player to set it, which looks like the knob simply not existing.
TEST(lower_customvar_carries_the_ui_facet_and_comment)
{
    ordered_json N{{"LABEL", "v"}, {"TYPE", "CustomVar"}, {"KEY", "RES"}, {"DEFAULT", "1920"},
                   {"COMMENT", "why this exists"},
                   {"UI", {{"LABEL", "Resolution"}, {"CONTROL", "enum"},
                           {"CHOICES", ordered_json::array({ordered_json{{"LABEL", "HD"}, {"VALUE", "1920"}}})}}}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 1);
    CHECK_EQ(L[0].value("KEY", std::string()), std::string("RES"));
    CHECK_EQ(L[0].value("DEFAULT", std::string()), std::string("1920"));
    CHECK_EQ(L[0].value("COMMENT", std::string()), std::string("why this exists"));
    CHECK(L[0].contains("UI"));
    if (L[0].contains("UI"))                       // a missing key would THROW on the next read, aborting the
    {                                              // whole binary before the summary — report, don't abort
        CHECK_EQ(L[0]["UI"].value("LABEL", std::string()), std::string("Resolution"));
        CHECK_EQ((int)L[0]["UI"].value("CHOICES", ordered_json::array()).size(), 1);
    }
}

// ---- DeclareExec / DeclareLibraryItem / Group ----------------------------------------------------

// ONE type, two readings. No GUEST => a launchable (DeclareExec, PLATFORM/CONTENTPATH/EXEARGS); GUEST =>
// a runner (DeclareRunner, HOST/GUEST/EXECUTABLE/ARGS). Getting this backwards makes a game unlaunchable or a
// runner invisible, so it is pinned in both directions.
TEST(lower_declareexec_splits_launchable_from_runner_on_guest)
{
    ordered_json Game{{"TYPE", "DeclareExec"}, {"HOST", "win32"}, {"PATH", "g.exe"},
                      {"ARGS", ordered_json::array({"xres=1920", "a b"})}, {"LABEL", "GOTY"},
                      {"RECOMMENDED", true}, {"WORKDIR", "w"}, {"RUNNER", "pinned"}};
    const ordered_json LG = Lower(Game);
    CHECK_EQ((int)LG.size(), 1);
    CHECK_EQ(LG[0].value("TYPE", std::string()), std::string("DeclareExec"));
    CHECK_EQ(LG[0].value("PLATFORM", std::string()), std::string("win32"));
    CHECK_EQ(LG[0].value("CONTENTPATH", std::string()), std::string("g.exe"));
    CHECK_EQ(LG[0].value("LABEL", std::string()), std::string("GOTY"));
    CHECK_EQ(LG[0].value("RUNNER", std::string()), std::string("pinned"));
    // ARGS stay an ARRAY of argv entries: an argument containing a space must not be re-split downstream.
    CHECK_EQ(LG[0].value("EXEARGS", ordered_json()).dump(),
             ordered_json::array({"xres=1920", "a b"}).dump());

    // The launchable's remaining fields, each a silent failure if dropped: a wrong cwd, or the wrong variant
    // pre-selected in the picker.
    CHECK_EQ(LG[0].value("WORKDIR", std::string()), std::string("w"));
    CHECK(LG[0].value("RECOMMENDED", false));

    ordered_json Runner{{"LABEL", "r"}, {"TYPE", "DeclareExec"}, {"HOST", "linux64"},
                        {"GUEST", ordered_json::array({"win32", "win64"})},
                        {"PATH", "%RunnerMount%/proton"}, {"ARGS", ordered_json::array({"waitforexitandrun"})},
                        {"ENV", {{"K", "V"}}}, {"ENV_REMOVE", ordered_json::array({"LD_LIBRARY_PATH"})},
                        {"CONTENT_ROOT", "pfx/drive_c/1"}, {"PREFIX_GENERATE", true}};
    const ordered_json LR = Lower(Runner);
    CHECK_EQ((int)LR.size(), 1);
    CHECK_EQ(LR[0].value("TYPE", std::string()), std::string("DeclareRunner"));
    CHECK_EQ(LR[0].value("EXECUTABLE", std::string()), std::string("%RunnerMount%/proton"));
    CHECK_EQ((int)LR[0].value("GUEST", ordered_json::array()).size(), 2);
    //The runner's own ARGS are the launcher verb (proton's "waitforexitandrun"). Dropping them silently runs
    //the game with no verb at all.
    CHECK_EQ(LR[0].value("ARGS", ordered_json()).dump(), ordered_json::array({"waitforexitandrun"}).dump());
    CHECK_EQ(LR[0].value("REMOVE_ENV", ordered_json::array()).dump(), ordered_json::array({"LD_LIBRARY_PATH"}).dump());
    CHECK(LR[0].value("PREFIX_GENERATE", false));
    // CONTENT_ROOT decides where the game's content lands inside the prefix. Dropping it mounts content off
    // drive_c\<UID> and the Windows program cannot find itself — the "create process: 2" instant crash.
    CHECK_EQ(LR[0].value("CONTENT_ROOT", std::string()), std::string("pfx/drive_c/1"));
    CHECK_EQ(LR[0].value("ENV", ordered_json::object()).value("K", std::string()), std::string("V"));
    ordered_json Unified = Runner; Unified["UNIFIED_RUNTIME"] = true;
    CHECK(Lower(Unified)[0].value("UNIFIED_RUNTIME", false));

    // An EMPTY guest list is a launchable, not a runner with no guests.
    ordered_json Empty = Game; Empty["GUEST"] = ordered_json::array();
    CHECK_EQ(Lower(Empty)[0].value("TYPE", std::string()), std::string("DeclareExec"));
}

// The tile's core fields stay named; everything else rides in an opaque META bag on disk and is flattened back
// out here, because the catalog reads those fields flat.
TEST(lower_librarayitem_flattens_the_meta_bag)
{
    ordered_json N{{"LABEL", "t"}, {"TYPE", "DeclareLibraryItem"}, {"UID", "749"}, {"TITLE", "AoE2"},
                   {"COVER", {{"PATH", "c.jpg"}, {"SOURCE", {{"CID", "Qm1"}}}}},
                   {"META", {{"DEVELOPER", "Ensemble"}, {"TGDBID", "12"}}}};
    const ordered_json L = Lower(N);
    CHECK_EQ((int)L.size(), 1);
    CHECK_EQ(L[0].value("UID", std::string()), std::string("749"));
    CHECK_EQ(L[0].value("TITLE", std::string()), std::string("AoE2"));
    // COVER travels as a content CID and is what every tile in the library renders.
    CHECK(L[0].contains("COVER"));
    if (L[0].contains("COVER"))
    {
        CHECK_EQ(L[0]["COVER"].value("PATH", std::string()), std::string("c.jpg"));
        CHECK_EQ(L[0]["COVER"].value("SOURCE", ordered_json::object()).value("CID", std::string()),
                 std::string("Qm1"));
    }
    CHECK_EQ(L[0].value("DEVELOPER", std::string()), std::string("Ensemble"));   // flattened, not nested
    CHECK_EQ(L[0].value("TGDBID", std::string()), std::string("12"));
    CHECK(!L[0].contains("META"));
}

// Group contributes NOTHING but must still lower cleanly — it exists so a payload-less composition node is a
// real node. Dropping it made every referrer silently lose an edge rather than dangle.
TEST(lower_group_is_empty_but_not_an_error)
{
    std::string Err;
    const ordered_json L = Lower(ordered_json{{"LABEL", "g"}, {"TYPE", "Group"},
                                              {"PARENTS", ordered_json::array({"a"})}}, &Err);
    CHECK(L.is_array());
    CHECK(L.empty());
    CHECK(Err.empty());

    // A Group's empty result and a REFUSAL are both empty arrays — only Err separates them.
    //A REFUSAL must SET Err. Returning an empty array with an empty Err is indistinguishable from a valid
    //Group — the node would be indexed with no LowerError, contribute nothing, and be reported by nobody.
    {
        std::string E;
        NodeLower::Lower(ordered_json{{"LABEL", "x"}, {"TYPE", 5}}, "x", E);
        CHECK(!E.empty());
    }
    CHECK(Refused(ordered_json{{"LABEL", "x"}}));                        // no TYPE at all is an ERROR
    CHECK(Refused(ordered_json{{"LABEL", "x"}, {"TYPE", "Nonsense"}}));
}

//ENV on a LAUNCHABLE was copied for runners only, so a game's own environment was lowered away in silence —
//the field survived every save and never reached the process. Tonic Trouble paid for this with a binary patch.
TEST(declareexec_launchable_keeps_ENV_and_ENV_REMOVE)
{
    ordered_json N = {{"LABEL","g"},{"TYPE","DeclareExec"},{"HOST","win32"},{"PATH","G.exe"},
                      {"ENV", {{"SDL_JOYSTICK_WGI","0"}}},
                      {"ENV_REMOVE", ordered_json::array({"LD_PRELOAD"})}};
    const ordered_json L = Lower(N);
    CHECK_EQ(L.size(), (size_t)1);
    CHECK_EQ(L[0].value("TYPE", std::string()), std::string("DeclareExec"));   // launchable, not runner
    //Guarded: CHECK does not abort, so dereferencing a missing key on the next line would throw and take the
    //WHOLE suite down — a regression that hides every other result instead of naming itself.
    CHECK(L[0].contains("ENV"));
    if (L[0].contains("ENV"))
        CHECK_EQ(L[0]["ENV"].value("SDL_JOYSTICK_WGI", std::string()), std::string("0"));
    CHECK(L[0].contains("REMOVE_ENV"));                                        // same spelling as the runner layer
    if (L[0].contains("REMOVE_ENV") && L[0]["REMOVE_ENV"].is_array() && L[0]["REMOVE_ENV"].size() == 1)
        CHECK_EQ(L[0]["REMOVE_ENV"][0].get<std::string>(), std::string("LD_PRELOAD"));
    else
        CHECK(false);
}

//...and a launchable that declares none must not sprout empty ones: an absent ENV is absent, not {}.
TEST(declareexec_launchable_without_ENV_emits_none)
{
    ordered_json N = {{"LABEL","g"},{"TYPE","DeclareExec"},{"HOST","win32"},{"PATH","G.exe"}};
    const ordered_json L = Lower(N);
    CHECK(!L[0].contains("ENV"));
    CHECK(!L[0].contains("REMOVE_ENV"));
}
