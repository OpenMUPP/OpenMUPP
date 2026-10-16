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
        TestActivityLed.cpp
        TestErrorLed.cpp
        TestTargetPowerLed.cpp
        TestUptimeMicros.cpp
        TestUsbDescriptors.cpp
        TestUsbEnumeration.cpp
        TestCmsisDapDescriptors.cpp
        TestCmsisDapEnumeration.cpp
        TestLogSink.cpp
        TestFrameFormat.cpp
        TestUartBridge.cpp
        TestDapUart.cpp
        TestSerialEventQueue.cpp
        TestBootloaderEntry.cpp
        TestPersonalityStore.cpp
        TestPersonalitySwitch.cpp
        TestSwitchRequest.cpp
        TestKeyGesture.cpp
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
        TestRvSwdEngine.cpp
        TestDebugModule.cpp
        TestCmsisDap.cpp
        TestCmsisDapJtag.cpp
        TestWchLink.cpp
        TestWchLinkFlash.cpp
        TestPowerBlock.cpp
        ../src/protocol/CommandAssembler.cpp
        ../src/protocol/CommandProcessor.cpp
        ../src/protocol/cmsisdap/DapJtag.cpp
        ../src/protocol/cmsisdap/DapProcessor.cpp
        ../src/protocol/wchlink/WchLinkFlash.cpp
        ../src/protocol/wchlink/WchLinkProcessor.cpp
        ../src/protocol/GpioBlock.cpp
        ../src/protocol/JtagRawBlock.cpp
        ../src/protocol/PowerBlock.cpp
        ../src/protocol/SwdBlock.cpp
        ../src/protocol/UsbToAll.cpp
        ../src/protocol/UsbToXxx.cpp
        ../src/usb/VersaloonFunction.cpp
        ../src/usb/CmsisDapFunction.cpp
        ../src/usb/WchLinkFunction.cpp
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
# All personalities, so the tests see the full personality list
target_compile_definitions(vsl-tests PRIVATE UNIT_TESTING VSL_WITH_VERSALOON VSL_WITH_CMSIS_DAP VSL_WITH_WCH_LINK
        VSL_MULTI_PERSONALITY VSL_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay")
target_link_libraries(vsl-tests PRIVATE doctest::doctest etl::etl)

# Discover all tests
doctest_discover_tests(vsl-tests)

# The personality options of the firmware build (cmake/Personalities.cmake),
# in script mode. Each case passes when the output matches <regex>: the result
# line, or the text of the error or of the deprecation warning.
function(vsl_personality_test name regex)
    add_test(NAME personalities.${name}
            COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResolvePersonalities.cmake)
    set_tests_properties(personalities.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

vsl_personality_test(default "set=versaloon default=versaloon tag=\\[\\] defines=VSL_WITH_VERSALOON\n")
vsl_personality_test(cmsis-dap "set=cmsis-dap default=cmsis-dap tag=\\[-cmsis-dap\\] defines=VSL_WITH_CMSIS_DAP\n"
        -DVSL_PERSONALITIES=cmsis-dap)
vsl_personality_test(wch-link "set=wch-link default=wch-link tag=\\[-wch-link\\] defines=VSL_WITH_WCH_LINK\n"
        -DVSL_PERSONALITIES=wch-link)
vsl_personality_test(wch-link-wch-id "tag=\\[-wch-link-wch-id\\] defines=VSL_WITH_WCH_LINK,VSL_WCH_LINK_WCH_USB_ID\n"
        -DVSL_PERSONALITIES=wch-link -DVSL_WCH_LINK_WCH_USB_ID=ON)
# A CMake list, in any order: the result is in canonical order
vsl_personality_test(list "set=versaloon,cmsis-dap default=versaloon tag=\\[-vsl\\+dap\\] defines=VSL_WITH_VERSALOON,VSL_WITH_CMSIS_DAP,VSL_MULTI_PERSONALITY,VSL_DEFAULT_PERSONALITY=0\n"
        "-DVSL_PERSONALITIES=cmsis-dap$<SEMICOLON>versaloon")
vsl_personality_test(dap-default "set=versaloon,cmsis-dap default=cmsis-dap tag=\\[-dap\\+vsl\\] .*VSL_DEFAULT_PERSONALITY=1\n"
        -DVSL_PERSONALITIES=versaloon,cmsis-dap -DVSL_DEFAULT_PERSONALITY=cmsis-dap)
vsl_personality_test(triple "set=versaloon,cmsis-dap,wch-link default=versaloon tag=\\[-vsl\\+dap\\+wch\\] defines=VSL_WITH_VERSALOON,VSL_WITH_CMSIS_DAP,VSL_WITH_WCH_LINK,VSL_MULTI_PERSONALITY,VSL_DEFAULT_PERSONALITY=0\n"
        "-DVSL_PERSONALITIES=wch-link, versaloon ,cmsis-dap")
vsl_personality_test(dap-wch "set=cmsis-dap,wch-link default=wch-link tag=\\[-wch\\+dap\\] .*VSL_DEFAULT_PERSONALITY=2\n"
        -DVSL_PERSONALITIES=cmsis-dap,wch-link -DVSL_DEFAULT_PERSONALITY=wch-link)
vsl_personality_test(multi-wch-id "tag=\\[-dap\\+wch-wch-id\\] .*VSL_WCH_LINK_WCH_USB_ID\n"
        -DVSL_PERSONALITIES=cmsis-dap,wch-link -DVSL_WCH_LINK_WCH_USB_ID=ON)
vsl_personality_test(duplicate "set=versaloon,cmsis-dap default=versaloon"
        -DVSL_PERSONALITIES=versaloon,cmsis-dap,versaloon)
vsl_personality_test(unknown "Unknown personality 'avr'" -DVSL_PERSONALITIES=versaloon,avr)
vsl_personality_test(empty "VSL_PERSONALITIES has no personality" "-DVSL_PERSONALITIES=,")
vsl_personality_test(default-not-in-set "VSL_DEFAULT_PERSONALITY 'wch-link' is not in the personalities"
        -DVSL_PERSONALITIES=versaloon,cmsis-dap -DVSL_DEFAULT_PERSONALITY=wch-link)
vsl_personality_test(wch-id-without-wch-link "VSL_WCH_LINK_WCH_USB_ID needs wch-link"
        -DVSL_PERSONALITIES=versaloon -DVSL_WCH_LINK_WCH_USB_ID=ON)

# The deprecated VSL_PROTOCOL: the same result as the new option, with a warning
vsl_personality_test(protocol-warning "VSL_PROTOCOL is deprecated"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=cmsis-dap)
vsl_personality_test(protocol-dual "set=versaloon,cmsis-dap default=versaloon tag=\\[-vsl\\+dap\\]"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=versaloon)
vsl_personality_test(protocol-dual-cmsis-dap "set=versaloon,cmsis-dap default=cmsis-dap tag=\\[-dap\\+vsl\\]"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=cmsis-dap)
vsl_personality_test(protocol-wch-link "set=wch-link default=wch-link tag=\\[-wch-link\\]"
        -DVSL_PROTOCOL=wch-link -DVSL_DEFAULT_PROTOCOL=versaloon)
vsl_personality_test(protocol-unknown "Unknown VSL_PROTOCOL 'triple'" -DVSL_PROTOCOL=triple)
vsl_personality_test(protocol-and-personalities "Set VSL_PERSONALITIES or the deprecated VSL_PROTOCOL, not both"
        -DVSL_PROTOCOL=cmsis-dap -DVSL_PERSONALITIES=cmsis-dap)
vsl_personality_test(default-protocol-alone "VSL_DEFAULT_PROTOCOL is deprecated, and ignored"
        -DVSL_PERSONALITIES=versaloon -DVSL_DEFAULT_PROTOCOL=cmsis-dap)
