#!/bin/sh
# The Metal and Vulkan implementations export the same API symbols, so the
# encoder and decoder run in separate processes.
set -eu
: "${PYROWAVE_METAL_INCLUDE_DIR:?Set to the directory containing pyrowave_metal.h}"
: "${PYROWAVE_METAL_LIB_DIR:?Set to the directory containing libpyrowave-metal.dylib}"
: "${VULKAN_INCLUDE_DIR:?Set to the directory containing vulkan/vulkan.h}"
pkg-config --exists pyrowave-shared
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
common_dir="$repo_dir/moonlight-common-c/moonlight-common-c/src"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/moonlight-pyrowave-roundtrip.XXXXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -I"$common_dir" -c "$common_dir/PyrowaveProtocol.c" -o "$tmp_dir/protocol.o"
${CC:-cc} -std=c11 -I"$common_dir" -c "$common_dir/PyrowaveReassembly.c" -o "$tmp_dir/reassembly.o"
${CXX:-c++} -std=c++17 -I"$PYROWAVE_METAL_INCLUDE_DIR" \
    "$repo_dir/tests/pyrowave_roundtrip/metal_encode.cpp" -L"$PYROWAVE_METAL_LIB_DIR" \
    -lpyrowave-metal -Wl,-rpath,"$PYROWAVE_METAL_LIB_DIR" -o "$tmp_dir/encode"
# pkg-config's output contains compiler/linker arguments and intentionally uses shell splitting.
# shellcheck disable=SC2046
${CXX:-c++} -std=c++17 -I"$VULKAN_INCLUDE_DIR" -I"$common_dir" \
    $(pkg-config --cflags pyrowave-shared) "$repo_dir/tests/pyrowave_roundtrip/vulkan_decode.cpp" \
    "$tmp_dir/protocol.o" "$tmp_dir/reassembly.o" $(pkg-config --libs pyrowave-shared) \
    -Wl,-rpath,"$(pkg-config --variable=libdir pyrowave-shared)" -o "$tmp_dir/decode"
"$tmp_dir/encode" "$tmp_dir/frame.bin"
"$tmp_dir/decode" "$tmp_dir/frame.bin"
