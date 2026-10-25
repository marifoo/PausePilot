#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_dir/build/ci-clazy"
mkdir -p "$build_dir"
cd "$build_dir"

# Ubuntu 24.04's Clazy plugin is built against Clang 15, not the default
# Clang 18. Loading it into another Clang version can crash the compiler.
export CLANGXX=clang++-15
export CLAZY_CHECKS=level0
# Gate correctness checks; keep level-0 performance suggestions visible
# without requiring an unrelated application-wide cleanup to introduce CI.
export CLAZY_CHECKS_AS_ERRORS=connect-non-signal,lambda-in-connect,lambda-unique-connection,temporary-iterator,wrong-qevent-cast,writing-to-temporary

qmake "$repo_dir/uTimer.pro" -spec linux-clang \
    'QMAKE_CC=clang-15' \
    'QMAKE_CXX=clazy' \
    'QMAKE_LINK=clang++-15'
make -j2
