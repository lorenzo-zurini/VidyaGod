// test_registrywrapper.cpp — the wine-registry model: RegEdit application, value semantics, hive round-trip.
//
// 784 lines and no tests, in a layer where a mistake is invisible: a registry value the package declared simply
// never exists, and the game behaves as if the package had not said it. The engine has already been bitten here
// once — a whole-hive merge ordering bug wiped package-authored install keys and produced "not installed" errors
// from games that were installed.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "registrywrapper.h"
#include "commonutils.h"

using nlohmann::ordered_json;

namespace {

// Collect the WARN/ERR a call produces, so "did it complain?" is itself assertable.
struct Complaints
{
    std::vector<std::string> Lines;
    Complaints()  { SetLogCallback([this](LogLevel L, const std::string &C, const std::string &M){
                        if (L == LogLevel::WARN || L == LogLevel::ERR) Lines.push_back(C + "|" + M); }); }
    ~Complaints() { ClearLogCallback(); }
    bool Mentions(const std::string &Needle) const
    {
        for (const auto &L : Lines) if (L.find(Needle) != std::string::npos) return true;
        return false;
    }
};

ordered_json RegEdit(const std::string &Path, const ordered_json &Values,
                     bool Override = false, const std::string &Arch = {})
{
    ordered_json J{{"TYPE", "RegEdit"}, {"REGPATH", Path}, {"KEYVALUES", Values}};
    if (Override)     J["OVERRIDE"]     = true;
    if (!Arch.empty()) J["ARCHITECTURE"] = Arch;
    return J;
}

} // namespace

class RegistryWrapperTest : public QObject
{
    Q_OBJECT
private slots:

    // ---- applying RegEdits ----

    void applies_values_under_the_declared_path()
    {
        RegistryWrapper RW;
        QVERIFY(RW.ApplyRegEdits(ordered_json::array({
            RegEdit("HKLM\\Software\\Acme\\Game", {{"InstallDir", "C:\\Games\\Acme"}, {"Language", "ENG"}})
        }), /*WantOverride=*/false));

        const RegistryValue *V = RW.GetValue("HKLM\\Software\\Acme\\Game", "InstallDir");
        QVERIFY2(V, "the declared value must exist after applying");
        QCOMPARE(V->Str, std::string("C:\\Games\\Acme"));
        QVERIFY(RW.GetValue("HKLM\\Software\\Acme\\Game", "Language"));
    }

    // The OVERRIDE flag selects the pass. An edit from the wrong pass must NOT be applied, or base and override
    // semantics collapse into each other.
    void the_override_flag_selects_the_pass()
    {
        RegistryWrapper RW;
        const ordered_json Subs = ordered_json::array({
            RegEdit("HKLM\\Software\\BaseOnly",     {{"V", "1"}}, /*Override=*/false),
            RegEdit("HKLM\\Software\\OverrideOnly", {{"V", "1"}}, /*Override=*/true),
        });

        RW.ApplyRegEdits(Subs, /*WantOverride=*/false);
        QVERIFY(RW.GetValue("HKLM\\Software\\BaseOnly", "V"));
        QVERIFY2(!RW.GetValue("HKLM\\Software\\OverrideOnly", "V"),
                 "an OVERRIDE edit must not be applied during the base pass");

        RegistryWrapper RW2;
        RW2.ApplyRegEdits(Subs, /*WantOverride=*/true);
        QVERIFY(RW2.GetValue("HKLM\\Software\\OverrideOnly", "V"));
        QVERIFY(!RW2.GetValue("HKLM\\Software\\BaseOnly", "V"));
    }

    // null KEYVALUES is the documented key-only form (Worms 4 ships exactly this) and must succeed silently.
    void null_keyvalues_creates_the_key_only()
    {
        Complaints C;
        RegistryWrapper RW;
        ordered_json J{{"TYPE", "RegEdit"}, {"REGPATH", "HKLM\\Software\\KeyOnly"}, {"KEYVALUES", nullptr}};
        QVERIFY2(RW.ApplyRegEdits(ordered_json::array({J}), false),
                 "key-only creation is legitimate authoring and must not be reported as a failure");
        QVERIFY2(C.Lines.empty(), "key-only creation must not warn");
    }

    // Present-but-malformed KEYVALUES used to be dropped in COMPLETE silence: the key was created and every
    // value the author wrote was discarded, while the caller was told it succeeded.
    void malformed_keyvalues_fails_loudly_instead_of_silently_dropping()
    {
        Complaints C;
        RegistryWrapper RW;
        ordered_json J{{"TYPE", "RegEdit"}, {"REGPATH", "HKLM\\Software\\Bad"},
                       {"KEYVALUES", "InstallDir=C:\\Games"}};   // a string, not an object
        QVERIFY2(!RW.ApplyRegEdits(ordered_json::array({J}), false),
                 "malformed KEYVALUES must make the pass report failure");
        QVERIFY2(C.Mentions("not an object"), "it must say WHY nothing was written");
    }

    // An empty REGPATH is unusable; it must be reported and must not silently count as applied.
    void empty_regpath_is_reported()
    {
        Complaints C;
        RegistryWrapper RW;
        ordered_json J{{"TYPE", "RegEdit"}, {"REGPATH", ""}, {"KEYVALUES", {{"V", "1"}}}};
        QVERIFY(!RW.ApplyRegEdits(ordered_json::array({J}), false));
        QVERIFY(C.Mentions("REGPATH"));
    }

