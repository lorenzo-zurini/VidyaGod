#include "vgtest.h"
#include "fileedits.h"
#include "launchparams.h"   // ContainerParams — ProcessFileEdits takes one

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
std::filesystem::path TmpDir(const char * tag)
{
    auto d = std::filesystem::temp_directory_path() / ("vgfe_" + std::string(tag) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
std::string ReadAll(const std::filesystem::path & p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void Write(const std::filesystem::path & p, const std::string & s) { std::ofstream(p, std::ios::binary) << s; }
}

// AppendLine creates the file and appends each line in call order — the load-order primitive (openmw.cfg content=).
TEST(appendline_creates_and_appends_in_order)
{
    auto dir = TmpDir("order"); auto cfg = dir / "openmw.cfg";
    CHECK(FileEdits::AppendLine("content=Morrowind.esm", cfg));
    CHECK(FileEdits::AppendLine("content=Tribunal.esm", cfg));
    CHECK(FileEdits::AppendLine("content=BetterClothes.esp", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("content=Morrowind.esm\ncontent=Tribunal.esm\ncontent=BetterClothes.esp\n"));
    std::filesystem::remove_all(dir);
}

// Idempotent: re-appending an identical line is a no-op (so re-launching doesn't duplicate the load order).
TEST(appendline_is_idempotent)
{
    auto dir = TmpDir("idem"); auto cfg = dir / "f.cfg";
    CHECK(FileEdits::AppendLine("content=A", cfg));
    CHECK(FileEdits::AppendLine("content=B", cfg));
    CHECK(FileEdits::AppendLine("content=A", cfg));  // dup of line 1 → skipped
    CHECK_EQ(ReadAll(cfg), std::string("content=A\ncontent=B\n"));
    std::filesystem::remove_all(dir);
}

// Appends cleanly onto a pre-shipped config that doesn't end in a newline (no fused line).
TEST(appendline_separates_unterminated_last_line)
{
    auto dir = TmpDir("nl"); auto cfg = dir / "f.cfg";
    Write(cfg, "data=\"/data/vanilla\"");          // no trailing newline
    CHECK(FileEdits::AppendLine("content=Morrowind.esm", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("data=\"/data/vanilla\"\ncontent=Morrowind.esm\n"));
    std::filesystem::remove_all(dir);
}

// ---- ConfigWrite (P7 fill-in: only AppendLine was covered) ----

// Prefix-matched line replacement: any line starting with Key becomes Key+Value; others untouched.
TEST(configwrite_replaces_prefixed_line)
{
    auto dir = TmpDir("cw"); auto cfg = dir / "settings.ini";
    Write(cfg, "Resolution=640x480\nFullscreen=1\nVolume=8\n");
    CHECK(FileEdits::ConfigWrite("Resolution=", "1920x1080", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("Resolution=1920x1080\nFullscreen=1\nVolume=8\n"));
    std::filesystem::remove_all(dir);
}

// A missing file fails loudly (false) instead of silently creating garbage.
TEST(configwrite_missing_file_fails)
{
    auto dir = TmpDir("cwm");
    CHECK(!FileEdits::ConfigWrite("K=", "v", dir / "nope.ini"));
    std::filesystem::remove_all(dir);
}

// Multiple lines sharing the prefix ALL get replaced (documented prefix semantics).
TEST(configwrite_replaces_every_prefixed_line)
{
    auto dir = TmpDir("cwa"); auto cfg = dir / "multi.ini";
    Write(cfg, "content=A\ncontent=B\nother=x\n");
    CHECK(FileEdits::ConfigWrite("content=", "Z", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("content=Z\ncontent=Z\nother=x\n"));
    std::filesystem::remove_all(dir);
}

// ---- FileOverwrite ----

// Whole-content write, creating parents (the WC3 .w3k key-file path).
TEST(fileoverwrite_creates_parents_and_replaces)
{
    auto dir = TmpDir("fo"); auto f = dir / "sub" / "deep" / "roc.w3k";
    CHECK(FileEdits::FileOverwrite("ABCD-1234", f));
    CHECK_EQ(ReadAll(f), std::string("ABCD-1234"));
    CHECK(FileEdits::FileOverwrite("NEW", f));      // second write REPLACES (trunc), not appends
    CHECK_EQ(ReadAll(f), std::string("NEW"));
    std::filesystem::remove_all(dir);
}

//A package arrives from a peer by CID, so a FileEdit's FILE is UNTRUSTED INPUT. Neither stripping a leading
//separator nor is_absolute() stops a "../" climb, and following one is an arbitrary file write outside the
//runtime, at launch, from data someone else authored.
TEST(a_fileedit_path_that_climbs_out_of_its_base_is_refused)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_escape";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base);
    const std::filesystem::path Outside = Root / "outside.txt";

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","../outside.txt"},
                               {"MODE","Overwrite"},{"VALUE","escaped\n"}}});

    const bool Ok = FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base);
    CHECK(!Ok);                                             // reported as a FAILED edit, not quietly skipped
    CHECK(!std::filesystem::exists(Outside));               // and nothing was written outside the base
    std::filesystem::remove_all(Root);
}

//...while an ordinary nested path still works, so the guard is not simply refusing everything.
TEST(a_fileedit_path_inside_its_base_still_applies)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_ok";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base);

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","deep/nested/ok.txt"},
                               {"MODE","Overwrite"},{"VALUE","fine\n"}}});

    CHECK(FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base));
    CHECK(std::filesystem::exists(Base / "deep" / "nested" / "ok.txt"));
    std::filesystem::remove_all(Root);
}

