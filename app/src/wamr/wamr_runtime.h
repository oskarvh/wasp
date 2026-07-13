/*
 * Thin wrapper around the WAMR runtime.
 *
 * Everything here runs on the executor thread only — the runtime, the
 * loaded module, and the exec env are all single-threaded by design
 * (lifecycle and call requests are serialized through wasp_exec_q).
 */
#ifndef WASP_WAMR_RUNTIME_H_
#define WASP_WAMR_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize the WAMR runtime with the static heap pool. */
int wasp_wamr_init(void);

/* Register the wasp.* host functions (native_remote.c); must run after
 * wasm_runtime_full_init and before any module is loaded. */
int wasp_remote_register_natives(void);

/* Connection generation of the CALL currently executing — set by the
 * executor before entering the module, read by the host functions to
 * stamp their RPCs. */
extern uint8_t wasp_exec_conn;

#endif /* WASP_WAMR_RUNTIME_H_ */
