#!/usr/bin/env bash
# Build the production app before apptest, preserving its sibling binary path.
# Any arguments are forwarded to qmake for all three targets.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${PAUSEPILOT_CI_BUILD_DIR:-$repo_dir/build/ci}"

export QT_QPA_PLATFORM=offscreen
export XDG_RUNTIME_DIR="${RUNNER_TEMP:-$build_dir}/pausepilot-runtime"
install -d -m 700 "$XDG_RUNTIME_DIR"

build_target() {
    local output_dir="$1"
    local project_file="$2"
    shift 2
    mkdir -p "$output_dir"
    (
        cd "$output_dir"
        qmake "$project_file" "$@"
        make -j2
    )
}

build_target "$build_dir" "$repo_dir/PausePilot.pro" "$@"
build_target "$build_dir/qtest" "$repo_dir/qtest/qtest.pro" "$@"
"$build_dir/qtest/qtest"
build_target "$build_dir/apptest" "$repo_dir/apptest/apptest.pro" "$@"
"$build_dir/apptest/apptest"
