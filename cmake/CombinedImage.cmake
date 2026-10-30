# Makes the combined image of openmupp-bootloader and the app-8k image, in
# script mode. CMakeLists.txt runs it as the command of the target
# versaloon-ng-with-bootloader.
#
#   cmake -DPYTHON=<python3> -DTOOL=<vsl_combine_images.py> -DBOOTLOADER=<bin>
#         -DAPP=<bin> -DAPP_OFFSET=<bytes> -DIMAGE=<name> -DCOPY=<name>
#         -P CombinedImage.cmake
#
# The script writes <IMAGE>.bin and .hex, and copies them to <COPY>.bin and
# .hex. When the build skipped the app-8k image (cmake/ImageSize.cmake), APP
# does not exist. Then the script removes the old combined image and its
# copies, and shows a warning. The build continues.
cmake_minimum_required(VERSION 3.21)

foreach (_arg PYTHON TOOL BOOTLOADER APP APP_OFFSET IMAGE COPY)
    if (NOT DEFINED ${_arg} OR "${${_arg}}" STREQUAL "")
        message(FATAL_ERROR "CombinedImage.cmake: set -D${_arg}=...")
    endif ()
endforeach ()

if (NOT EXISTS "${APP}")
    file(REMOVE "${IMAGE}.bin" "${IMAGE}.hex" "${COPY}.bin" "${COPY}.hex")
    message(WARNING "${APP} not found: the build skipped the app-8k image. "
            "The build also skips the combined image ${IMAGE}.")
    return()
endif ()

execute_process(
        COMMAND "${PYTHON}" "${TOOL}" --bootloader "${BOOTLOADER}" --app "${APP}" --app-offset ${APP_OFFSET}
            --bin "${IMAGE}.bin" --hex "${IMAGE}.hex"
        COMMAND_ERROR_IS_FATAL ANY)
file(COPY_FILE "${IMAGE}.bin" "${COPY}.bin")
file(COPY_FILE "${IMAGE}.hex" "${COPY}.hex")
