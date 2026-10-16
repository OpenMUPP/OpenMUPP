# The USB personalities of the firmware image (docs/usb-device.md,
# src/personality/Personality.h).
#
# The table below has one vsl_personality() block per personality. A new
# personality (for example avr) is one more block. vsl_resolve_personalities()
# reads the build options, checks them and sets the defines, the sources to
# drop and the image tag. The host test build runs the checks in script mode
# (tests/cmake/ResolvePersonalities.cmake).

# vsl_personality(<name> CODE <code> SHORT <short> DEFINE <define> SOURCES <regex>...)
#   <name>    the value in VSL_PERSONALITIES, for example cmsis-dap
#   CODE      the code of personality::Personality. The blocks are in code
#             order: this order is the canonical order.
#   SHORT     the short name in the tag of a multi image, for example dap
#   DEFINE    the VSL_WITH_<P> define of the personality
#   SOURCES   the regular expressions of the sources that only this
#             personality uses. An image without the personality drops them.
function(vsl_personality name)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "CODE;SHORT;DEFINE" "SOURCES")
    list(LENGTH VSL_ALL_PERSONALITIES _count)
    if (NOT "${_arg_CODE}" STREQUAL "${_count}")
        message(FATAL_ERROR "vsl_personality(${name}): CODE must be ${_count}. The blocks must be in code order.")
    endif ()
    list(APPEND VSL_ALL_PERSONALITIES ${name})
    set(VSL_ALL_PERSONALITIES ${VSL_ALL_PERSONALITIES} PARENT_SCOPE)
    set(_vsl_personality_${name}_code ${_arg_CODE} PARENT_SCOPE)
    set(_vsl_personality_${name}_short ${_arg_SHORT} PARENT_SCOPE)
    set(_vsl_personality_${name}_define ${_arg_DEFINE} PARENT_SCOPE)
    set(_vsl_personality_${name}_sources ${_arg_SOURCES} PARENT_SCOPE)
endfunction()

set(VSL_ALL_PERSONALITIES "")
#   versaloon  the Versaloon interface of OpenOCD
vsl_personality(versaloon CODE 0 SHORT vsl DEFINE VSL_WITH_VERSALOON SOURCES
        "src/protocol/CommandAssembler\\.cpp" "src/protocol/CommandProcessor\\.cpp"
        "src/protocol/UsbToAll\\.cpp" "src/protocol/UsbToXxx\\.cpp"
        "src/protocol/GpioBlock\\.cpp" "src/protocol/JtagRawBlock\\.cpp"
        "src/protocol/PowerBlock\\.cpp" "src/protocol/SwdBlock\\.cpp"
        "src/usb/VersaloonFunction\\.cpp" "src/boot/Bootloader\\.cpp")
#   cmsis-dap  the CMSIS-DAP v2 interface of OpenOCD, pyOCD and probe-rs
vsl_personality(cmsis-dap CODE 1 SHORT dap DEFINE VSL_WITH_CMSIS_DAP SOURCES
        "src/protocol/cmsisdap/DapJtag\\.cpp" "src/protocol/cmsisdap/DapProcessor\\.cpp"
        "src/usb/CmsisDapFunction\\.cpp")
#   wch-link   the WCH-Link interface of wlink and minichlink: the dongle
#              debugs a WCH RISC-V target (CH32V) over the two-wire or the
#              one-wire debug interface. It has no serial port.
vsl_personality(wch-link CODE 2 SHORT wch DEFINE VSL_WITH_WCH_LINK SOURCES
        "src/protocol/wchlink/WchLinkFlash\\.cpp" "src/protocol/wchlink/WchLinkProcessor\\.cpp"
        "src/usb/WchLinkFunction\\.cpp" "src/board/BoardRvSwd\\.cpp" "src/board/BoardDebugModule\\.cpp")

# The values of the deprecated VSL_PROTOCOL option
set(VSL_PROTOCOLS versaloon cmsis-dap dual wch-link)

