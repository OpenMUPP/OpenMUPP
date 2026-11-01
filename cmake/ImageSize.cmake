# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the size of one application image after the link, in script mode.
# CMakeLists.txt runs it as the last post-build step of each image.
#
#   cmake -DIMAGE=<name> -DLIMIT=<bytes> -DCOPY=<name> [-DSTRICT=ON] -P ImageSize.cmake
#
# IMAGE is the image without the extension: <IMAGE>.bin and <IMAGE>.hex are
# in the current directory. LIMIT is the flash for the application, in bytes.
#
# When the image fits, the script copies <IMAGE>.bin and .hex to <COPY>.bin
# and .hex. When the image does not fit:
#   STRICT ON   the script fails (MUPP_STRICT_SIZE, CI)
#   STRICT OFF  the script removes the .bin and .hex files of the image and
#               the copies, and shows a warning. The build continues.
cmake_minimum_required(VERSION 3.21)

foreach (_arg IMAGE LIMIT COPY)
    if (NOT DEFINED ${_arg} OR "${${_arg}}" STREQUAL "")
        message(FATAL_ERROR "ImageSize.cmake: set -D${_arg}=...")
    endif ()
endforeach ()
if (NOT EXISTS "${IMAGE}.bin")
    message(FATAL_ERROR "ImageSize.cmake: ${IMAGE}.bin not found")
endif ()

file(SIZE "${IMAGE}.bin" _size)
if (_size GREATER LIMIT)
    math(EXPR _over "${_size} - ${LIMIT}")
    set(_text "${IMAGE} is ${_size} bytes, ${_over} bytes larger than the limit of ${LIMIT} bytes")
    if (STRICT)
        message(FATAL_ERROR "${_text}. MUPP_STRICT_SIZE is ON.")
    endif ()
    file(REMOVE "${IMAGE}.bin" "${IMAGE}.hex" "${COPY}.bin" "${COPY}.hex")
    message(WARNING "${_text}. The build skips this image: it has no .bin and no .hex file. "
            "Set MUPP_STRICT_SIZE=ON to stop the build instead.")
    return()
endif ()

file(COPY_FILE "${IMAGE}.bin" "${COPY}.bin")
file(COPY_FILE "${IMAGE}.hex" "${COPY}.hex")
message(STATUS "${IMAGE}: ${_size} bytes of ${LIMIT} bytes")
