# Runs mupp_resolve_personalities() (cmake/Personalities.cmake) in script mode
# and prints the result in one line. tests/CMakeLists.txt runs it with the
# options of each test case and checks the line or the error text.
#
# Usage: cmake -DMUPP_PERSONALITIES=... [-D...] -P ResolvePersonalities.cmake
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Personalities.cmake")
mupp_resolve_personalities()
string(REPLACE ";" "," _set "${MUPP_PERSONALITY_SET}")
string(REPLACE ";" "," _defines "${MUPP_PERSONALITY_DEFINES}")
message(STATUS "set=${_set} default=${MUPP_DEFAULT_PERSONALITY_NAME} tag=[${MUPP_IMAGE_TAG}] defines=${_defines}")
