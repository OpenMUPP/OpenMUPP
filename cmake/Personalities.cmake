# The USB personalities of the firmware image (docs/usb-device.md,
# src/personality/Personality.h).
#
# The table below has one vsl_personality() block per personality. A new
# personality (for example avr) is one more block. vsl_resolve_personalities()
# reads the build options, checks them and sets the defines, the sources to
# drop and the image tag. The host test build runs the checks in script mode
# (tests/cmake/ResolvePersonalities.cmake).

# vsl_personality(<name> CODE <code> SHORT <short> DEFINE <define> SOURCES <regex>...)
#   <name>    the value in MUPP_PERSONALITIES, for example cmsis-dap
#   CODE      the code of personality::Personality. The blocks are in code
#             order: this order is the canonical order.
#   SHORT     the short name in the tag of a multi image, for example dap
#   DEFINE    the MUPP_WITH_<P> define of the personality
#   SOURCES   the regular expressions of the sources that only this
#             personality uses. An image without the personality drops them.
function(vsl_personality name)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "CODE;SHORT;DEFINE" "SOURCES")
    list(LENGTH MUPP_ALL_PERSONALITIES _count)
    if (NOT "${_arg_CODE}" STREQUAL "${_count}")
        message(FATAL_ERROR "vsl_personality(${name}): CODE must be ${_count}. The blocks must be in code order.")
    endif ()
    list(APPEND MUPP_ALL_PERSONALITIES ${name})
    set(MUPP_ALL_PERSONALITIES ${MUPP_ALL_PERSONALITIES} PARENT_SCOPE)
    set(_mupp_personality_${name}_code ${_arg_CODE} PARENT_SCOPE)
    set(_mupp_personality_${name}_short ${_arg_SHORT} PARENT_SCOPE)
    set(_mupp_personality_${name}_define ${_arg_DEFINE} PARENT_SCOPE)
    set(_mupp_personality_${name}_sources ${_arg_SOURCES} PARENT_SCOPE)
endfunction()

set(MUPP_ALL_PERSONALITIES "")
#   versaloon  the Versaloon interface of OpenOCD
vsl_personality(versaloon CODE 0 SHORT vsl DEFINE MUPP_WITH_VERSALOON SOURCES
        "src/protocol/versaloon/.*\\.cpp"
        "src/usb/VersaloonFunction\\.cpp")
#   cmsis-dap  the CMSIS-DAP v2 interface of OpenOCD, pyOCD and probe-rs
vsl_personality(cmsis-dap CODE 1 SHORT dap DEFINE MUPP_WITH_CMSIS_DAP SOURCES
        "src/protocol/cmsisdap/DapJtag\\.cpp" "src/protocol/cmsisdap/DapProcessor\\.cpp"
        "src/protocol/cmsisdap/RvDtmTap\\.cpp" "src/protocol/cmsisdap/RvBridge\\.cpp"
        "src/usb/CmsisDapFunction\\.cpp")
#   wch-link   the WCH-Link interface of wlink and minichlink: the dongle
#              debugs a WCH RISC-V target (CH32V) over the two-wire or the
#              one-wire debug interface. It has no serial port.
# The RISC-V debug wire engine: the wch-link personality, and the RISC-V
# bridge of cmsis-dap (MUPP_CMSIS_DAP_RV_BRIDGE, on by default)
set(MUPP_RV_WIRE_SOURCES "src/board/BoardRvSwd\\.cpp" "src/board/BoardDebugModule\\.cpp")
vsl_personality(wch-link CODE 2 SHORT wch DEFINE MUPP_WITH_WCH_LINK SOURCES
        "src/protocol/wchlink/WchLinkFlash\\.cpp" "src/protocol/wchlink/WchLinkProcessor\\.cpp"
        "src/usb/WchLinkFunction\\.cpp" ${MUPP_RV_WIRE_SOURCES})

