#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds a configure preset and checks the size of each image. Used by the GitHub workflow.
set -e
preset=$1
cmake --preset "$preset" -G "Unix Makefiles" -DMUPP_STRICT_SIZE=ON
cmake --build --preset "$preset" -- -j"$(nproc)"
while read -r image limit; do
    ci/check-size.sh "build/$preset/$image" "$limit"
done <"build/$preset/mupp-images.txt"
