# Runs cmake/ImageSize.cmake or cmake/CombinedImage.cmake on fake image
# files in the directory DIR, and prints the result in one line.
# tests/CMakeLists.txt runs it for each test case and checks the line or the
# warning text.
#
# Usage: cmake -DDIR=<dir> -DSCRIPT=size|combined [-DSIZE=<bytes>]
#              [-DLIMIT=<bytes>] [-DSTRICT=ON] [-DAPP=ON] [-DPYTHON=<python3>]
#              -P ImageSizeCase.cmake
#
# size      writes img.bin with SIZE bytes, img.hex and old copies, then runs
#           ImageSize.cmake with LIMIT and STRICT
# combined  writes boot.bin, app.bin when APP is ON, and an old combined
#           image, then runs CombinedImage.cmake
#
# The line is "exit=<0|1> image=<state> copy=<state> image-hex=<state>
# copy-hex=<state>": the exit code of the script, and the state of the .bin
# and .hex files of the image and of the copy: missing, old (the file of the
# older build) or new.
cmake_minimum_required(VERSION 3.21)

set(_cmake_dir "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
if (SCRIPT STREQUAL "size")
    string(REPEAT "x" ${SIZE} _data)
    file(WRITE "${DIR}/img.bin" "${_data}")
    file(WRITE "${DIR}/img.hex" "hex")
    # Copies of an older build: a skipped image must not keep them
    file(WRITE "${DIR}/copy.bin" "old")
    file(WRITE "${DIR}/copy.hex" "old")
    set(_args -DIMAGE=img -DLIMIT=${LIMIT} -DCOPY=copy -DSTRICT=${STRICT}
            -P "${_cmake_dir}/ImageSize.cmake")
elseif (SCRIPT STREQUAL "combined")
    file(WRITE "${DIR}/boot.bin" "boot")
    if (APP)
        file(WRITE "${DIR}/app.bin" "app")
    endif ()
    file(WRITE "${DIR}/img.bin" "old")
    file(WRITE "${DIR}/img.hex" "old")
    file(WRITE "${DIR}/copy.bin" "old")
    file(WRITE "${DIR}/copy.hex" "old")
    set(_args -DPYTHON=${PYTHON} -DTOOL=${CMAKE_CURRENT_LIST_DIR}/../../tools/vsl_combine_images.py
            -DBOOTLOADER=boot.bin -DAPP=app.bin -DAPP_OFFSET=16 -DIMAGE=img -DCOPY=copy
            -P "${_cmake_dir}/CombinedImage.cmake")
else ()
    message(FATAL_ERROR "ImageSizeCase.cmake: unknown SCRIPT '${SCRIPT}'")
endif ()

execute_process(COMMAND ${CMAKE_COMMAND} ${_args} WORKING_DIRECTORY "${DIR}"
        RESULT_VARIABLE _exit OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
message("${_out}")
# The state of a .bin file: missing, old (from the older build) or new
function(_mupp_state file out)
    set(_state missing)
    if (EXISTS "${file}")
        file(READ "${file}" _data)
        if (_data STREQUAL "old")
            set(_state old)
        else ()
            set(_state new)
        endif ()
    endif ()
    set(${out} ${_state} PARENT_SCOPE)
endfunction()
_mupp_state("${DIR}/img.bin" _image)
_mupp_state("${DIR}/copy.bin" _copy)
_mupp_state("${DIR}/img.hex" _image_hex)
_mupp_state("${DIR}/copy.hex" _copy_hex)
if (NOT _exit EQUAL 0)
    set(_exit 1)
endif ()
message(STATUS "exit=${_exit} image=${_image} copy=${_copy} image-hex=${_image_hex} copy-hex=${_copy_hex}")
