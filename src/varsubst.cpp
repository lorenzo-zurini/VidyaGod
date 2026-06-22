#include "varsubst.h"
#include "commonutils.h"   // Log*

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

//Replaces all %KEY% tokens in SourceString with values from VariablesMap.
//Scans left-to-right looking for paired '%' delimiters; an unmatched '%' stops
//Translates a display-layer value into its raw storage format based on VARTYPE.
//Called after Layer-1 substitution so the value is already a concrete string.
//  dword  : decimal integer → "dword:XXXXXXXX" (8-digit hex, 32-bit unsigned)
//  qword  : decimal integer → "hex(b):XX,XX,...,XX" (8 bytes little-endian)
//  bool   : "1"/"true"/"yes" → "dword:00000001"; anything else → "dword:00000000"
//  string / number / options / unknown → returned unchanged
std::string VarSubst::TranslateCustomVarValue(const std::string &Value, const std::string &VarType)
{
    if (VarType == "dword")
    {
        try {
            uint32_t n = static_cast<uint32_t>(std::stoul(Value));
            std::ostringstream oss;
            oss << "dword:" << std::hex << std::setw(8) << std::setfill('0') << n;
            return oss.str();
        } catch (...) {
            LogWarn("TranslateCustomVarValue", "Could not parse dword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    else if (VarType == "qword")
    {
        try {
            uint64_t n = std::stoull(Value);
            std::ostringstream oss;
            oss << "hex(b):";
            for (int i = 0; i < 8; i++) {
                if (i > 0) oss << ",";
                oss << std::hex << std::setw(2) << std::setfill('0') << ((n >> (8 * i)) & 0xFF);
            }
            return oss.str();
        } catch (...) {
            LogWarn("TranslateCustomVarValue", "Could not parse qword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    else if (VarType == "bool")
    {
        std::string lower = Value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        bool isTrue = (lower == "1" || lower == "true" || lower == "yes");
        return isTrue ? "dword:00000001" : "dword:00000000";
    }
    // string, number, options — no translation
    return Value;
}

//further substitution and logs a warning (the remainder is appended unchanged).
//Unknown keys are left as %KEY% and logged as warnings.
//Returns true if at least one replacement was made.
bool VarSubst::StringVariableSubstitution(
    std::string &SourceString,
    const std::map<std::string, std::string>& VariablesMap)
{
    LogOut("StringVariableSubstitution", "Starting substitution.");
    LogOut("StringVariableSubstitution", "Original string: \"" + SourceString + "\"");

    std::string result;
    bool replaced = false;
    size_t pos = 0;

    while (pos < SourceString.size())
    {
        size_t start = SourceString.find('%', pos);

        //No more '%' characters — append the rest of the string unchanged.
        if (start == std::string::npos)
        {
            result += SourceString.substr(pos);
            break;
        }

        size_t end = SourceString.find('%', start + 1);

        //Unmatched opening '%' — stop substitution, preserve remainder.
        if (end == std::string::npos)
        {
            LogWarn("StringVariableSubstitution", "Unmatched '%' at position " + std::to_string(start) + ". Aborting further substitution.");
            result += SourceString.substr(pos);
            break;
        }

        // Append text before variable
        result += SourceString.substr(pos, start - pos);

        std::string key = SourceString.substr(start + 1, end - start - 1);

        LogOut("StringVariableSubstitution", "Found variable: %" + key + "%");

        auto it = VariablesMap.find(key);
        if (it != VariablesMap.end())
        {
            LogOut("StringVariableSubstitution", "Replacing with: \"" + it->second + "\"");
            result += it->second;
            replaced = true;
        }
        else
        {
            //Leave the token in place so the caller can diagnose the missing variable.
            LogWarn("StringVariableSubstitution", "Variable not found in map. Leaving unchanged.");
            result += "%" + key + "%";
        }

        pos = end + 1;
    }

    LogOut("StringVariableSubstitution", "Final string: \"" + result + "\"");
    LogOut("StringVariableSubstitution", std::string("Substitution performed: ") + (replaced ? "YES" : "NO"));

    SourceString = std::move(result);
    return replaced;
}
