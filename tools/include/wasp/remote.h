/*
 * wasp remote memory — module-side API (phase 1: explicit calls).
 *
 * Include this from C compiled with clang --target=wasm32. The functions
 * are imports from the "wasp" namespace, resolved by the node's runtime;
 * wasm-ld accepts them as imports because of the import_module /
 * import_name attributes (no --allow-undefined needed).
 *
 * A remote reference names memory in the *coordinator's* RAM:
 * (region_id:8 | offset:24) packed into 32 bits, so ordinary arithmetic
 * (ref + n) moves through a region. Regions are capabilities the
 * coordinator exports; there is no way to name unshared memory.
 *
 * Return codes must match WASP_REMOTE_* in app/src/protocol.h.
 */
#ifndef WASP_REMOTE_H_
#define WASP_REMOTE_H_

#define WASP_REF(region, offset) (((unsigned)(region) << 24) | (unsigned)(offset))
#define WASP_REF_REGION(ref) ((unsigned)(ref) >> 24)
#define WASP_REF_OFFSET(ref) ((unsigned)(ref) & 0x00FFFFFFu)

#define WASP_REMOTE_OK 0
#define WASP_REMOTE_ELOCKED (-1)   /* region locked by someone else */
#define WASP_REMOTE_EBOUNDS (-2)   /* access outside the region */
#define WASP_REMOTE_ENOREGION (-3) /* no such exported region */
#define WASP_REMOTE_EIO (-4)       /* coordinator refused for another reason */

#define WASP_IMPORT(name) __attribute__((import_module("wasp"), import_name(name)))

/* Copy len bytes of coordinator memory at ref into dst. */
WASP_IMPORT("mem_read") int wasp_mem_read(unsigned ref, void *dst, unsigned len);

/* Copy len bytes from src into coordinator memory at ref. */
WASP_IMPORT("mem_write") int wasp_mem_write(unsigned ref, const void *src, unsigned len);

/*
 * Hold a region's mutex across multiple reads/writes (compound
 * operations). Individual mem_read/mem_write calls are always atomic
 * on their own — the coordinator serializes them — so lock only around
 * read-modify-write sequences. Grants are leases: a crashed holder is
 * revoked by the coordinator when the lease expires. While someone
 * else holds the lock, all access fails fast with WASP_REMOTE_ELOCKED.
 */
WASP_IMPORT("lock") int wasp_lock(unsigned region);
WASP_IMPORT("unlock") int wasp_unlock(unsigned region);

/* Byte length of an exported region, or a WASP_REMOTE_* error. */
WASP_IMPORT("region_size") int wasp_region_size(unsigned region);

/*
 * Atomic primitives (nodes with feature bit 0x02): the coordinator
 * performs the whole read-modify-write while servicing the one
 * request, so these are race-free against every other node with no
 * lock, no lease, and exactly one round-trip. Both store the OLD value
 * through *old and return a WASP_REMOTE_* status. They respect
 * explicit locks (WASP_REMOTE_ELOCKED while another node holds the
 * region).
 */

/* *ref += delta (i32). Old value out via *old. */
WASP_IMPORT("add") int wasp_add(unsigned ref, int delta, int *old);

/*
 * Write desired to *ref only if it currently equals expected; *old
 * receives what was there. You won the swap iff *old == expected.
 * Retry loops cost one round-trip per attempt and only repeat when the
 * value truly changed underneath you.
 */
WASP_IMPORT("cas") int wasp_cas(unsigned ref, unsigned expected, unsigned desired,
				unsigned *old);

/*
 * --- Phase 2: transparent remote pointers ----------------------------
 *
 * Declare a pointer wasp_remote and dereference it like any other
 * pointer; loads, stores, indexing, and whole-struct copies through it
 * become remote-memory RPCs. Requires compiling with the wasp-remote
 * pass plugin and linking tools/lib/wasp_remote_rt.c (see
 * tools/build_test_module.sh); without the plugin, taking one of these
 * pointers apart still works but dereferencing will fail the build in
 * the wasm backend rather than miscompile.
 *
 *   wasp_remote int *arr = WASP_REMOTE_PTR(int, 1, 0);
 *   arr[3] = arr[0] + arr[1];      // three RPCs
 *
 * A failed dereference (bad region, out of bounds, region locked by
 * someone else, coordinator gone) traps the module — the remote
 * equivalent of a segfault, surfaced to the coordinator as
 * ERROR(TRAP). When an access can legitimately fail — contended locks,
 * probing region sizes — use the explicit wasp_mem_read/wasp_mem_write
 * API above and check the return code.
 *
 * Address space 100 avoids the LLVM wasm backend's reserved spaces.
 */
#define wasp_remote __attribute__((address_space(100)))

/* A typed remote pointer to (region, byte offset). */
#define WASP_REMOTE_PTR(T, region, offset) \
	((wasp_remote T *)WASP_REF(region, offset))

/*
 * Scoped lock guard: the body runs exactly once if the lock was
 * acquired and not at all otherwise (check wasp_lock() yourself if you
 * must distinguish). Do not break/goto/return out of the body — the
 * unlock would be skipped.
 *
 *   wasp_locked(REGION) {
 *       int v = *counter;
 *       *counter = v + 1;
 *   }
 */
#define wasp_locked(region)                                          \
	for (int _wasp_lk = (wasp_lock(region) == WASP_REMOTE_OK) ? 0 : 2; \
	     _wasp_lk == 0; wasp_unlock(region), _wasp_lk = 1)

#endif /* WASP_REMOTE_H_ */
