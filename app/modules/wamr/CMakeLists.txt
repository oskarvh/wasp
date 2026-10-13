# CMake glue for the wasm-micro-runtime Zephyr module. WAMR declares
# `cmake-ext: True` in its module.yml but ships no glue of its own;
# app/CMakeLists.txt points ZEPHYR_WASM_MICRO_RUNTIME_CMAKE_DIR here.
#
# Builds WAMR's vmlib as a Zephyr library from the sources selected by
# WAMR's own runtime_lib.cmake.

if(CONFIG_WAMR)

set(WAMR_ROOT_DIR ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR})

set(WAMR_BUILD_PLATFORM "zephyr")

# Pick the WAMR target from the Zephyr CPU/FPU configuration so new boards
# work without touching this file.
if(CONFIG_CPU_CORTEX_M)
  if(CONFIG_ARMV6_M_ARMV8_M_BASELINE)
    # Cortex-M0/M0+/M1 (e.g. RP2040): generic soft-float thumb target.
    set(WAMR_BUILD_TARGET "THUMB")
  elseif(CONFIG_FPU AND CONFIG_FP_HARDABI)
    set(WAMR_BUILD_TARGET "THUMBV7EM_VFP")
  else()
    set(WAMR_BUILD_TARGET "THUMBV7EM")
  endif()
elseif(CONFIG_RISCV AND CONFIG_64BIT)
  set(WAMR_BUILD_TARGET "RISCV64")
elseif(CONFIG_RISCV)
  set(WAMR_BUILD_TARGET "RISCV32")
elseif(CONFIG_XTENSA)
  set(WAMR_BUILD_TARGET "XTENSA")
else()
  message(FATAL_ERROR "wasp: no WAMR_BUILD_TARGET mapping for this CPU — add one in ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_MINI_LOADER 0)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

zephyr_library_named(wamr)
zephyr_library_sources(${WAMR_RUNTIME_LIB_SOURCE})

#
# Optimise the interpreter for speed even when the rest of the image is
# built for size, which is Zephyr's default and the right default for
# everything else here.
#
# This is not a guess. The mandelbrot demo is compute-bound in the
# interpreter and nowhere near the network: doubling the iteration count
# doubles the run time, while doubling the number of remote-memory round
# trips (half-height tiles, same pixels) changes it by less than 5%. So
# whatever the interpreter's dispatch loop costs is essentially the whole
# bill.
#
# Measured on one wasp node board, 256x192 at 96 iterations: 4.3 s with
# the library at -Os against 2.3 s at -O2, a 1.87x speedup from a
# compiler flag. Building the whole image -O2 achieves the same thing but
# costs ~33 KB of flash to do it; confining it to this library gets the
# win where it is actually earned.
#
# WAMR_BUILD_FAST_INTERP is already on, so this stacks on top of the
# faster of WAMR's two interpreters rather than papering over the slower.
#
# Set as a source property rather than with target_compile_options: the
# -Os comes from zephyr_interface, whose INTERFACE options are appended
# after a target's own PRIVATE ones, so target_compile_options yields
# "-O2 -Os" and gcc takes the last. Source properties are emitted later
# still, which puts -O2 where it wins. Check with:
#   grep -o '\-O[a-z0-9]*' build/<d>/compile_commands.json | ...
if(CONFIG_SIZE_OPTIMIZATIONS)
  set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE}
    TARGET_DIRECTORY wamr
    PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# Public runtime API (wasm_export.h) for the application.
zephyr_include_directories(${WAMR_ROOT_DIR}/core/iwasm/include)

endif()
