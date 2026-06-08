#include "jsonoperations.h"
#include "commonutils.h"
#include <unordered_set>
#include <unordered_map>

JSONOps::JSONOps() {}

//Returns the ID field for an array given its key (and merge context). Arrays of objects carrying
//this field are merged by ID; everything else is concatenated. Runner platform arrays (under
//RUNNERS) are keyed by NAME, detected via the context path.
static std::string ManifestArrayIdField(const std::string &ArrayKey, const std::string &Ctx)
{
    if (ArrayKey == "GAMES")      return "GAMEID";
    if (ArrayKey == "COMPONENTS") return "COMPONENTID";
    if (ArrayKey == "VARIANTS")   return "VARIANT_ID";
    if (ArrayKey == "RUNNERS")    return "RUNNER_ID"; // flat top-level runner array
    //CustomVars are SUBCOMPONENTS now (TYPE:"CustomVar"); SUBCOMPONENTS concatenate, no id-merge.
    (void)Ctx;
    return "";
}

//Parses the file at JSONFile into JSONDocument.
//Uses nlohmann::ordered_json::accept() for a cheap validity pre-check before the full
//parse, which avoids throwing exceptions on malformed input.
//Returns 0 on success, 1 on failure — intentionally matching shell exit-code convention
//so callers can test with `if (LoadJSON(...))` to detect failure.
bool JSONOps::LoadJSON(QFile * JSONFile, nlohmann::ordered_json * JSONDocument)
{
    LogOut("JSONOperations", "Parsing JSON " + JSONFile->fileName().toStdString());

    if (!JSONFile->exists())
    {
        LogErr("JSONOperations", "File " + JSONFile->fileName().toStdString() + " does not exist.");
        return 1; //fail
    }
    if (JSONFile->open(QFile::ReadOnly))
    {
        LogSucc("JSONOperations", "File " + JSONFile->fileName().toStdString() + " opened for reading successfully!");
        QByteArray JSONFileData = JSONFile->readAll();

        //Pre-validate before parsing to avoid exceptions on corrupt/empty files.
        if (nlohmann::ordered_json::accept(JSONFileData))
        {
            LogOut("JSONOperations", "File " + JSONFile->fileName().toStdString() + " appears valid, parsing.");
            (*JSONDocument) = nlohmann::ordered_json::parse(JSONFileData);
            //std::cout << JSONDocument->dump(4) << std::endl;
            JSONFile->close();
            LogOut("JSONOperations", "Parse done!");
            return 0; //success
        }
        else
        {
            LogErr("JSONOperations", "Invalid JSON!");
            return 1; //fail
        }
    }
    else
    {
        LogErr("JSONOperations", "Could not open file for reading!");
        return 1; //fail
    }
}

//Writes JSONDocument to JSONFile with 4-space pretty-printing.
//Truncates the file if it already exists so stale content is never left behind.
//Returns true on success, false if the file cannot be opened.
bool JSONOps::SaveJSON(nlohmann::ordered_json * JSONDocument, QFile * JSONFile)
{
    if (JSONFile->open(QFile::WriteOnly))
    {
        LogOut("JSONOperations", "File opened for writing successfully!");
        LogOut("JSONOperations", "Saved " + JSONFile->fileName().toStdString());
        QTextStream OutFileStream(JSONFile);
        OutFileStream << QString::fromStdString((*JSONDocument).dump(4));
        JSONFile->close();
        return true;
    }
    else
    {
        return false;
    }
}

