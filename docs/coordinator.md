# Coordinator design sketch: libwasp + peer load sharing

Design sketch (2026-07-18) for the coordinator effort. Two goals:

1. **One C library (`libwasp`)** implementing the coordinator role,
   used three ways: linked into a native C coordinator, wrapped as a
   Python extension (today's `wasp_client.py` tooling becomes a thin
   layer over it), and — the payoff — compiled into node firmware so
   MCUs can play the coordinator role toward each other.
2. **1–N nodes, 0–1 coordinators.** A dedicated coordinator is
   optional. Nodes carry a pool of pre-built worker modules in flash
   and hand work to idle peers, best-effort: if no peer is free, run
   it yourself.

## The unifying idea: roles, not boxes

Today "coordinator" bundles several jobs. Split them:

| role | what it does | today |
| --- | --- | --- |
| **worker** | executes `LOAD`/`CALL`/`UNLOAD` | every node |
| **region host** | owns regions, serves `MEM_*`/`LOCK`/`ADD`/`CAS` | Python coordinator |
| **job owner** | pushes modules, issues CALLs, collects results | Python coordinator |

The wire protocol doesn't care which box a role runs in — traffic is
already bidirectional over one TCP socket, and the node never asks
who its peer is. So:

- A Linux **coordinator** = region host + job owner with lots of RAM,
  a Python API, and no executor.
- A **node that farms out work** = worker + (small) region host +
  job owner for the job it owns. Its peers can't tell it apart from
  the Linux coordinator. **No new protocol semantics needed** — the
  work owner simply dials the peer's port 4242 and behaves like a
  coordinator.

The 0-coordinator system falls out naturally: some node's application
module *is* the job, that node hosts the job's regions in its own
RAM, and it delegates function calls to idle peers.

## libwasp layout

```
libwasp/
├── core/     # framing, command codes, region/lock/atomic engine,
│             #   RPC slots — no OS dependencies, fixed-size pools,
│             #   pluggable allocator (shares app/src/protocol.h)
├── host/     # job-owner + region-host logic: fleet table, module
│             #   push, CALL tracking, MEM_* serving, discovery
├── port/
│   ├── posix.c   # sockets/threads/time for Linux/mac
│   └── zephyr.c  # same API over Zephyr BSD sockets + k_threads
└── bindings/python/   # `import wasp`
```

Rules that keep it embeddable:

- `core/` and `host/` are allocation-free after init (caller provides
  pools) and use only the `port/` API — the exact discipline the
  firmware already follows, so the region-server that runs on Linux
  is byte-for-byte the one in firmware.
- **The Python binding is event-queue based, not callback based.**
  libwasp runs its own I/O thread(s); Python (or a module on an MCU)
  consumes `wasp_poll(ctx, &event)` — events like `NODE_FOUND`,
  `NODE_LOST`, `CALL_DONE`, `MEM_WRITE_OBSERVED`. No C→Python
  callbacks means no GIL contortions, and the demo's
  `_node_response` provenance hook becomes a `for ev in
  fleet.events():` loop. CFFI (or plain ctypes) over a small C ABI —
  no hand-written CPython extension to maintain.

Sketch of the C API surface:

```c
wasp_ctx  *wasp_init(const wasp_port *port, const wasp_cfg *cfg);
wasp_node *wasp_connect(wasp_ctx *, const char *addr);   /* or via discovery */
int  wasp_load(wasp_node *, const void *wasm, size_t len);
int  wasp_load_pooled(wasp_node *, uint32_t pool_id);
int  wasp_call(wasp_node *, const char *fn, const int32_t *argv, int argc,
               uint32_t *token);                          /* async; result via event */
int  wasp_region_register(wasp_ctx *, uint8_t id, void *base, uint32_t len,
                          uint32_t flags);                /* served to all peers */
int  wasp_poll(wasp_ctx *, wasp_event *ev, int timeout_ms);
```

Acceptance test for the library: `mandelbrot_demo.py` runs unchanged
on top of the binding, same numbers as [performance.md](performance.md).

## The worker-function pool

Work is **not** shipped as arbitrary bytes at delegation time — each
image embeds a pool of pre-built worker modules (they're 1–8 KiB;
flash is the cheap resource):

- Build system generates `pool_manifest.c` from a Kconfig-listed set
  of `.wasm` files; each entry is (fnv1a-32 of name+bytes, ptr, len).
- `ANNOUNCE` v2 carries a **pool hash** (hash of the sorted entry
  hashes) plus a **status byte**: executor free/busy, free instance
  count.
- New command `LOAD_POOLED(id)`: instantiate from local flash instead
  of a TCP payload. If the peer doesn't have the id: `ERROR(NOPOOL)`,
  and the owner either falls back to a normal byte-push `LOAD` (peers
  with RAM headroom) or just keeps the task (best-effort).

Same-pool fleets (the normal case: one firmware image) skip all
negotiation — matching pool hash in ANNOUNCE is the fast path.

## Delegation flow (v1: push model)

The module API is two host functions:

```c
int wasp_spawn(uint32_t pool_id, uint32_t fn, const int32_t argv[4],
               wasp_task *handle);   /* WASP_EAGAIN if no peer free */
int wasp_join(wasp_task *handle, int32_t *result, uint32_t timeout_ms);
```

Under the hood, on the owning node:

1. Firmware keeps a **neighbor table** built from overheard ANNOUNCEs
   (they're already broadcast; nodes just start listening too).
   ~24 B/entry, capped small.
2. `wasp_spawn` picks an idle neighbor, dials it (one outbound socket,
   K=1 concurrent delegation per node to start), `LOAD_POOLED`,
   `CALL`. No idle neighbor → `EAGAIN` immediately.
3. The peer's remote-memory RPCs flow back over that same socket, and
   the **owner serves them from its own RAM** — this is `libwasp
   host/` running in firmware. Rule: *the job owner is the region
   host for the job it farms out.* No peer-to-peer distributed shared
   memory in v1; delegated functions take value args and owner-hosted
   regions only.
4. `CALL_RESULT` resolves the handle; `wasp_join` returns it.

**Best-effort fallback:** on `EAGAIN` (or a join timeout after a peer
dies) the caller runs the function locally. Cheapest robust form: the
pooled worker function is *also linked into the calling module*, so
the fallback is a plain wasm call — zero extra RAM, no nested
instantiation. The SDK can generate the
`spawn_or_local()` shim. Consequence: worker functions should be
idempotent (a timed-out task may have half-run remotely).

A **pull/steal model** (idle nodes poll busy nodes' task boards via a
`STEAL` command) is the natural v2 — it load-balances better and puts
the polling cost on idle nodes — but push needs the least new
machinery (`LOAD_POOLED` + ANNOUNCE status + on-node client role) and
proves the architecture. The Mandelbrot counter pattern is a degenerate
task board already.

## 0–M job owners

Nothing limits the system to one coordinator: "job owner" is a role,
and any number of boxes can hold it simultaneously — including
several Python processes sharing the fleet. No protocol change and no
coordinator-to-coordinator state; the deltas are concurrency
etiquette:

- **Allocation races are the normal case.** Two owners dial the same
  idle worker; one wins, the other gets `BUSY` — treated exactly like
  `EAGAIN`: next neighbor, then local fallback. No reservations, no
  arbiter. Owners must pick neighbors **randomized** (not first-idle)
  or every owner herds onto the same worker.
- **Workers accept a few concurrent inbound connections** (2–4,
  ~1–2 KiB each — the main RAM cost), if only to answer `BUSY`
  quickly while the executor is taken. Kconfig-capped.
- **Isolation comes free from connection scoping.** A module's RPCs
  flow up the connection that loaded it, so region IDs, locks, and
  atomics live in per-owner namespaces that never meet. That is *why*
  M owners needs no global anything. Rule: never pass a ref to a node
  loaded by a different owner.
- **Delegation depth cap.** A delegated task's own `wasp_spawn` runs
  locally (depth 1) — no deadlock exists (local fallback guarantees
  progress) but unbounded re-delegation amplifies latency.
- **ANNOUNCE status is advisory** and often stale under M owners;
  the optimistic dial-and-retry loop absorbs it.
- **Trust:** `LOAD_POOLED` only runs code already in the worker's own
  flash; the byte-push `LOAD` path is what accepts foreign bytecode.
  Later knob: pool-only mode for fleets that shouldn't.

## Feasibility

Yes, with three honest caveats:

- **Task granularity.** Delegation costs ≥2 RPCs: ~6 ms node↔node on
  Ethernet, ~50 ms over WiFi ([performance.md](performance.md)).
  Worker functions need ≳10× that in compute — ≥0.5 s per task on
  WiFi — to win. Mandelbrot-tile-sized work qualifies; tiny functions
  never will, and `spawn` cost estimates could gate on that later.
- **Pico W RAM.** At ~93% full it can be a worker and little else.
  Peer roles (`neighbor table`, delegate engine, region server) must
  be Kconfig-gated so small nodes compile worker-only. Job-owner
  nodes are the RAM-rich ones — the Nucleo today, the
  [H723 board](hardware.md) later. This is fine: the model is
  asymmetric by nature.
- **Single executor per node.** A node whose executor is busy can
  still *delegate* (net thread does that work) and still *serve
  regions*, but can't accept work — so "idle" = executor free, which
  ANNOUNCE now advertises. The multi-instance roadmap item is the
  multiplier here, not a prerequisite.

## Build order

1. **libwasp on POSIX** + Python binding — **done 2026-07-18.**
   `libwasp/` implements the coordinator role in C: framing, the
   blocking request loop that services node-initiated RPCs inline,
   the region/lock/atomics engine (incl. queued locks and lease
   expiry), UDP discovery, and write-observation events (the
   event-queue design from above; the binding is plain ctypes, no
   dependencies, and auto-builds the .so). `tools/libwasp.py`
   exposes a wasp_client-compatible `WaspNode`, so the *unchanged*
   wasp_client test functions run through C —
   `tools/libwasp_selftest.py` passes all four suites on both board
   types, `make -C libwasp check` covers the engine hardware-free
   with a scripted fake node, and `mandelbrot_demo.py` now runs on
   libwasp (4.2 s / 256×192 on the 6-node fleet, chaos re-issue and
   GIF recording included). `wasp_client.py` stays as the
   pure-Python reference; `swarm_test.py`/`perf_test.py` still use
   it and can port later.
2. **Client role in firmware**: node dials a peer, `LOAD`s, `CALL`s —
   demo: Nucleo drives one Pico with no Linux box involved.
3. **Region host in firmware** (link `libwasp host/` into the app):
   owner-hosted regions; demo: coordinator-less Mandelbrot — Nucleo
   owns params/counter/framebuffer in its RAM, farms tiles to Picos.
4. **Pool + ANNOUNCE v2 + spawn/join** host functions; chaos test:
   kill a peer mid-task, watch the local fallback complete the job.

Step 1 is worth doing even if peer sharing never ships; steps 2–4
each demo independently on today's fleet.
