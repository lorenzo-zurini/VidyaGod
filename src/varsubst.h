#ifndef VARSUBST_H
#define VARSUBST_H

#include <map>
#include <string>

// VarSubst — the pure %TOKEN% substitution + custom-var value encoding seam, lifted out of ContainerWrapper.
// Zero heavy deps (no Qt / IPFS / catalog), so it is unit-testable in isolation (tests/test_varsubst.cpp).
namespace VarSubst
{
//Replaces all %KEY% tokens in SourceString with values from VariablesMap. An unmatched '%' aborts further
//substitution (the remainder is preserved); unknown keys are left as %KEY% and logged. Returns true if at
//least one replacement was made.
bool StringVariableSubstitution(std::string &SourceString, const std::map<std::string, std::string> &VariablesMap);

//Translates a display-layer value into its raw registry storage format based on VARTYPE:
//  dword  : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned)
//  qword  : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)
//  bool   : "1"/"true"/"yes" → "dword:00000001"; anything else → "dword:00000000"
//  string / number / options / unknown → returned unchanged
std::string TranslateCustomVarValue(const std::string &Value, const std::string &VarType);
}

#endif // VARSUBST_H
