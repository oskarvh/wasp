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

#endif /* WASP_REMOTE_H_ */
