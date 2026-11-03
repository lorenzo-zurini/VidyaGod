#include "optionreadback.h"
#include "launchparams.h"   // ContainerParams
#include "persistlayer.h"   // SavedCopy / HasSavedCopy
#include "fileedits.h"      // XmlGet
#include "keynames.h"       // a key option's value must be a key name
#include "varsubst.h"       // FILE's %variables%

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <regex>
#include <iterator>
#include <set>

namespace {

using json = nlohmann::ordered_json;

bool TokenChar(char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_'; }

//The single %NAME[:format]% in Value, with the text before and after it; false when there is none or more than one.
bool OneToken(const std::string &Value, std::string &Name, std::string &Format, std::string &Prefix, std::string &Suffix)
{
    int Found = 0;
    for (size_t I = 0; I < Value.size(); ++I)
    {
        if (Value[I] != '%') continue;
        const size_t E = Value.find('%', I + 1);
        if (E == std::string::npos) break;
        const std::string Inner = Value.substr(I + 1, E - I - 1);
        const size_t Colon = Inner.find(':');
        const std::string N = Inner.substr(0, Colon);
        if (N.empty() || !std::all_of(N.begin(), N.end(), TokenChar)) continue;   // a literal '%', not a token
        if (++Found > 1) return false;
        Name = N;
        Format = Colon == std::string::npos ? std::string() : Inner.substr(Colon + 1);
        Prefix = Value.substr(0, I);
        Suffix = Value.substr(E + 1);
        I = E;
    }
    return Found == 1;
}

//The option's raw value that the format renders as Text; nullopt when the format does not undo.
std::optional<std::string> Undo(const std::string &Text, const std::string &Format, const std::map<std::string, json> &Decls)
{
    if (Format.empty()) return Text;
    if (Format == "bool")
    {
        if (Text == "true") return std::string("1");
        if (Text == "false") return std::string("0");
        return std::nullopt;
    }
    if (Format == "winpath")
    {
        std::string S = Text;
        std::replace(S.begin(), S.end(), '\\', '/');
        return S;
    }
    if (Format.rfind("map:", 0) == 0)
    {
        const auto It = Decls.find(Format.substr(4));
        if (It == Decls.end() || !It->second.contains("MAP") || !It->second["MAP"].is_object()) return std::nullopt;
        const json &Map = It->second["MAP"];
        //A table that can be off (its WHEN) passes values through then; an entry built from %variables% is not what
        //it says. Neither undoes reliably.
        if (It->second.contains("WHEN")) return std::nullopt;
        for (const auto &[K, V] : Map.items()) if (!V.is_string() || V.get<std::string>().find('%') != std::string::npos) return std::nullopt;
        std::vector<std::string> Keys;
        for (const auto &[K, V] : Map.items()) if (V.is_string() && V.get<std::string>() == Text) Keys.push_back(K);
        if (Keys.size() == 1) return Keys.front();
        if (!Keys.empty()) return std::nullopt;                              // two values written the same way
        //Not in the table: the table's ELSE (which many values become), or the value passed through unchanged.
        if (It->second.contains("ELSE")) return std::nullopt;
        if (Map.contains(Text)) return std::nullopt;                          // that value would have been mapped
        return Text;
    }
    return std::nullopt;
}

//Whether the option can hold V: an enum's choice, a key name, a whole number for an int.
bool Holds(const json &Decl, const std::string &V)
{
    const json UI = Decl.value("UI", json::object());
    const std::string Control = UI.value("CONTROL", std::string());
    if (Control == "enum")
    {
        if (!UI.contains("CHOICES") || !UI["CHOICES"].is_array()) return false;
        for (const auto &C : UI["CHOICES"]) if (C.is_object() && C.value("VALUE", std::string()) == V) return true;
        return false;
    }
    if (Control == "key") return KeyNames::IsValue(V);
    if (Control == "int" || Control == "float")
    {
        if (V.empty() || V.size() > 32) return false;
        size_t Used = 0;
        double D = 0;
        try { D = Control == "int" ? static_cast<double>(std::stoll(V, &Used)) : std::stod(V, &Used); }
        catch (const std::exception &) { return false; }
        if (Used != V.size()) return false;
        if (UI.contains("MIN") && UI["MIN"].is_number() && D < UI["MIN"].get<double>()) return false;
        if (UI.contains("MAX") && UI["MAX"].is_number() && D > UI["MAX"].get<double>()) return false;
        return true;
    }
    if (Control == "bool") return V == "0" || V == "1";
    if (UI.contains("PATTERN") && UI["PATTERN"].is_string())
    {
        try { return std::regex_match(V, std::regex(UI["PATTERN"].get<std::string>())); }
        catch (const std::regex_error &) { return false; }
    }
    return true;
}

//The option's value as Read reports it: a bool as 1/0 (its render's own reading), a winpath with '/'.
std::string Canon(const std::string &V, const std::string &Format)
{
    if (Format == "bool")
    {
        std::string L = V;
        std::transform(L.begin(), L.end(), L.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        return L == "1" || L == "true" || L == "yes" ? "1" : "0";
    }
    if (Format == "winpath") { std::string S = V; std::replace(S.begin(), S.end(), '\\', '/'); return S; }
    return V;
}

//A settings file is small; one larger than this is not one (a game, or something it runs, can make a kept file huge).
constexpr std::uintmax_t MaxReadBytes = 16u << 20;

//The file's text, or nullopt: not a plain file of its own (a link, as the sandboxed game can leave in a kept folder, is
//not followed), a link on the way out of Base, or too large.
std::optional<std::string> SafeRead(const std::filesystem::path &Base, const std::filesystem::path &P)
{
    std::error_code Ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(P, Ec))) return std::nullopt;
    if (std::string Why; FileEdits::LinkLeadsOut(Base, P, Why)) return std::nullopt;
    const std::uintmax_t Size = std::filesystem::file_size(P, Ec);
    if (Ec || Size > MaxReadBytes) return std::nullopt;
    std::ifstream In(P, std::ios::binary);
    if (!In) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
}

std::string RuntimeRelative(const ContainerParams &CP, std::string File)
{
    VarSubst::StringVariableSubstitution(File, CP.GetVariablesMap());
    std::replace(File.begin(), File.end(), '\\', '/');
    const std::string Root = CP.RuntimePath.generic_string();
    if (!Root.empty() && File.rfind(Root + "/", 0) == 0) File = File.substr(Root.size() + 1);
    while (!File.empty() && File.front() == '/') File.erase(File.begin());
    return File;
}

//What the file holds where the site writes: the texts it could have been written as (a JSON string either bare or
//quoted), nullopt when the key is not there. False when there is nothing to read (a ConfigWrite line that is not there
//was never written; a file that does not parse).
bool AtSite(const OptionReadBack::Site &S, const std::string &Text, std::optional<std::vector<std::string>> &Out)
{
    if (S.Mode == "ConfigWrite")
    {
        size_t P = 0;
        while (P <= Text.size())
        {
            const size_t E = std::min(Text.find('\n', P), Text.size());
            std::string Line = Text.substr(P, E - P);
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Line.rfind(S.Key, 0) == 0) { Out = std::vector<std::string>{ Line.substr(S.Key.size()) }; return true; }
            P = E + 1;
        }
        return false;
    }
    if (S.Mode == "JsonSet")
    {
        const json Doc = json::parse(Text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        if (Doc.is_discarded()) return false;
        try
        {
            const json::json_pointer Ptr(S.Key);
            if (!Doc.contains(Ptr)) { Out.reset(); return true; }
            const json &V = Doc.at(Ptr);
            Out = V.is_string() ? std::vector<std::string>{ V.get<std::string>(), V.dump() } : std::vector<std::string>{ V.dump() };
            return true;
        }
        catch (const json::exception &) { return false; }
    }
    if (S.Mode == "XmlSet")
    {
        if (Text.find_first_not_of(" \t\r\n") == std::string::npos) return false;   // empty: a save cut short, not a removal
        std::string Why;
        const std::optional<std::string> V = FileEdits::XmlGet(S.Key, Text, Why);
        if (!Why.empty()) return false;
        if (V) Out = std::vector<std::string>{ *V }; else Out.reset();
        return true;
    }
    return false;
}

}

