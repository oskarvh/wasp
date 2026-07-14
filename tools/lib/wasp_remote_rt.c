/*
 * wasp remote-memory runtime shims — the functions the wasp-remote
 * LLVM pass lowers address-space-100 loads/stores into. Link this file
 * into any module built with the pass plugin (build_test_module.sh
 * does). Everything funnels into the phase-1 wasp.* imports, so the
 * node firmware needs nothing new.
 *
 * Error contract: a transparent dereference has no error channel, so
 * any failure traps the module (ERROR(TRAP) at the coordinator) — the
 * remote equivalent of a segfault. Code that expects failure (lock
 * contention, probing) should use the explicit wasp_mem_* API.
 */
#include <stdint.h>

#include <wasp/remote.h>

static inline void check(int rc)
{
	if (rc != WASP_REMOTE_OK) {
		__builtin_trap();
	}
}

#define WASP_RT_LOAD(suffix, type)                          \
	type __wasp_remote_load_##suffix(unsigned ref)      \
	{                                                   \
		type v;                                     \
		check(wasp_mem_read(ref, &v, sizeof(v)));   \
		return v;                                   \
	}

#define WASP_RT_STORE(suffix, type)                              \
	void __wasp_remote_store_##suffix(unsigned ref, type v)  \
	{                                                        \
		check(wasp_mem_write(ref, &v, sizeof(v)));       \
	}

WASP_RT_LOAD(i8, uint8_t)
WASP_RT_LOAD(i16, uint16_t)
WASP_RT_LOAD(i32, uint32_t)
WASP_RT_LOAD(i64, uint64_t)
WASP_RT_LOAD(f32, float)
WASP_RT_LOAD(f64, double)

WASP_RT_STORE(i8, uint8_t)
WASP_RT_STORE(i16, uint16_t)
WASP_RT_STORE(i32, uint32_t)
WASP_RT_STORE(i64, uint64_t)
WASP_RT_STORE(f32, float)
WASP_RT_STORE(f64, double)

void __wasp_remote_read(void *dst, unsigned ref, unsigned len)
{
	check(wasp_mem_read(ref, dst, len));
}

void __wasp_remote_write(unsigned ref, const void *src, unsigned len)
{
	check(wasp_mem_write(ref, src, len));
}
