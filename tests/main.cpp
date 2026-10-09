#include "vgtest.h"

#include <exception>

// Runs every TEST() registered across the test translation units; exits non-zero if any CHECK failed.
int main()
{
    int Failed = 0;
    for (const auto &T : VgTests())
    {
        const int Before = VgFailures();
        //An exception escaping a test is that test's failure, not the end of the run: one Windows-only throw used to
        //abort every test after it, unreported.
        try { T.Fn(); }
        catch (const std::exception &E) { ++VgFailures(); std::printf("    THREW %s\n", E.what()); }
        catch (...)                     { ++VgFailures(); std::printf("    THREW (not a std::exception)\n"); }
        const bool Ok = (VgFailures() == Before);
        std::printf("[%s] %s\n", Ok ? "PASS" : "FAIL", T.Name);
        if (!Ok) ++Failed;
    }
    std::printf("\n%zu test(s), %d failed, %d check failure(s)\n",
                VgTests().size(), Failed, VgFailures());
    return VgFailures() == 0 ? 0 : 1;
}
