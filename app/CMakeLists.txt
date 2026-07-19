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
  src/led.c
  src/rpc.c
  src/net/net_thread.c
  src/net/announce.c
  src/agent/agent_thread.c
  src/wamr/wamr_runtime.c
  src/wamr/native_remote.c
)

target_include_directories(app PRIVATE src)

# Peer (job-owner) role: the libwasp core compiled into the firmware.
if(CONFIG_WASP_PEER)
  target_sources(app PRIVATE
    ../libwasp/src/core.c
    ../libwasp/src/host.c
    ../libwasp/src/port_zephyr.c
  )
  target_include_directories(app PRIVATE ../libwasp/include ../libwasp/src)
  # MCU-sized tables: 8 regions (~0.6 KiB) and a 32-entry event queue.
  target_compile_definitions(app PRIVATE WASP_MAX_REGIONS=8 WASP_EVQ_CAP=32)
endif()

if(CONFIG_WASP_PEER_SMOKE)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/remote_module.wasm)
    message(FATAL_ERROR "WASP_PEER_SMOKE needs tools/remote_module.wasm — run tools/build_test_module.sh")
  endif()
  target_sources(app PRIVATE src/peer/peer_smoke.c)
  generate_inc_file_for_target(app
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/remote_module.wasm
    ${ZEPHYR_BINARY_DIR}/include/generated/remote_module.wasm.inc)
endif()
