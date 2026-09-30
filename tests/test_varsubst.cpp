#include <limits>
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

TEST(render_ascii_and_asciiz)
{
    CHECK_EQ(RenderValue("10.66.1.2", "ascii"),  std::string("31302e36362e312e32"));
    CHECK_EQ(RenderValue("10.66.1.2", "asciiz"), std::string("31302e36362e312e3200"));
    CHECK_EQ(RenderValue("", "asciiz"),          std::string("00"));   // empty → just the NUL
    CHECK_EQ(RenderValue("AB", "ascii"),         std::string("4142"));
}

TEST(render_empty_or_unknown_format_unchanged)
{
    CHECK_EQ(RenderValue("hello", ""),            std::string("hello"));
    CHECK_EQ(RenderValue("x", "unknownformat"),   std::string("x"));
}

// ---- WHEN condition evaluator ----
using VarSubst::EvaluateCondition;

TEST(cond_equality_and_inequality)
{
    std::map<std::string, std::string> m{{"NETMODE", "host"}};
    CHECK(EvaluateCondition("%NETMODE% == host", m));
    CHECK(!EvaluateCondition("%NETMODE% == join", m));
    CHECK(EvaluateCondition("%NETMODE% != join", m));
    CHECK(!EvaluateCondition("%NETMODE% != host", m));
    CHECK(EvaluateCondition("%NETMODE% == \"host\"", m));   // quoted RHS
}

TEST(cond_truthy_operand)
{
    std::map<std::string, std::string> m{{"A", "1"}, {"B", "0"}, {"C", ""}, {"D", "false"}};
    CHECK(EvaluateCondition("%A%", m));
    CHECK(!EvaluateCondition("%B%", m));
    CHECK(!EvaluateCondition("%C%", m));
    CHECK(!EvaluateCondition("%D%", m));
    CHECK(EvaluateCondition("%MISSING% == \"\"", m));       // undefined key → empty string
}

TEST(cond_boolean_ops_and_precedence)
{
    std::map<std::string, std::string> m{{"MODE", "join"}, {"ADV", "1"}};
    CHECK(EvaluateCondition("%MODE% == join && %ADV%", m));
    CHECK(!EvaluateCondition("%MODE% == host && %ADV%", m));
    CHECK(EvaluateCondition("%MODE% == host || %MODE% == join", m));
    CHECK(EvaluateCondition("!(%MODE% == host)", m));
    CHECK(!EvaluateCondition("!(%MODE% == join)", m));
    // precedence: ! > && > ||  →  "host||join&&adv" is host || (join && adv) = true
    CHECK(EvaluateCondition("%MODE% == host || %MODE% == join && %ADV%", m));
}

TEST(cond_injection_safe)
{
    // A value containing operator characters must be compared as data, never parsed as operators.
    std::map<std::string, std::string> m{{"X", "a && b"}, {"Y", "a && b"}};
    CHECK(EvaluateCondition("%X% == %Y%", m));
    std::map<std::string, std::string> m2{{"X", "1 || 1"}};
    CHECK(!EvaluateCondition("%X% == host", m2));           // "1 || 1" != host, not evaluated as OR
}

