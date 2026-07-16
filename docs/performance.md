# Remote-memory performance: what's fast, what's slow, and why

Everything in this guide is measured, not argued: the numbers come from
`tools/perf_test.py` driving `tools/perf_module.c` on the real fleet
(1× Nucleo-F439ZI on Ethernet + 4× Pico W on WiFi, coordinator =
`wasp_client.py` on a LAN host, 2026-07-15). Re-run it yourself:

```sh
tools/build_test_module.sh
python3 tools/perf_test.py tools/perf_module.wasm discover
```

The one mental model that explains every number below: **a remote
access costs milliseconds; local memory costs nanoseconds.** The
round-trip dominates everything — payload size, alignment, and local
computation are noise next to the per-RPC cost. All performance
engineering for wasp therefore reduces to two questions: *how many
round-trips does this code emit*, and *how many of them contend on the
same thing*.

## T1 — the round-trip is the unit of cost, and payload is nearly free

One `MEM_READ`/`MEM_WRITE` per iteration, timed over hundreds of
iterations, as the transfer size grows:

| transfer | Nucleo (Ethernet) read | Pico W (WiFi) read |
| ---: | ---: | ---: |
| 4 B | 2.8 ms/op | 24 ms/op |
| 64 B | 2.8 ms/op | 23 ms/op |
| 256 B | 3.0 ms/op | 25 ms/op |
| 1 KiB | 3.2 ms/op | 26 ms/op |
| 4 KiB | 4.8 ms/op (830 KiB/s) | 54 ms/op (74 KiB/s) |

(Writes are within a few percent of reads at every size.)

Two lessons:

- **The per-RPC overhead is the whole story**: ~2.8 ms on Ethernet,
  ~24 ms on WiFi. Moving 256× more data (4 B → 1 KiB) costs almost
  nothing extra on either transport.
- **A 4-byte read costs the same as a 1 KiB read.** If there is any
  chance you'll need the neighboring data, take it in the same
  transfer — "read too much" is free, "read again" never is.

WiFi nodes pay ~8× the latency of Ethernet nodes for every single
RPC. If part of a workload is chatty with remote memory, put it on the
wired node; use the WiFi swarm for compute-heavy slices.

## T2 — batch, don't peek (the biggest single win)

Scaling an n-int array in place: per-element loop (2 RPCs per element —
exactly what a transparent `v[i] = v[i] * k` loop compiles into) vs one
bulk read + local compute + one bulk write (2 RPCs total), Ethernet
node:

| ints | per-element | bulk | speedup |
| ---: | ---: | ---: | ---: |
| 16 | 0.10 s | 9 ms | 11× |
| 64 | 0.36 s | 9 ms | 42× |
| 256 | 1.44 s | 9 ms | **159×** |

The speedup is simply the RPC-count ratio, so it grows linearly with
the data — and on WiFi every one of those round-trips is 8× dearer.
Transparent remote pointers (`wasp_remote`, `remote_ptr<T>`) make the
per-element version *look* innocent; reach for them for small or
sparse access, and stage anything loop-shaped through a local buffer
(`wasp_mem_read`/`try_read`, whole-struct assignment — all one bulk
RPC).

## T3 — alignment makes no measurable difference (and why it still matters)

64-byte transfers at offsets +0/+1/+3, and `MEM_ADD` on a counter at
+0 vs straddling a word boundary at +2, Ethernet node:

```
64 B read  @ +0   2.83 ms/op        MEM_ADD @ +0   2.83 ms/op
64 B read  @ +1   2.84 ms/op        MEM_ADD @ +2   2.83 ms/op
64 B read  @ +3   2.83 ms/op
```

Identical to the third digit. This is by design: the wire protocol is
byte-granular (`ref` + explicit length), locks are region-granular, and
the coordinator serializes whole requests — there is no cache line, no
word-granule, nothing for alignment to collide with. **Misaligned data
does not cause extra collisions or slow transfers in wasp today.**

So should you pack structs to save bytes? No — keep natural (4-byte)
alignment anyway, for two forward-looking reasons:

1. **A C coordinator will want hardware atomics.** Today's Python
   coordinator serves `MEM_ADD`/`MEM_CAS` under a mutex, so any offset
   works. A C coordinator can serve them with a single lock-free
   `atomic_fetch_add` on the region byte — *if* the target is a
   naturally-aligned `uint32_t`. Unaligned atomics are undefined
   behavior on every relevant platform, so misaligned counters would
   force the slow mutex path forever.
2. **Layout is a cross-machine ABI contract** (next section) — and
   natural alignment is the only convention both a wasm32 module and
   an x86-64/ARM coordinator agree on without `packed` tricks.

What *does* reduce collisions is covered by T4 and T5: overlap and
region partitioning, not alignment.

## T4 — keep hot words out of bulk-written extents

The distributed cousin of false sharing. One node runs an **unlocked**
read-modify-write churn over a 256-byte struct (read all, tweak, write
all back — "updating its telemetry block") while four other nodes
`MEM_ADD` a shared counter 160× total:

| counter placed | adds surviving |
| --- | ---: |
| inside the churned extent (offset 128) | 148 / 160 — **12 erased** |
| outside it (offset 256) | 160 / 160 |

Every atomic add that landed between the churner's read and write-back
was overwritten by stale data — silently, with every individual RPC
perfectly atomic. The atomicity of `MEM_ADD` cannot protect a word
that somebody else bulk-writes without a lock.

