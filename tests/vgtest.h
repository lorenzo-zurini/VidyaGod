#pragma once
// Tiny zero-dependency test harness for the pure-logic C++ units (no Qt, no framework).
// Define tests with TEST(name){ ... } and assert with CHECK / CHECK_EQ; main.cpp runs them all
// and exits non-zero on any failure (so ctest reports pass/fail).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct VgTestCase { const char *Name; std::function<void()> Fn; };

inline std::vector<VgTestCase> &VgTests() { static std::vector<VgTestCase> v; return v; }
inline int &VgFailures()                  { static int f = 0; return f; }

struct VgRegistrar { VgRegistrar(const char *n, std::function<void()> f) { VgTests().push_back({n, std::move(f)}); } };

#define TEST(name)                                                        \
    static void name();                                                   \
    static VgRegistrar vg_reg_##name(#name, name);                        \
    static void name()

#define CHECK(cond)                                                       \
    do { if (!(cond)) { ++VgFailures();                                   \
        std::fprintf(stderr, "    FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b)                                                    \
    do { auto vg_a = (a); auto vg_b = (b); if (!(vg_a == vg_b)) { ++VgFailures();   \
        std::fprintf(stderr, "    FAIL %s:%d  CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); } } while (0)