# Declares the cache entries of the personality options. Both are empty by
# default: empty MUPP_PERSONALITIES is versaloon.
function(vsl_personality_options)
    # CMake ignores an unknown option, so an old name would change the image
    # without an error.
    get_cmake_property(_cached CACHE_VARIABLES)
    list(FILTER _cached INCLUDE REGEX "^VSL_")
    if (_cached)
        message(FATAL_ERROR "The VSL_ options are now MUPP_ options. Rename ${_cached}. "
                "A build directory from before the rename needs --fresh.")
    endif ()
    set(MUPP_PERSONALITIES "" CACHE STRING
            "USB personalities of the image, a list with ';' or ','. Empty: versaloon. Supported: ${MUPP_ALL_PERSONALITIES}")
    set(MUPP_DEFAULT_PERSONALITY "" CACHE STRING
            "Personality of a multi image when no personality is stored. Empty: the first one of ${MUPP_ALL_PERSONALITIES}")
endfunction()

# Reads these variables of the caller:
#   MUPP_PERSONALITIES        the personalities of the image, a CMake list or
#                             a comma-separated list. Empty: versaloon.
#   MUPP_DEFAULT_PERSONALITY  the personality when no personality is stored.
#                             Empty: the first one in canonical order.
#   MUPP_WCH_LINK_WCH_USB_ID  the USB ID of WCH for the WCH-Link personality
#   MUPP_CMSIS_DAP_RV_BRIDGE  the RISC-V bridge of the CMSIS-DAP personality.
#                             Not defined: ON. Ignored without cmsis-dap.
#   MUPP_DEBUG_CDC            the debug log on the CDC port
# Sets these variables in the caller:
#   MUPP_PERSONALITY_SET         the personalities, in canonical order
#   MUPP_PERSONALITY_COUNT       the number of personalities
#   MUPP_DEFAULT_PERSONALITY_NAME, MUPP_DEFAULT_PERSONALITY_CODE
#   MUPP_PERSONALITY_DEFINES     the compile definitions
#   MUPP_PERSONALITY_DROPPED_SOURCES
#                                the source regexes of the other personalities
#   MUPP_IMAGE_TAG               the tag after the board in the file names
#   MUPP_RV_BRIDGE               ON when the image has the RISC-V bridge
function(vsl_resolve_personalities)
    set(_set "${MUPP_PERSONALITIES}")
    set(_default "${MUPP_DEFAULT_PERSONALITY}")

    # Commas instead of ';', because ';' is awkward in a shell and in a preset
    string(REPLACE "," ";" _set "${_set}")
    set(_requested "")
    foreach (_name IN LISTS _set)
        string(STRIP "${_name}" _name)
        if (_name STREQUAL "")
            continue()
        endif ()
        if (NOT _name IN_LIST MUPP_ALL_PERSONALITIES)
            message(FATAL_ERROR "Unknown personality '${_name}' in MUPP_PERSONALITIES. Supported: ${MUPP_ALL_PERSONALITIES}")
        endif ()
        list(APPEND _requested ${_name})
    endforeach ()
    if (_requested STREQUAL "")
        if (NOT "${MUPP_PERSONALITIES}" STREQUAL "")
            message(FATAL_ERROR "MUPP_PERSONALITIES has no personality. Supported: ${MUPP_ALL_PERSONALITIES}")
        endif ()
        set(_requested versaloon)
    endif ()

    # Canonical (code) order, whatever the order of MUPP_PERSONALITIES is
    set(_personalities "")
    foreach (_name IN LISTS MUPP_ALL_PERSONALITIES)
        if (_name IN_LIST _requested)
            list(APPEND _personalities ${_name})
        endif ()
    endforeach ()
    list(LENGTH _personalities _count)

    string(STRIP "${_default}" _default)
    if (_default STREQUAL "")
        list(GET _personalities 0 _default)
    elseif (NOT _default IN_LIST _personalities)
        message(FATAL_ERROR "MUPP_DEFAULT_PERSONALITY '${_default}' is not in the personalities of the image: ${_personalities}. "
                "Set it to one of them, or remove it from the cache with -UMUPP_DEFAULT_PERSONALITY.")
    endif ()

    if (MUPP_WCH_LINK_WCH_USB_ID AND NOT "wch-link" IN_LIST _personalities)
        message(FATAL_ERROR "MUPP_WCH_LINK_WCH_USB_ID needs wch-link in MUPP_PERSONALITIES.")
    endif ()
    # The bridge uses the pins and the timer of the wire engine in the JTAG
    # port of CMSIS-DAP. In a multi image only the active personality sets
    # them up (src/main.cpp, isActive()), so each image with cmsis-dap can
    # have the bridge.
    if (NOT DEFINED MUPP_CMSIS_DAP_RV_BRIDGE OR MUPP_CMSIS_DAP_RV_BRIDGE)
        set(_bridge ON)
    else ()
        set(_bridge OFF)
    endif ()
    set(_bridge_off_tag "")
    if (NOT "cmsis-dap" IN_LIST _personalities)
        set(_bridge OFF)
    elseif (NOT _bridge)
        set(_bridge_off_tag "-no-rv-bridge")
    endif ()
    # The log goes to the CDC port. In a multi image the log is silent while
    # the WCH-Link personality is active.
    if (MUPP_DEBUG_CDC AND _personalities STREQUAL "wch-link")
        message(FATAL_ERROR "An image with only the wch-link personality has no CDC port for the debug log. Set -DMUPP_DEBUG_CDC=OFF.")
    endif ()

    set(_defines "")
    set(_dropped "")
    foreach (_name IN LISTS MUPP_ALL_PERSONALITIES)
        if (_name IN_LIST _personalities)
            list(APPEND _defines ${_mupp_personality_${_name}_define})
        else ()
            list(APPEND _dropped ${_mupp_personality_${_name}_sources})
        endif ()
    endforeach ()
    set(_code ${_mupp_personality_${_default}_code})
    if (_count GREATER 1)
        list(APPEND _defines MUPP_MULTI_PERSONALITY MUPP_DEFAULT_PERSONALITY=${_code})
    endif ()
    if (MUPP_WCH_LINK_WCH_USB_ID)
        list(APPEND _defines MUPP_WCH_LINK_WCH_USB_ID)
    endif ()
    if (_bridge)
        list(APPEND _defines MUPP_CMSIS_DAP_RV_BRIDGE)
        list(REMOVE_ITEM _dropped ${MUPP_RV_WIRE_SOURCES})
    endif ()

    # Image tag (#79, decision Q2). A single image: "" for versaloon, else
    # "-<name>". A multi image: "-" and the short names joined by "+", the
    # default personality first, the others in canonical order.
    if (_count EQUAL 1)
        if (_default STREQUAL "versaloon")
            set(_tag "")
        else ()
            set(_tag "-${_default}")
        endif ()
    else ()
        set(_tag "-${_mupp_personality_${_default}_short}")
        foreach (_name IN LISTS _personalities)
            if (NOT _name STREQUAL _default)
                string(APPEND _tag "+${_mupp_personality_${_name}_short}")
            endif ()
        endforeach ()
    endif ()
    # The name tells the image apart: it must not be a release artifact
    if (MUPP_WCH_LINK_WCH_USB_ID)
        string(APPEND _tag "-wch-id")
    endif ()
    # The bridge is the default: only an image without it has a tag
    string(APPEND _tag "${_bridge_off_tag}")

    set(MUPP_PERSONALITY_SET ${_personalities} PARENT_SCOPE)
    set(MUPP_PERSONALITY_COUNT ${_count} PARENT_SCOPE)
    set(MUPP_DEFAULT_PERSONALITY_NAME ${_default} PARENT_SCOPE)
    set(MUPP_DEFAULT_PERSONALITY_CODE ${_code} PARENT_SCOPE)
    set(MUPP_PERSONALITY_DEFINES ${_defines} PARENT_SCOPE)
    set(MUPP_PERSONALITY_DROPPED_SOURCES ${_dropped} PARENT_SCOPE)
    set(MUPP_IMAGE_TAG "${_tag}" PARENT_SCOPE)
    set(MUPP_RV_BRIDGE ${_bridge} PARENT_SCOPE)
endfunction()