//A first component that merely STARTS with ".." is not a climb. Comparing two characters instead of the whole
//component refused "..config/settings.ini", a legitimate (if unusual) file name.
TEST(a_fileedit_path_whose_first_component_starts_with_dotdot_is_allowed)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_dotdot";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base);

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","..config/settings.ini"},
                               {"MODE","Overwrite"},{"VALUE","kept\n"}}});

    CHECK(FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base));
    CHECK(std::filesystem::exists(Base / "..config" / "settings.ini"));
    std::filesystem::remove_all(Root);
}

//An ABSOLUTE path that already lands inside the base must be used AS IS. "%RuntimePath%/game/x.ini" is the
//shape: it substitutes to a real absolute path under the runtime, and stripping its leading separator turned
//it into "<base>/home/.../RUNTIME/game/x.ini" — a path that exists nowhere. Only the PrefixRoot-is-empty
//shape ("/drive_c/...") needs re-anchoring, and that one genuinely escapes.
TEST(an_absolute_fileedit_path_already_inside_the_base_is_used_as_is)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_absin";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base / "game");

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},
                               {"FILE", (Base / "game" / "x.ini").string()},   // absolute, inside the base
                               {"MODE","Overwrite"},{"VALUE","landed\n"}}});

    CHECK(FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base));
    CHECK(std::filesystem::exists(Base / "game" / "x.ini"));
    //...and NOT re-anchored under the base a second time.
    CHECK(!std::filesystem::exists(Base / Base.relative_path() / "game" / "x.ini"));
    std::filesystem::remove_all(Root);
}

// ---- JsonSet ----

// One value set by JSON pointer: the rest of the document and its key order stay; objects missing on the way are made.
TEST(jsonset_sets_a_value_by_pointer_and_keeps_the_rest)
{
    auto dir = TmpDir("js"); auto f = dir / "settings.json";
    Write(f, "{\"z\": 1, \"video\": {\"fullscreen\": false, \"keep\": \"me\"}, \"a\": [1, 2]}");
    CHECK(FileEdits::JsonSet("/video/fullscreen", "true", f));
    CHECK(FileEdits::JsonSet("/video/resolution/width", "1920", f));
    CHECK(FileEdits::JsonSet("/general/language", "english", f));        // not JSON ⇒ a string
    CHECK(FileEdits::JsonSet("/general/code", "\"1920\"", f));           // a JSON string stays a string
    CHECK(FileEdits::JsonSet("/a/1", "5", f));
    const auto J = nlohmann::ordered_json::parse(ReadAll(f));
    CHECK(J["video"]["fullscreen"] == true);
    CHECK(J["video"]["keep"] == "me");
    CHECK(J["video"]["resolution"]["width"] == 1920);
    CHECK(J["general"]["language"] == "english");
    CHECK(J["general"]["code"] == "1920");
    CHECK(J["a"] == nlohmann::ordered_json::array({1, 5}));
    CHECK_EQ(J.begin().key(), std::string("z"));                          // key order kept
    std::filesystem::remove_all(dir);
}

// The file is made when absent (a game that writes its settings only once something changes).
TEST(jsonset_creates_the_file)
{
    auto dir = TmpDir("jsc"); auto f = dir / "sub" / "settings.json";
    CHECK(FileEdits::JsonSet("/video/fullscreen", "false", f));
    CHECK(nlohmann::ordered_json::parse(ReadAll(f)) == nlohmann::ordered_json({{"video", {{"fullscreen", false}}}}));
    std::filesystem::remove_all(dir);
}