The rule: **a word updated by atomics must not live inside an extent
that anyone bulk-writes unlocked.** Give hot counters their own words
outside the bulk data (or their own region), or take the region lock
around the churn (which also makes the atomics fail `ELOCKED` instead
of being erased — loud beats silent). Moving the counter is free;
finding the lost updates later is not.

## T5 — locks are region-granular: partition per writer

Five nodes each doing 15 *locked* read-modify-write increments (queued
locks, so zero retry waste — this measures pure serialization):

| layout | time | result |
| --- | ---: | --- |
| all 5 nodes lock one shared region | 4.37 s | exact |
| each node locks its own region | 1.46 s | exact |

**3× from a one-line layout change.** A region is the unit of locking,
so unrelated data sharing a region contend for no reason — the DSM
version of putting two unrelated hot variables on one cache line. If
different nodes own different slices, make the slices *regions* (there
are 256 of them, up to 16 MiB each). Reserve the shared-region-plus-
lock pattern for data that is genuinely shared. (Plain unlocked
reads/writes to disjoint slices of one region don't lock and don't
have this problem.)

## T6 — MEM_ADD doesn't degrade under contention; CAS loops do

30 increments per node on one shared counter, growing the contender
count (mixed Ethernet + WiFi):

| nodes | MEM_ADD | CAS retry loop | CAS attempts | amplification |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.09 s | 0.09 s | 30 | 1.00× |
| 3 | 0.72 s | 1.40 s | 145 | 1.61× |
| 5 | 0.73 s | 2.71 s | 372 | 2.48× |

`MEM_ADD` cannot fail, so contention costs it nothing: 5 nodes take
the same wall time as 3. A CAS loop retries every time it loses the
race, and each retry is a full round-trip, so the amplification grows
with the swarm — 2.5× the RPCs at 5 nodes and climbing. **If the
update can be expressed as an add, use `wasp_add`; reserve
`wasp_cas` for updates that genuinely need it** (min/max, ownership
claims, state machines), and expect its cost to scale with contention.

## When to use what — the cheat sheet

| you want to… | use | cost |
| --- | --- | --- |
| read/write a blob, array, or struct | one bulk `wasp_mem_read`/`wasp_mem_write` (or whole-struct `*p` copy) | 1 RPC |
| dereference a few scattered words | transparent `wasp_remote` / `remote_ptr<T>` | 1 RPC per access — fine when accesses are few |
| count / accumulate / grab-next-index | `wasp_add` | 1 RPC, contention-proof |
| claim ownership, track min/max, CAS-shaped updates | `wasp_cas` | 1 RPC per attempt; attempts grow with contention |
| update multiple words as one invariant | `wasp_lock` + bulk read + local compute + bulk write + `wasp_unlock` | 4 RPCs; hold time serializes rivals — keep it minimal |
| per-node private state | its own region | no contention by construction |

And the anti-patterns, each measured above: per-element loops over
remote arrays (T2, up to 159× slower), hot counters inside bulk-written
extents (T4, silent lost updates), unrelated writers sharing one locked
region (T5, 3× slower), CAS loops for additive updates (T6, 2.5× RPC
amplification and rising).

## Sharing structs with a C coordinator

The coordinator side of a region is just bytes — today a Python
`bytearray`, tomorrow a C buffer. When both ends are C, the natural
move is one shared header defining the region layout, included by the
wasm32 module *and* the coordinator. That works — both are
little-endian and agree on naturally-aligned fixed-width types — but
only if the header sticks to the contract:

```c
/* shared_layout.h — compiled by BOTH wasm32 (module) and the
 * coordinator (x86-64/ARM64). Fixed-width types only. */
#include <stdint.h>

struct node_slot {
	uint32_t counter;    /* MEM_ADD target: 4-byte aligned, own word */
	uint32_t state;
	int32_t  result[8];
	uint8_t  tag;
	uint8_t  _pad[3];    /* explicit padding — never implicit */
};
_Static_assert(sizeof(struct node_slot) == 44, "layout drift");
```

The rules, and what breaks without them:

- **Fixed-width types only** (`uint32_t`, `int8_t`, …). `long` is 4
  bytes on wasm32 but 8 on an x86-64 coordinator; `size_t` likewise.
  One `long` field silently shifts every field after it.
- **No pointers in shared structs** — 4 vs 8 bytes, and a local
  address is meaningless on the other machine anyway. Store offsets
  or packed remote refs (`u32`) instead.
- **Spell out padding** and `_Static_assert` the total size on both
  sides. Implicit padding happens to match between wasm32 and x86-64
  for naturally-aligned fields, but one assert per struct turns "layout
  drift" from a heisenbug into a compile error.
- **Don't use `__attribute__((packed))`** to "optimize". T3 shows it
  saves nothing on the wire, and it makes field addresses unusable for
  hardware atomics on the C coordinator side.
- **Keep atomically-updated words naturally aligned and exclusively
  owned** — a `uint32_t` at a 4-byte boundary that no bulk write ever
  overlaps (T3 + T4). Then a C coordinator can serve `MEM_ADD` on it
  with a lock-free `atomic_fetch_add` instead of a global mutex.
- **`double` is fine, `long double` is not; avoid bitfields and enums
  in shared layouts** (their sizes are implementation choices).

One more C-coordinator note: the current Python coordinator serializes
*all* requests under one mutex, which is correct but flat. A C
coordinator can keep per-region mutexes and lock-free atomics for
`MEM_ADD`/`MEM_CAS` — every layout rule above is what keeps that
upgrade a pure implementation detail that modules never notice.
