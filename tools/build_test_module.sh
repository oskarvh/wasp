#!/bin/sh
# Build all test modules with clang + wasm-ld, and the wasp-remote LLVM
# pass plugin they need (address-space-100 lowering for transparent
# remote pointers in C).
set -eu
cd "$(dirname "$0")"

CLANG=${CLANG:-clang-18}
CLANGXX=${CLANGXX:-clang++-18}
LLVM_CONFIG=${LLVM_CONFIG:-llvm-config-18}
PLUGIN=wasp-remote-pass/libWaspRemotePass.so

WASM_FLAGS="--target=wasm32 -O2 -nostdlib -Iinclude \
	-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
	-z stack-size=1024"

# Rebuild the pass plugin only when its source is newer.
if [ ! -f "$PLUGIN" ] || [ wasp-remote-pass/WaspRemotePass.cpp -nt "$PLUGIN" ]; then
	echo "building wasp-remote pass plugin..."
	$CLANGXX -shared -fPIC $($LLVM_CONFIG --cxxflags) \
		wasp-remote-pass/WaspRemotePass.cpp -o "$PLUGIN"
fi

build() {
	$CLANG $WASM_FLAGS -o "$1.wasm" "$1.c"
	ls -l "$1.wasm"
}

# C with transparent remote pointers: pass plugin + runtime shims.
build_as() {
	$CLANG $WASM_FLAGS -fpass-plugin="$PLUGIN" \
		-o "$1.wasm" "$1.c" lib/wasp_remote_rt.c
	ls -l "$1.wasm"
}

# C++ (remote.hpp needs no plugin and no shims).
build_cpp() {
	$CLANGXX $WASM_FLAGS -fno-exceptions -fno-rtti \
		-o "$1.wasm" "$1.cpp"
	ls -l "$1.wasm"
}

build test_module
build remote_module
build_as remote_as_module
build_cpp remote_cpp_module
