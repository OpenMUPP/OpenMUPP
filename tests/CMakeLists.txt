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
        TestIntDiv.cpp
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
        TestFlashWordSlot.cpp
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
        ../src/protocol/versaloon/CommandAssembler.cpp
        ../src/protocol/versaloon/CommandProcessor.cpp
        ../src/protocol/cmsisdap/DapJtag.cpp
        ../src/protocol/cmsisdap/DapProcessor.cpp
        ../src/protocol/wchlink/WchLinkFlash.cpp
        ../src/protocol/wchlink/WchLinkProcessor.cpp
        ../src/protocol/versaloon/GpioBlock.cpp
        ../src/protocol/versaloon/JtagRawBlock.cpp
        ../src/protocol/versaloon/PowerBlock.cpp
        ../src/protocol/versaloon/SwdBlock.cpp
        ../src/protocol/versaloon/UsbToAll.cpp
        ../src/protocol/versaloon/UsbToXxx.cpp
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
vsl_personality_test(debug-cdc-wch-link-only "only the wch-link personality has no CDC port"
        -DVSL_PERSONALITIES=wch-link -DVSL_DEBUG_CDC=ON)
vsl_personality_test(debug-cdc-multi-wch-link "set=versaloon,wch-link default=versaloon"
        -DVSL_PERSONALITIES=versaloon,wch-link -DVSL_DEBUG_CDC=ON)
vsl_personality_test(default-not-in-set-hint "-UVSL_DEFAULT_PERSONALITY"
        -DVSL_PERSONALITIES=cmsis-dap -DVSL_DEFAULT_PERSONALITY=versaloon)

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
vsl_personality_test(default-protocol-unknown "Unknown VSL_DEFAULT_PROTOCOL 'wch-link'"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=wch-link)
vsl_personality_test(default-protocol-and-personality "Set VSL_DEFAULT_PERSONALITY or the deprecated VSL_DEFAULT_PROTOCOL"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=versaloon -DVSL_DEFAULT_PERSONALITY=cmsis-dap)
vsl_personality_test(default-protocol-and-personality-same "set=versaloon,cmsis-dap default=cmsis-dap"
        -DVSL_PROTOCOL=dual -DVSL_DEFAULT_PROTOCOL=cmsis-dap -DVSL_DEFAULT_PERSONALITY=cmsis-dap)

# The cache across configure runs: tests/cmake/fixture is configured in
# one build directory again and again, in this order. Each step needs the
# cache of the step before it.
set(_vsl_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/personalities-fixture")
set(_vsl_fixture_previous "")
function(vsl_personality_cache_test name regex)
    add_test(NAME personalities.cache.${name}
            COMMAND ${CMAKE_COMMAND} ${ARGN} -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fixture -B ${_vsl_fixture_dir})
    set_tests_properties(personalities.cache.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}"
            FIXTURES_SETUP vsl-cache-${name} FIXTURES_REQUIRED "${_vsl_fixture_previous}")
    set(_vsl_fixture_previous vsl-cache-${name} PARENT_SCOPE)
endfunction()
add_test(NAME personalities.cache.clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${_vsl_fixture_dir})
set_tests_properties(personalities.cache.clean PROPERTIES FIXTURES_SETUP vsl-cache-clean)
set(_vsl_fixture_previous vsl-cache-clean)
# A first configure without options caches the empty defaults
vsl_personality_cache_test(default "set=versaloon default=versaloon tag=\\[\\]")
# Then the deprecated option in the same directory: no error
vsl_personality_cache_test(protocol "set=versaloon,cmsis-dap default=versaloon tag=\\[-vsl\\+dap\\]" -DVSL_PROTOCOL=dual)
# The new option while VSL_PROTOCOL is still in the cache: an error
vsl_personality_cache_test(both "not both" -DVSL_PERSONALITIES=cmsis-dap)
# Removed from the cache: the new option works
vsl_personality_cache_test(unset "set=cmsis-dap default=cmsis-dap" -UVSL_PROTOCOL -DVSL_PERSONALITIES=cmsis-dap)
# A default that the new set does not have: an error with a hint
vsl_personality_cache_test(stale-default "-UVSL_DEFAULT_PERSONALITY" -DVSL_DEFAULT_PERSONALITY=cmsis-dap -DVSL_PERSONALITIES=versaloon)

# The size check of each image and the combined image (cmake/ImageSize.cmake,
# cmake/CombinedImage.cmake), in script mode on fake files. Each case passes
# when the output matches <regex>: the result line, or the warning text.
find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
function(vsl_image_size_test name regex)
    add_test(NAME image-size.${name}
            COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_BINARY_DIR}/image-size/${name}
                -DPYTHON=${Python3_EXECUTABLE} ${ARGN} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImageSizeCase.cmake)
    set_tests_properties(image-size.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

vsl_image_size_test(fits "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=size -DSIZE=100 -DLIMIT=100)
vsl_image_size_test(fits-strict "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=size -DSIZE=100 -DLIMIT=100 -DSTRICT=ON)
# One byte too large: the build skips the image and removes the old copies
vsl_image_size_test(skip "exit=0 image=missing copy=missing image-hex=missing copy-hex=missing\n" -DSCRIPT=size -DSIZE=101 -DLIMIT=100)
# CMake wraps the warning text: [ \n]+ matches a space or a line break
vsl_image_size_test(skip-warning "img is 101 bytes, 1 bytes larger than the limit of 100 bytes\\.[ \n]+The build[ \n]+skips this image"
        -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=OFF)
# The build stops: the script does not change the files
vsl_image_size_test(strict "exit=1 image=new copy=old image-hex=new copy-hex=old\n" -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=ON)
vsl_image_size_test(strict-error "VSL_STRICT_SIZE is ON" -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=ON)
vsl_image_size_test(combined "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=combined -DAPP=ON)
# No app-8k image: the build skips the combined image and removes the old one
vsl_image_size_test(combined-skip "exit=0 image=missing copy=missing image-hex=missing copy-hex=missing\n" -DSCRIPT=combined -DAPP=OFF)
vsl_image_size_test(combined-skip-warning "also[ \n]+skips the combined image" -DSCRIPT=combined -DAPP=OFF)