std::vector<OptionReadBack::Site> OptionReadBack::Sites(const ContainerParams &CP)
{
    std::vector<Site> Out;
    for (const auto &Sub : CP.SubComponentsArray)
    {
        if (!Sub.is_object() || Sub.value("TYPE", std::string()) != "FileEdit" || !Sub.value("OVERRIDE", false)) continue;
        Site S;
        S.Mode = Sub.value("MODE", std::string());
        if (S.Mode != "ConfigWrite" && S.Mode != "JsonSet" && S.Mode != "XmlSet") continue;
        //The VALUE as the package wrote it (the resolve substitutes VALUE; it keeps the original as VALUE_TEMPLATE).
        const std::string Template = Sub.value("VALUE_TEMPLATE", Sub.value("VALUE", std::string()));
        if (!OneToken(Template, S.Option, S.Format, S.Prefix, S.Suffix)) continue;
        const auto D = CP.VarDecls.find(S.Option);
        if (D == CP.VarDecls.end() || !D->second.is_object()) continue;
        const json &Decl = D->second;
        //A setting the user sees: not a hidden variable, not a secret, not a computed value, not a table.
        if (!Decl.contains("UI") || !Decl["UI"].is_object() || Decl["UI"].value("CONTROL", std::string()) == "secret"
            || Decl.value("EVAL", false) || Decl.contains("MAP")) continue;
        if (!S.Format.empty() && S.Format != "bool" && S.Format != "winpath" && S.Format.rfind("map:", 0) != 0) continue;
        S.Key = Sub.value("KEY", std::string());
        S.File = RuntimeRelative(CP, Sub.value("FILE", std::string()));
        S.IfUnsaved = Sub.value("IF_UNSAVED", false);
        if (!S.File.empty()) Out.push_back(std::move(S));
    }
    return Out;
}

