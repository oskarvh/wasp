# wasp

**WebAssembly Swarm Processor** — a WAMR network compute distribution for MCUs.

A *wasp node* is a deliberately dumb MCU: it boots Zephyr, joins the network,
and waits. A *coordinator* (future work, not in this repo yet) pushes
WebAssembly modules and execution commands to nodes over a TCP socket. The
node loads the module into the [WebAssembly Micro Runtime
(WAMR)](https://github.com/bytecodealliance/wasm-micro-runtime), executes it,
and reports results back. All application intelligence lives in the WASM
modules and the coordinator — the firmware is a thin, portable execution
shell.

```
┌─────────────┐        TCP / framed protocol         ┌──────────────────────┐
│ Coordinator │ ───────────────────────────────────▶ │  wasp node (MCU)     │
│ (future)    │ ◀─────────────────────────────────── │  Zephyr + WAMR       │
└─────────────┘      results / logs / status         └──────────────────────┘
                                                        … × N nodes (swarm)
```

## Firmware architecture

The firmware is a Zephyr application with three long-lived threads connected
by message queues. Ownership is strict: only the network thread touches the
socket, only the executor thread calls into a running WASM module.

```
                         wasp node firmware
┌─────────────────────────────────────────────────────────────────────┐
│                                                                     │
│  ┌───────────────┐  rx msgq   ┌──────────────┐  exec q  ┌─────────┐ │
│  │ network       │ ─────────▶ │ agent        │ ───────▶ │ wasm    │ │
│  │ handler       │            │              │          │ executor│ │
│  │ thread        │ ◀───────── │ • protocol   │          └───┬─────┘ │
│  └───────┬───────┘  tx msgq   │   dispatch   │              │       │
│          │             ▲      │ • routing    │        WAMR runtime  │
│   Zephyr network stack │      └──────────────┘        (interpreter, │
│   (sockets, DHCP)      └─────────────────────────────  static heap  │
│          │                 responses from executor     pool) +      │
│   Ethernet driver (per-board)                          module       │
│                                                        lifecycle    │
└─────────────────────────────────────────────────────────────────────┘
```

### Threads

| Thread              | Responsibility                                                                                                                              |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| **network handler** | Brings up the network interface (DHCP by default), runs the TCP server socket, frames/deframes protocol messages, forwards complete commands to the agent via the rx queue, and drains the tx queue back to the coordinator. Owns the socket exclusively. Framing-level errors (bad magic, oversize, no memory) are handled here. |
| **agent**           | Protocol dispatcher. Answers protocol-level commands (HELLO, PING) itself and routes WASM lifecycle commands (LOAD_MODULE / UNLOAD_MODULE / CALL) to the executor via the exec queue. Never blocks on WASM execution. |
| **wasm executor**   | The **only** thread that touches the WAMR runtime. Consumes lifecycle and call requests from the exec queue — serialized, so a module can never be unloaded mid-call — and posts responses directly to the tx queue. Isolated in its own thread so a long-running or misbehaving module cannot wedge the agent or the network handler. |

Zephyr's own threads (net stack rx/tx, system workqueue, logging) run
alongside these.

### Message flow

1. Coordinator connects and sends a framed command (e.g. *load module*,
   *call function*).
2. Network handler validates the frame and posts it to the **rx msgq**.
3. Agent answers protocol commands directly on the **tx msgq**; lifecycle
   and call commands are routed (payload ownership included) to the
   **exec queue**.
4. Executor performs the runtime operation and posts the RESULT/ERROR
   response directly to the **tx msgq**.
5. Network handler drains the tx msgq onto the socket.

### Wire protocol

Length-prefixed binary frames over a single TCP connection (default port
`4242`, configurable via Kconfig). Canonical definition:
`app/src/protocol.h`; reference implementation of the coordinator side:
`tools/wasp_client.py`.

```
┌────────┬────────┬───────────┬─────────────┬───────────┐
│ "WA"   │ type   │ seq       │ payload len │ payload   │
│ 2 B    │ 1 B    │ 1 B       │ 4 B LE      │ n B       │
└────────┴────────┴───────────┴─────────────┴───────────┘
```

Responses echo the request's `seq`. Payloads are capped at
`CONFIG_WASP_MAX_MODULE_SIZE`, advertised in `HELLO_ACK`; oversized frames
are drained and answered with `ERROR(TOO_LARGE)` without losing stream
sync. A bad magic means the stream is desynchronized and the node drops
the connection.

Command set:

- `HELLO` / `HELLO_ACK` — version exchange; ack carries the payload cap
- `LOAD_MODULE` — payload is a `.wasm` binary; node loads + instantiates it
  (one module at a time; ack is an empty `RESULT`)
- `UNLOAD_MODULE` — tears the module down and frees its memory
- `CALL` — `[name_len u8][name][argc u8][arg u32 LE × argc]`; invokes an
  exported function. v1 calling convention: **i32 args/results only**,
  signature-checked on the node via WAMR introspection
- `RESULT` — `[nresults u8][value u32 LE × n]` (empty for lifecycle acks)
- `ERROR` — `[code u8][optional utf8 detail]`, e.g. `TRAP(wasm operand
  stack overflow)`; codes in `app/src/protocol.h`
- `PING` / `PONG` — liveness; `PONG` echoes the `PING` payload
- `IDENTIFY` — strobes the status LED (default 10 s) so a human can find
  the physical board: `wasp_client.py <ip> identify [seconds]`
- `REBOOT` — remote reboot; mode 1 enters the USB bootloader (RP2040
  only), so Pico Ws can be reflashed without touching BOOTSEL:
  `wasp_client.py <ip> reboot [--bootsel]`
- `MEM_READ` / `MEM_WRITE` / `LOCK` / `UNLOCK` / `REGION_INFO` (0x4x) —
  **node-initiated** remote-memory requests, sent mid-`CALL` in the
  node's own seq space and answered by the coordinator; see
  [Remote memory](#remote-memory--design-plan)

A module that traps (e.g. `unreachable`, out-of-bounds access) returns
`ERROR(TRAP, exception-text)` and the node carries on — the WASM sandbox
holds, and the next `CALL` works.

### Discovery & status LED

Nodes don't need to be known in advance: once online, every node
broadcasts an `ANNOUNCE` datagram to UDP port `4243` every 5 seconds
(both Kconfig-tunable) carrying its protocol version, feature bits, TCP
port, busy flag (coordinator currently connected) and board name.

```
python3 tools/wasp_client.py discover            # human-readable list
python3 tools/wasp_client.py discover --ips      # for scripting
python3 tools/swarm_test.py tools/remote_module.wasm discover
```

Boards with an `led0` devicetree alias (the Nucleo's green LED, the Pico
W's WiFi-chip LED — wired up in the app overlay) show their state at a
glance:

| Pattern | Meaning |
| --- | --- |
| fast blink (5 Hz) | no network yet (WiFi joining / DHCP) |
| heartbeat (short blip every second) | online, waiting for a coordinator |
| solid on | coordinator connected |
| frantic strobe (10 Hz) | `IDENTIFY` received — "here I am" |

### Memory strategy

WAMR runs from a **static heap pool** (`Alloc_With_Pool`) so WASM memory is
bounded and isolated from the Zephyr kernel heap. Pool size, thread stack
sizes, and the maximum module size are Kconfig options tuned per board.

The pool must live in **main SRAM** and comfortably exceed 64 KiB: a WASM
page is fixed at 64 KiB, so instantiating any standard clang-built module
demands one contiguous 64 KiB+ allocation for its linear memory. The
payload heap (protocol buffers + the raw `.wasm` of the loaded module)
takes the board's tightly-coupled memory instead when available — on the
F439: 88 KiB SRAM pool for WAMR, 40 KiB payload heap in CCM.

## Repository layout

```
wasp/
├── app/                    # the Zephyr application (manifest repo for west)
│   ├── west.yml            # pins Zephyr, HALs, and WAMR
│   ├── CMakeLists.txt
│   ├── Kconfig             # wasp-specific options (port, pool sizes, …)
│   ├── prj.conf            # board-agnostic configuration
│   ├── boards/             # per-board config & devicetree overlays
│   │   └── nucleo_f439zi.conf
│   └── src/
│       ├── main.c          # boot, queue setup, thread start
│       ├── led.c           # status LED thread (patterns + IDENTIFY strobe)
│       ├── rpc.c           # node->coordinator RPC slot (remote memory)
│       ├── net/            # network handler thread + framing + ANNOUNCE broadcast
│       ├── agent/          # agent thread + protocol dispatch
│       └── wamr/           # WAMR wrapper + executor + wasp.* host functions
├── deps/                   # west-managed dependencies (NOT committed)
│   ├── zephyr/
│   └── modules/            # hal_stm32, cmsis, wasm-micro-runtime, …
├── docs/
│   └── writing-modules.md  # C function -> node -> result, step by step
├── tools/
│   ├── wasp_client.py      # test client + remote-memory host (embryonic coordinator)
│   ├── swarm_test.py       # multi-node concurrency test (race + locks + distributed sum)
│   ├── include/wasp/remote.h  # module-side remote memory API
│   ├── test_module.c       # test WASM module (add/fib/boom)
│   ├── remote_module.c     # remote-memory test module
│   └── build_test_module.sh
└── README.md
```

External code (Zephyr, STM32 HAL, WAMR) is pulled by **west** from the
manifest in `app/west.yml`, pinned to exact release tags — same
reproducibility as git submodules, but west also resolves Zephyr's internal
module graph, which plain submodules cannot. `deps/` is gitignored.

## Portability

Nothing in `src/` may include board- or SoC-specific headers. Portability is
handled by the Zephyr layers:

- **Board selection** is a build-time switch: `west build -b <board> app`.
- **Per-board tuning** (heap pool size, network driver options, devicetree
  overlays) lives in `app/boards/<board>.conf` / `.overlay`.
- **New MCU support** = add a board conf/overlay (and a HAL module to the
  manifest allowlist if it's a new vendor). No source changes.

Supported boards:

- **ST Nucleo-F439ZI** (`nucleo_f439zi`) — Cortex-M4F @ 180 MHz, 192 KiB
  SRAM + 64 KiB CCM, on-board Ethernet (LAN8742 PHY).
- **Raspberry Pi Pico W** (`rpi_pico/rp2040/w`) — Cortex-M0+ @ 133 MHz,
  264 KiB SRAM, CYW43439 WiFi (Infineon AIROC driver; needs a one-time
  `west blobs fetch hal_infineon` and WiFi credentials, see
  [Building](#building)). Console is USB CDC-ACM. WAMR runs the generic
  soft-float `THUMB` target on ARMv6-M.

## Building

Dependencies: CMake ≥ 3.20, Python ≥ 3.10, a toolchain (Zephyr SDK, or GNU
Arm Embedded with `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb`).

```sh
# one-time workspace setup (from the repo root)
python3 -m venv .venv && source .venv/bin/activate
pip install west ninja
west init -l app
west update                # clones Zephyr + WAMR into deps/
pip install -r deps/zephyr/scripts/requirements-base.txt

# if using GNU Arm Embedded instead of the Zephyr SDK:
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/usr   # prefix of arm-none-eabi-gcc

# build & flash — Nucleo-F439ZI (Ethernet)
west build -b nucleo_f439zi app
west flash                 # needs stlink (openocd or STM32CubeProgrammer)

# build & flash — Raspberry Pi Pico W (WiFi)
west blobs fetch hal_infineon                   # one-time: CYW43439 firmware
cp app/wifi_credentials.conf.example app/wifi_credentials.conf
#   ...then edit app/wifi_credentials.conf with your SSID/PSK. The file is
#   gitignored and merged into the build automatically whenever it exists.
west build -b rpi_pico/rp2040/w app -d build/pico
west flash -d build/pico -r uf2                 # board must be in BOOTSEL mode
```

The BOOTSEL-button dance (hold it while plugging in) is only needed the
**first** time a Pico is flashed with wasp. From then on, reflash over
the network:

```sh
python3 tools/wasp_client.py <node-ip> reboot --bootsel   # RP2040 only
# ...the RPI-RP2 drive enumerates over USB; mount it and copy the image:
cp build/pico/zephyr/zephyr.uf2 /media/$USER/RPI-RP2/
# node reboots into the new firmware and rejoins the network in ~20 s
```

Talking to a node — no serial cable needed, nodes announce themselves
over UDP broadcast (see
[Discovery & status LED](#discovery--status-led)):

```sh
python3 tools/wasp_client.py discover               # list nodes on the LAN
python3 tools/wasp_client.py <node-ip> identify     # strobe LED: which board is this?
python3 tools/wasp_client.py <node-ip> check        # protocol self-test
tools/build_test_module.sh                          # needs clang + wasm-ld
python3 tools/wasp_client.py <node-ip> lifecycle tools/test_module.wasm
python3 tools/wasp_client.py <node-ip> load tools/test_module.wasm
python3 tools/wasp_client.py <node-ip> call fib 20
python3 tools/wasp_client.py <node-ip> unload
python3 tools/wasp_client.py <node-ip> reboot       # remote reboot (any board)
```

WASM modules for wasp are plain clang output — no SDK required:

```sh
clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
      -z stack-size=1024 -o module.wasm module.c
```

For the full developer walk-through — writing a C function, compiling it,
pushing it to a node, and reading the result back — see
**[docs/writing-modules.md](docs/writing-modules.md)**.

Note on WAMR integration: WAMR's `zephyr/module.yml` declares external
CMake/Kconfig glue but does not ship it; `app/modules/wamr/` provides that
glue and builds WAMR's `vmlib` as a Zephyr library, enabled by
`CONFIG_WAMR=y`. Memory placement is described under [Memory
strategy](#memory-strategy) above.

## Remote memory — design plan

This is **software distributed shared memory**: when a module needs data
that lives in the *coordinator's* RAM, the node fetches (or writes back)
that memory over the wire on the module's behalf — so C code can work
with pass-by-reference data whose backing store is on another machine.
Phase 1 (explicit `wasp_mem_read`/`wasp_mem_write`/`wasp_lock` host
functions — see `tools/include/wasp/remote.h`) is implemented and tested
on hardware; the later phases below are the plan.

### Can we overload the dereference?

A hard constraint first, because it shapes everything below. Inside WASM
a C pointer is just an i32 offset into the module's own linear memory —
there is no MMU, no page fault, and an out-of-bounds access traps
**non-resumably** in WAMR. So a *raw* C pointer to coordinator RAM can
never be transparently dereferenced without modifying the interpreter
itself. What we can do, in increasing order of transparency:

1. **Explicit API (phase 1)** — host functions the module imports:
   `wasp_mem_read(region, offset, dst, len)` / `wasp_mem_write(...)` plus
   `wasp_lock` / `wasp_unlock`. Ugly but honest; everything else builds
   on it.
2. **Compile-time typed transparency for C (phase 2)** — clang's named
   address spaces put "this pointer is remote" into the *type system*:
   `#define wasp_remote __attribute__((address_space(1)))`, then
   `wasp_remote int *arr` reads like a normal pointer (`arr[i]`,
   `p->field`, pointer arithmetic) but is a distinct type — assigning it
   to a local pointer without an explicit cast is a **compile error**,
   so local/remote confusion is caught at build time and propagated
   automatically through every assignment, argument, and dereference. A
   small LLVM pass plugin (`-fpass-plugin=`) rewrites loads/stores
   through address space 1 into `__wasp_remote_load/store` calls, which
   are ordinary WASM imports: the emitted module is plain, valid wasm32,
   and code using only local pointers compiles exactly as before, zero
   overhead. C++ gets the same ergonomics with no compiler plugin at
   all, via `remote_ptr<T>` overloading `operator*` / `operator->` /
   `operator[]` with a write-through proxy.
3. **Fully transparent raw pointers (phase 4, research)** — for code
   that cannot be annotated (third-party sources, `void *` plumbing,
   `memcpy`): either an ASan-style pass that instruments *every*
   load/store with a reserved-window check (fully transparent, but a
   compare on each access), or patching the interpreter's bounds check
   so a reserved address window above linear memory faults into a fetch
   hook instead of trapping (WAMR's *shared heap* feature already routes
   such addresses to a second host-managed heap, so the plumbing
   half-exists — but it means carrying a WAMR patch). Explicitly out of
   scope until phases 1–3 are proven.

Why not *infer* remoteness with no annotation at all? Pointer
provenance in C is statically undecidable — pointers flow through
integers, unions, `memcpy`, and data structures no compiler can trace —
and link time is too late (wasm-ld sees opaque function bodies, not
types). One qualifier at the declaration site, type-checked from there
on, is the practical ceiling for compile-time detection.

### Remote references are capabilities, not addresses

A module must never hold a raw coordinator RAM address — only memory the
coordinator has **explicitly exported** is reachable, and non-shared
coordinator memory simply has no name a node could use. The coordinator
registers a buffer as a *region* — `(region_id, base, length)` on its
side — and hands modules remote references as ordinary i32 `CALL`
arguments, packed `region_id:8 | offset:24` (up to 256 live regions of
16 MiB each). The packing keeps the v1 i32-only calling convention
intact and, crucially, lets plain C pointer arithmetic (`ref + i`) work
on the packed value. Every access carries an explicit length and is
bounds-checked **twice**: on the node against a cached region descriptor
(fast local failure), and again by the coordinator, which is
authoritative — even a compromised node cannot reach outside a region.

### The role reversal: node-initiated requests

Everything so far is strictly coordinator-asks, node-answers. Remote
memory reverses this mid-call: while the executor is inside
`wasm_runtime_call_wasm`, a host function must send a request *to* the
coordinator and block until the reply arrives. That needs:

- **Protocol v2**: a node-initiated message range (`MEM_READ`,
  `MEM_WRITE`, `LOCK`, `UNLOCK` + their acks) with its own seq space, so
  both directions can be in flight on the one TCP connection without
  ambiguity. Feature-negotiated in `HELLO` so old clients keep working.
- **An RPC slot in the firmware**: executor posts the request to the tx
  queue and blocks on a semaphore; the agent routes the coordinator's
  reply back to the slot. Executor is serialized, so exactly one
  outstanding remote op — no table needed. A timeout or connection drop
  fails the RPC and the host function raises a WASM exception, surfacing
  to the coordinator as `ERROR(TRAP, "remote memory timeout")` — a hung
  coordinator must not wedge the node.

### Races: who protects the coordinator's RAM?

The coordinator's copy is the **single authoritative copy**, and **all
lock state lives in the coordinator** — a node never decides whether a
region is locked; it only sends requests. Concretely:

- **Every read and write RPC is mutex-protected, unconditionally.** The
  coordinator wraps each `MEM_READ`/`MEM_WRITE` in the target region's
  mutex, so a single access is always atomic and a torn read or write
  can never be observed. A module cannot opt out: the serialization
  happens on the coordinator, after the request has left the node.
- **Explicit `LOCK` = holding that same region mutex across multiple
  RPCs**, for compound operations (read-modify-write, multi-field
  updates). Locks are granted as **leases** with an expiry, so a node
  that dies mid-critical-section cannot deadlock the swarm — the lease
  expires and the coordinator revokes it.
- **Enforced, not advisory.** While node A holds a region's lock, a
  read or write from anyone else **fails fast** with `ERROR(LOCKED)` —
  it neither interleaves silently nor blocks. (Blocking would stall the
  other node's executor invisibly mid-WASM-call; a fast error lets that
  module retry or take the lock properly.)

Consistency model: phase 1 is *no caching* — every access is a
round-trip, so it is sequentially consistent and slow (~ms per access vs
ns locally; the API shape pushes users toward bulk transfers). Phase 3
adds a node-side page cache with **release consistency**: acquire a lock
→ invalidate cached pages of that region; release → write back dirty
pages. That is the classic DSM trade (IVY/TreadMarks lineage) and it is
where the real performance lives.

### Implementation todo list

Phase 1 — explicit remote memory, end to end (**implemented**):

- [x] Protocol v2 in `protocol.h`: node-initiated types (`MEM_READ`,
      `MEM_DATA`, `MEM_WRITE`, `MEM_ACK`, `LOCK`, `LOCK_GRANT`,
      `UNLOCK`, `REGION_INFO` for descriptor fetch), `ERROR(LOCKED)`
      code, separate node-side seq space, `HELLO` feature flag.
- [x] Firmware RPC core (`app/src/rpc.c`): blocking
      executor→coordinator request/response slot (semaphore + timeout),
      reply routing in the agent; a silent coordinator traps the module
      (`ERROR(TRAP)`) after `CONFIG_WASP_RPC_TIMEOUT_MS` and the node
      carries on.
- [x] Host function module (`app/src/wamr/native_remote.c`): `wasp.*`
      natives registered with WAMR; module buffer pointers validated
      and translated by WAMR's `*~` signature markers; imports from the
      `wasp` namespace now resolve at instantiation.
- [x] Coordinator side (`tools/wasp_client.py`): region registry
      (`register_region(id, bytearray)`), serves `MEM_READ`/`MEM_WRITE`
      inline while requests are in flight, lease-based locks with
      expiry-revocation, fail-fast `LOCKED` rejection of non-holders,
      authoritative bounds checks.
- [x] Tests (`wasp_client.py <ip> remote tools/remote_module.wasm` +
      deaf-coordinator timeout test): remote sum (element-wise, bulk,
      via reference arithmetic), remote write-back, locked
      read-modify-write, fail-fast `LOCKED` for reads/writes/locks
      while a rival holds the lease, lease expiry revocation,
      out-of-bounds rejection, RPC timeout → `TRAP` with node survival.
- [x] Swarm test (`tools/swarm_test.py <module> <ip> <ip> ...`, or
      `... discover` to use ANNOUNCE broadcasts): drives N nodes
      concurrently against one shared coordinator memory —
      distributed sum sliced across the swarm (exact), unlocked
      read-modify-write from all nodes **loses updates** (~55–65% lost
      with 5–6 real nodes racing), the same workload under `wasp_lock`
      is exact. The concurrency story is proven on hardware.

Phase 2 — ergonomics:

- [ ] `wasp/remote.h` for C: `wasp_remote` address-space qualifier,
      packed-reference helpers, bulk read/write wrappers, lock guard
      macros — usable stand-alone (explicit calls) before the pass
      plugin exists.
- [ ] LLVM pass plugin (`-fpass-plugin=`): lower loads/stores through
      address space 1 to `__wasp_remote_load/store_*` imports; verify
      pointer arithmetic, structs, and arrays on packed references;
      unsupported constructs must be a compile error, never a silent
      miscompile.
- [ ] `wasp/remote.hpp` for C++: `remote_ptr<T>` with overloaded
      `operator*` / `operator->` / `operator[]` and write-through proxy;
      `remote_lock` RAII guard — same transparency, no compiler plugin
      needed. Verify clang wasm32 C++ (`-fno-exceptions -fno-rtti
      -nostdlib`) fits the size budget.
- [ ] `docs/writing-modules.md`: new chapter with a worked
      pass-by-reference example and performance guidance (batch, don't
      peek).

Phase 3 — performance:

- [ ] Node-side page cache (DTCM candidate) with dirty tracking,
      write-back on unlock/flush, invalidate on lock acquire; cache
      statistics queryable over the protocol.

Phase 4 — research:

- [ ] Un-annotated code: ASan-style instrument-every-access pass, or a
      transparent raw-pointer window via interpreter bounds-check hook /
      shared-heap adaptation; upstream-ability of such a WAMR patch.

## Status / roadmap

- [x] Architecture (this document)
- [x] Workspace scaffolding: west manifest, Zephyr app skeleton, WAMR integration
- [x] Thread + queue skeleton (network handler, agent, executor)
- [x] Wire protocol implementation (framing, HELLO/PING/ERROR)
- [x] WASM module lifecycle (load / call / unload) end to end — i32-only v1 calling convention
- [x] Remote memory phase 1: modules read/write/lock coordinator RAM via
      `wasp.*` host functions (see
      [Remote memory — design plan](#remote-memory--design-plan));
      phases 2–4 (typed transparency, caching, un-annotated code) planned
- [x] Second board: Raspberry Pi Pico W (RP2040 + CYW43439 WiFi) — full
      suite passing on Ethernet and WiFi nodes simultaneously
- [x] 6-node swarm (1× Nucleo + 5× Pico W): distributed sum, cross-node
      lost-update race demonstrated, lock exactness under contention
- [x] Fleet ergonomics: status LED, UDP `ANNOUNCE` discovery, `IDENTIFY`
      LED strobe, remote `REBOOT` (incl. RP2040 USB-bootloader entry for
      cable-free reflashing)
- [ ] Coordinator (separate effort)
- [ ] More boards
