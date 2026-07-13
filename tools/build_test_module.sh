#!/bin/sh
# Build the test modules (test_module.c, remote_module.c) with clang + wasm-ld.
set -eu
cd "$(dirname "$0")"

build() {
	clang --target=wasm32 -O2 -nostdlib -Iinclude \
		-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
		-z stack-size=1024 \
		-o "$1.wasm" "$1.c"
	ls -l "$1.wasm"
}

build test_module
build remote_module