    // One bad edit must not stop the good ones, but must still make the pass report failure — otherwise a
    // package with one typo silently loses that setting and nothing says so.
    void one_bad_edit_does_not_hide_the_good_ones_nor_pass_silently()
    {
        RegistryWrapper RW;
        ordered_json Bad{{"TYPE", "RegEdit"}, {"REGPATH", ""}, {"KEYVALUES", {{"V", "1"}}}};
        QVERIFY(!RW.ApplyRegEdits(ordered_json::array({
            Bad, RegEdit("HKLM\\Software\\Good", {{"V", "yes"}})
        }), false));
        QVERIFY2(RW.GetValue("HKLM\\Software\\Good", "V"), "a later valid edit must still be applied");
    }

    void non_regedit_subcomponents_are_ignored()
    {
        RegistryWrapper RW;
        QVERIFY(RW.ApplyRegEdits(ordered_json::array({
            ordered_json{{"TYPE", "FileEdit"}, {"MODE", "Overwrite"}, {"FILE", "x"}},
            ordered_json{{"TYPE", "CustomVar"}, {"KEY", "k"}, {"DEFAULT", "v"}},
        }), false));
    }

    void an_empty_or_non_array_input_is_safe()
    {
        RegistryWrapper RW;
        QVERIFY(RW.ApplyRegEdits(ordered_json::array(), false));
        QVERIFY(RW.ApplyRegEdits(ordered_json::object(), false));
    }

    // ARCHITECTURE:32 redirects under Wow6432Node — that redirect is how a 32-bit game finds its own keys, and
    // getting it wrong makes an installed game look uninstalled.
    void architecture_32_redirects_under_wow6432node()
    {
        RegistryWrapper RW;
        RW.ApplyRegEdits(ordered_json::array({
            RegEdit("HKLM\\Software\\Acme", {{"V", "1"}}, /*Override=*/false, /*Arch=*/"32")
        }), false);
        QVERIFY2(RW.GetValue("HKLM\\Software\\Wow6432Node\\Acme", "V"),
                 "a 32-bit RegEdit must land in the WoW64 view");
        QVERIFY2(!RW.GetValue("HKLM\\Software\\Acme", "V"),
                 "and must NOT also land in the 64-bit view");
    }

    // ---- value semantics ----

    void setvalue_overwrites_and_deletevalue_removes()
    {
        RegistryWrapper RW;
        RW.EnsureKey("HKCU\\Software\\T");
        RW.SetValue("HKCU\\Software\\T", "N", RegistryWrapper::ManifestStringToValue("first"));
        RW.SetValue("HKCU\\Software\\T", "N", RegistryWrapper::ManifestStringToValue("second"));
        const RegistryValue *V = RW.GetValue("HKCU\\Software\\T", "N");
        QVERIFY(V);
        QCOMPARE(V->Str, std::string("second"));

        QVERIFY(RW.DeleteValue("HKCU\\Software\\T", "N"));
        QVERIFY2(!RW.GetValue("HKCU\\Software\\T", "N"), "a deleted value must be gone");
        QVERIFY2(!RW.DeleteValue("HKCU\\Software\\T", "N"), "deleting a missing value reports false");
    }

    void missing_keys_and_values_read_back_null()
    {
        RegistryWrapper RW;
        QVERIFY(!RW.GetValue("HKLM\\Software\\Nope", "V"));
        RW.EnsureKey("HKLM\\Software\\Exists");
        QVERIFY(!RW.GetValue("HKLM\\Software\\Exists", "AbsentValue"));
    }

    void deletekey_removes_the_key()
    {
        RegistryWrapper RW;
        RW.ApplyRegEdits(ordered_json::array({ RegEdit("HKLM\\Software\\Gone", {{"V", "1"}}) }), false);
        QVERIFY(RW.GetValue("HKLM\\Software\\Gone", "V"));
        QVERIFY(RW.DeleteKey("HKLM\\Software\\Gone"));
        QVERIFY(!RW.GetValue("HKLM\\Software\\Gone", "V"));
    }

    // ---- hive round-trip: what actually reaches the game ----

    // Everything above is in-memory. If the save/load round-trip loses a value, every edit above is moot.
    void values_survive_a_save_and_reload()
    {
        QTemporaryDir Dir;
        const std::filesystem::path P = Dir.path().toStdString();

        RegistryWrapper Out;
        Out.ApplyRegEdits(ordered_json::array({
            RegEdit("HKLM\\Software\\Acme\\Game", {{"InstallDir", "C:\\Games\\Acme"}}),
            RegEdit("HKCU\\Software\\Acme",       {{"Nick", "Lorenzo"}}),
        }), false);
        QVERIFY2(Out.SavePrefix(P), "saving the hives must succeed");

        RegistryWrapper In;
        QVERIFY(In.LoadPrefix(P));
        const RegistryValue *M = In.GetValue("HKLM\\Software\\Acme\\Game", "InstallDir");
        const RegistryValue *U = In.GetValue("HKCU\\Software\\Acme", "Nick");
        QVERIFY2(M, "an HKLM value must survive the round-trip");
        QCOMPARE(M->Str, std::string("C:\\Games\\Acme"));
        QVERIFY2(U, "an HKCU value must survive the round-trip");
        QCOMPARE(U->Str, std::string("Lorenzo"));
    }

    // A path's root decides which hive file it lands in; misrouting puts a value where the game never looks.
    void the_root_prefix_selects_the_hive()
    {
        QCOMPARE(RegistryWrapper::RootForPath("HKLM\\Software\\X"), HiveRoot::Machine);
        QCOMPARE(RegistryWrapper::RootForPath("HKCU\\Software\\X"), HiveRoot::User);
    }
};

QTEST_MAIN(RegistryWrapperTest)
#include "test_registrywrapper.moc"