TEST(cond_blank_and_garbage_fail_open)
{
    std::map<std::string, std::string> m;
    CHECK(EvaluateCondition("", m));                        // empty → always true
    CHECK(EvaluateCondition("   ", m));
    CHECK(EvaluateCondition("== ==", m));                   // garbage → fail-open true
    CHECK(EvaluateCondition("%A% ==", m));                  // dangling → fail-open true
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

//The value-walk replaced a dump-substitute-reparse splice, which substituted object KEYS as well as values.
//A RegEdit's KEYVALUES is keyed by the registry VALUE NAME, so a token there is legitimate authoring — and
//losing it would write the literal "%TOKEN%" into the registry with no diagnostic at all.
TEST(json_substitution_covers_keys_values_and_nesting)
{
    const std::map<std::string, std::string> Vars = {{"MODE", "host"}, {"DIR", "C:\\Program Files"}};
    nlohmann::ordered_json In = {
        {"TYPE", "RegEdit"},
        {"KEYVALUES", {{"%MODE%_Port", "%MODE%"}}},
        {"LIST", nlohmann::ordered_json::array({"%DIR%/a", 7, true})},
        {"NESTED", {{"inner", {{"%MODE%", "%DIR%"}}}}},
    };
    const nlohmann::ordered_json Out = VarSubst::SubstituteJsonValues(In, Vars);
    CHECK(Out["KEYVALUES"].contains("host_Port"));
    CHECK_EQ(Out["KEYVALUES"].value("host_Port", std::string()), std::string("host"));
    //A value that would break JSON if spliced into serialised text survives verbatim.
    CHECK_EQ(Out["LIST"][0].get<std::string>(), std::string("C:\\Program Files/a"));
    CHECK_EQ(Out["LIST"][1].get<int>(), 7);              // non-strings pass through untouched
    CHECK_EQ(Out["LIST"][2].get<bool>(), true);
    CHECK(Out["NESTED"]["inner"].contains("host"));
    CHECK_EQ(Out["NESTED"]["inner"].value("host", std::string()), std::string("C:\\Program Files"));
}

//A layer's or an op's COMMENT is prose, carried verbatim: UserPatch's save-name comment quotes "%s-%s", and rendering
//it warned about undefined variables on every launch. A value merely NAMED COMMENT elsewhere is still data.
//Teeth: substitute COMMENT like any value (the op's comment loses nothing here, but the ENV check below keeps the
//rule from widening to every key named COMMENT).
TEST(json_substitution_leaves_prose_comments_alone)
{
    const std::map<std::string, std::string> Vars = {{"s", "S"}, {"MODE", "host"}};
    const nlohmann::ordered_json Op = {{"MODE", "Replace"}, {"COMMENT", "quotes '%s%s.%s' and %MODE%"}, {"VALUE", "%MODE%"}};
    const nlohmann::ordered_json Out = VarSubst::SubstituteJsonValues(Op, Vars);
    CHECK_EQ(Out["COMMENT"].get<std::string>(), std::string("quotes '%s%s.%s' and %MODE%"));
    CHECK_EQ(Out["VALUE"].get<std::string>(), std::string("host"));
    const nlohmann::ordered_json Env = {{"ENV", {{"COMMENT", "%MODE%"}}}};
    CHECK_EQ(VarSubst::SubstituteJsonValues(Env, Vars)["ENV"]["COMMENT"].get<std::string>(), std::string("host"));
}

// EVAL's integer expressions: a registry bitfield from independent options (UserPatch's Mini-map Colors: darken red
// 0x02, darken purple 0x20, light grey 0x40 unless darkened). C precedence; hex and decimal; anything unresolved or
// malformed fails and leaves the output alone. Teeth: drop | (the bits no longer combine); parse a %token% as 0;
// give + and | one precedence.
TEST(eval_integer_expressions)
{
    long long V = -1;
    CHECK(VarSubst::EvaluateInteger("1*2 | 0*32 | (1-0)*64", V)); CHECK_EQ(V, 66LL);
    CHECK(VarSubst::EvaluateInteger("0x40 & ~0x40", V));          CHECK_EQ(V, 0LL);
    CHECK(VarSubst::EvaluateInteger("1 << 3 | 2", V));             CHECK_EQ(V, 10LL);
    CHECK(VarSubst::EvaluateInteger("2 + 3 | 8", V));              CHECK_EQ(V, 13LL);     // + binds tighter than |
    CHECK(VarSubst::EvaluateInteger(" 2*(3+4) ", V));              CHECK_EQ(V, 14LL);
    V = 99;
    CHECK(!VarSubst::EvaluateInteger("%a%*2", V));                 CHECK_EQ(V, 99LL);     // not resolved yet
    CHECK(!VarSubst::EvaluateInteger("", V));
    CHECK(!VarSubst::EvaluateInteger("3+", V));
    CHECK(!VarSubst::EvaluateInteger("(1", V));
    CHECK(!VarSubst::EvaluateInteger("4/0", V));
    CHECK(!VarSubst::EvaluateInteger("12abc", V));
}

// What has no defined result fails instead of trapping or invoking UB — package data reaches this on the launch path:
// INT64_MIN / -1 and % -1 (a hardware trap on x86), shift counts outside 0..63, literals beyond 64 bits, runaway
// nesting. Overflow wraps as 64-bit two's complement. The Python reference (resolve.py self-test) holds the same table.
// Teeth: drop the MIN/-1 guard (the test process dies of SIGFPE); drop the shift range check (UBSan, and 1<<64 == 1).
TEST(eval_integer_undefined_results_fail)
{
    long long V = 7;
    CHECK(!VarSubst::EvaluateInteger("(~0x7fffffffffffffff)/-1", V));
    CHECK(!VarSubst::EvaluateInteger("(~0x7fffffffffffffff)%-1", V));
    CHECK(!VarSubst::EvaluateInteger("1<<64", V));
    CHECK(!VarSubst::EvaluateInteger("1<<-1", V));
    CHECK(!VarSubst::EvaluateInteger("8>>64", V));
    CHECK(!VarSubst::EvaluateInteger("0x1ffffffffffffffff", V));
    CHECK(!VarSubst::EvaluateInteger("99999999999999999999", V));
    CHECK(!VarSubst::EvaluateInteger(std::string(200, '(') + "1" + std::string(200, ')'), V));
    CHECK(!VarSubst::EvaluateInteger(std::string(200, '-') + "1", V));
    CHECK(!VarSubst::EvaluateInteger("1\xc2\xa0+1", V));                // only ASCII whitespace separates
    CHECK_EQ(V, 7LL);
    CHECK(VarSubst::EvaluateInteger("0x7fffffffffffffff + 1", V));   CHECK_EQ(V, std::numeric_limits<long long>::min());
    CHECK(VarSubst::EvaluateInteger("-1 << 63", V));                 CHECK_EQ(V, std::numeric_limits<long long>::min());
    CHECK(VarSubst::EvaluateInteger("-8 >> 1", V));                  CHECK_EQ(V, -4LL);
    CHECK(VarSubst::EvaluateInteger("-7 / 2", V));                   CHECK_EQ(V, -3LL);
    CHECK(VarSubst::EvaluateInteger("-7 % 2", V));                   CHECK_EQ(V, -1LL);
    CHECK(VarSubst::EvaluateInteger("9007199254740993 / 1", V));     CHECK_EQ(V, 9007199254740993LL);
    CHECK(VarSubst::EvaluateInteger(std::string(30, '(') + "1" + std::string(30, ')'), V));  CHECK_EQ(V, 1LL);
}
