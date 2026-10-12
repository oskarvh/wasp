/*
 * Thin wrapper around the WAMR runtime.
 *
 * A pool of CONFIG_WASP_MAX_INSTANCES executor threads shares the module
 * and instance tables behind one mutex. Calls to different instances run
 * concurrently; one instance never runs two at once.
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

/* Bytes the WAMR pool has left; 0 if it cannot be queried. */
uint32_t wasp_wamr_pool_free(void);

/* Instance slots not currently in use. */
uint8_t wasp_wamr_free_slots(void);

#endif /* WASP_WAMR_RUNTIME_H_ */
