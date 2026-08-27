#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace ppm::test
{

struct Failure
{
    std::string testName, expression, file;
    int line;
};

/** Collects the tests declared by PPM_TEST and runs them, reporting every failure rather
    than aborting on the first, so one run tells you everything that is broken.
*/
class Registry
{
public:
    static Registry& instance()
    {
        static Registry r;
        return r;
    }

    void add (std::string name, std::function<void()> body)
    {
        tests.push_back ({ std::move (name), std::move (body) });
    }

    void recordFailure (Failure f) { failures.push_back (std::move (f)); }

    void setCurrent (const std::string& name) { current = name; }
    const std::string& getCurrent() const { return current; }

    int runAll()
    {
        std::size_t failed = 0;

        for (auto& t : tests)
        {
            const auto before = failures.size();
            current = t.name;
            t.body();

            const auto ok = failures.size() == before;
            std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name << '\n';
            failed += ok ? 0 : 1;
        }

        for (const auto& f : failures)
            std::cout << "  " << f.file << ':' << f.line << "  in \"" << f.testName
                      << "\":  " << f.expression << '\n';

        std::cout << '\n' << (tests.size() - failed) << '/' << tests.size() << " tests passed\n";
        return failed == 0 ? 0 : 1;
    }

private:
    struct Test { std::string name; std::function<void()> body; };

    std::vector<Test> tests;
    std::vector<Failure> failures;
    std::string current;
};

struct Registrar
{
    Registrar (const char* name, std::function<void()> body)
    {
        Registry::instance().add (name, std::move (body));
    }
};

} // namespace ppm::test

#define PPM_TEST(name)                                                                     \
    static void ppmTestBody_##name();                                                      \
    static ::ppm::test::Registrar ppmTestReg_##name { #name, ppmTestBody_##name };          \
    static void ppmTestBody_##name()

#define PPM_CHECK(expr)                                                                    \
    do {                                                                                   \
        if (! (expr))                                                                      \
            ::ppm::test::Registry::instance().recordFailure (                              \
                { ::ppm::test::Registry::instance().getCurrent(), #expr, __FILE__, __LINE__ }); \
    } while (false)

#define PPM_CHECK_EQ(a, b)                                                                 \
    do {                                                                                   \
        const auto ppmLhs = (a);                                                           \
        const auto ppmRhs = (b);                                                           \
        if (! (ppmLhs == ppmRhs))                                                          \
            ::ppm::test::Registry::instance().recordFailure (                              \
                { ::ppm::test::Registry::instance().getCurrent(),                          \
                  std::string (#a " == " #b "  (")                                         \
                      + std::to_string (ppmLhs) + " vs " + std::to_string (ppmRhs) + ")",  \
                  __FILE__, __LINE__ });                                                   \
    } while (false)