// A file that is not JSON, or a pointer through a plain value, fails and leaves the file exactly as it was.
TEST(jsonset_refuses_and_leaves_the_file)
{
    auto dir = TmpDir("jsr"); auto f = dir / "settings.json";
    Write(f, "[general]\nx=1\n");
    CHECK(!FileEdits::JsonSet("/x", "2", f));
    CHECK_EQ(ReadAll(f), std::string("[general]\nx=1\n"));
    Write(f, "{\"a\": 3}");
    CHECK(!FileEdits::JsonSet("/a/b", "2", f));
    CHECK_EQ(ReadAll(f), std::string("{\"a\": 3}"));
    CHECK(!FileEdits::JsonSet("no-slash", "2", f));
    CHECK_EQ(ReadAll(f), std::string("{\"a\": 3}"));
    std::filesystem::remove_all(dir);
}

// Hostile bounds (a package's KEY/VALUE, or the game's own file, is untrusted): an index past an array's end is refused
// (the library would pad with nulls — /a/4000000000 allocates until the machine runs out of memory); appending at the
// end or with "-" works. A pointer or a result nested deeper than the writer can recurse is refused, and so is a value
// that cannot be written (not UTF-8) — in every refusal the file is exactly as it was, never truncated.
TEST(jsonset_bounds_hostile_pointers_and_values)
{
    auto dir = TmpDir("jsb"); auto f = dir / "settings.json";
    const std::string Orig = "{\"a\": [1, 2]}";
    Write(f, Orig);
    CHECK(!FileEdits::JsonSet("/a/5", "9", f));
    CHECK_EQ(ReadAll(f), Orig);
    std::string Deep;
    for (int I = 0; I < 200; ++I) Deep += "/x";
    CHECK(!FileEdits::JsonSet(Deep, "1", f));
    CHECK_EQ(ReadAll(f), Orig);
    CHECK(!FileEdits::JsonSet("/b", std::string(200, '[') + std::string(200, ']'), f));
    CHECK_EQ(ReadAll(f), Orig);
    CHECK(!FileEdits::JsonSet("/b", "\xff\xfe", f));
    CHECK_EQ(ReadAll(f), Orig);
    CHECK(FileEdits::JsonSet("/a/2", "3", f));
    CHECK(FileEdits::JsonSet("/a/-", "4", f));
    CHECK(nlohmann::ordered_json::parse(ReadAll(f))["a"] == nlohmann::ordered_json::array({1, 2, 3, 4}));
    std::filesystem::remove_all(dir);
}

// Delete removes a file, or a folder and everything in it; a link goes as a link (its target stays); nothing there is
// a failure, not a quiet success. Teeth: make a missing path succeed (the false CHECK fails); follow links (the
// target file disappears).
TEST(deletepath_removes_files_folders_and_links_but_not_what_links_name)
{
    auto dir = TmpDir("delete");
    Write(dir / "intro.bik", "x");
    std::filesystem::create_directories(dir / "videos" / "sub");
    Write(dir / "videos" / "sub" / "a.bik", "x");
    Write(dir / "victim.txt", "keep me");
    std::filesystem::create_symlink(dir / "victim.txt", dir / "link.txt");
    CHECK(FileEdits::DeletePath(dir / "intro.bik"));
    CHECK(!std::filesystem::exists(dir / "intro.bik"));
    CHECK(FileEdits::DeletePath(dir / "videos"));
    CHECK(!std::filesystem::exists(dir / "videos"));
    CHECK(FileEdits::DeletePath(dir / "link.txt"));
    CHECK(!std::filesystem::is_symlink(dir / "link.txt"));
    CHECK_EQ(ReadAll(dir / "victim.txt"), std::string("keep me"));
    CHECK(!FileEdits::DeletePath(dir / "nothing-here"));
    std::filesystem::remove_all(dir);
}

