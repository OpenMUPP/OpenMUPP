// SPDX-License-Identifier: GPL-3.0-or-later

#include <Version.h>
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#define MUPP_STR_IMPL(x) #x
#define MUPP_STR(x) MUPP_STR_IMPL(x)

TEST_CASE("Version string is vMAJOR.MINOR.BUILD with an optional build revision") {
   const std::string_view version(versionString);
   const std::string prefix = "v" MUPP_STR(VERSION_MAJOR) "." MUPP_STR(VERSION_MINOR) "." MUPP_STR(VERSION_BUILD);

#if VERSION_HAVE_BUILDREV
   CHECK(version == prefix + "-" + VERSION_BUILDREV);
#else
   CHECK(version == prefix);
#endif
}