namespace {

//What one site's file holds, as the option holds it: nullopt for nothing to read (and for a removed value).
std::optional<std::string> ValueAt(const ContainerParams &CP, const OptionReadBack::Site &S, const std::string &Text)
{
    std::optional<std::vector<std::string>> Texts;
    if (!AtSite(S, Text, Texts) || !Texts) return std::nullopt;
    for (const std::string &T : *Texts)
    {
        if (T.size() < S.Prefix.size() + S.Suffix.size() || T.compare(0, S.Prefix.size(), S.Prefix) != 0
            || T.compare(T.size() - S.Suffix.size(), S.Suffix.size(), S.Suffix) != 0) continue;
        const std::optional<std::string> V = Undo(T.substr(S.Prefix.size(), T.size() - S.Prefix.size() - S.Suffix.size()), S.Format, CP.VarDecls);
        if (V && Holds(CP.VarDecls.at(S.Option), *V)) return V;
    }
    return std::nullopt;                                                     // written some other way: not ours to read
}

}

std::map<std::string, OptionReadBack::Reading> OptionReadBack::Read(const ContainerParams &CP)
{
    std::map<std::string, Reading> Out;
    std::map<std::string, bool> AnyWritten;                                  // a site the launcher writes each launch
    std::set<std::string> Disagree;
    for (const Site &S : Sites(CP))
    {
        AnyWritten[S.Option] = AnyWritten[S.Option] || !S.IfUnsaved;
        const std::optional<std::filesystem::path> Saved = PersistLayer::SavedCopy(CP, S.File);
        if (!Saved) continue;
        const std::optional<std::string> Text = SafeRead(CP.UserDataPath, *Saved);
        if (!Text) continue;
        const std::optional<std::string> V = ValueAt(CP, S, *Text);
        if (!V) continue;
        const auto It = Out.find(S.Option);
        if (It == Out.end()) Out[S.Option] = Reading{ *V, false };
        else if (It->second.Value != *V) Disagree.insert(S.Option);
    }
    for (const std::string &K : Disagree) Out.erase(K);
    for (auto &[K, R] : Out) R.DefaultOnly = !AnyWritten[K];
    return Out;
}

