#include "jsonoperations.h"
#include "commonutils.h"
#include <set>
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
        else if (Key == "PACKAGEUID" || Key == "PACKAGENAME")
            continue; // identity conflicts are reported as hard errors by AssembleManifest
        else
            Warnings.push_back("Conflict at '" + ChildCtx + "': keeping first value (" + BVal.dump() + "), ignoring (" + OVal.dump() + ")");
    }
}

//The top-level scalar fields that define a package's identity. A package directory must declare
//exactly one consistent value for each — conflicting values across fragments are a hard error.
static const char *const kIdentityFields[] = { "PACKAGEUID", "PACKAGENAME" };

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
    // Null-safe string read: returns the string value, or Def if the key is absent OR not a string.
    // (nlohmann's .value() THROWS type_error.302 on a present-but-NULL field — which a half-finished
    // package routinely has, e.g. an unnamed component with "COMPONENTID": null. Validation must never
    // crash on malformed input.)
    auto Str = [](const nlohmann::ordered_json &J, const char *Key, const std::string &Def = std::string()) -> std::string {
        auto It = J.find(Key);
        return (It != J.end() && It->is_string()) ? It->get<std::string>() : Def;
    };

    // Assembly-time errors carried forward in the doc (e.g. conflicting identity across fragments).
    if (Assembled.contains("__VG_ERRORS__") && Assembled["__VG_ERRORS__"].is_array())
        for (const auto &E : Assembled["__VG_ERRORS__"])
            if (E.is_string()) Errors.push_back(std::string(E));

    // Identity.
    for (const char *Key : {"PACKAGEUID", "PACKAGENAME"})
    {
        if (!Assembled.contains(Key) || Assembled[Key].is_null() ||
            (Assembled[Key].is_string() && std::string(Assembled[Key]).empty()))
            Errors.push_back(std::string("Missing identity field: ") + Key);
    }

    // HOST_PLATFORM is now per-variant (the platform each game/runner variant targets), validated in the
    // GAMES/RUNNERS loops below. There is no package-level HOST_PLATFORM anymore.

    // Component id set + parent map. Also flag duplicate CustomVar KEYs within one component.
    std::unordered_set<std::string> ComponentIds;
    std::unordered_map<std::string, std::string> ParentOf;
    if (Assembled.contains("COMPONENTS") && Assembled["COMPONENTS"].is_array())
        for (const auto &C : Assembled["COMPONENTS"])
        {
            std::string Id = Str(C, "COMPONENTID");
            if (Id.empty()) { Warnings.push_back("A component has an empty COMPONENTID"); continue; }
            ComponentIds.insert(Id);
            std::string Parent;
            if (C.contains("PARENTCOMPONENT") && C["PARENTCOMPONENT"].is_string())
                Parent = std::string(C["PARENTCOMPONENT"]);
            ParentOf[Id] = Parent;

            // CustomVar subcomponents: KEYs must be unique within the component. They resolve into a
            // bare global %KEY% namespace; the same KEY across components is one shared knob (last in
            // recipe order wins), so cross-component duplicates are intentional, not an error.
            std::unordered_set<std::string> VarKeys;
            if (C.contains("SUBCOMPONENTS") && C["SUBCOMPONENTS"].is_array())
                for (const auto &S : C["SUBCOMPONENTS"])
                {
                    if (!S.is_object()) continue;
                    std::string T = Str(S, "TYPE");
                    // Every content layer MUST declare a local PATH (where its content lives / hydrates to).
                    // A path-less layer (e.g. a CID-only runner build) is a hard error.
                    if ((T == "VFSZipLayer" || T == "VFSDirLayer" || T == "VFSFileLayer") && Str(S, "PATH").empty())
                        Errors.push_back("Component '" + Id + "' has a " + T + " with no PATH — content must declare a local path.");
                    if (T == "CustomVar")
                    {
                        std::string K = Str(S, "KEY");
                        if (K.empty()) Warnings.push_back("Component '" + Id + "' has a CustomVar with no KEY");
                        else if (!VarKeys.insert(K).second)
                            Warnings.push_back("Component '" + Id + "' has duplicate CustomVar KEY '" + K + "'");
                    }
                    // PersistDir/PersistFile need a runtime-root-relative PATH; RegPersist takes no fields.
                    else if ((T == "PersistDir" || T == "PersistFile") && Str(S, "PATH").empty())
                        Warnings.push_back("Component '" + Id + "' has a " + T + " with an empty PATH");
                    // RegKeyPersist needs a registry key REGPATH (HKLM\.. / HKCU\..).
                    else if (T == "RegKeyPersist" && Str(S, "REGPATH").empty())
                        Warnings.push_back("Component '" + Id + "' has a RegKeyPersist with an empty REGPATH");
                    // VFSZipLayer/VFSDirLayer SUBMOUNTS: each entry is "src:dst"; flag malformed entries and
                    // duplicate destinations within the layer (the FS skips malformed and later-wins on dup).
                    else if ((T == "VFSZipLayer" || T == "VFSDirLayer") && S.contains("SUBMOUNTS") && S["SUBMOUNTS"].is_array())
                    {
                        std::unordered_set<std::string> Dests;
                        for (const auto &E : S["SUBMOUNTS"])
                        {
                            if (!E.is_string())
                            { Warnings.push_back("Component '" + Id + "' has a non-string SUBMOUNTS entry"); continue; }
                            std::string M = E.get<std::string>();
                            size_t Colon = M.find(':');
                            if (Colon == std::string::npos)
                            { Warnings.push_back("Component '" + Id + "' SUBMOUNTS entry '" + M + "' is malformed (expected 'src:dst')"); continue; }
                            std::string Dst = M.substr(Colon + 1);
                            if (!Dests.insert(Dst).second)
                                Warnings.push_back("Component '" + Id + "' has duplicate SUBMOUNTS destination '" + Dst + "'");
                        }
                    }
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

    // Helper: a MODULES array's COMPONENT id at index — checks each entry is an object with COMPONENT.
    auto ModuleComponents = [](const nlohmann::ordered_json &Owner) {
        std::vector<std::string> Comps;
        if (Owner.contains("MODULES") && Owner["MODULES"].is_array())
            for (const auto &M : Owner["MODULES"])
                if (M.is_object() && M.contains("COMPONENT") && M["COMPONENT"].is_string())
                    Comps.push_back(std::string(M["COMPONENT"]));
        return Comps;
    };

    // Helper: mutually-exclusive (EXCLUDE) pairs in a MODULES set where BOTH are REQUIRED — unsatisfiable,
    // since at most one of an exclusive pair may be enabled but two are forced on. Returns "A <-> B" strings.
    auto ExclusiveRequiredConflicts = [](const nlohmann::ordered_json &Owner) {
        std::vector<std::string> Out;
        if (!Owner.contains("MODULES") || !Owner["MODULES"].is_array()) return Out;
        std::map<std::string, bool> Req;
        for (const auto &M : Owner["MODULES"])
            if (M.is_object() && M.contains("COMPONENT") && M["COMPONENT"].is_string())
                Req[std::string(M["COMPONENT"])] = M.value("REQUIRED", true);
        std::set<std::string> Seen;
        for (const auto &M : Owner["MODULES"])
        {
            if (!M.is_object() || !M.contains("COMPONENT") || !M["COMPONENT"].is_string()) continue;
            std::string C = M["COMPONENT"];
            if (!M.value("REQUIRED", true) || !M.contains("EXCLUDE") || !M["EXCLUDE"].is_array()) continue;
            for (const auto &E : M["EXCLUDE"])
            {
                if (!E.is_string()) continue;
                std::string Other = E;
                if (!Req.count(Other) || !Req[Other]) continue; // the other side isn't a REQUIRED module here
                std::string a = C, b = Other; if (a > b) std::swap(a, b);
                std::string Key = a + " <-> " + b;
                if (Seen.insert(Key).second) Out.push_back(Key);
            }
        }
        return Out;
    };

    // Game / variant / module checks.
    if (Assembled.contains("GAMES") && Assembled["GAMES"].is_array())
        for (const auto &G : Assembled["GAMES"])
        {
            std::string GID = Str(G, "GAMEID", "?");
            // A declared cover MUST resolve to a local PATH (a filename string, or an object with PATH).
            const nlohmann::ordered_json *Cov = nullptr;
            if (G.contains("METADATA") && G["METADATA"].is_object() && G["METADATA"].contains("COVER")) Cov = &G["METADATA"]["COVER"];
            else if (G.contains("COVER")) Cov = &G["COVER"];
            if (Cov)
            {
                const bool HasPath = (Cov->is_string() && !Cov->get<std::string>().empty())
                                  || (Cov->is_object() && !Cov->value("PATH", std::string()).empty());
                if (!HasPath) Errors.push_back("Game '" + GID + "' has a COVER with no PATH — a cover must declare a local path.");
            }
            const auto Variants = (G.contains("VARIANTS") && G["VARIANTS"].is_array()) ? G["VARIANTS"] : nlohmann::ordered_json::array();
            if (Variants.empty()) Warnings.push_back("Game '" + GID + "' has no variants");
            int RecCount = 0;
            for (const auto &V : Variants)
            {
                std::string VID = Str(V, "VARIANT_ID", "?");
                if (V.contains("RECOMMENDED") && V["RECOMMENDED"].is_boolean() && V["RECOMMENDED"].get<bool>()) RecCount++;
                if (Str(V, "HOST_PLATFORM").empty())
                    Warnings.push_back("Variant '" + VID + "' in game '" + GID + "' has no HOST_PLATFORM (needed to resolve a runner)");
                const auto Comps = ModuleComponents(V);
                if (Comps.empty()) Warnings.push_back("Variant '" + VID + "' in game '" + GID + "' has no modules");
                for (const auto &C : Comps)
                    if (!ComponentIds.count(C))
                        Errors.push_back("Variant '" + VID + "' (game '" + GID + "') module '" + C + "' is not a known component");
                for (const auto &P : ExclusiveRequiredConflicts(V))
                    Errors.push_back("Variant '" + VID + "' (game '" + GID + "') has mutually-exclusive REQUIRED modules: " + P);
            }
            if (RecCount > 1) Warnings.push_back("Game '" + GID + "' has " + std::to_string(RecCount) + " RECOMMENDED variants");
        }

    // Runner checks. A runner mirrors a game: RUNNERS[].{RUNNER_ID, VARIANTS[]}. Each runner VARIANT needs
    // HOST_PLATFORM (the host it runs on) and GUEST_PLATFORM (the platforms it serves). A variant's MODULES
    // reference its OWN package's components — only a warning if absent here (validated in isolation).
    if (Assembled.contains("RUNNERS") && Assembled["RUNNERS"].is_array())
        for (const auto &R : Assembled["RUNNERS"])
        {
            std::string RID = Str(R, "RUNNER_ID");
            if (RID.empty()) RID = "?";
            else if (Str(R, "RUNNER_ID").empty()) Warnings.push_back("A runner has no RUNNER_ID");
            const auto Variants = (R.contains("VARIANTS") && R["VARIANTS"].is_array()) ? R["VARIANTS"] : nlohmann::ordered_json::array();
            if (Variants.empty()) Warnings.push_back("Runner '" + RID + "' has no variants");
            for (const auto &V : Variants)
            {
                std::string VID = Str(V, "VARIANT_ID", "?");
                if (Str(V, "HOST_PLATFORM").empty())
                    Warnings.push_back("Runner '" + RID + "' variant '" + VID + "' has no HOST_PLATFORM");
                if (!V.contains("GUEST_PLATFORM") || !V["GUEST_PLATFORM"].is_array() || V["GUEST_PLATFORM"].empty())
                    Warnings.push_back("Runner '" + RID + "' variant '" + VID + "' has no GUEST_PLATFORM");
                // A prefix-generating runner mounts game content inside a wine drive — its CONTENT_ROOT must
                // route through drive_c (e.g. "drive_c/%PackageUID%" or "pfx/drive_c/%PackageUID%").
                if (V.value("PREFIX_GENERATE", false)
                    && Str(V, "CONTENT_ROOT").find("drive_c") == std::string::npos)
                    Warnings.push_back("Runner '" + RID + "' variant '" + VID + "' has PREFIX_GENERATE but its CONTENT_ROOT has no drive_c");
                for (const auto &C : ModuleComponents(V))
                    if (!ComponentIds.count(C))
                        Warnings.push_back("Runner '" + RID + "' variant '" + VID + "' module '" + C + "' is not a component in this package");
                for (const auto &P : ExclusiveRequiredConflicts(V))
                    Errors.push_back("Runner '" + RID + "' variant '" + VID + "' has mutually-exclusive REQUIRED modules: " + P);
            }
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
