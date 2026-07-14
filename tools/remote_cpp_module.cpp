/*
 * Test module for the C++ remote-pointer API (wasp/remote.hpp) — the
 * no-compiler-plugin route to transparent remote memory. Mirrors
 * remote_as_module.c so the client can run the same checks against
 * both.
 *
 * Build: tools/build_test_module.sh (clang++, -fno-exceptions
 * -fno-rtti -nostdlib; no pass plugin, no runtime shims).
 */
#include <wasp/remote.hpp>

#define EXPORT(name) extern "C" __attribute__((export_name(name)))

EXPORT("cpp_sum")
int cpp_sum(unsigned region, int count)
{
	wasp::remote_ptr<int> p(region, 0);
	int s = 0;

	for (int i = 0; i < count; i++) {
		s += p[i];
	}
	return s;
}

EXPORT("cpp_store")
int cpp_store(unsigned region, int index, int value)
{
	wasp::remote_ptr<int> p(region, 0);

	p[index] = value;
	return p[index];
}

/* Bulk read: one RPC for the whole slice, summed locally. */
EXPORT("cpp_sum_bulk")
int cpp_sum_bulk(unsigned region, int count)
{
	int buf[64];

	if (count > 64) {
		return WASP_REMOTE_EBOUNDS;
	}

	wasp::remote_ptr<int> p(region, 0);
	int rc = p.try_read(buf, (unsigned)count);

	if (rc != WASP_REMOTE_OK) {
		return rc;
	}

	int s = 0;

	for (int i = 0; i < count; i++) {
		s += buf[i];
	}
	return s;
}

struct pair {
	int a;
	int b;
};

/* operator-> loads the whole struct once and reads a field. */
EXPORT("cpp_pair_b")
int cpp_pair_b(unsigned region)
{
	wasp::remote_ptr<pair> p(region, 0);

	return p->b;
}

/* RAII-locked read-modify-write. Returns the new value, or
 * WASP_REMOTE_ELOCKED when someone else holds the region. */
EXPORT("cpp_locked_inc")
int cpp_locked_inc(unsigned region)
{
	wasp::remote_lock lock(region);

	if (!lock) {
		return WASP_REMOTE_ELOCKED;
	}

	wasp::remote_ptr<int> p(region, 0);
	int v = *p + 1;

	*p = v;
	return v;
}

/* += goes through the read-modify-write proxy (two RPCs, unlocked —
 * exactly the kind of access that loses updates under contention). */
EXPORT("cpp_add")
int cpp_add(unsigned region, int delta)
{
	wasp::remote_ptr<int> p(region, 0);

	*p += delta;
	return *p;
}

/* Out-of-bounds dereference must trap. */
EXPORT("cpp_oob")
int cpp_oob(unsigned region)
{
	wasp::remote_ptr<int> p(region, 0x00FFFFF0);

	return *p;
}
