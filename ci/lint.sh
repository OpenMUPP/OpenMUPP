#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fails when clang-format changes a file. Used by the GitHub workflow.
set -e
cmake -S . -B clangformat-build -G "Unix Makefiles"
cmake --build clangformat-build --target clangformat
if ! git diff --quiet; then
    echo "Detected formatting issues; please fix"
    git diff
    exit 1
fi
