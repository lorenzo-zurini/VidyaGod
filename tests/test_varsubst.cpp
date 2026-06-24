#include "vgtest.h"
#include "varsubst.h"

#include <map>
#include <string>

using VarSubst::StringVariableSubstitution;
using VarSubst::RenderValue;

static std::string Sub(std::string s, const std::map<std::string, std::string> &m)
{
    StringVariableSubstitution(s, m);
    return s;
}

TEST(subst_expands_known_tokens)
{
    std::map<std::string, std::string> m{{"PackageUID", "abc"}, {"ContentPath", "game.exe"}};
    CHECK_EQ(Sub("C:\\%PackageUID%\\%ContentPath%", m), std::string("C:\\abc\\game.exe"));
    CHECK_EQ(Sub("no tokens here", m), std::string("no tokens here"));
    CHECK_EQ(Sub("%PackageUID%", m), std::string("abc"));
}

TEST(subst_returns_whether_replaced)
{
    std::map<std::string, std::string> m{{"K", "v"}};
    std::string a = "x%K%y";
    CHECK(StringVariableSubstitution(a, m));          // a replacement happened
    std::string b = "no tokens";
    CHECK(!StringVariableSubstitution(b, m));         // nothing to replace
}

TEST(subst_unknown_token_left_in_place)
{
    std::map<std::string, std::string> m{{"K", "v"}};
    // Unknown key is preserved verbatim (so the caller can diagnose), known key still expands.
    CHECK_EQ(Sub("%K%-%MISSING%", m), std::string("v-%MISSING%"));
}

TEST(subst_unmatched_percent_aborts_preserving_remainder)
{
    std::map<std::string, std::string> m{{"K", "v"}};
    // A lone '%' with no closing delimiter: substitution stops, remainder kept unchanged.
    CHECK_EQ(Sub("%K% then 50% off", m), std::string("v then 50% off"));
    CHECK_EQ(Sub("100% raw", m), std::string("100% raw"));
}

TEST(subst_empty_token_and_empty_value)
{
    std::map<std::string, std::string> m{{"", "EMPTYKEY"}, {"K", ""}};
    CHECK_EQ(Sub("%%", m), std::string("EMPTYKEY"));  // %% → key "" → its value
    CHECK_EQ(Sub("a%K%b", m), std::string("ab"));     // known key mapping to empty string
}

// ---- Use-site render formats: %KEY:format% ----

TEST(render_dword)
{
    CHECK_EQ(RenderValue("0", "dword"),   std::string("dword:00000000"));
    CHECK_EQ(RenderValue("1", "dword"),   std::string("dword:00000001"));
    CHECK_EQ(RenderValue("255", "dword"), std::string("dword:000000ff"));
    CHECK_EQ(RenderValue("notanum", "dword"), std::string("notanum"));   // unparseable → unchanged
}

TEST(render_qword_is_little_endian)
{
    CHECK_EQ(RenderValue("0", "qword"), std::string("hex(b):00,00,00,00,00,00,00,00"));
    CHECK_EQ(RenderValue("1", "qword"), std::string("hex(b):01,00,00,00,00,00,00,00"));
    CHECK_EQ(RenderValue("256", "qword"), std::string("hex(b):00,01,00,00,00,00,00,00"));
}

TEST(render_bool_is_text)
{
    // bool now renders human text (true/false); use :dword for the Wine registry form.
    CHECK_EQ(RenderValue("1", "bool"),     std::string("true"));
    CHECK_EQ(RenderValue("true", "bool"),  std::string("true"));
    CHECK_EQ(RenderValue("YES", "bool"),   std::string("true"));
    CHECK_EQ(RenderValue("0", "bool"),     std::string("false"));
    CHECK_EQ(RenderValue("anything", "bool"), std::string("false"));
    CHECK_EQ(RenderValue("1", "dword"),    std::string("dword:00000001"));  // the registry form
}

TEST(render_case_and_winpath)
{
    CHECK_EQ(RenderValue("a/b/c.dll", "winpath"), std::string("a\\b\\c.dll"));
    CHECK_EQ(RenderValue("MixedCase", "upper"),   std::string("MIXEDCASE"));
    CHECK_EQ(RenderValue("MixedCase", "lower"),   std::string("mixedcase"));
}

TEST(render_empty_or_unknown_format_unchanged)
{
    CHECK_EQ(RenderValue("hello", ""),            std::string("hello"));
    CHECK_EQ(RenderValue("x", "unknownformat"),   std::string("x"));
}

TEST(subst_applies_use_site_format)
{
    // The big win: one raw value, rendered differently per consumer via %KEY:format%.
    std::map<std::string, std::string> m{{"FULLSCREEN", "1"}, {"DIR", "a/b"}};
    CHECK_EQ(Sub("%FULLSCREEN%", m),       std::string("1"));                 // config: raw
    CHECK_EQ(Sub("%FULLSCREEN:dword%", m), std::string("dword:00000001"));    // registry: dword
    CHECK_EQ(Sub("%FULLSCREEN:bool%", m),  std::string("true"));             // text: true/false
    CHECK_EQ(Sub("C:\\%DIR:winpath%", m),  std::string("C:\\a\\b"));         // guest path
    // Unknown key keeps the whole token (incl. format) in place.
    CHECK_EQ(Sub("%MISSING:dword%", m),    std::string("%MISSING:dword%"));
}
