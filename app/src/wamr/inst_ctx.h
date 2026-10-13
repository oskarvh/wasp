/*
 * Per-instance context hung off the exec_env.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between the executor (wamr_runtime.c), which owns the instance
 * table, and the host functions (native_remote.c), which only ever see a
 * wasm_exec_env_t. The executor points the exec_env's user data at this
 * struct so a host function can find both the connection to answer on and
 * somewhere to charge its time.
 *
 * Why the counters live here rather than being summed per node: the
 * question worth answering is how much of a call went into the
 * interpreter and how much went into waiting for the coordinator, and
 * that is per instance by nature — instances of one node interleave
 * precisely so that one can compute while another waits, so a node-wide
 * total hides the thing being measured.
 *
 * The arithmetic that makes it useful: an instance's RPC waits happen
 * *inside* its call, so
 *
 *     compute = exec_ns - rpc_wait_ns
 *     wait fraction = rpc_wait_ns / exec_ns
 *
 * No locking. Each field is written only by the one executor thread
 * currently running that instance (the `running` claim guarantees there
 * is at most one), and read by the agent thread answering STATS, where a
 * torn 64-bit read would cost a wrong number in a report and nothing
 * else.
 */
#ifndef WASP_INST_CTX_H_
#define WASP_INST_CTX_H_

#include <stdint.h>

struct wasp_inst_ctx {
	/* Connection generation of the call in flight, for stamping RPCs. */
	uint8_t conn;

	uint32_t calls;       /* CALLs completed on this instance */
	uint64_t exec_ns;     /* cumulative time inside wasm_runtime_call_wasm */
	uint32_t rpc_count;   /* host-function RPCs issued */
	uint64_t rpc_wait_ns; /* cumulative time blocked awaiting a response */
	uint32_t rpc_tx;      /* request payload bytes sent */
	uint32_t rpc_rx;      /* response payload bytes received */
	uint32_t traps;       /* calls that ended in a trap */
};

#endif /* WASP_INST_CTX_H_ */
