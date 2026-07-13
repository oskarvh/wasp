/*
 * wasp.* host functions — the module side of remote memory.
 *
 * Each function turns into a node->coordinator RPC (wasp_rpc_call) and
 * blocks the executor until the coordinator answers. Bounds and locks
 * are checked authoritatively by the coordinator; the node only rejects
 * what is locally provable (offset arithmetic past the 24-bit reference
 * space, oversized transfers).
 *
 * Buffer arguments arrive already validated and translated by WAMR (the
 * '*~' signature markers), so dst/src point into the module's linear
 * memory and are exactly `len` bytes long.
 *
 * Runs on the executor thread only, mid-wasm_runtime_call_wasm.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "wasm_export.h"

#include "wamr_runtime.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_remote, LOG_LEVEL_INF);

/* Connection generation of the CALL being executed (set by the executor
 * before wasm_runtime_call_wasm), so RPC responses can be routed and
 * stale ones discarded. */
uint8_t wasp_exec_conn;

#define RPC_TIMEOUT K_MSEC(CONFIG_WASP_RPC_TIMEOUT_MS)

/* Map a coordinator ERROR payload to a module-visible return code. */
static int32_t map_error(const struct wasp_msg *resp)
{
	uint8_t code = resp->len >= 1 ? resp->payload[0] : 0;

	switch (code) {
	case WASP_ERR_LOCKED:
		return WASP_REMOTE_ELOCKED;
	case WASP_ERR_BAD_ARGS:
		return WASP_REMOTE_EBOUNDS;
	case WASP_ERR_NO_REGION:
		return WASP_REMOTE_ENOREGION;
	default:
		return WASP_REMOTE_EIO;
	}
}

/*
 * Issue one RPC and normalize the outcome: 0 with *resp filled on any
 * coordinator answer (caller frees resp->payload), or a trap raised on
 * the module when the coordinator cannot be reached at all.
 */
static int rpc(wasm_exec_env_t exec_env, uint8_t type, const void *payload, uint32_t len,
	       struct wasp_msg *resp)
{
	int rc = wasp_rpc_call(type, payload, len, wasp_exec_conn, resp, RPC_TIMEOUT);

	if (rc != 0) {
		wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env),
					   "wasp: remote memory RPC failed (coordinator gone?)");
	}
	return rc;
}

/* Expected-response check shared by all callers. */
static int32_t finish(struct wasp_msg *resp, uint8_t want_type, uint32_t want_len,
		      int32_t ok_value)
{
	int32_t ret;

	if (resp->type == want_type && (want_len == UINT32_MAX || resp->len == want_len)) {
		ret = ok_value;
	} else if (resp->type == WASP_MSG_ERROR) {
		ret = map_error(resp);
	} else {
		LOG_WRN("unexpected RPC response type 0x%02x len %u", resp->type, resp->len);
		ret = WASP_REMOTE_EIO;
	}
	if (resp->payload != NULL) {
		k_heap_free(&wasp_payload_heap, resp->payload);
	}
	return ret;
}

static int32_t native_mem_read(wasm_exec_env_t exec_env, uint32_t ref, void *dst, uint32_t len)
{
	uint8_t req[8];
	struct wasp_msg resp;

	if (len == 0) {
		return WASP_REMOTE_OK;
	}
	if (len > CONFIG_WASP_MAX_MODULE_SIZE ||
	    WASP_REF_OFFSET(ref) + len > WASP_REF_MAX_OFFSET) {
		return WASP_REMOTE_EBOUNDS;
	}

	sys_put_le32(ref, &req[0]);
	sys_put_le32(len, &req[4]);
	if (rpc(exec_env, WASP_MSG_MEM_READ, req, sizeof(req), &resp) != 0) {
		return WASP_REMOTE_EIO;
	}
	if (resp.type == WASP_MSG_MEM_DATA && resp.len == len) {
		memcpy(dst, resp.payload, len);
	}
	return finish(&resp, WASP_MSG_MEM_DATA, len, WASP_REMOTE_OK);
}

static int32_t native_mem_write(wasm_exec_env_t exec_env, uint32_t ref, void *src, uint32_t len)
{
	struct wasp_msg resp;
	uint8_t *req;
	int rc;

	if (len == 0) {
		return WASP_REMOTE_OK;
	}
	if (len > CONFIG_WASP_MAX_MODULE_SIZE - 4 ||
	    WASP_REF_OFFSET(ref) + len > WASP_REF_MAX_OFFSET) {
		return WASP_REMOTE_EBOUNDS;
	}

	req = k_heap_alloc(&wasp_payload_heap, 4 + len, K_MSEC(100));
	if (req == NULL) {
		return WASP_REMOTE_EIO;
	}
	sys_put_le32(ref, &req[0]);
	memcpy(&req[4], src, len);
	rc = rpc(exec_env, WASP_MSG_MEM_WRITE, req, 4 + len, &resp);
	k_heap_free(&wasp_payload_heap, req);
	if (rc != 0) {
		return WASP_REMOTE_EIO;
	}
	return finish(&resp, WASP_MSG_MEM_ACK, 0, WASP_REMOTE_OK);
}

static int32_t native_lock(wasm_exec_env_t exec_env, uint32_t region)
{
	uint8_t req = region;
	struct wasp_msg resp;

	if (region > UINT8_MAX) {
		return WASP_REMOTE_ENOREGION;
	}
	if (rpc(exec_env, WASP_MSG_LOCK, &req, 1, &resp) != 0) {
		return WASP_REMOTE_EIO;
	}
	return finish(&resp, WASP_MSG_LOCK_GRANT, UINT32_MAX, WASP_REMOTE_OK);
}

static int32_t native_unlock(wasm_exec_env_t exec_env, uint32_t region)
{
	uint8_t req = region;
	struct wasp_msg resp;

	if (region > UINT8_MAX) {
		return WASP_REMOTE_ENOREGION;
	}
	if (rpc(exec_env, WASP_MSG_UNLOCK, &req, 1, &resp) != 0) {
		return WASP_REMOTE_EIO;
	}
	return finish(&resp, WASP_MSG_MEM_ACK, 0, WASP_REMOTE_OK);
}

static int32_t native_region_size(wasm_exec_env_t exec_env, uint32_t region)
{
	uint8_t req = region;
	struct wasp_msg resp;
	int32_t size = WASP_REMOTE_EIO;

	if (region > UINT8_MAX) {
		return WASP_REMOTE_ENOREGION;
	}
	if (rpc(exec_env, WASP_MSG_REGION_INFO, &req, 1, &resp) != 0) {
		return WASP_REMOTE_EIO;
	}
	if (resp.type == WASP_MSG_REGION_DESC && resp.len == 4) {
		size = (int32_t)sys_get_le32(resp.payload);
	}
	return finish(&resp, WASP_MSG_REGION_DESC, 4, size);
}

int wasp_remote_register_natives(void)
{
	static NativeSymbol symbols[] = {
		{"mem_read", native_mem_read, "(i*~)i", NULL},
		{"mem_write", native_mem_write, "(i*~)i", NULL},
		{"lock", native_lock, "(i)i", NULL},
		{"unlock", native_unlock, "(i)i", NULL},
		{"region_size", native_region_size, "(i)i", NULL},
	};

	if (!wasm_runtime_register_natives("wasp", symbols, ARRAY_SIZE(symbols))) {
		LOG_ERR("failed to register wasp.* natives");
		return -EINVAL;
	}
	return 0;
}
