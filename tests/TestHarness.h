#pragma once

#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*function)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> value;
    return value;
}

struct Register {
    Register(const char* name, void (*function)()) { cases().push_back({name, function}); }
};

struct AbortCase final : std::exception {};

inline int currentFailures = 0;
inline int totalFailures = 0;

inline void fail(const char* file, int line, const std::string& message, bool fatal) {
    std::cerr << file << ':' << line << ": " << message << '\n';
    ++currentFailures;
    ++totalFailures;
    if (fatal) throw AbortCase{};
}

inline void check(bool condition, const char* expression, const char* file, int line, bool fatal = false) {
    if (!condition) fail(file, line, std::string("check failed: ") + expression, fatal);
}

template <typename Left, typename Right>
void checkEqual(const Left& left, const Right& right, const char* expressions, const char* file, int line, bool fatal = false) {
    if (!(left == right)) fail(file, line, std::string("equality failed: ") + expressions, fatal);
}

inline void checkNear(double left, double right, double tolerance, const char* expressions, const char* file, int line) {
    if (std::abs(left - right) > tolerance) {
        std::ostringstream message;
        message << "near check failed: " << expressions << " (" << left << " vs " << right << ')';
        fail(file, line, message.str(), false);
    }
}

} // namespace test

#define CATAN_TEST_JOIN_INNER(a, b) a##b
#define CATAN_TEST_JOIN(a, b) CATAN_TEST_JOIN_INNER(a, b)
#define TEST(name) \
    static void name(); \
    static ::test::Register CATAN_TEST_JOIN(register_, name)(#name, &name); \
    static void name()
#define CHECK(expression) ::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define REQUIRE(expression) ::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__, true)
#define CHECK_EQ(left, right) ::test::checkEqual((left), (right), #left " == " #right, __FILE__, __LINE__)
#define REQUIRE_EQ(left, right) ::test::checkEqual((left), (right), #left " == " #right, __FILE__, __LINE__, true)
#define CHECK_NEAR(left, right, tolerance) ::test::checkNear((left), (right), (tolerance), #left " ~= " #right, __FILE__, __LINE__)
