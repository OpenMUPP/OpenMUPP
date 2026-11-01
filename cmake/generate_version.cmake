# SPDX-License-Identifier: GPL-3.0-or-later
# Derives the firmware version from `git describe` and writes Version.h into the build directory.
# A tag vMAJOR.MINOR.BUILD gives the version numbers. Without such a tag (for example before the first
# release) the version is 0.0.0 and the describe output is the build revision.
find_package(Git)

execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE FIRMWARE_VERSION)

if (NOT FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "unknown")
endif ()

if (FIRMWARE_VERSION MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-(.+))?$")
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_BUILD ${CMAKE_MATCH_3})
    set(VERSION_BUILDREV "${CMAKE_MATCH_5}")
else ()
    set(VERSION_MAJOR 0)
    set(VERSION_MINOR 0)
    set(VERSION_BUILD 0)
    set(VERSION_BUILDREV "${FIRMWARE_VERSION}")
endif ()

if (VERSION_BUILDREV)
    set(VERSION_HAVE_BUILDREV "true")
else ()
    set(VERSION_HAVE_BUILDREV "false")
endif ()

configure_file(${CMAKE_CURRENT_LIST_DIR}/../Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/Version.h)
