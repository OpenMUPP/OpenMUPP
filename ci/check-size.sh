#!/bin/sh
# Reports the flash size of a firmware image and fails when it is larger than
# the limit. The default limit is 54 KB: the 64 KB of an STM32F103C8 minus the
# 10 KB of versaloon-bootloader.
#
# Usage: ci/check-size.sh <image.bin> [limit-in-bytes]
set -eu

image="$1"
limit="${2:-55296}"

size=$(wc -c < "$image")
percent=$((size * 100 / limit))
echo "Image size: ${size} bytes of ${limit} bytes (${percent} %): ${image}"

if [ "$size" -gt "$limit" ]; then
   echo "ERROR: the image is $((size - limit)) bytes larger than the limit" >&2
   exit 1
fi
