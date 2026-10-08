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

# ETL, for the CDC-ACM function of libArT (same tag as libArT)
FetchContent_Declare(
        etl
        GIT_REPOSITORY https://github.com/ETLCPP/etl.git
        GIT_TAG 20.41.7
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(etl)

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
        TestLogSink.cpp
        TestFrameFormat.cpp
        TestUartBridge.cpp
        TestSerialEventQueue.cpp
        TestBootloaderEntry.cpp
        TestCommandAssembler.cpp
        TestCommandProcessor.cpp
        TestUsbToAll.cpp
        TestReplay.cpp
        TestGpioBlock.cpp
        TestJtagRawBlock.cpp
        TestJtagBitBang.cpp
        TestJtagFastPath.cpp
        TestSwdBlock.cpp
        TestSwdBitBang.cpp
        TestPowerBlock.cpp
        ../src/protocol/CommandAssembler.cpp
        ../src/protocol/CommandProcessor.cpp
        ../src/protocol/GpioBlock.cpp
        ../src/protocol/JtagRawBlock.cpp
        ../src/protocol/PowerBlock.cpp
        ../src/protocol/SwdBlock.cpp
        ../src/protocol/UsbToAll.cpp
        ../src/protocol/UsbToXxx.cpp
        ../src/usb/VersaloonFunction.cpp
        # Logger and printf of libArT, for the log sink test
        ../dependencies/libArT/src/logging/LogManager.cpp
        ../dependencies/libArT/src/streams/Writable.cpp
        ../dependencies/libArT/src/utils/tfp_printf.cpp
        # USB device stack of libArT (no ETL needed), for the enumeration test
        ../dependencies/libArT/src/usb/Device.cpp
        ../dependencies/libArT/src/usb/VendorBulkFunction.cpp
        # CDC-ACM function of libArT, for the UART bridge test
        ../dependencies/libArT/src/usb/CdcAcmFunction.cpp
)

target_compile_options(vsl-tests PRIVATE -O0 -g -Wall -Wextra)
# libArT code: not the code under test, keep its warnings out of the test build
set_source_files_properties(../dependencies/libArT/src/utils/tfp_printf.cpp PROPERTIES COMPILE_OPTIONS -Wno-unused-function)
target_include_directories(vsl-tests PRIVATE
        ../src
        ${PROJECT_BINARY_DIR}                 # Version.h
        ../dependencies/libArT/src            # USB device stack
        ../dependencies/libArT/tests/usb      # FakeDeviceController.h
        ../dependencies/system-stm32/src      # peripherals/UsbDeviceTypes.h
)
target_compile_definitions(vsl-tests PRIVATE UNIT_TESTING VSL_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay")
target_link_libraries(vsl-tests PRIVATE doctest::doctest etl::etl)

# Discover all tests
doctest_discover_tests(vsl-tests)
