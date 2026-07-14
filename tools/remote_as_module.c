/*
 * Test module for phase-2 transparent remote pointers (wasp_remote /
 * address space 100). Every dereference below is lowered by the
 * wasp-remote pass plugin into remote-memory RPCs — there is not a
 * single explicit wasp_mem_read/write call in this file.
 *
 * Build: tools/build_test_module.sh (plugin + wasp_remote_rt.c).
 */
#include <wasp/remote.h>

#define EXPORT(name) __attribute__((export_name(name)))

/* Sum `count` i32s — one load RPC per element (the "don't do this for
 * big arrays" case; bulk copies below are the fast path). */
EXPORT("as_sum")
int as_sum(unsigned region, int count)
{
	wasp_remote int *p = WASP_REMOTE_PTR(int, region, 0);
	int s = 0;

	for (int i = 0; i < count; i++) {
		s += p[i];
	}
	return s;
}

/* Byte-wide loads (i8 shim). */
EXPORT("as_sum_bytes")
int as_sum_bytes(unsigned region, int count)
{
	wasp_remote unsigned char *p = WASP_REMOTE_PTR(unsigned char, region, 0);
	int s = 0;

	for (int i = 0; i < count; i++) {
		s += p[i];
	}
	return s;
}

/* Plain store + readback through the same pointer. */
EXPORT("as_store")
int as_store(unsigned region, int index, int value)
{
	wasp_remote int *p = WASP_REMOTE_PTR(int, region, 0);

	p[index] = value;
	return p[index];
}

/* Sum of f64s, truncated to i32 for the v1 calling convention. */
EXPORT("as_sum_f64")
int as_sum_f64(unsigned region, int count)
{
	wasp_remote double *p = WASP_REMOTE_PTR(double, region, 0);
	double s = 0.0;

	for (int i = 0; i < count; i++) {
		s += p[i];
	}
	return (int)s;
}

struct pair {
	int a;
	int b;
};

/* Whole-struct copies: one bulk read RPC in, one bulk write RPC out —
 * the pass lowers the implicit memcpys, not field-by-field access. */
EXPORT("as_swap")
int as_swap(unsigned region)
{
	wasp_remote struct pair *rp = WASP_REMOTE_PTR(struct pair, region, 0);
	struct pair t = *rp;
	int a = t.a;

	t.a = t.b;
	t.b = a;
	*rp = t;
	return t.a;
}

/* Read-modify-write under the scoped lock guard. Returns the new value,
 * or WASP_REMOTE_ELOCKED when the region is held by someone else. */
EXPORT("as_locked_inc")
int as_locked_inc(unsigned region)
{
	wasp_remote int *p = WASP_REMOTE_PTR(int, region, 0);
	int out = WASP_REMOTE_ELOCKED;

	wasp_locked(region) {
		int v = *p;

		*p = v + 1;
		out = v + 1;
	}
	return out;
}

/* Dereference far outside any sane region: must trap the module (the
 * remote segfault), not return garbage. */
EXPORT("as_oob")
int as_oob(unsigned region)
{
	wasp_remote int *p = WASP_REMOTE_PTR(int, region, 0x00FFFFF0);

	return *p;
}
