#include "varsubst.h"
#include "commonutils.h"   // Log*

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

//Renders a raw value into a consumer-specific form, requested at the point of use as %KEY:format%.
//See the header for the format table. "" / unknown format returns the value unchanged.
std::string VarSubst::RenderValue(const std::string &Value, const std::string &Format)
{
    if (Format.empty()) return Value;

    if (Format == "dword")
    {
        try {
            uint32_t n = static_cast<uint32_t>(std::stoul(Value));
            std::ostringstream oss;
            oss << "dword:" << std::hex << std::setw(8) << std::setfill('0') << n;
            return oss.str();
        } catch (...) {
            LogWarn("RenderValue", "Could not parse dword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    if (Format == "qword")
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
            LogWarn("RenderValue", "Could not parse qword value: '" + Value + "', leaving unchanged.");
            return Value;
        }
    }
    if (Format == "bool")
    {
        std::string lower = Value;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return (lower == "1" || lower == "true" || lower == "yes") ? "true" : "false";
    }
    if (Format == "winpath")
    {
        std::string Out = Value;
        std::replace(Out.begin(), Out.end(), '/', '\\');
        return Out;
    }
    if (Format == "upper") { std::string O = Value; std::transform(O.begin(), O.end(), O.begin(), ::toupper); return O; }
    if (Format == "lower") { std::string O = Value; std::transform(O.begin(), O.end(), O.begin(), ::tolower); return O; }

    LogWarn("RenderValue", "Unknown render format ':" + Format + "' — leaving value unchanged.");
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
            std::string Msg = "Unmatched '%' at position ";
            Msg += std::to_string(start);
            Msg += " in \"";
            Msg += SourceString;
            Msg += "\" — everything from there on is left unsubstituted, so any later %TOKEN% also reaches the "
                   "consumer raw.";
            LogWarn("StringVariableSubstitution", Msg);
            result += SourceString.substr(pos);
            break;
        }

        // Append text before variable
        result += SourceString.substr(pos, start - pos);

        const std::string token = SourceString.substr(start + 1, end - start - 1);

        //A token may carry a use-site render format: %KEY:format%. Split on the first ':'.
        std::string key = token, format;
        if (const auto colon = token.find(':'); colon != std::string::npos)
        { key = token.substr(0, colon); format = token.substr(colon + 1); }

        LogOut("StringVariableSubstitution", "Found variable: %" + token + "%");

        auto it = VariablesMap.find(key);
        if (it != VariablesMap.end())
        {
            const std::string rendered = format.empty() ? it->second : RenderValue(it->second, format);
            LogOut("StringVariableSubstitution", "Replacing with: \"" + rendered + "\"");
            result += rendered;
            replaced = true;
        }
        else
        {
            //Leave the whole token in place so the caller can diagnose the missing variable.
            //NAME the token and quote the string: the launch-verdict tally dedupes by context+message, so a
            //message that said only "Variable not found in map" collapsed EVERY unresolved variable in a launch
            //into one indistinguishable line — you learned that something broke but never what or where.
            std::string Msg = "Undefined variable %";
            Msg += token;
            Msg += "% in \"";
            Msg += SourceString;
            Msg += "\" — left unsubstituted (the literal token reaches the consumer: a path, an argument or a "
                   "config value).";
            LogWarn("StringVariableSubstitution", Msg);
            result += "%" + token + "%";
        }

        pos = end + 1;
    }

    LogOut("StringVariableSubstitution", "Final string: \"" + result + "\"");
    LogOut("StringVariableSubstitution", std::string("Substitution performed: ") + (replaced ? "YES" : "NO"));

    SourceString = std::move(result);
    return replaced;
}
