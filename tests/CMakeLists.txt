# SPDX-License-Identifier: GPL-3.0-or-later

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

add_executable(mupp-tests
        test_main.cpp
        TestVersion.cpp
        TestBoards.cpp
        TestHeartbeat.cpp
        TestActivityLed.cpp
        TestErrorLed.cpp
        TestTargetPowerLed.cpp
        TestUptimeMicros.cpp
        TestIntDiv.cpp
        TestClockPacer.cpp
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
        TestRvDtmTap.cpp
        TestRvBridge.cpp
        TestWchLink.cpp
        TestWchLinkFlash.cpp
        TestPowerBlock.cpp
        TestDapTargetPower.cpp
        ../src/protocol/versaloon/CommandAssembler.cpp
        ../src/protocol/versaloon/CommandProcessor.cpp
        ../src/protocol/cmsisdap/DapJtag.cpp
        ../src/protocol/cmsisdap/DapProcessor.cpp
        ../src/protocol/cmsisdap/RvDtmTap.cpp
        ../src/protocol/cmsisdap/RvBridge.cpp
        ../src/protocol/wchlink/WchLinkFlash.cpp
        ../src/protocol/wchlink/WchLinkProcessor.cpp
        ../src/protocol/versaloon/GpioBlock.cpp
        ../src/protocol/versaloon/JtagRawBlock.cpp
        ../src/protocol/versaloon/PowerBlock.cpp
        ../src/protocol/TargetPower.cpp
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

target_compile_options(mupp-tests PRIVATE -O0 -g -Wall -Wextra)
# libArT code: not the code under test, keep its warnings out of the test build
set_source_files_properties(../dependencies/libArT/src/utils/tfp_printf.cpp PROPERTIES COMPILE_OPTIONS -Wno-unused-function)
target_include_directories(mupp-tests PRIVATE
        ../src
        ${PROJECT_BINARY_DIR}                 # Version.h
        ../dependencies/libArT/src            # USB device stack
        ../dependencies/libArT/tests/usb      # FakeDeviceController.h
        ../dependencies/system-stm32/src      # peripherals/UsbDeviceTypes.h
)
# All personalities, so the tests see the full personality list
target_compile_definitions(mupp-tests PRIVATE UNIT_TESTING MUPP_WITH_VERSALOON MUPP_WITH_CMSIS_DAP MUPP_WITH_WCH_LINK
        MUPP_MULTI_PERSONALITY MUPP_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay")
target_link_libraries(mupp-tests PRIVATE doctest::doctest etl::etl)

# Discover all tests
doctest_discover_tests(mupp-tests)

# The personality options of the firmware build (cmake/Personalities.cmake),
# in script mode. Each case passes when the output matches <regex>: the result
# line, or the text of the error or of the deprecation warning.
function(mupp_personality_test name regex)
    add_test(NAME personalities.${name}
            COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResolvePersonalities.cmake)
    set_tests_properties(personalities.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

mupp_personality_test(default "set=versaloon default=versaloon tag=\\[\\] defines=MUPP_WITH_VERSALOON\n")
mupp_personality_test(cmsis-dap "set=cmsis-dap default=cmsis-dap tag=\\[-cmsis-dap\\] defines=MUPP_WITH_CMSIS_DAP,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        -DMUPP_PERSONALITIES=cmsis-dap)
mupp_personality_test(wch-link "set=wch-link default=wch-link tag=\\[-wch-link\\] defines=MUPP_WITH_WCH_LINK\n"
        -DMUPP_PERSONALITIES=wch-link)
mupp_personality_test(wch-link-wch-id "tag=\\[-wch-link-wch-id\\] defines=MUPP_WITH_WCH_LINK,MUPP_WCH_LINK_WCH_USB_ID\n"
        -DMUPP_PERSONALITIES=wch-link -DMUPP_WCH_LINK_WCH_USB_ID=ON)
mupp_personality_test(no-rv-bridge "tag=\\[-cmsis-dap-no-rv-bridge\\] defines=MUPP_WITH_CMSIS_DAP\n"
        -DMUPP_PERSONALITIES=cmsis-dap -DMUPP_CMSIS_DAP_RV_BRIDGE=OFF)
mupp_personality_test(rv-bridge-multi "tag=\\[-vsl\\+dap\\] defines=.*,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        -DMUPP_PERSONALITIES=versaloon,cmsis-dap)
mupp_personality_test(no-rv-bridge-multi "tag=\\[-vsl\\+dap-no-rv-bridge\\] defines=.*MUPP_DEFAULT_PERSONALITY=0\n"
        -DMUPP_PERSONALITIES=versaloon,cmsis-dap -DMUPP_CMSIS_DAP_RV_BRIDGE=OFF)
# Ignored without cmsis-dap
mupp_personality_test(rv-bridge-wch-link "tag=\\[-wch-link\\] defines=MUPP_WITH_WCH_LINK\n"
        -DMUPP_PERSONALITIES=wch-link -DMUPP_CMSIS_DAP_RV_BRIDGE=ON)
# A CMake list, in any order: the result is in canonical order
mupp_personality_test(list "set=versaloon,cmsis-dap default=versaloon tag=\\[-vsl\\+dap\\] defines=MUPP_WITH_VERSALOON,MUPP_WITH_CMSIS_DAP,MUPP_MULTI_PERSONALITY,MUPP_DEFAULT_PERSONALITY=0,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        "-DMUPP_PERSONALITIES=cmsis-dap$<SEMICOLON>versaloon")
mupp_personality_test(dap-default "set=versaloon,cmsis-dap default=cmsis-dap tag=\\[-dap\\+vsl\\] .*MUPP_DEFAULT_PERSONALITY=1,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        -DMUPP_PERSONALITIES=versaloon,cmsis-dap -DMUPP_DEFAULT_PERSONALITY=cmsis-dap)
mupp_personality_test(triple "set=versaloon,cmsis-dap,wch-link default=versaloon tag=\\[-vsl\\+dap\\+wch\\] defines=MUPP_WITH_VERSALOON,MUPP_WITH_CMSIS_DAP,MUPP_WITH_WCH_LINK,MUPP_MULTI_PERSONALITY,MUPP_DEFAULT_PERSONALITY=0,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        "-DMUPP_PERSONALITIES=wch-link, versaloon ,cmsis-dap")
mupp_personality_test(dap-wch "set=cmsis-dap,wch-link default=wch-link tag=\\[-wch\\+dap\\] .*MUPP_DEFAULT_PERSONALITY=2,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        -DMUPP_PERSONALITIES=cmsis-dap,wch-link -DMUPP_DEFAULT_PERSONALITY=wch-link)
mupp_personality_test(multi-wch-id "tag=\\[-dap\\+wch-wch-id\\] .*MUPP_WCH_LINK_WCH_USB_ID,MUPP_CMSIS_DAP_RV_BRIDGE\n"
        -DMUPP_PERSONALITIES=cmsis-dap,wch-link -DMUPP_WCH_LINK_WCH_USB_ID=ON)
mupp_personality_test(duplicate "set=versaloon,cmsis-dap default=versaloon"
        -DMUPP_PERSONALITIES=versaloon,cmsis-dap,versaloon)
mupp_personality_test(unknown "Unknown personality 'avr'" -DMUPP_PERSONALITIES=versaloon,avr)
mupp_personality_test(empty "MUPP_PERSONALITIES has no personality" "-DMUPP_PERSONALITIES=,")
mupp_personality_test(default-not-in-set "MUPP_DEFAULT_PERSONALITY 'wch-link' is not in the personalities"
        -DMUPP_PERSONALITIES=versaloon,cmsis-dap -DMUPP_DEFAULT_PERSONALITY=wch-link)
mupp_personality_test(wch-id-without-wch-link "MUPP_WCH_LINK_WCH_USB_ID needs wch-link"
        -DMUPP_PERSONALITIES=versaloon -DMUPP_WCH_LINK_WCH_USB_ID=ON)
mupp_personality_test(debug-cdc-wch-link-only "only the wch-link personality has no CDC port"
        -DMUPP_PERSONALITIES=wch-link -DMUPP_DEBUG_CDC=ON)
mupp_personality_test(debug-cdc-multi-wch-link "set=versaloon,wch-link default=versaloon"
        -DMUPP_PERSONALITIES=versaloon,wch-link -DMUPP_DEBUG_CDC=ON)
mupp_personality_test(default-not-in-set-hint "-UMUPP_DEFAULT_PERSONALITY"
        -DMUPP_PERSONALITIES=cmsis-dap -DMUPP_DEFAULT_PERSONALITY=versaloon)

# The cache across configure runs: tests/cmake/fixture is configured in
# one build directory again and again, in this order. Each step needs the
# cache of the step before it.
set(_mupp_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/personalities-fixture")
set(_mupp_fixture_previous "")
function(mupp_personality_cache_test name regex)
    add_test(NAME personalities.cache.${name}
            COMMAND ${CMAKE_COMMAND} ${ARGN} -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fixture -B ${_mupp_fixture_dir})
    set_tests_properties(personalities.cache.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}"
            FIXTURES_SETUP mupp-cache-${name} FIXTURES_REQUIRED "${_mupp_fixture_previous}")
    set(_mupp_fixture_previous mupp-cache-${name} PARENT_SCOPE)
endfunction()
add_test(NAME personalities.cache.clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${_mupp_fixture_dir})
set_tests_properties(personalities.cache.clean PROPERTIES FIXTURES_SETUP mupp-cache-clean)
set(_mupp_fixture_previous mupp-cache-clean)
# A first configure without options caches the empty defaults
mupp_personality_cache_test(default "set=versaloon default=versaloon tag=\\[\\]")
# Then a set in the same directory
mupp_personality_cache_test(set "set=cmsis-dap default=cmsis-dap" -DMUPP_PERSONALITIES=cmsis-dap)
# A default that the new set does not have: an error with a hint
mupp_personality_cache_test(stale-default "-UMUPP_DEFAULT_PERSONALITY" -DMUPP_DEFAULT_PERSONALITY=cmsis-dap -DMUPP_PERSONALITIES=versaloon)
# An option with its old name: an error, not a different image
mupp_personality_cache_test(old-prefix "Rename VSL_PERSONALITIES" -DVSL_PERSONALITIES=wch-link)

# The size check of each image and the combined image (cmake/ImageSize.cmake,
# cmake/CombinedImage.cmake), in script mode on fake files. Each case passes
# when the output matches <regex>: the result line, or the warning text.
find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
function(mupp_image_size_test name regex)
    add_test(NAME image-size.${name}
            COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_BINARY_DIR}/image-size/${name}
                -DPYTHON=${Python3_EXECUTABLE} ${ARGN} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImageSizeCase.cmake)
    set_tests_properties(image-size.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

mupp_image_size_test(fits "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=size -DSIZE=100 -DLIMIT=100)
mupp_image_size_test(fits-strict "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=size -DSIZE=100 -DLIMIT=100 -DSTRICT=ON)
# One byte too large: the build skips the image and removes the old copies
mupp_image_size_test(skip "exit=0 image=missing copy=missing image-hex=missing copy-hex=missing\n" -DSCRIPT=size -DSIZE=101 -DLIMIT=100)
# CMake wraps the warning text: [ \n]+ matches a space or a line break
mupp_image_size_test(skip-warning "img is 101 bytes, 1 bytes larger than the limit of 100 bytes\\.[ \n]+The build[ \n]+skips this image"
        -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=OFF)
# The build stops: the script does not change the files
mupp_image_size_test(strict "exit=1 image=new copy=old image-hex=new copy-hex=old\n" -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=ON)
mupp_image_size_test(strict-error "MUPP_STRICT_SIZE is ON" -DSCRIPT=size -DSIZE=101 -DLIMIT=100 -DSTRICT=ON)
mupp_image_size_test(combined "exit=0 image=new copy=new image-hex=new copy-hex=new\n" -DSCRIPT=combined -DAPP=ON)
# No app-8k image: the build skips the combined image and removes the old one
mupp_image_size_test(combined-skip "exit=0 image=missing copy=missing image-hex=missing copy-hex=missing\n" -DSCRIPT=combined -DAPP=OFF)
mupp_image_size_test(combined-skip-warning "also[ \n]+skips the combined image" -DSCRIPT=combined -DAPP=OFF)
