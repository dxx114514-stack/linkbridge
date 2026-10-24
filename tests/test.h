#pragma once
#include <cstdio>
#include <string>
#include <vector>

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& test_registry();
int& test_failures();

struct TestRegistrar {
    TestRegistrar(const char* name, void (*fn)()) { test_registry().push_back({name, fn}); }
};

#define TEST(name)                                    \
    static void name();                               \
    static TestRegistrar reg_##name(#name, name);     \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++test_failures();                                               \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                                  \
    do {                                                                                \
        const std::string _a = (actual);                                                \
        const std::string _e = (expected);                                              \
        if (_a != _e) {                                                                 \
            std::printf("  [FAIL] %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,       \
                        _a.c_str(), _e.c_str());                                        \
            ++test_failures();                                                          \
        }                                                                               \
    } while (0)
