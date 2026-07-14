/*
 * wasp remote memory — C++ transparency without a compiler plugin.
 *
 * remote_ptr<T> makes coordinator memory feel like a pointer using
 * operator overloading alone: no address spaces, no LLVM pass, plain
 * clang++ --target=wasm32 -fno-exceptions -fno-rtti -nostdlib.
 *
 *   wasp::remote_ptr<int> arr(REGION, 0);
 *   arr[3] = arr[0] + arr[1];        // three RPCs
 *   int v = *arr;                    // one RPC
 *   pt->x;                           // loads the whole struct, reads .x
 *
 * Semantics match the C plugin path: reads/writes move sizeof(T) bytes
 * per access, whole objects at a time; a failed access traps the
 * module (ERROR(TRAP) at the coordinator). Use try_read/try_write for
 * a checked error-code path, and remote_lock as an RAII guard around
 * read-modify-write sequences.
 */
#ifndef WASP_REMOTE_HPP_
#define WASP_REMOTE_HPP_

#include <wasp/remote.h>

namespace wasp {

inline void check(int rc)
{
	if (rc != WASP_REMOTE_OK) {
		__builtin_trap();
	}
}

/* Proxy for *p and p[i]: converts on read, RPCs on assignment. */
template <typename T> class remote_ref {
public:
	explicit constexpr remote_ref(unsigned ref) : ref_(ref) {}

	operator T() const
	{
		T v;

		check(wasp_mem_read(ref_, &v, sizeof(T)));
		return v;
	}

	remote_ref &operator=(const T &v)
	{
		check(wasp_mem_write(ref_, &v, sizeof(T)));
		return *this;
	}

	remote_ref &operator=(const remote_ref &other)
	{
		return *this = static_cast<T>(other);
	}

	remote_ref &operator+=(const T &v) { return *this = static_cast<T>(*this) + v; }
	remote_ref &operator-=(const T &v) { return *this = static_cast<T>(*this) - v; }

private:
	unsigned ref_;
};

/* p->field support: operator-> loads the whole T into this temporary
 * and drills into it. Reads only — write through *p = or remote_ref. */
template <typename T> class remote_arrow {
public:
	explicit remote_arrow(unsigned ref) { check(wasp_mem_read(ref, &v_, sizeof(T))); }
	const T *operator->() const { return &v_; }

private:
	T v_;
};

template <typename T> class remote_ptr {
public:
	constexpr remote_ptr(unsigned region, unsigned offset)
		: ref_(WASP_REF(region, offset))
	{
	}
	explicit constexpr remote_ptr(unsigned ref) : ref_(ref) {}

	remote_ref<T> operator*() const { return remote_ref<T>(ref_); }
	remote_ref<T> operator[](int i) const
	{
		return remote_ref<T>(ref_ + (unsigned)(i * (int)sizeof(T)));
	}
	remote_arrow<T> operator->() const { return remote_arrow<T>(ref_); }

	remote_ptr operator+(int n) const
	{
		return remote_ptr(ref_ + (unsigned)(n * (int)sizeof(T)));
	}
	remote_ptr operator-(int n) const { return *this + (-n); }
	remote_ptr &operator++() { return *this = *this + 1; }
	bool operator==(const remote_ptr &o) const { return ref_ == o.ref_; }
	bool operator!=(const remote_ptr &o) const { return ref_ != o.ref_; }

	/* Bulk transfers — one RPC for count elements; prefer these in
	 * loops (see the performance rule in docs/writing-modules.md). */
	int try_read(T *dst, unsigned count) const
	{
		return wasp_mem_read(ref_, dst, count * sizeof(T));
	}
	int try_write(const T *src, unsigned count) const
	{
		return wasp_mem_write(ref_, src, count * sizeof(T));
	}

	/* Atomic fetch-and-add: race-free against every other node in one
	 * round-trip, no lock needed. Returns the OLD value; traps on
	 * failure like a dereference. i32-sized T only. */
	T fetch_add(T delta) const
	{
		static_assert(sizeof(T) == 4, "atomics are i32-sized");
		int old;

		check(wasp_add(ref_, (int)delta, &old));
		return (T)old;
	}

	/* Atomic compare-and-swap: writes desired iff the current value is
	 * expected. Returns the old value — you won iff it == expected. */
	T compare_exchange(T expected, T desired) const
	{
		static_assert(sizeof(T) == 4, "atomics are i32-sized");
		unsigned old;

		check(wasp_cas(ref_, (unsigned)expected, (unsigned)desired, &old));
		return (T)old;
	}

	constexpr unsigned ref() const { return ref_; }
	constexpr unsigned region() const { return WASP_REF_REGION(ref_); }
	constexpr unsigned offset() const { return WASP_REF_OFFSET(ref_); }

private:
	unsigned ref_;
};

/* RAII lease on a region's lock. Failure to acquire is not fatal —
 * test with operator bool and retry/back off as the workload demands. */
class remote_lock {
public:
	explicit remote_lock(unsigned region)
		: region_(region), held_(wasp_lock(region) == WASP_REMOTE_OK)
	{
	}
	~remote_lock()
	{
		if (held_) {
			wasp_unlock(region_);
		}
	}
	remote_lock(const remote_lock &) = delete;
	remote_lock &operator=(const remote_lock &) = delete;

	explicit operator bool() const { return held_; }

private:
	unsigned region_;
	bool held_;
};

} /* namespace wasp */

#endif /* WASP_REMOTE_HPP_ */
