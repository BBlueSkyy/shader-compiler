#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
build=${SHADER_TEST_BUILD_DIR:-$root/build}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build" -j "${SHADER_TEST_JOBS:-4}"
ctest --test-dir "$build" --output-on-failure
for module in "$build"/modules/*.spv; do
    spirv-val --target-env vulkan1.1 "$module"
done

python3 "$root/check_spirv.py" "$build/modules"