std::map<std::string, std::string> OptionReadBack::Current(const ContainerParams &CP)
{
    std::map<std::string, std::string> Out;
    for (const Site &S : Sites(CP))
        if (const auto V = CP.CustomVariables.find(S.Option); V != CP.CustomVariables.end())
            Out[S.Option] = Canon(V->second, S.Format);
    return Out;
}

std::vector<std::string> OptionReadBack::Adopt(nlohmann::ordered_json &InstanceConfig, const std::map<std::string, Reading> &Found,
                                               const std::map<std::string, std::string> &Now, bool &Touched)
{
    std::vector<std::string> Changed;
    Touched = false;
    if (Found.empty()) return Changed;
    if (!InstanceConfig.is_object()) InstanceConfig = json::object();
    //Both made first: an ordered_json object is a vector, so adding the second key would move the first under a
    //reference already taken to it.
    for (const char *K : {"VARIABLES", "LAST_IN_FILE"})
        if (!InstanceConfig.contains(K) || !InstanceConfig[K].is_object()) InstanceConfig[K] = json::object();
    json &Vars = InstanceConfig["VARIABLES"];
    json &Last = InstanceConfig["LAST_IN_FILE"];
    for (const auto &[K, R] : Found)
    {
        const auto N = Now.find(K);
        const bool Differs = N == Now.end() || N->second != R.Value;
        const bool Known = Last.contains(K) && Last[K].is_string();
        bool Take = false;
        if (R.DefaultOnly) Take = Differs;                                    // the launcher never writes it: the file decides
        else if (!Known) Take = Differs;                                      // first seen: the file is what the game uses
        else if (Last[K].get<std::string>() == R.Value) continue;             // as last known: nothing changed in the game
        else Take = N != Now.end() && N->second == Last[K].get<std::string>(); // changed in the game; the launcher's
                                                                              // own change since wins
        if (!Known || Last[K].get<std::string>() != R.Value)
            if (Take || R.DefaultOnly || !Known) { Last[K] = R.Value; Touched = true; }
        if (Take && (!Vars.contains(K) || Vars[K] != R.Value)) { Vars[K] = R.Value; Changed.push_back(K); Touched = true; }
    }
    return Changed;
}

std::map<std::string, std::string> OptionReadBack::Written(const ContainerParams &CP)
{
    std::map<std::string, std::string> Out;
    std::set<std::string> Disagree;
    const std::map<std::string, std::string> Now = Current(CP);
    for (const Site &S : Sites(CP))
    {
        if (S.IfUnsaved && PersistLayer::HasSavedCopy(CP, S.File)) continue;  // not written: the user's copy stood
        const auto N = Now.find(S.Option);
        if (N == Now.end()) continue;
        const std::optional<std::string> Text = SafeRead(CP.RuntimePath, CP.RuntimePath / S.File);
        if (!Text || ValueAt(CP, S, *Text) != N->second) continue;          // the edit did not land as written
        const auto It = Out.find(S.Option);
        if (It == Out.end()) Out[S.Option] = N->second;
        else if (It->second != N->second) Disagree.insert(S.Option);
    }
    for (const std::string &K : Disagree) Out.erase(K);
    return Out;
}

bool OptionReadBack::Commit(nlohmann::ordered_json &InstanceConfig, const std::map<std::string, std::string> &Written)
{
    if (Written.empty()) return false;
    if (!InstanceConfig.is_object()) InstanceConfig = json::object();
    if (!InstanceConfig.contains("LAST_IN_FILE") || !InstanceConfig["LAST_IN_FILE"].is_object()) InstanceConfig["LAST_IN_FILE"] = json::object();
    json &Last = InstanceConfig["LAST_IN_FILE"];
    bool Touched = false;
    for (const auto &[K, V] : Written)
        if (!Last.contains(K) || Last[K] != V) { Last[K] = V; Touched = true; }
    return Touched;
}
