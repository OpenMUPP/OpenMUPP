# SPDX-License-Identifier: GPL-3.0-or-later
# Central build configuration helper
# Creates an INTERFACE target with the passed NAME and attaches common
# compile definitions/options and link options based on variables defined
# by the including project (e.g., PROJECT_DEFINES, COMMON_OPTIONS, etc.).

function(art_setup_build_config NAME)
    if (NOT NAME)
        message(FATAL_ERROR "art_setup_build_config(NAME): NAME is required")
    endif ()

    if (TARGET ${NAME})
        return()
    endif ()

    add_library(${NAME} INTERFACE)

    # Compile definitions
    if (DEFINED PROJECT_DEFINES)
        target_compile_definitions(${NAME} INTERFACE ${PROJECT_DEFINES})
    endif ()
    if (DEFINED DEBUG_DEFINES)
        target_compile_definitions(${NAME} INTERFACE "$<$<CONFIG:Debug>:${DEBUG_DEFINES}>")
    endif ()

    # Compile options
    if (DEFINED COMMON_OPTIONS)
        target_compile_options(${NAME} INTERFACE ${COMMON_OPTIONS})
    endif ()
    if (DEFINED CXX_OPTIONS)
        target_compile_options(${NAME} INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:${CXX_OPTIONS}>")
    endif ()
    if (DEFINED C_OPTIONS)
        target_compile_options(${NAME} INTERFACE "$<$<COMPILE_LANGUAGE:C>:${C_OPTIONS}>")
    endif ()
    if (DEFINED DEBUG_OPTIONS)
        target_compile_options(${NAME} INTERFACE "$<$<CONFIG:Debug>:${DEBUG_OPTIONS}>")
    endif ()

    # Linker options (take effect on consumers that perform linking)
    if (DEFINED LINKER_OPTIONS)
        target_link_options(${NAME} INTERFACE ${LINKER_OPTIONS})
    endif ()

    # Optional includes to propagate
    if (DEFINED PROJECT_INCLUDE_DIRS OR DEFINED ADDITIONAL_INCLUDE_DIRS)
        target_include_directories(${NAME} INTERFACE
                ${PROJECT_INCLUDE_DIRS}
                ${ADDITIONAL_INCLUDE_DIRS}
        )
    endif ()
endfunction()
