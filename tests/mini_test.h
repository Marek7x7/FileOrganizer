#pragma once

// Minimal, dependency-free test harness for this project. Not a general
// framework — just enough to register named test cases across translation
// units and report a pass/fail summary, so we don't need to vendor a
// third-party library for a handful of pure-function tests.

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace minitest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

// Meyer's singletons: safe to call from static initializers across .cpp
// files regardless of translation-unit initialization order.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void checkFailed(const std::string& expr, const char* file, int line) {
    std::cerr << "    CHECK failed: " << expr << " (" << file << ":" << line << ")\n";
    ++failureCount();
}

inline int runAll() {
    int total = 0, failedTests = 0;
    for (auto& t : registry()) {
        ++total;
        int before = failureCount();
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "    Uncaught exception: " << e.what() << "\n";
            ++failureCount();
        } catch (...) {
            std::cerr << "    Uncaught non-exception throw\n";
            ++failureCount();
        }
        bool passed = (failureCount() == before);
        std::cout << (passed ? "[  OK  ] " : "[ FAIL ] ") << t.name << "\n";
        if (!passed) ++failedTests;
    }
    std::cout << "\n" << (total - failedTests) << "/" << total << " test cases passed\n";
    return failedTests == 0 ? 0 : 1;
}

} // namespace minitest

#define MT_CONCAT_INNER(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                              \
    static void MT_CONCAT(mt_test_fn_, __LINE__)();                                  \
    static ::minitest::Registrar MT_CONCAT(mt_test_reg_, __LINE__)(                  \
        name, MT_CONCAT(mt_test_fn_, __LINE__));                                     \
    static void MT_CONCAT(mt_test_fn_, __LINE__)()

#define CHECK(expr) \
    do { if (!(expr)) ::minitest::checkFailed(#expr, __FILE__, __LINE__); } while (0)

#define CHECK_EQ(a, b) \
    do { if (!((a) == (b))) ::minitest::checkFailed(#a " == " #b, __FILE__, __LINE__); } while (0)
