#include <Version.h>
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#define VSL_STR_IMPL(x) #x
#define VSL_STR(x) VSL_STR_IMPL(x)

TEST_CASE("Version string is vMAJOR.MINOR.BUILD with an optional build revision") {
   const std::string_view version(versionString);
   const std::string prefix = "v" VSL_STR(VERSION_MAJOR) "." VSL_STR(VERSION_MINOR) "." VSL_STR(VERSION_BUILD);

#if VERSION_HAVE_BUILDREV
   CHECK(version == prefix + "-" + VERSION_BUILDREV);
#else
   CHECK(version == prefix);
#endif
}
