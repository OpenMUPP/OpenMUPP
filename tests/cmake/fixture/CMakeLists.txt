# A small project that declares the personality options as the firmware
# build does, and prints the result. tests/CMakeLists.txt configures it more
# than one time in the same build directory, so the cache of one configure
# goes into the next one.
cmake_minimum_required(VERSION 3.20)
project(vsl-personalities-fixture NONE)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/Personalities.cmake")
vsl_personality_options()
include("${CMAKE_CURRENT_LIST_DIR}/../ResolvePersonalities.cmake")