// ---- XmlSet ----
namespace {
// Kingdom Come: Deliverance II's profile (profiles/default/attributes.xml): one <Attr> per setting, the key bindings as
// escaped XML inside one attribute, and settings left at their default absent altogether.
const std::string Kcd2Profile =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Attributes>\n"
    "\t<Attr name=\"g_resolution\" value=\"1920x1080\" />\n"
    "\t<!-- the game's own comment -->\n"
    "\t<Attr name=\"keybinds_settings\" value=\"&lt;keybinds&gt;&lt;superaction name=&quot;jump&quot; controller=&quot;keyboard&quot;&gt;"
    "&lt;control input=&quot;space&quot; slot=&quot;0&quot;/&gt;&lt;/superaction&gt;&lt;/keybinds&gt;\" />\n"
    "</Attributes>\n";
const std::string Jump = "/Attributes/Attr[@name='keybinds_settings']/@value!/keybinds/superaction[@name='jump'][@controller='keyboard']/control[@slot='0']/@input";
std::string Edit(const std::string &Key, const std::string &Value, const std::string &In)
{
    std::string Out, Why;
    if (!FileEdits::XmlEdit(Key, Value, In, Out, Why)) return "<failed: " + Why + ">";
    return Out;
}
std::string Get(const std::string &Key, const std::string &In)
{
    std::string Why;
    const auto V = FileEdits::XmlGet(Key, In, Why);
    return V ? *V : (Why.empty() ? "<absent>" : "<failed: " + Why + ">");
}
}

// Age of Mythology's rebinds: <keymapdata> elements told apart only by their children's text. [name='…'] picks the one
// whose <name> child holds that text; two predicates must both hold, so the "add" pair's event changes and the "drop"
// pair's stays. A missing pair is created holding its predicate children in predicate order, and a second op on the
// same predicates (its groupname) finds it instead of making another. Child and @attribute predicates mix; a predicate's
// text is compared unescaped and written escaped. A path ending on an element told apart by its children has no text
// to set: refused. Teeth: drop the child branch of Matches (the pair is not found: a second one is created); create
// without the predicate children (the groupname op makes a second pair); a child predicate parsed as an attribute.
TEST(xmlset_tells_elements_apart_by_their_childrens_text)
{
    const std::string Prf =
        "<playerprofile>\r\n"
        "\t<resolution>1280</resolution>\r\n"
        "\t<keymapdata>\r\n\t\t<name>Select Group 1</name>\r\n\t\t<event>1</event>\r\n\t\t<groupname>Selection</groupname>\r\n\t\t<actiontype>1</actiontype>\r\n\t</keymapdata>\r\n"
        "\t<keymapdata>\r\n\t\t<name>Select Group 1</name>\r\n\t\t<event>f12</event>\r\n\t\t<groupname>Selection</groupname>\r\n\t\t<actiontype>0</actiontype>\r\n\t</keymapdata>\r\n"
        "</playerprofile>\r\n";
    const std::string Add1 = "/playerprofile/keymapdata[name='Select Group 1'][actiontype='0']";
    std::string Want = Prf;
    Want.replace(Want.find("f12"), 3, "f11");
    const std::string Out = Edit(Add1 + "/event", "f11", Prf);
    CHECK_EQ(Out, Want);
    CHECK_EQ(Get(Add1 + "/event", Out), std::string("f11"));
    CHECK_EQ(Get("/playerprofile/keymapdata[name='Select Group 1'][actiontype='1']/event", Out), std::string("1"));
    CHECK_EQ(Get("/playerprofile/keymapdata[name='Select Group 1'][actiontype='2']/event", Out), std::string("<absent>"));

    const std::string Add2 = "/playerprofile/keymapdata[name='Select Group 2'][actiontype='0']";
    const std::string Made = Edit(Add2 + "/groupname", "Selection", Edit(Add2 + "/event", "f10", Prf));
    std::string Want2 = Prf;
    Want2.insert(Want2.rfind("\r\n</playerprofile>"), "\r\n\t<keymapdata><name>Select Group 2</name><actiontype>0</actiontype>"
                 "<event>f10</event><groupname>Selection</groupname></keymapdata>");
    CHECK_EQ(Made, Want2);
    CHECK_EQ(Get(Add2 + "/event", Made), std::string("f10"));

    CHECK_EQ(Edit("/r/e[@k='1'][c='A & B']/@v", "x", "<r>\n</r>\n"), std::string("<r>\n\t<e k=\"1\" v=\"x\"><c>A &amp; B</c></e>\n</r>\n"));
    CHECK_EQ(Get("/r/e[@k='1'][c='A & B']/@v", "<r><e k=\"1\" v=\"y\"><c>A &amp; B</c></e></r>"), std::string("y"));
    CHECK_EQ(Get("/r/e[c='A & B']/@v", "<r><e v=\"y\" c=\"A &amp; B\"/></r>"), std::string("<absent>"));   // an attribute is no child
    CHECK(Edit(Add1, "x", Prf).rfind("<failed:", 0) == 0);                                                 // no text to set
    CHECK(Edit("/r/e[c='1']", "x", "<r/>").rfind("<failed:", 0) == 0);                                    // nor to create
    CHECK(Edit("/playerprofile/keymapdata[name=Select]/event", "x", Prf).rfind("<failed:", 0) == 0);      // unquoted
}

