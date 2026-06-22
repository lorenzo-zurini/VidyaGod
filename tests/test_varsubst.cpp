#include "vgtest.h"
#include "varsubst.h"

#include <map>
#include <string>

using VarSubst::StringVariableSubstitution;
using VarSubst::TranslateCustomVarValue;

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

TEST(translate_dword)
{
    CHECK_EQ(TranslateCustomVarValue("0", "dword"),   std::string("dword:00000000"));
    CHECK_EQ(TranslateCustomVarValue("1", "dword"),   std::string("dword:00000001"));
    CHECK_EQ(TranslateCustomVarValue("255", "dword"), std::string("dword:000000ff"));
    // Unparseable → left unchanged.
    CHECK_EQ(TranslateCustomVarValue("notanum", "dword"), std::string("notanum"));
}

TEST(translate_qword_is_little_endian)
{
    CHECK_EQ(TranslateCustomVarValue("0", "qword"), std::string("hex(b):00,00,00,00,00,00,00,00"));
    CHECK_EQ(TranslateCustomVarValue("1", "qword"), std::string("hex(b):01,00,00,00,00,00,00,00"));
    CHECK_EQ(TranslateCustomVarValue("256", "qword"), std::string("hex(b):00,01,00,00,00,00,00,00"));
}

TEST(translate_bool)
{
    CHECK_EQ(TranslateCustomVarValue("1", "bool"),     std::string("dword:00000001"));
    CHECK_EQ(TranslateCustomVarValue("true", "bool"),  std::string("dword:00000001"));
    CHECK_EQ(TranslateCustomVarValue("YES", "bool"),   std::string("dword:00000001"));
    CHECK_EQ(TranslateCustomVarValue("0", "bool"),     std::string("dword:00000000"));
    CHECK_EQ(TranslateCustomVarValue("anything", "bool"), std::string("dword:00000000"));
}

TEST(translate_passthrough_types)
{
    CHECK_EQ(TranslateCustomVarValue("hello", "string"), std::string("hello"));
    CHECK_EQ(TranslateCustomVarValue("42", "number"),    std::string("42"));
    CHECK_EQ(TranslateCustomVarValue("opt", "options"),  std::string("opt"));
    CHECK_EQ(TranslateCustomVarValue("x", "unknowntype"),std::string("x"));
}
