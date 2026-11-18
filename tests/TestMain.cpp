#include <cstdio>
#include <cstring>

#include "TestRunner.hpp"

int main(const int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failedCases = 0;
    for (const auto& c : test::Registry()) {
        if (filter && !std::strstr(c.name, filter)) {
            continue;
        }
        const int before = test::Failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        ++run;
        if (test::Failures() != before) {
            ++failedCases;
            std::printf("[ FAIL ] %s\n", c.name);
        }
    }
    std::printf("%d test cases, %d failed, %d failed checks\n", run, failedCases, test::Failures());
    return failedCases == 0 && run > 0 ? 0 : 1;
}