// An attribute that is there changes in place: every other byte of the file (declaration, comment, quoting, the
// "/>" spacing) stays. Teeth: rewrite the document instead of splicing; match the wrong <Attr> (no predicate check).
TEST(xmlset_changes_an_attribute_in_place)
{
    std::string Want = Kcd2Profile;
    Want.replace(Want.find("1920x1080"), 9, "2560x1440");
    CHECK_EQ(Edit("/Attributes/Attr[@name='g_resolution']/@value", "2560x1440", Kcd2Profile), Want);
    CHECK_EQ(Get("/Attributes/Attr[@name='g_resolution']/@value", Want), std::string("2560x1440"));
}

// A setting the game leaves out while it is at its default (KCD2's language) is created: an element with the
// predicate's attribute and the value, after its parent's last child, indented like it. Teeth: no creation (the op is
// a no-op, as ConfigWrite was); insert before the root instead of inside it.
TEST(xmlset_creates_a_missing_element_after_its_siblings)
{
    const std::string Out = Edit("/Attributes/Attr[@name='g_language']/@value", "german", Kcd2Profile);
    std::string Want = Kcd2Profile;
    Want.insert(Want.find("\n</Attributes>"), "\n\t<Attr name=\"g_language\" value=\"german\" />");
    CHECK_EQ(Out, Want);
    CHECK_EQ(Get("/Attributes/Attr[@name='g_language']/@value", Out), std::string("german"));
    CHECK_EQ(Get("/Attributes/Attr[@name='g_language']/@value", Kcd2Profile), std::string("<absent>"));
}

// '!' steps into the XML an attribute holds: the key bindings. A binding there is set and read; one the game left out
// (it saves only those that differ from its defaults) is created inside the inner document; the outer file stays
// escaped. Teeth: write the inner value unescaped; drop the '!' step (set the outer attribute to the bare value).
TEST(xmlset_reaches_into_xml_inside_an_attribute)
{
    const std::string Out = Edit(Jump, "w", Kcd2Profile);
    CHECK_EQ(Get(Jump, Out), std::string("w"));
    CHECK(Out.find("control input=&quot;w&quot; slot=&quot;0&quot;") != std::string::npos);
    CHECK(Out.find("g_resolution\" value=\"1920x1080\" />\n\t<!-- the game's own comment -->") != std::string::npos);
    const std::string Crouch = "/Attributes/Attr[@name='keybinds_settings']/@value!/keybinds/superaction[@name='crouch'][@controller='keyboard']/control[@slot='0']/@input";
    const std::string Out2 = Edit(Crouch, "left ctrl", Out);
    CHECK_EQ(Get(Crouch, Out2), std::string("left ctrl"));
    CHECK_EQ(Get(Jump, Out2), std::string("w"));
    CHECK(Out2.find('<', Out2.find("value=\"&lt;keybinds")) == Out2.find("\n</Attributes>") + 1);   // nothing raw inside
}

// Element text, an empty parent, an absent file's document, values that need escaping, and a CRLF file's own newlines.
// Teeth: escape nothing (the & breaks the file); insert with "\n" into a CRLF file.
TEST(xmlset_sets_text_escapes_and_keeps_newlines)
{
    CHECK_EQ(Edit("/cfg/video/width", "1024", "<cfg><video><width>800</width></video></cfg>"),
             std::string("<cfg><video><width>1024</width></video></cfg>"));
    CHECK_EQ(Edit("/cfg/video/width", "1024", "<cfg><video/></cfg>"),
             std::string("<cfg><video>\n\t<width>1024</width>\n</video></cfg>"));
    CHECK_EQ(Edit("/cfg/video/@mode", "1", ""), std::string("<cfg><video mode=\"1\" /></cfg>\n"));
    const std::string Esc = Edit("/a/@v", "R&D \"<x>\"", "<a v='1'/>");
    CHECK_EQ(Esc, std::string("<a v='R&amp;D \"&lt;x&gt;\"'/>"));
    CHECK_EQ(Get("/a/@v", Esc), std::string("R&D \"<x>\""));
    CHECK_EQ(Edit("/a/b[@n='2']/@v", "y", "<a>\r\n  <b n=\"1\" v=\"x\"/>\r\n</a>\r\n"),
             std::string("<a>\r\n  <b n=\"1\" v=\"x\"/>\r\n  <b n=\"2\" v=\"y\" />\r\n</a>\r\n"));
    CHECK_EQ(Get("/a/b", "<a><b><![CDATA[1 < 2]]></b></a>"), std::string("1 < 2"));
    //A parent holding only a comment keeps it; the new element goes after it.
    CHECK_EQ(Edit("/cfg/video/res/@w", "1", "<cfg>\n  <video>\n    <!-- written by the game -->\n  </video>\n</cfg>"),
             std::string("<cfg>\n  <video>\n    <!-- written by the game -->\n  \t<res w=\"1\" />\n  </video>\n</cfg>"));
    //An attribute's literal newline reads as a space, as XML says.
    CHECK_EQ(Get("/a/@v", "<a v=\"x\ny\"/>"), std::string("x y"));
}

