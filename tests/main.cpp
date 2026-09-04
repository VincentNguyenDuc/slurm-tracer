#include "check.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    int ran = 0;
    for (const auto& c : testing::cases()) {
        if (filter && std::strstr(c.name, filter) == nullptr)
            continue;
        ++ran;
        const int before = testing::failures();
        std::printf("  %s\n", c.name);
        try {
            c.fn();
        } catch (const testing::AbortCase&) {
            // REQUIRE already recorded the failure.
        } catch (const std::exception& e) {
            ++testing::failures();
            std::printf("    FAIL threw: %s\n", e.what());
        }
        if (testing::failures() == before)
            std::printf("    ok\n");
    }

    std::printf("\n%d case(s) run, %d failure(s)\n", ran, testing::failures());
    return testing::failures() == 0 ? 0 : 1;
}
