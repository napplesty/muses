// Smoke test: validates the test harness (doctest include path, sanitizer
// flags, CTest registration) works end to end. Replaced by real component
// tests in later phases.
#include <doctest.h>

#include "muses/config.hpp"

TEST_CASE("sanity: arithmetic works") {
    CHECK(1 + 1 == 2);
}

TEST_CASE("sanity: platform detected") {
#if defined(MUSES_PLATFORM_APPLE)
    CHECK(true);
#elif defined(MUSES_PLATFORM_LINUX)
    CHECK(true);
#else
    CHECK(false);
#endif
}
