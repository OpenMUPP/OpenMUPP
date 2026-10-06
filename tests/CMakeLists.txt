cmake_minimum_required(VERSION 3.20)
Include(FetchContent)

FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.12
        GIT_SHALLOW TRUE
)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

list(APPEND CMAKE_MODULE_PATH ${doctest_SOURCE_DIR}/scripts/cmake)

# Unit tests
include(doctest)

add_executable(vsl-tests
        test_main.cpp
        TestVersion.cpp
        TestBoards.cpp
)

target_compile_options(vsl-tests PRIVATE -O0 -g -Wall -Wextra)
target_include_directories(vsl-tests PRIVATE
        ../src
        ${PROJECT_BINARY_DIR}                 # Version.h
)
target_compile_definitions(vsl-tests PRIVATE UNIT_TESTING)
target_link_libraries(vsl-tests PRIVATE doctest::doctest)

# Discover all tests
doctest_discover_tests(vsl-tests)