//Merges two arrays. If they look like an ID-keyed collection (every element is an object carrying
//the ID field for this key/context), merges element-by-element by ID; otherwise concatenates.
nlohmann::ordered_json JSONOps::MergeArrays(const nlohmann::ordered_json &Base, const nlohmann::ordered_json &Overlay, const std::string &ArrayKey, const std::string &Ctx, std::vector<std::string> &Warnings)
{
    const std::string IdField = ManifestArrayIdField(ArrayKey, Ctx);

    auto HasId = [&](const nlohmann::ordered_json &Arr) {
        if (IdField.empty()) return false;
        for (const auto &E : Arr)
            if (!E.is_object() || !E.contains(IdField) || !E[IdField].is_string()) return false;
        return true;
    };

    // Plain arrays (no usable ID): concatenate.
    if (!HasId(Base) || !HasId(Overlay))
    {
        nlohmann::ordered_json Result = Base;
        for (const auto &E : Overlay) Result.push_back(E);
        return Result;
    }

    // ID-keyed: merge same-ID elements, append new ones.
    nlohmann::ordered_json Result = Base;
    std::unordered_map<std::string, int> Index;
    for (int i = 0; i < (int)Result.size(); i++)
        Index[std::string(Result[i][IdField])] = i;

    for (const auto &E : Overlay)
    {
        std::string Id = std::string(E[IdField]);
        auto It = Index.find(Id);
        if (It != Index.end())
            MergeManifestInto(Result[It->second], E, Ctx + "[" + Id + "]", Warnings);
        else
        {
            Result.push_back(E);
            Index[Id] = (int)Result.size() - 1;
        }
    }
    return Result;
}

//Recursively merges Overlay into Base (see header).
void JSONOps::MergeManifestInto(nlohmann::ordered_json &Base, const nlohmann::ordered_json &Overlay, const std::string &Ctx, std::vector<std::string> &Warnings)
{
    if (!Overlay.is_object() || !Base.is_object())
    {
        // Non-object root (shouldn't happen for manifests) — overlay wins.
        Base = Overlay;
        return;
    }
    for (auto It = Overlay.begin(); It != Overlay.end(); ++It)
    {
        const std::string Key = It.key();
        const std::string ChildCtx = Ctx.empty() ? Key : (Ctx + "." + Key);
        const nlohmann::ordered_json &OVal = It.value();

        if (!Base.contains(Key))
        {
            Base[Key] = OVal;
            continue;
        }
        nlohmann::ordered_json &BVal = Base[Key];
        if (BVal.is_object() && OVal.is_object())
            MergeManifestInto(BVal, OVal, ChildCtx, Warnings);
        else if (BVal.is_array() && OVal.is_array())
            Base[Key] = MergeArrays(BVal, OVal, Key, ChildCtx, Warnings);
        else if (BVal == OVal)
            continue; // identical scalar — fine
        else if (Key.rfind("__", 0) == 0)
            continue; // editor provenance tag (e.g. __FILE__) — keep first silently
        else if (Key == "PACKAGEUID" || Key == "PACKAGENAME" || Key == "PACKAGEVERSION")
            continue; // identity conflicts are reported as hard errors by AssembleManifest
        else
            Warnings.push_back("Conflict at '" + ChildCtx + "': keeping first value (" + BVal.dump() + "), ignoring (" + OVal.dump() + ")");
    }
}

//The top-level scalar fields that define a package's identity. A package directory must declare
//exactly one consistent value for each — conflicting values across fragments are a hard error.
static const char *const kIdentityFields[] = { "PACKAGEUID", "PACKAGENAME", "PACKAGEVERSION" };

//Assembles every *.json in PackageDir into a single manifest (see header).
bool JSONOps::AssembleManifest(const QString &PackageDir, nlohmann::ordered_json &Out, std::vector<std::string> &Warnings)
{
    Out = nlohmann::ordered_json::object();
    QDir Dir(PackageDir);
    QStringList Files = Dir.entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    bool Any = false;

    //Track the first declaration (value + file) of each identity field to detect conflicts.
    std::unordered_map<std::string, std::pair<std::string, std::string>> SeenIdentity; // field -> (value, file)
    std::vector<std::string> IdentityErrors;

    for (const QString &FileName : Files)
    {
        nlohmann::ordered_json Frag;
        QFile F(Dir.filePath(FileName));
        if (JSONOps::LoadJSON(&F, &Frag))
        {
            Warnings.push_back(FileName.toStdString() + ": unreadable or invalid JSON, skipped");
            continue;
        }
        if (!Frag.is_object())
        {
            Warnings.push_back(FileName.toStdString() + ": top-level JSON is not an object, skipped");
            continue;
        }

        //Identity conflict check: a different non-empty value for an already-declared field is an error.
        for (const char *Field : kIdentityFields)
        {
            if (!Frag.contains(Field) || !Frag[Field].is_string()) continue;
            std::string Val = std::string(Frag[Field]);
            if (Val.empty()) continue;
            auto It = SeenIdentity.find(Field);
            if (It == SeenIdentity.end())
                SeenIdentity[Field] = { Val, FileName.toStdString() };
            else if (It->second.first != Val)
                IdentityErrors.push_back(std::string("Conflicting ") + Field + ": '" + It->second.first + "' (" +
                                         It->second.second + ") vs '" + Val + "' (" + FileName.toStdString() +
                                         ") — a package directory must hold exactly one identity");
        }

        // Use the filename as the context root so conflict warnings name the offending file.
        MergeManifestInto(Out, Frag, FileName.toStdString(), Warnings);
        Any = true;
    }

    //Carry identity errors forward in the assembled doc so ValidateManifest can surface them.
    if (!IdentityErrors.empty())
        Out["__VG_ERRORS__"] = IdentityErrors;

    return Any;
}