// What it cannot do safely it refuses, and the file is not touched: a file that is not well-formed, a root other than
// the path's, text set on an element that holds elements, an entity only a DTD defines, a malformed path.
TEST(xmlset_refuses_what_it_cannot_write_back)
{
    for (const auto &[Key, In] : std::vector<std::pair<std::string, std::string>>{
             {"/a/@v", "<a><b></a>"}, {"/a/@v", "<a><b></c></a>"}, {"/a/@v", "<a/><b/>"}, {"/x/@v", "<a/>"}, {"/a", "<a><b/></a>"},
             {"/a/@v", "<a v=\"&custom;\"/>"}, {"a/@v", "<a/>"}, {"/a/b[1]/@v", "<a/>"}, {"/a/", "<a/>"},
             {"/a/b", "<a><b><!x></b></a>"}, {"/a/b", "<a><b><!DOCTYPE z></b></a>"},              // no markup as a value
             {"/a/b[@='1']/@v", "<a/>"}, {"/a/b[@k><evil/><z='1']/@v", "<a/>"}, {"/a/b&x/@v", "<a/>"},  // XML names only
             {"/a/b[@na me='x']/@v", "<a/>"}, {"/a/@v", "<a v=\"&#xD800;\"/>"}, {"/a/b/@v", "<a>text<!-- note --></a>"}})
    {
        std::string Out, Why;
        CHECK(!FileEdits::XmlEdit(Key, "1", In, Out, Why));
        CHECK(!Why.empty());
    }
    auto Dir = TmpDir("xml");
    const auto F = Dir / "bad.xml";
    Write(F, "<a><b></a>");
    CHECK(!FileEdits::XmlSet("/a/@v", "1", F));
    CHECK_EQ(ReadAll(F), std::string("<a><b></a>"));
    std::filesystem::remove_all(Dir);
}

namespace {
// Age of Mythology's profile (Users/Default.prf): UTF-16 LE with a byte-order mark. Written here by hand, one unit per
// character; the name holds an é (one unit) and an emoji (a surrogate pair), so both kinds come back as they were.
const std::string AomName = "D\xC3\xA9" "fault\xF0\x9F\x98\x80";   // "Default" with é, then U+1F600
std::string Utf16(const std::string &Ascii, bool Le)
{
    std::string Out = Le ? "\xFF\xFE" : "\xFE\xFF";
    const auto Unit = [&](unsigned char Hi, unsigned char Lo) { Out += static_cast<char>(Le ? Lo : Hi); Out += static_cast<char>(Le ? Hi : Lo); };
    for (size_t I = 0; I < Ascii.size(); ++I)
        if (Ascii.compare(I, AomName.size(), AomName) == 0)
        {
            for (char C : std::string("D")) Unit(0, static_cast<unsigned char>(C));
            Unit(0x00, 0xE9);
            for (char C : std::string("fault")) Unit(0, static_cast<unsigned char>(C));
            Unit(0xD8, 0x3D); Unit(0xDE, 0x00);
            I += AomName.size() - 1;
        }
        else Unit(0, static_cast<unsigned char>(Ascii[I]));
    return Out;
}
std::string AomProfile(const std::string &Res)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n\r\n<playerprofile>\r\n\t<profilename>" + AomName
         + "</profilename>\r\n\t<profileguid>{00000000-0000-4000-8000-000000000000}</profileguid>\r\n\t<resolution>" + Res
         + "</resolution>\r\n\t<displaybitdepth>32</displaybitdepth>\r\n</playerprofile>\r\n";
}
}

