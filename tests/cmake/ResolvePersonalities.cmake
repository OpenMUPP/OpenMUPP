# Runs vsl_resolve_personalities() (cmake/Personalities.cmake) in script mode
# and prints the result in one line. tests/CMakeLists.txt runs it with the
# options of each test case and checks the line or the error text.
#
# Usage: cmake -DVSL_PERSONALITIES=... [-D...] -P ResolvePersonalities.cmake
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Personalities.cmake")
vsl_resolve_personalities()
string(REPLACE ";" "," _set "${VSL_PERSONALITY_SET}")
string(REPLACE ";" "," _defines "${VSL_PERSONALITY_DEFINES}")
message(STATUS "set=${_set} default=${VSL_DEFAULT_PERSONALITY_NAME} tag=[${VSL_IMAGE_TAG}] defines=${_defines}")
