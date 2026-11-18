#pragma once

// A tiny test runner without dependencies.
//
//   TEST_CASE(SomethingWorks)
//   {
//       CHECK(1 + 1 == 2);
//       CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
//   }
//
// Every *Tests.cpp file in tests/ is built into SC4SeamlessRegionTests. A failed CHECK prints
// the file and line and the test exits with a non-zero code. Run with a name to run only the
// tests whose name contains it.

#include <cmath>
#include <cstdio>
#include <vector>

namespace test
{
    struct Case
    {
        const char* name;
        void (*fn)();
    };

    inline std::vector<Case>& Registry()
    {
        static std::vector<Case> cases;
        return cases;
    }

    inline int& Failures()
    {
        static int failures = 0;
        return failures;
    }

    struct Registrar
    {
        Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
    };

    inline void Fail(const char* file, const int line, const char* what)
    {
        ++Failures();
        std::printf("  FAILED %s(%d): %s\n", file, line, what);
    }
}

#define TEST_CASE(name)                                         \
    static void name();                                         \
    static const ::test::Registrar name##Registrar(#name, &name); \
    static void name()

#define CHECK(cond)                                    \
    do {                                               \
        if (!(cond)) {                                 \
            ::test::Fail(__FILE__, __LINE__, #cond);   \
        }                                              \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                       \
    do {                                                                                              \
        const double checkActual_ = static_cast<double>(actual);                                      \
        const double checkExpected_ = static_cast<double>(expected);                                  \
        if (!(std::fabs(checkActual_ - checkExpected_) <= static_cast<double>(tolerance))) {         \
            ::test::Fail(__FILE__, __LINE__, #actual " near " #expected);                             \
            std::printf("    actual %.6f, expected %.6f, tolerance %g\n", checkActual_, checkExpected_, \
                        static_cast<double>(tolerance));                                              \
        }                                                                                             \
    } while (false)
