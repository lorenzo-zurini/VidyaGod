#ifndef VARSUBST_H
#define VARSUBST_H

#include <map>
#include <string>

// VarSubst — the pure %TOKEN% substitution + custom-var value encoding seam, lifted out of ContainerWrapper.
// Zero heavy deps (no Qt / IPFS / catalog), so it is unit-testable in isolation (tests/test_varsubst.cpp).
namespace VarSubst
{
//Replaces all %KEY% tokens in SourceString with values from VariablesMap. A token MAY carry a use-site render
//format: %KEY:format% looks up KEY, then renders the value via RenderValue(value, format) (see below). A bare
//%KEY% yields the raw value. An unmatched '%' aborts further substitution (the remainder is preserved); unknown
//keys are left as %KEY%[:format] and logged. Returns true if at least one replacement was made.
bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap);

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

//Renders a raw value into a consumer-specific form, requested at the point of use as %KEY:format%. This decouples a
//variable's value from how a particular consumer needs it formatted (one value → many encodings). Formats:
//  dword   : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned) — Wine registry
//  qword   : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)   — Wine registry
//  bool    : "1"/"true"/"yes" → "true"; anything else → "false"               — config text
//  winpath : '/' → '\\'                                                        — wine/guest paths
//  upper / lower : ASCII case fold
//  u8 / u16le / u16be / u32le / u32be : decimal → hex byte-pairs of that width/endianness ("10"→u32le→"0a000000")
//                                       — BinaryPatch Poke VALUE / bytes inside a Replace or Cave payload
//  "" or unknown format → the value unchanged
std::string RenderValue(const std::string &Value, const std::string &Format);
}

#endif // VARSUBST_H
