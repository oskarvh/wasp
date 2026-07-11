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

#endif /* WASP_WAMR_RUNTIME_H_ */
