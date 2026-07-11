/*
 * Thin wrapper around the WAMR runtime. Only the executor thread and the
 * agent's lifecycle handlers may call these.
 */
#ifndef WASP_WAMR_RUNTIME_H_
#define WASP_WAMR_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize the WAMR runtime with the static heap pool. */
int wasp_wamr_init(void);

/* TODO(lifecycle): load/instantiate, unload, call — added with the
 * protocol implementation. */

#endif /* WASP_WAMR_RUNTIME_H_ */
