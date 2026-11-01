#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Reports the flash size of a firmware image and fails when it is larger than
# the limit. The limit depends on the image: the build directory has the file
# mupp-images.txt with the limit of each image (CMakeLists.txt).
#
# Usage: ci/check-size.sh <image.bin> <limit-in-bytes>
set -eu

image="$1"
limit="$2"

size=$(wc -c < "$image")
percent=$((size * 100 / limit))
echo "Image size: ${size} bytes of ${limit} bytes (${percent} %): ${image}"

if [ "$size" -gt "$limit" ]; then
   echo "ERROR: the image is $((size - limit)) bytes larger than the limit" >&2
   exit 1
fi
