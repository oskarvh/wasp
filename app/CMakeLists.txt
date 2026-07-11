cmake_minimum_required(VERSION 3.20.0)

# WAMR's zephyr module declares cmake-ext/kconfig-ext but ships no glue;
# supply it from app/modules/wamr (must be set before find_package(Zephyr)).
set(ZEPHYR_WASM_MICRO_RUNTIME_CMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/wamr)
set(ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG ${CMAKE_CURRENT_SOURCE_DIR}/modules/wamr/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wasp LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/net/net_thread.c
  src/agent/agent_thread.c
  src/wamr/wamr_runtime.c
)

target_include_directories(app PRIVATE src)
