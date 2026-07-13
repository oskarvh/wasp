cmake_minimum_required(VERSION 3.20.0)

# WAMR's zephyr module declares cmake-ext/kconfig-ext but ships no glue;
# supply it from app/modules/wamr (must be set before find_package(Zephyr)).
set(ZEPHYR_WASM_MICRO_RUNTIME_CMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/wamr)
set(ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG ${CMAKE_CURRENT_SOURCE_DIR}/modules/wamr/Kconfig)

# Local WiFi credentials (gitignored; copy wifi_credentials.conf.example).
# Merged automatically so build commands stay the same on every board.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/wifi_credentials.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/wifi_credentials.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wasp LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/rpc.c
  src/net/net_thread.c
  src/agent/agent_thread.c
  src/wamr/wamr_runtime.c
  src/wamr/native_remote.c
)

target_include_directories(app PRIVATE src)