//Validates an assembled manifest (see header).
void JSONOps::ValidateManifest(const nlohmann::ordered_json &Assembled, std::vector<std::string> &Errors, std::vector<std::string> &Warnings)
{
    // Assembly-time errors carried forward in the doc (e.g. conflicting identity across fragments).
    if (Assembled.contains("__VG_ERRORS__") && Assembled["__VG_ERRORS__"].is_array())
        for (const auto &E : Assembled["__VG_ERRORS__"])
            if (E.is_string()) Errors.push_back(std::string(E));

    // Identity.
    for (const char *Key : {"PACKAGEUID", "PACKAGENAME", "PACKAGEVERSION"})
    {
        if (!Assembled.contains(Key) || Assembled[Key].is_null() ||
            (Assembled[Key].is_string() && std::string(Assembled[Key]).empty()))
            Errors.push_back(std::string("Missing identity field: ") + Key);
    }

    // HOST_PLATFORM — the platform this package runs as (matched against runner GUEST_PLATFORM).
    // Advisory: a pure-dependency package (no GAMES/RUNNERS) legitimately omits it.
    if (!Assembled.contains("HOST_PLATFORM") ||
        !Assembled["HOST_PLATFORM"].is_string() || std::string(Assembled["HOST_PLATFORM"]).empty())
        Warnings.push_back("No HOST_PLATFORM declared (needed to resolve a runner for a game)");

    // Component id set + parent map. Also flag duplicate CustomVar KEYs within one component.
    std::unordered_set<std::string> ComponentIds;
    std::unordered_map<std::string, std::string> ParentOf;
    if (Assembled.contains("COMPONENTS") && Assembled["COMPONENTS"].is_array())
        for (const auto &C : Assembled["COMPONENTS"])
        {
            std::string Id = C.value("COMPONENTID", std::string());
            if (Id.empty()) { Warnings.push_back("A component has an empty COMPONENTID"); continue; }
            ComponentIds.insert(Id);
            std::string Parent;
            if (C.contains("PARENTCOMPONENT") && C["PARENTCOMPONENT"].is_string())
                Parent = std::string(C["PARENTCOMPONENT"]);
            ParentOf[Id] = Parent;

            // CustomVar subcomponents: KEYs must be unique within the component (they namespace as
            // %COMPONENTID.KEY%).
            std::unordered_set<std::string> VarKeys;
            if (C.contains("SUBCOMPONENTS") && C["SUBCOMPONENTS"].is_array())
                for (const auto &S : C["SUBCOMPONENTS"])
                    if (S.is_object() && S.value("TYPE", std::string()) == "CustomVar")
                    {
                        std::string K = S.value("KEY", std::string());
                        if (K.empty()) Warnings.push_back("Component '" + Id + "' has a CustomVar with no KEY");
                        else if (!VarKeys.insert(K).second)
                            Warnings.push_back("Component '" + Id + "' has duplicate CustomVar KEY '" + K + "'");
                    }
        }

    // Dangling parents.
    for (const auto &[Id, Parent] : ParentOf)
        if (!Parent.empty() && !ComponentIds.count(Parent))
            Errors.push_back("Component '" + Id + "' has unknown PARENTCOMPONENT '" + Parent + "'");

    // Parent cycles.
    for (const std::string &Start : ComponentIds)
    {
        std::unordered_set<std::string> Seen;
        std::string Cur = Start;
        while (!Cur.empty() && ComponentIds.count(Cur))
        {
            if (Seen.count(Cur)) { Errors.push_back("PARENTCOMPONENT cycle involving '" + Start + "'"); break; }
            Seen.insert(Cur);
            auto It = ParentOf.find(Cur);
            Cur = (It == ParentOf.end()) ? std::string() : It->second;
        }
    }

    // Game / variant / endpoint checks.
    if (Assembled.contains("GAMES") && Assembled["GAMES"].is_array())
        for (const auto &G : Assembled["GAMES"])
        {
            std::string GID = G.value("GAMEID", std::string("?"));
            const auto Variants = (G.contains("VARIANTS") && G["VARIANTS"].is_array()) ? G["VARIANTS"] : nlohmann::ordered_json::array();
            if (Variants.empty()) Warnings.push_back("Game '" + GID + "' has no variants");
            int RecCount = 0;
            for (const auto &V : Variants)
            {
                std::string VID = V.value("VARIANT_ID", std::string("?"));
                if (V.value("RECOMMENDED", false)) RecCount++;
                const auto Endpoints = (V.contains("ENDPOINTS") && V["ENDPOINTS"].is_array()) ? V["ENDPOINTS"] : nlohmann::ordered_json::array();
                if (Endpoints.empty()) Warnings.push_back("Variant '" + VID + "' in game '" + GID + "' has no endpoints");
                for (const auto &E : Endpoints)
                    if (E.is_string() && !ComponentIds.count(std::string(E)))
                        Errors.push_back("Variant '" + VID + "' (game '" + GID + "') endpoint '" + std::string(E) + "' is not a known component");
            }
            if (RecCount > 1) Warnings.push_back("Game '" + GID + "' has " + std::to_string(RecCount) + " RECOMMENDED variants");
        }

    // Runner checks (RUNNERS array). Each runner needs a GUEST_PLATFORM. A runner's ENDPOINTS often
    // reference its OWN package's components — but a registry runner is validated in isolation, so an
    // endpoint not present in this doc is only a warning (it may resolve in the runner's own package).
    if (Assembled.contains("RUNNERS") && Assembled["RUNNERS"].is_array())
        for (const auto &R : Assembled["RUNNERS"])
        {
            std::string RID = R.value("RUNNER_ID", std::string());
            if (RID.empty()) Warnings.push_back("A runner has no RUNNER_ID");
            if (!R.contains("GUEST_PLATFORM") || !R["GUEST_PLATFORM"].is_array() || R["GUEST_PLATFORM"].empty())
                Warnings.push_back("Runner '" + (RID.empty() ? std::string("?") : RID) + "' has no GUEST_PLATFORM");
            const auto Endpoints = (R.contains("ENDPOINTS") && R["ENDPOINTS"].is_array()) ? R["ENDPOINTS"] : nlohmann::ordered_json::array();
            for (const auto &E : Endpoints)
                if (E.is_string() && !ComponentIds.count(std::string(E)))
                    Warnings.push_back("Runner '" + (RID.empty() ? std::string("?") : RID) + "' endpoint '" + std::string(E) + "' is not a component in this package (may resolve in the runner's own package)");
        }
}

//Archetype tests — a package shows in the library if it has games, and is offered to the resolver
//if it has runners. Both/neither give the bundle / pure-dependency archetypes.
bool JSONOps::HasGames(const nlohmann::ordered_json &Doc)
{
    return Doc.contains("GAMES") && Doc["GAMES"].is_array() && !Doc["GAMES"].empty();
}
bool JSONOps::HasRunners(const nlohmann::ordered_json &Doc)
{
    return Doc.contains("RUNNERS") && Doc["RUNNERS"].is_array() && !Doc["RUNNERS"].empty();
}