# Reads these variables of the caller:
#   VSL_PERSONALITIES        the personalities of the image, a CMake list or
#                            a comma-separated list. Empty: versaloon.
#   VSL_DEFAULT_PERSONALITY  the personality when no personality is stored.
#                            Empty: the first one in canonical order.
#   VSL_PROTOCOL, VSL_DEFAULT_PROTOCOL
#                            deprecated aliases, see below
#   VSL_WCH_LINK_WCH_USB_ID  the USB ID of WCH for the WCH-Link personality
# Sets these variables in the caller:
#   VSL_PERSONALITY_SET         the personalities, in canonical order
#   VSL_PERSONALITY_COUNT       the number of personalities
#   VSL_DEFAULT_PERSONALITY_NAME, VSL_DEFAULT_PERSONALITY_CODE
#   VSL_PERSONALITY_DEFINES     the compile definitions
#   VSL_PERSONALITY_DROPPED_SOURCES
#                               the source regexes of the other personalities
#   VSL_IMAGE_TAG               the tag after the board in the file names
function(vsl_resolve_personalities)
    set(_set "${VSL_PERSONALITIES}")
    set(_default "${VSL_DEFAULT_PERSONALITY}")

    # VSL_PROTOCOL is a deprecated alias for one release (#79, decision Q1)
    if (NOT "${VSL_PROTOCOL}" STREQUAL "")
        if (NOT "${_set}" STREQUAL "")
            message(FATAL_ERROR "Set VSL_PERSONALITIES or the deprecated VSL_PROTOCOL, not both. "
                    "Remove VSL_PROTOCOL from the cache with -UVSL_PROTOCOL.")
        endif ()
        if (NOT VSL_PROTOCOL IN_LIST VSL_PROTOCOLS)
            message(FATAL_ERROR "Unknown VSL_PROTOCOL '${VSL_PROTOCOL}'. Supported protocols: ${VSL_PROTOCOLS}")
        endif ()
        if (VSL_PROTOCOL STREQUAL "dual")
            set(_set versaloon cmsis-dap)
            if (NOT "${VSL_DEFAULT_PROTOCOL}" STREQUAL "")
                if (NOT VSL_DEFAULT_PROTOCOL IN_LIST _set)
                    message(FATAL_ERROR "Unknown VSL_DEFAULT_PROTOCOL '${VSL_DEFAULT_PROTOCOL}'. Supported: ${_set}")
                endif ()
                if (NOT "${_default}" STREQUAL "" AND NOT _default STREQUAL VSL_DEFAULT_PROTOCOL)
                    message(FATAL_ERROR "Set VSL_DEFAULT_PERSONALITY or the deprecated VSL_DEFAULT_PROTOCOL, not both. "
                            "Remove VSL_DEFAULT_PROTOCOL from the cache with -UVSL_DEFAULT_PROTOCOL.")
                endif ()
                set(_default "${VSL_DEFAULT_PROTOCOL}")
            endif ()
        else ()
            set(_set "${VSL_PROTOCOL}")
        endif ()
        string(REPLACE ";" "," _set_text "${_set}")
        set(_hint "-DVSL_PERSONALITIES=${_set_text}")
        if (VSL_PROTOCOL STREQUAL "dual" AND NOT "${_default}" STREQUAL "")
            string(APPEND _hint " -DVSL_DEFAULT_PERSONALITY=${_default}")
        endif ()
        message(DEPRECATION "VSL_PROTOCOL is deprecated. Use ${_hint} instead, "
                "and remove VSL_PROTOCOL and VSL_DEFAULT_PROTOCOL with -UVSL_PROTOCOL -UVSL_DEFAULT_PROTOCOL.")
    elseif (NOT "${VSL_DEFAULT_PROTOCOL}" STREQUAL "")
        message(DEPRECATION "VSL_DEFAULT_PROTOCOL is deprecated, and ignored without VSL_PROTOCOL. "
                "Use VSL_DEFAULT_PERSONALITY, and remove VSL_DEFAULT_PROTOCOL with -UVSL_DEFAULT_PROTOCOL.")
    endif ()

    # Commas instead of ';', because ';' is awkward in a shell and in a preset
    string(REPLACE "," ";" _set "${_set}")
    set(_requested "")
    foreach (_name IN LISTS _set)
        string(STRIP "${_name}" _name)
        if (_name STREQUAL "")
            continue()
        endif ()
        if (NOT _name IN_LIST VSL_ALL_PERSONALITIES)
            message(FATAL_ERROR "Unknown personality '${_name}' in VSL_PERSONALITIES. Supported: ${VSL_ALL_PERSONALITIES}")
        endif ()
        list(APPEND _requested ${_name})
    endforeach ()
    if (_requested STREQUAL "")
        if (NOT "${VSL_PERSONALITIES}" STREQUAL "")
            message(FATAL_ERROR "VSL_PERSONALITIES has no personality. Supported: ${VSL_ALL_PERSONALITIES}")
        endif ()
        set(_requested versaloon)
    endif ()

    # Canonical (code) order, whatever the order of VSL_PERSONALITIES is
    set(_personalities "")
    foreach (_name IN LISTS VSL_ALL_PERSONALITIES)
        if (_name IN_LIST _requested)
            list(APPEND _personalities ${_name})
        endif ()
    endforeach ()
    list(LENGTH _personalities _count)

    string(STRIP "${_default}" _default)
    if (_default STREQUAL "")
        list(GET _personalities 0 _default)
    elseif (NOT _default IN_LIST _personalities)
        message(FATAL_ERROR "VSL_DEFAULT_PERSONALITY '${_default}' is not in the personalities of the image: ${_personalities}")
    endif ()

    if (VSL_WCH_LINK_WCH_USB_ID AND NOT "wch-link" IN_LIST _personalities)
        message(FATAL_ERROR "VSL_WCH_LINK_WCH_USB_ID needs wch-link in VSL_PERSONALITIES.")
    endif ()

    set(_defines "")
    set(_dropped "")
    foreach (_name IN LISTS VSL_ALL_PERSONALITIES)
        if (_name IN_LIST _personalities)
            list(APPEND _defines ${_vsl_personality_${_name}_define})
        else ()
            list(APPEND _dropped ${_vsl_personality_${_name}_sources})
        endif ()
    endforeach ()
    set(_code ${_vsl_personality_${_default}_code})
    if (_count GREATER 1)
        list(APPEND _defines VSL_MULTI_PERSONALITY VSL_DEFAULT_PERSONALITY=${_code})
    endif ()
    if (VSL_WCH_LINK_WCH_USB_ID)
        list(APPEND _defines VSL_WCH_LINK_WCH_USB_ID)
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
        set(_tag "-${_vsl_personality_${_default}_short}")
        foreach (_name IN LISTS _personalities)
            if (NOT _name STREQUAL _default)
                string(APPEND _tag "+${_vsl_personality_${_name}_short}")
            endif ()
        endforeach ()
    endif ()
    # The name tells the image apart: it must not be a release artifact
    if (VSL_WCH_LINK_WCH_USB_ID)
        string(APPEND _tag "-wch-id")
    endif ()

    set(VSL_PERSONALITY_SET ${_personalities} PARENT_SCOPE)
    set(VSL_PERSONALITY_COUNT ${_count} PARENT_SCOPE)
    set(VSL_DEFAULT_PERSONALITY_NAME ${_default} PARENT_SCOPE)
    set(VSL_DEFAULT_PERSONALITY_CODE ${_code} PARENT_SCOPE)
    set(VSL_PERSONALITY_DEFINES ${_defines} PARENT_SCOPE)
    set(VSL_PERSONALITY_DROPPED_SOURCES ${_dropped} PARENT_SCOPE)
    set(VSL_IMAGE_TAG "${_tag}" PARENT_SCOPE)
endfunction()