// A UTF-16 file (either byte order, by its mark) is edited in its own encoding: the value changes and every other byte
// stays, the é and the surrogate pair included; reading it gives UTF-8. Teeth: write the edit back as UTF-8; read
// without decoding (the root is not found); swap the byte order on the way back.
TEST(xmlset_edits_a_utf16_file_in_its_own_encoding)
{
    for (const bool Le : {true, false})
    {
        const std::string In = Utf16(AomProfile("1280"), Le);
        CHECK_EQ(Edit("/playerprofile/resolution", "1920", In), Utf16(AomProfile("1920"), Le));
        CHECK_EQ(Get("/playerprofile/resolution", In), std::string("1280"));
        CHECK_EQ(Get("/playerprofile/profilename", In), AomName);
        //A missing element is created in the file's encoding too.
        const std::string Added = Edit("/playerprofile/windowmode", "1", In);
        CHECK(Added.rfind(Le ? "\xFF\xFE" : "\xFE\xFF", 0) == 0);
        CHECK_EQ(Get("/playerprofile/windowmode", Added), std::string("1"));
        CHECK_EQ(Get("/playerprofile/resolution", Added), std::string("1280"));
    }
    auto Dir = TmpDir("xml16");
    const auto F = Dir / "Default.prf";
    Write(F, Utf16(AomProfile("1280"), true));
    CHECK(FileEdits::XmlSet("/playerprofile/resolution", "2560", F));
    CHECK_EQ(ReadAll(F), Utf16(AomProfile("2560"), true));
    std::filesystem::remove_all(Dir);
}

// What does not turn back into its own bytes is refused, and the file is left as it was: an odd length, a lone
// surrogate, a UTF-32 file, a value that is not UTF-8 text. Teeth: decode leniently (drop the odd byte, pass the lone
// surrogate through) or encode a bad value byte by byte.
TEST(xmlset_refuses_a_utf16_file_it_cannot_write_back)
{
    const std::string Good = Utf16(AomProfile("1280"), true);
    std::string LoneHigh = Good;
    LoneHigh.replace(LoneHigh.find(std::string("\xDE", 1)) - 1, 2, std::string("A\0", 2));   // the emoji's low half gone
    for (const std::string &In : {Good + "x", LoneHigh, std::string("\xFF\xFE\0\0<\0\0\0", 8)})
    {
        std::string Out, Why;
        CHECK(!FileEdits::XmlEdit("/playerprofile/resolution", "1920", In, Out, Why));
        CHECK(!Why.empty());
        CHECK(!FileEdits::XmlGet("/playerprofile/resolution", In, Why));
    }
    std::string Out, Why;
    CHECK(!FileEdits::XmlEdit("/playerprofile/resolution", "19\xFF" "20", Good, Out, Why));
    CHECK(Why.find("UTF-8") != std::string::npos);
    auto Dir = TmpDir("xml16bad");
    const auto F = Dir / "Default.prf";
    Write(F, Good + "x");
    CHECK(!FileEdits::XmlSet("/playerprofile/resolution", "1920", F));
    CHECK_EQ(ReadAll(F), Good + "x");
    std::filesystem::remove_all(Dir);
}

// The XmlSet MODE reaches XmlSet through ProcessFileEdits (KEY the path, VALUE the value). Teeth: drop the dispatch.
TEST(xmlset_mode_applies_through_the_edit_pass)
{
    const std::filesystem::path Root = TmpDir("xmlmode");
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base / "profiles");
    Write(Base / "profiles" / "attributes.xml", Kcd2Profile);
    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","profiles/attributes.xml"},{"MODE","XmlSet"},
                               {"KEY","/Attributes/Attr[@name='g_language']/@value"},{"VALUE","german"}}});
    CHECK(FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base));
    CHECK_EQ(Get("/Attributes/Attr[@name='g_language']/@value", ReadAll(Base / "profiles" / "attributes.xml")), std::string("german"));
    std::filesystem::remove_all(Root);
}

