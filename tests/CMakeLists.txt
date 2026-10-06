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
        TestHeartbeat.cpp
        TestUsbDescriptors.cpp
        TestUsbEnumeration.cpp
        ../src/usb/VersaloonFunction.cpp
        # USB device stack of libArT (no ETL needed), for the enumeration test
        ../dependencies/libArT/src/usb/Device.cpp
        ../dependencies/libArT/src/usb/VendorBulkFunction.cpp
)

target_compile_options(vsl-tests PRIVATE -O0 -g -Wall -Wextra)
target_include_directories(vsl-tests PRIVATE
        ../src
        ${PROJECT_BINARY_DIR}                 # Version.h
        ../dependencies/libArT/src            # USB device stack
        ../dependencies/libArT/tests/usb      # FakeDeviceController.h
)
target_compile_definitions(vsl-tests PRIVATE UNIT_TESTING)
target_link_libraries(vsl-tests PRIVATE doctest::doctest)

# Discover all tests
doctest_discover_tests(vsl-tests)
