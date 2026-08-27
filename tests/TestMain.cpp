/*  PhonePostMix test runner.

    Deliberately dependency-free: a handful of macros over <cassert>-style checks, so the
    test target compiles with nothing but JUCE and a C++17 compiler. Adding a test
    framework would be one more thing a contributor has to download before `ctest` works.
*/

#include "TestSupport.h"

int main()
{
    return ppm::test::Registry::instance().runAll();
}
