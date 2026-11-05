#ifndef VARSUBST_H
#define VARSUBST_H

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

// VarSubst — the pure %TOKEN% substitution + custom-var value encoding seam, lifted out of ContainerWrapper.
// Zero heavy deps (no Qt / IPFS / catalog), so it is unit-testable in isolation (tests/test_varsubst.cpp).
namespace VarSubst
{

//Substitutes %variables% in every STRING inside a JSON value, recursively (arrays and objects included).
//
//Use this rather than dump -> substitute text -> parse. The text form is only safe while no substituted VALUE
//can break JSON, and several can: filesystem paths are backslash-laden on Windows ("\\U" and "\\T" are invalid
//escapes), package TITLEs are free text, and CustomVar values are typed by the user in the pre-launch dialog.
//One quote or backslash and the re-parse either throws out of the launch or — worse — is swallowed and the
//layer is used with its %tokens% intact.
nlohmann::ordered_json SubstituteJsonValues(const nlohmann::ordered_json &V, bool KeepEscapes,
                                            const std::map<std::string, std::string> &Vars);
nlohmann::ordered_json SubstituteJsonValues(const nlohmann::ordered_json &V,
                                            const std::map<std::string, std::string> &Vars);

//Replaces all %KEY% tokens in SourceString with values from VariablesMap. A token MAY carry a use-site render
//format: %KEY:format% looks up KEY, then renders the value via RenderValue(value, format) (see below). A bare
//%KEY% yields the raw value. An unmatched '%' aborts further substitution (the remainder is preserved); unknown
//keys are left as %KEY%[:format] and logged. Returns true if at least one replacement was made.
//"%%" is a literal '%' (spec 08 §8.1): a value the format carries verbatim, such as a registry value NAMED %Value%,
//written "%%Value%%". A variable's value keeps it doubled (KeepEscapes, while variables resolve against each other); a
//final substitution writes one '%', in its own text and in the values it inserts. So an escape is undone exactly once:
//"100%%%%" (meaning 100%%) is not undone again where its variable is inserted.
bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap,
                                bool KeepEscapes = false);
//"%%" → "%": a resolved variable's value as written out. Escape is the reverse: a text (a value read from a game's
//file, a rendered format) as the format would spell it.
std::string Unescape(const std::string &S);
std::string Escape(const std::string &S);
//What %KEY:format% inserts for Value (a variable's value, escaped): the format applied to the value it stands for
//(a :map: table looked up by it; its entries are format text), in escaped form when KeepEscapes, written out otherwise.
std::string RenderInsert(const std::string &Value, const std::string &Format, const std::map<std::string, std::string> &Vars,
                         bool KeepEscapes);

//The KEYs a string's %KEY% / %KEY:format% tokens name, in order — tokens read exactly as StringVariableSubstitution
//reads them (pairwise '%', the format after the first ':'); an unmatched trailing '%' names nothing.
std::vector<std::string> TokenKeys(const std::string &S);

//Evaluates a WHEN condition string to a bool, resolving %KEY% operands from VariablesMap. This is the data-driven
//conditional primitive: a layer with WHEN applies only when it holds (a CustomVar gates its value + UI, any other
//layer gates whether it is applied). Grammar (small boolean expression language):
//  operand    : %KEY% (→ its value)  |  "quoted"  |  bare-word   — a lone operand is TRUE iff non-empty & not 0/false/no
//  comparison : <operand> == <operand>   |   <operand> != <operand>
//  expr       : comparison | operand, combined with  &&  ||  ! and ( ) — precedence  ! > && > ||
//Operands are resolved DURING evaluation (never blind whole-string substitution), so a value containing '&&'/'=='
//cannot inject operators. An empty or unparseable condition returns TRUE (fail-open) and logs a warning.
bool EvaluateCondition(const std::string &Expr, const std::map<std::string, std::string> &VariablesMap);

//Static well-formedness check for a WHEN condition (structure only — values don't affect parseability). A blank
//condition parses. Used by the validator/audit to flag a malformed WHEN that would otherwise silently fail-open.
bool ConditionParses(const std::string &Expr);

//An integer expression, for a declaration with EVAL (a registry bitfield from independent options:
//"%red%*2 | %purple%*32"): decimal or 0x hex numbers, ( ), unary - ~ !, * / %, + -, << >>, &, ^, | (C precedence).
//False on anything else — an operand still a %token% or empty, a division by zero — leaving Out untouched.
bool EvaluateInteger(const std::string &Expr, long long &Out);

//Renders a raw value into a consumer-specific form, requested at the point of use as %KEY:format%. This decouples a
//variable's value from how a particular consumer needs it formatted (one value → many encodings). Formats:
//  dword   : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned) — Wine registry
//  qword   : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)   — Wine registry
//  bool    : "1"/"true"/"yes" → "true"; anything else → "false"               — config text
//  winpath : '/' → '\\'                                                        — wine/guest paths
//  upper / lower : ASCII case fold
//  u8 / u16le / u16be / u32le / u32be : decimal → hex byte-pairs of that width/endianness ("10"→u32le→"0a000000")
//                                       — BinaryPatch Poke VALUE / bytes inside a Replace or Cave payload
//  ascii / asciiz : the value's ASCII bytes as hex, asciiz appends a NUL — write a C string into a binary (Poke)
//  "" or unknown format → the value unchanged
std::string RenderValue(const std::string &Value, const std::string &Format);

//MAP tables (spec 08 §8.3). A declaration with MAP is a table: any exact string → a string, with an optional ELSE.
//It lives in the one variable namespace under names no variable can have (a name holds no ':'): "T:?" = "1" while
//the table is on, "T:=<entry>" = that entry's value, "T:!" = its ELSE. Both resolvers seed these like any declaration
//(the table's WHEN gates them all), so an entry's own %tokens% resolve in the same fixpoint. Empty for a non-table.
std::vector<std::pair<std::string, std::string>> TableEntries(const std::string &Table, const nlohmann::ordered_json &Decl);

//%KEY:map:TABLE% — Value's entry in TABLE, else TABLE's ELSE, else Value unchanged (also when TABLE is off or
//declared nowhere: the validator reports the latter).
std::string MapValue(const std::string &Value, const std::string &Table, const std::map<std::string, std::string> &Vars);

}

#endif // VARSUBST_H
