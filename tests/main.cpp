#include "vgtest.h"

// Runs every TEST() registered across the test translation units; exits non-zero if any CHECK failed.
int main()
{
    int Failed = 0;
    for (const auto &T : VgTests())
    {
        const int Before = VgFailures();
        T.Fn();
        const bool Ok = (VgFailures() == Before);
        std::printf("[%s] %s\n", Ok ? "PASS" : "FAIL", T.Name);
        if (!Ok) ++Failed;
    }
    std::printf("\n%zu test(s), %d failed, %d check failure(s)\n",
                VgTests().size(), Failed, VgFailures());
    return VgFailures() == 0 ? 0 : 1;
}
