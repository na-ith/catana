#include "TestHarness.h"

#include <algorithm>
#include <string>

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0;
    int failed = 0;
    for (const auto& testCase : test::cases()) {
        if (!filter.empty() && std::string(testCase.name).find(filter) == std::string::npos) continue;
        ++run;
        test::currentFailures = 0;
        try {
            testCase.function();
        } catch (const test::AbortCase&) {
        } catch (const std::exception& error) {
            test::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what(), false);
        } catch (...) {
            test::fail(__FILE__, __LINE__, "unexpected non-standard exception", false);
        }
        if (test::currentFailures) {
            ++failed;
            std::cerr << "[FAIL] " << testCase.name << '\n';
        } else {
            std::cout << "[PASS] " << testCase.name << '\n';
        }
    }
    std::cout << "\n" << run << " cases, " << failed << " failed, " << test::totalFailures << " assertions failed\n";
    return failed ? 1 : 0;
}
