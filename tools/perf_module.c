/*
 * Remote-memory performance characterization module.
 * Built by build_test_module.sh; driven by tools/perf_test.py.
 * Results and the guidance they back live in docs/performance.md.
 *
 * Every export loops on-module so one CALL amortizes many remote-memory
 * RPCs — the coordinator times the CALL and divides by the rep count.
 */
#include "wasp/remote.h"

#define EXPORT(name) __attribute__((export_name(name)))

/* Staging buffer for bulk transfers; aligned so it can be viewed as
 * i32s. One 8 KiB buffer fits easily in the module's 64 KiB page. */
static __attribute__((aligned(8))) unsigned char buf[8192];

/* T1: reps bulk reads of len bytes at ref. Returns reps, or an error. */
EXPORT("read_loop") int read_loop(unsigned ref, int len, int reps)
{
	if (len < 1 || len > (int)sizeof(buf)) {
		return WASP_REMOTE_EBOUNDS;
	}
	for (int r = 0; r < reps; r++) {
		int rc = wasp_mem_read(ref, buf, (unsigned)len);

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return reps;
}

/* T1: reps bulk writes of len bytes at ref. Returns reps, or an error. */
EXPORT("write_loop") int write_loop(unsigned ref, int len, int reps)
{
	if (len < 1 || len > (int)sizeof(buf)) {
		return WASP_REMOTE_EBOUNDS;
	}
	for (int r = 0; r < reps; r++) {
		int rc = wasp_mem_write(ref, buf, (unsigned)len);

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return reps;
}

/* T2, the anti-pattern: scale n i32s by k, one element per round-trip
 * (2n RPCs). This is what a transparent-pointer loop compiles into. */
EXPORT("scale_per_element") int scale_per_element(unsigned ref, int n, int k)
{
	for (int i = 0; i < n; i++) {
		int v;
		int rc = wasp_mem_read(ref + 4u * i, &v, sizeof(v));

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		v *= k;
		rc = wasp_mem_write(ref + 4u * i, &v, sizeof(v));
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return n;
}

/* T2, the pattern: same work in exactly 2 RPCs. */
EXPORT("scale_bulk") int scale_bulk(unsigned ref, int n, int k)
{
	int *v = (int *)buf;

	if (n < 1 || n > (int)(sizeof(buf) / sizeof(int))) {
		return WASP_REMOTE_EBOUNDS;
	}
	int rc = wasp_mem_read(ref, v, (unsigned)n * sizeof(int));

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	for (int i = 0; i < n; i++) {
		v[i] *= k;
	}
	return wasp_mem_write(ref, v, (unsigned)n * sizeof(int));
}

/* T3/T6: reps atomic adds on one word. Returns the counter value after
 * this node's last add, or an error. */
EXPORT("add_loop") int add_loop(unsigned ref, int reps, int delta)
{
	int old = 0;

	for (int r = 0; r < reps; r++) {
		int rc = wasp_add(ref, delta, &old);

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return old + delta;
}

/* T6: n increments via a CAS retry loop. Each attempt is one RPC; a
 * lost race feeds the fresh value straight into the next attempt (no
 * re-read needed). Returns total attempts (>= n), or an error. */
EXPORT("cas_inc_loop") int cas_inc_loop(unsigned ref, int n)
{
	unsigned cur;
	int attempts = 0;
	int rc = wasp_mem_read(ref, &cur, sizeof(cur));

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}
	for (int done = 0; done < n;) {
		unsigned old;

		attempts++;
		rc = wasp_cas(ref, cur, cur + 1, &old);
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		if (old == cur) {
			done++;
			cur = old + 1;
		} else {
			cur = old;
		}
	}
	return attempts;
}

/* T4: unlocked read-modify-write churn over a whole extent — a node
 * "updating its telemetry block". Any concurrent update landing inside
 * [ref, ref+len) between the read and the write-back is erased. */
EXPORT("churn_rmw") int churn_rmw(unsigned ref, int len, int reps)
{
	if (len < 1 || len > (int)sizeof(buf)) {
		return WASP_REMOTE_EBOUNDS;
	}
	for (int r = 0; r < reps; r++) {
		int rc = wasp_mem_read(ref, buf, (unsigned)len);

		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
		buf[0]++;
		rc = wasp_mem_write(ref, buf, (unsigned)len);
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return reps;
}

/* T5: reps locked read-modify-writes with `inner` reads per critical
 * section. Retries contended locks; returns total ELOCKED retries (0
 * under queued locks), or a negative error. */
EXPORT("locked_rmw_loop") int locked_rmw_loop(unsigned region, unsigned ref,
					      int reps, int inner)
{
	int retries = 0;

	for (int r = 0; r < reps; r++) {
		int v = 0;
		int rc;

		for (;;) {
			rc = wasp_lock(region);
			if (rc == WASP_REMOTE_OK) {
				break;
			}
			if (rc != WASP_REMOTE_ELOCKED) {
				return rc;
			}
			retries++;
		}
		for (int j = 0; j < inner; j++) {
			rc = wasp_mem_read(ref, &v, sizeof(v));
			if (rc != WASP_REMOTE_OK) {
				wasp_unlock(region);
				return rc;
			}
		}
		v += 1;
		rc = wasp_mem_write(ref, &v, sizeof(v));
		wasp_unlock(region);
		if (rc != WASP_REMOTE_OK) {
			return rc;
		}
	}
	return retries;
}
