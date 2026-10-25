#!/usr/bin/env bash
# Instrument the production app and both test runners with Clang 18.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export PAUSEPILOT_CI_BUILD_DIR="$repo_dir/build/ci-sanitizers"
mkdir -p "$PAUSEPILOT_CI_BUILD_DIR"
report_dir="$(mktemp -d "$PAUSEPILOT_CI_BUILD_DIR/sanitizer-reports.XXXXXX")"

# QProcess children can capture diagnostics or intentionally exit nonzero.
# Fail on their sanitizer reports too, even when the parent suite passes.
report_sanitizers() {
    local status=$?
    shopt -s nullglob
    local reports=("$report_dir"/asan.* "$report_dir"/ubsan.*)
    if (( ${#reports[@]} > 0 )); then
        cat -- "${reports[@]}"
        status=1
    fi
    exit "$status"
}
trap report_sanitizers EXIT

# Qt's offscreen plugin retains process-lifetime allocations. This job
# checks invalid memory access and undefined behavior, excluding leak checks.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:log_path=$report_dir/asan"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$report_dir/ubsan"
ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer-18)"
export ASAN_SYMBOLIZER_PATH

bash "$repo_dir/scripts/ci-linux.sh" \
    -spec linux-clang \
    'QMAKE_CC=clang-18' \
    'QMAKE_CXX=clang++-18' \
    'QMAKE_LINK=clang++-18' \
    'CONFIG+=debug' \
    'CONFIG-=release' \
    'QMAKE_CXXFLAGS+=-fsanitize=address,undefined -fno-omit-frame-pointer' \
    'QMAKE_LFLAGS+=-fsanitize=address,undefined'
