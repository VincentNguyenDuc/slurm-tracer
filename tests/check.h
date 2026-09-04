// A test harness small enough to not be a dependency.
//
// TEST_CASE registers a function at static-init time; main() runs them all and
// reports failures. CHECK keeps going after a failure so one run reports every
// problem; REQUIRE aborts the case when continuing would be meaningless.

#ifndef SLURM_TRACER_TESTS_CHECK_H
#define SLURM_TRACER_TESTS_CHECK_H

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct AbortCase : std::exception {};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const char* expr, const std::string& detail) {
    ++failures();
    std::printf("    FAIL %s:%d: %s%s\n", file, line, expr, detail.c_str());
}

} // namespace testing

#define TEST_CASE(name)                                                                            \
    static void test_##name();                                                                     \
    static ::testing::Registrar registrar_##name(#name, test_##name);                              \
    static void test_##name()

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::testing::fail(__FILE__, __LINE__, #expr, "");                                        \
    } while (0)

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::testing::fail(__FILE__, __LINE__, #expr, " (cannot continue)");                      \
            throw ::testing::AbortCase{};                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto& lhs_ = (a);                                                                    \
        const auto& rhs_ = (b);                                                                    \
        if (!(lhs_ == rhs_)) {                                                                     \
            std::string detail_ = "\n      left:  " + ::testing::show(lhs_) +                      \
                                  "\n      right: " + ::testing::show(rhs_);                       \
            ::testing::fail(__FILE__, __LINE__, #a " == " #b, detail_);                            \
        }                                                                                          \
    } while (0)

namespace testing {

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return std::string("\"") + s + "\""; }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <typename T>
std::string show(const T& v) {
    return std::to_string(v);
}

} // namespace testing

#endif // SLURM_TRACER_TESTS_CHECK_H
