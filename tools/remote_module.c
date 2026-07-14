/*
 * Remote-memory test module: exercises every wasp.* host function.
 * Built by build_test_module.sh; driven by `wasp_client.py <ip> remote`.
 *
 * All region arguments arrive as packed remote references (WASP_REF) or
 * plain region ids, passed by the coordinator as ordinary i32 CALL args.
 */
#include "wasp/remote.h"

#define EXPORT(name) __attribute__((export_name(name)))

/* Sum n i32s starting at ref, one element per RPC (also proves that
 * arithmetic on a packed reference walks the region). */
EXPORT("sum_region") int sum_region(unsigned ref, int n)
{
	int sum = 0;

	for (int i = 0; i < n; i++) {
		int v;
		int rc = wasp_mem_read(ref + 4u * i, &v, sizeof(v));

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		sum += v;
	}
	return sum;
}

/* Same sum via one bulk read — the way it should be done. */
EXPORT("sum_region_bulk") int sum_region_bulk(unsigned ref, int n)
{
	int buf[64];
	int sum = 0;

	if (n < 0 || n > 64) {
		return WASP_REMOTE_EBOUNDS;
	}
	int rc = wasp_mem_read(ref, buf, (unsigned)n * sizeof(int));

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	for (int i = 0; i < n; i++) {
		sum += buf[i];
	}
	return sum;
}

/* Fill n i32s at ref with val; returns WASP_REMOTE_OK or an error. */
EXPORT("fill_region") int fill_region(unsigned ref, int n, int val)
{
	int buf[64];

	if (n < 0 || n > 64) {
		return WASP_REMOTE_EBOUNDS;
	}
	for (int i = 0; i < n; i++) {
		buf[i] = val;
	}
	return wasp_mem_write(ref, buf, (unsigned)n * sizeof(int));
}

/* Single write probe — used to check fail-fast LOCKED behavior. */
EXPORT("try_write") int try_write(unsigned ref, int val)
{
	return wasp_mem_write(ref, &val, sizeof(val));
}

/* UNLOCKED read-modify-write — deliberately racy. Two nodes running
 * this concurrently WILL lose updates; that is the point (see the
 * swarm test). Returns the new value it wrote, or a negative error. */
EXPORT("racy_add") int racy_add(unsigned ref, int delta)
{
	int v;
	int rc = wasp_mem_read(ref, &v, sizeof(v));

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	v += delta;
	rc = wasp_mem_write(ref, &v, sizeof(v));
	return rc == WASP_REMOTE_OK ? v : rc;
}

/* Locked read-modify-write: the reason explicit locks exist. Returns
 * the new counter value, or a negative WASP_REMOTE_* error. */
EXPORT("locked_add") int locked_add(unsigned region, unsigned ref, int delta)
{
	int v;
	int rc = wasp_lock(region);

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	rc = wasp_mem_read(ref, &v, sizeof(v));
	if (rc == WASP_REMOTE_OK) {
		v += delta;
		rc = wasp_mem_write(ref, &v, sizeof(v));
	}
	wasp_unlock(region);
	return rc == WASP_REMOTE_OK ? v : rc;
}

/* Expose region_size for the self-test. */
EXPORT("region_len") int region_len(unsigned region)
{
	return wasp_region_size(region);
}

/* Atomic fetch-and-add: the same increment as racy_add/locked_add in
 * exactly ONE round-trip, race-free with no lock. Returns the new
 * value, or a negative WASP_REMOTE_* error. */
EXPORT("atomic_add") int atomic_add(unsigned ref, int delta)
{
	int old;
	int rc = wasp_add(ref, delta, &old);

	return rc == WASP_REMOTE_OK ? old + delta : rc;
}

/* One compare-and-swap attempt; returns the OLD value (caller won iff
 * it equals expected), or a negative error. The self-test drives both
 * the win and lose paths through this. */
EXPORT("cas_swap") int cas_swap(unsigned ref, unsigned expected, unsigned desired)
{
	unsigned old;
	int rc = wasp_cas(ref, expected, desired, &old);

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	return (int)old;
}