// LineFlag: a line whose presence is the setting (AoM The Titans' "+EasyDragMilitary"). 1 adds it once, in the file's
// own line ending and never fused onto an unterminated last line; 0 removes every copy and nothing else. Lines are
// equal after trimming, case kept — never a prefix match. Teeth: prefix matching (removes "+EasyDragMilitaryX"); no
// trim (the indented copy stays); LF into a CRLF file; no separator; appending twice; accepting "true".
TEST(lineflag_makes_its_line_there_or_gone)
{
    const std::string K = "+EasyDragMilitary";
    std::string Out, Why;
    CHECK(FileEdits::LineFlagEdit(K, "0", "a=1\r\n+EasyDragMilitary\r\n+EasyDragMilitaryX\r\n  +EasyDragMilitary \r\n+easydragmilitary\r\nb=2", Out, Why));
    CHECK_EQ(Out, std::string("a=1\r\n+EasyDragMilitaryX\r\n+easydragmilitary\r\nb=2"));
    CHECK(FileEdits::LineFlagEdit(K, "0", "a=1\n+EasyDragMilitary", Out, Why));
    CHECK_EQ(Out, std::string("a=1\n"));
    CHECK(FileEdits::LineFlagEdit(K, "1", "a=1\r\nb=2\r\n", Out, Why));
    CHECK_EQ(Out, std::string("a=1\r\nb=2\r\n+EasyDragMilitary\r\n"));
    CHECK(FileEdits::LineFlagEdit(K, "1", "a=1", Out, Why));
    CHECK_EQ(Out, std::string("a=1\n+EasyDragMilitary\n"));
    CHECK(FileEdits::LineFlagEdit(K, "1", "", Out, Why));
    CHECK_EQ(Out, std::string("+EasyDragMilitary\n"));
    CHECK(FileEdits::LineFlagEdit(K, "1", "a=1\n\t+EasyDragMilitary\n", Out, Why));
    CHECK_EQ(Out, std::string("a=1\n\t+EasyDragMilitary\n"));                     // there already, as the game wrote it
    CHECK(FileEdits::LineHas(K, "x\r\n +EasyDragMilitary\r\n") && !FileEdits::LineHas(K, "+EasyDragMilitaryX\n+easydragmilitary"));
    for (const char *V : {"true", "", "2", " 1"})
    {
        Why.clear();
        CHECK(!FileEdits::LineFlagEdit(K, V, "a\n", Out, Why) && !Why.empty());
    }
    CHECK(!FileEdits::LineFlagEdit("  ", "1", "a\n", Out, Why));
    CHECK(!FileEdits::LineFlagEdit("a\nb", "1", "a\n", Out, Why));
}

// Through the edit pass: IF_EXISTS skips a file the game has not made (no stub), 0 on a missing file writes nothing, 1
// creates it, and a refused VALUE fails the pass with the file untouched. Teeth: create the file for a 0 (the stub
// appears); write before refusing (the file changes).
TEST(lineflag_mode_applies_through_the_edit_pass)
{
    const std::filesystem::path Base = TmpDir("lineflag");
    ContainerParams CP(Base / "PKG");
    const auto Op = [](const char *File, const char *Value, bool IfExists) {
        nlohmann::ordered_json O = {{"TYPE","FileEdit"},{"FILE",File},{"MODE","LineFlag"},{"KEY","+EasyDragMilitary"},{"VALUE",Value}};
        if (IfExists) O["IF_EXISTS"] = true;
        return O; };
    CP.SubComponentsArray = nlohmann::ordered_json::array({ Op("startup/persistent.cfg", "1", true), Op("zero.cfg", "0", false) });
    CHECK(FileEdits::ProcessFileEdits(CP, false, Base));
    CHECK(!std::filesystem::exists(Base / "startup" / "persistent.cfg"));
    CHECK(!std::filesystem::exists(Base / "zero.cfg"));
    std::filesystem::create_directories(Base / "startup");
    Write(Base / "startup" / "persistent.cfg", "+showFPS\r\n");
    CHECK(FileEdits::ProcessFileEdits(CP, false, Base));
    CHECK_EQ(ReadAll(Base / "startup" / "persistent.cfg"), std::string("+showFPS\r\n+EasyDragMilitary\r\n"));
    CP.SubComponentsArray = nlohmann::ordered_json::array({ Op("startup/persistent.cfg", "on", false), Op("new.cfg", "1", false) });
    CHECK(!FileEdits::ProcessFileEdits(CP, false, Base));
    CHECK_EQ(ReadAll(Base / "startup" / "persistent.cfg"), std::string("+showFPS\r\n+EasyDragMilitary\r\n"));
    CHECK_EQ(ReadAll(Base / "new.cfg"), std::string("+EasyDragMilitary\n"));
    CP.SubComponentsArray = nlohmann::ordered_json::array({ Op("startup/persistent.cfg", "0", true) });
    CHECK(FileEdits::ProcessFileEdits(CP, false, Base));
    CHECK_EQ(ReadAll(Base / "startup" / "persistent.cfg"), std::string("+showFPS\r\n"));
    std::filesystem::remove_all(Base);
}
