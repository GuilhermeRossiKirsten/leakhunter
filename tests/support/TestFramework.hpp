/// @file TestFramework.hpp
/// @brief ~100 lines of test harness, so the project needs no test dependency.
///
/// Deliberately minimal: registration, assertions, and a runner. If the suite
/// ever outgrows this, swapping in Catch2 or GoogleTest is a one-file change.

#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace lhtest {

struct AssertionFailure : std::exception {
    std::string message;

    explicit AssertionFailure(std::string text) : message(std::move(text)) {}
    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

template <typename T>
[[nodiscard]] std::string describe(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string describe(std::nullptr_t) { return "nullptr"; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

inline int runAll() {
    int failures = 0;

    for (const TestCase& test : registry()) {
        try {
            test.body();
            std::cout << "  [ ok ] " << test.name << '\n';
        } catch (const AssertionFailure& failure) {
            std::cout << "  [FAIL] " << test.name << "\n         " << failure.message << '\n';
            ++failures;
        } catch (const std::exception& error) {
            std::cout << "  [FAIL] " << test.name << "\n         unexpected exception: "
                      << error.what() << '\n';
            ++failures;
        }
    }

    std::cout << '\n'
              << (failures == 0 ? "all " : "") << registry().size() - failures << '/'
              << registry().size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace lhtest

#define LH_CONCAT_INNER(a, b) a##b
#define LH_CONCAT(a, b) LH_CONCAT_INNER(a, b)

/// Defines and registers a test case.
#define LH_TEST(suite, name)                                                     \
    static void LH_CONCAT(suite##_##name##_body, __LINE__)();                     \
    static const ::lhtest::Registrar LH_CONCAT(suite##_##name##_registrar,        \
                                               __LINE__){                         \
        #suite "." #name, &LH_CONCAT(suite##_##name##_body, __LINE__)};           \
    static void LH_CONCAT(suite##_##name##_body, __LINE__)()

#define LH_FAIL(message)                                                                    \
    throw ::lhtest::AssertionFailure(std::string(__FILE__ ":") + std::to_string(__LINE__) + \
                                     " -- " + (message))

#define LH_CHECK(condition)                                             \
    do {                                                                \
        if (!(condition)) {                                             \
            LH_FAIL("expected: " #condition);                           \
        }                                                               \
    } while (false)

/// Note the deliberate copy rather than `const auto&`.
///
/// A reference here dangles for a whole class of natural-looking arguments:
/// `LH_CHECK_EQ(makeThing().value().field, x)` binds to a member of a temporary
/// reached *through a function call*, which does not extend that temporary's
/// lifetime. It dies at the end of the declaration, before the comparison runs.
/// Copying costs nothing at test scale and removes the trap entirely.
#define LH_CHECK_EQ(actual, expected)                               \
    do {                                                            \
        const auto lhValueActual = (actual);                        \
        const auto lhValueExpected = (expected);                    \
        if (!(lhValueActual == lhValueExpected)) {                  \
            LH_FAIL("expected " #actual " == " #expected ", got " + \
                    ::lhtest::describe(lhValueActual) + " vs " +    \
                    ::lhtest::describe(lhValueExpected));           \
        }                                                           \
    } while (false)

#define LH_CHECK_NE(actual, expected)                                          \
    do {                                                                       \
        if ((actual) == (expected)) {                                          \
            LH_FAIL("expected " #actual " != " #expected);                     \
        }                                                                      \
    } while (false)
