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

//Renders a raw value into a consumer-specific form, requested at the point of use as %KEY:format%. This decouples a
//variable's value from how a particular consumer needs it formatted (one value → many encodings). Formats:
//  dword   : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned) — Wine registry
//  qword   : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)   — Wine registry
//  bool    : "1"/"true"/"yes" → "true"; anything else → "false"               — config text
//  winpath : '/' → '\\'                                                        — wine/guest paths
//  upper / lower : ASCII case fold
//  "" or unknown format → the value unchanged
std::string RenderValue(const std::string &Value, const std::string &Format);
}

#endif // VARSUBST_H
