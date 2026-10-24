#include "test.h"

#include <cstdio>

std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

int& test_failures() {
    static int failures = 0;
    return failures;
}

int main() {
    std::printf("LinkBridge 单元测试\n====================\n");
    int failed_tests = 0;
    for (const auto& test : test_registry()) {
        const int before = test_failures();
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        const int after = test_failures();
        if (after == before) {
            std::printf("[  OK  ] %s\n", test.name);
        } else {
            std::printf("[ FAIL ] %s\n", test.name);
            ++failed_tests;
        }
    }
    std::printf("--------------------\n");
    std::printf("%zu 个用例，%d 个失败，%d 处断言错误\n", test_registry().size(), failed_tests,
                test_failures());
    return failed_tests == 0 ? 0 : 1;
}
