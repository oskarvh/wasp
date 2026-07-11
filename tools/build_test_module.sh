#!/bin/sh
# Build tools/test_module.c into test_module.wasm using clang + wasm-ld.
set -eu
cd "$(dirname "$0")"

clang --target=wasm32 -O2 -nostdlib \
	-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
	-z stack-size=1024 \
	-o test_module.wasm test_module.c

ls -l test_module.wasm
