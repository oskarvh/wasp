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
`tools/wasp_client.py`. Sequence diagrams for every command and failure
path: [docs/diagrams/](docs/diagrams/README.md).

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
  and feature bits (`0x01` remote memory, `0x02` atomic primitives)
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
- `MEM_READ` / `MEM_WRITE` / `LOCK` / `UNLOCK` / `REGION_INFO` /
  `MEM_ADD` / `MEM_CAS` (0x4x) — **node-initiated** remote-memory
  requests, sent mid-`CALL` in the node's own seq space and answered by
  the coordinator; see [Remote memory](#remote-memory--design-plan)

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

Broadcast datagrams do not cross routers, so `ANNOUNCE` discovery is
LAN-only by nature. Reaching a coordinator on another network is a
planned expansion (unicast registration to a DNS-resolved coordinator
hostname — see the roadmap); DNS-based tricks cannot make an actual
*broadcast* leave the LAN.

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
│   ├── writing-modules.md  # C function -> node -> result, step by step
│   ├── performance.md      # measured remote-memory costs + optimization guide
│   ├── demo.md             # the distributed Mandelbrot demo, explained
│   └── diagrams/           # PlantUML sequence diagrams for every protocol scenario
├── tools/
│   ├── wasp_client.py      # test client + remote-memory host (embryonic coordinator)
│   ├── swarm_test.py       # multi-node concurrency test (distributed sum, race,
│   │                       #   fail-fast vs queued locks vs atomic MEM_ADD)
│   ├── include/wasp/remote.h   # module-side remote memory API (C)
│   ├── include/wasp/remote.hpp # remote_ptr<T> / remote_lock (C++, no plugin)
│   ├── wasp-remote-pass/   # LLVM pass plugin: lowers wasp_remote dereferences
│   ├── lib/wasp_remote_rt.c    # runtime shims the pass lowers into
│   ├── perf_test.py        # remote-memory performance characterization (docs/performance.md)
│   ├── mandelbrot_demo.py  # distributed-render demo coordinator (docs/demo.md)
│   ├── test_module.c       # test WASM module (add/fib/boom)
│   ├── remote_module.c     # remote-memory test module (explicit API)
│   ├── perf_module.c       # performance-measurement module (driven by perf_test.py)
│   ├── mandelbrot_module.c # fixed-point Mandelbrot kernel + work-stealing loop
│   ├── remote_as_module.c  # transparent wasp_remote pointer test module
│   ├── remote_cpp_module.cpp   # C++ remote_ptr test module
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

To see the whole stack do something at once, run the **distributed
Mandelbrot demo**: the swarm renders an image into coordinator RAM,
live in your terminal, self-balancing via an atomic work counter and
surviving node loss mid-render ([docs/demo.md](docs/demo.md)):

```sh
python3 tools/mandelbrot_demo.py discover
```

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
Phases 1 and 2 (explicit host functions, then transparent remote
pointers) plus most of phase 3 (queued locks, atomic primitives) are
implemented and tested on hardware; the checklists below track the
details, and phase 4 is research.

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
   `#define wasp_remote __attribute__((address_space(100)))`, then
   `wasp_remote int *arr` reads like a normal pointer (`arr[i]`,
   `p->field`, pointer arithmetic) but is a distinct type — assigning it
   to a local pointer without an explicit cast is a **compile error**,
   so local/remote confusion is caught at build time and propagated
   automatically through every assignment, argument, and dereference. A
   small LLVM pass plugin (`-fpass-plugin=`) rewrites loads/stores
   through that address space into `__wasp_remote_load/store` calls, which
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
  ambiguity. Advertised as feature bits in `HELLO_ACK`.
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
  it neither interleaves silently nor blocks. (Blocking a plain access
  would stall the other node's executor invisibly mid-WASM-call; a fast
  error lets that module retry or take the lock properly.) A contended
  `LOCK` *request* is the exception: the coordinator may park it in a
  per-region FIFO and grant it when the holder unlocks — see the
  phase-3 notes below.
- **Single-word read-modify-writes don't need a lock at all**: the
  `MEM_ADD` / `MEM_CAS` primitives do the whole update inside the
  coordinator's serialization window, one round-trip, race-free
  against every other node.

Consistency model: today there is *no caching* — every access is a
round-trip, so it is sequentially consistent and slow (~ms per access vs
ns locally; the API shape pushes users toward bulk transfers). Phase 3's
remaining item is a node-side page cache with **release consistency**:
acquire a lock → invalidate cached pages of that region; release → write
back dirty pages. That is the classic DSM trade (IVY/TreadMarks lineage)
and it is where the real performance lives.

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

Phase 2 — ergonomics (**implemented**, verified on Nucleo + Pico W):

- [x] `wasp/remote.h` for C: `wasp_remote` address-space qualifier,
      `WASP_REMOTE_PTR()` helper, `wasp_locked()` scoped lock guard —
      usable stand-alone (explicit calls) without the pass plugin.
- [x] LLVM pass plugin (`tools/wasp-remote-pass/`, `-fpass-plugin=`):
      lowers loads/stores through address space **100** (AS 1/10/20 are
      reserved by the wasm backend) to `__wasp_remote_load/store_*`
      shims (`tools/lib/wasp_remote_rt.c` — plain phase-1 imports, no
      firmware change), and remote↔local `memcpy` to single bulk RPCs,
      so whole-struct assignment is one round-trip. Pointer arithmetic,
      indexing, i8→f64 widths verified on hardware; unsupported
      constructs (remote↔local casts, remote memset, remote-to-remote
      copies, atomics) fail the compile — never a silent miscompile.
- [x] `wasp/remote.hpp` for C++: `remote_ptr<T>` with `operator*` /
      `operator->` / `operator[]` and a write-through proxy,
      `remote_lock` RAII guard, `try_read`/`try_write` bulk path — no
      compiler plugin needed. clang wasm32 C++ (`-fno-exceptions
      -fno-rtti -nostdlib`) output: ~1 KiB module.
- [x] Failed transparent dereferences trap the module (the "remote
      segfault"), reported as `ERROR(TRAP)`; expected-failure paths
      (contended locks) keep using explicit error codes.
- [x] `docs/writing-modules.md`: chapter with worked C and C++
      examples and performance guidance (batch, don't peek);
      self-test: `wasp_client.py <ip> remote2 …` (23 checks).

Phase 3 — performance (queueing + atomics **implemented**; measured on
the 6-node swarm, 150 contended increments):

- [x] Coordinator-side lock queueing: a `LOCK` on a held region parks
      in a per-region FIFO (bounded wait, then `ERROR(LOCKED)` after
      all) instead of failing fast; the grant is sent the moment the
      holder unlocks. Coordinator policy — no node/protocol change.
      Kills the retry storm: ~1000 wasted `ELOCKED` round-trips →
      **zero**, with FIFO fairness. Lease clock starts at grant; the
      parked wait is bounded (4 s) to stay under the node's RPC
      timeout.
- [x] Atomic RPC primitives `MEM_ADD` / `MEM_CAS`: the coordinator
      already serializes each request, so doing the read-modify-write
      *inside* that window is race-free with no lock and no lease —
      one round-trip per increment (~10 ms measured). Module API:
      `wasp_add`/`wasp_cas` (C), `fetch_add`/`compare_exchange` on
      `remote_ptr<T>` (C++); advertised as feature bit 0x02.
- [x] RPC latency (found while measuring the above): node-initiated
      requests waited up to 50 ms in the TX queue for the serve loop's
      poll tick, and header+payload as two `send()`s stalled on
      Nagle + delayed ACK. `SERVE_POLL_MS` 50→2 plus `TCP_NODELAY` on
      both ends ≈ 4× across every remote-memory workload.
- [ ] Node-side page cache (DTCM candidate) with dirty tracking,
      write-back on unlock/flush, invalidate on lock acquire; cache
      statistics queryable over the protocol.

Measured end to end (`tools/swarm_test.py`, 2026-07-13): 150 increments
of one shared counter, contended by all 6 nodes (1× Nucleo-F439ZI +
5× Pico W), each strategy run back to back on the same hardware:

| strategy | time | `ELOCKED` retries |
| --- | --- | --- |
| fail-fast locks, before latency fixes | 60.8 s | 1038 |
| fail-fast locks, after latency fixes | 13.6 s | 783 |
| queued locks | 10.2 s | **0** |
| atomic `MEM_ADD` | **1.5 s** | **0** |

40× total on the contended counter; the latency fixes alone also cut
the *uncontended* distributed-sum phase from 0.70 s to 0.22 s. Two
readings worth keeping: measuring in stages is what exposed the TX-poll
bottleneck at all (queueing looked like a modest 1.3× until the real
cost was traced to the serve loop, not the locks); and queued locks'
wall-clock gain is deliberately modest — a lock-serialized workload is
bounded by hold time regardless — its real value is deleting ~800
garbage round-trips, FIFO fairness, and traffic that no longer grows
with contention.

A full performance characterization — per-RPC cost on both transports,
batching, alignment, layout pitfalls, lock partitioning, `MEM_ADD` vs
CAS scaling — lives in [docs/performance.md](docs/performance.md)
(`tools/perf_test.py` + `tools/perf_module.c`).

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
      [Remote memory — design plan](#remote-memory--design-plan))
- [x] Remote memory phase 2: transparent dereferencing — `wasp_remote`
      pointers in C (LLVM pass plugin) and `remote_ptr<T>` in C++
      (header-only)
- [x] Remote memory phase 3 (partial): queued locks, atomic
      `MEM_ADD`/`MEM_CAS`, RPC latency fixes — contended counter 60 s
      → 1.5 s on the 6-node swarm; node-side caching still open
- [x] Second board: Raspberry Pi Pico W (RP2040 + CYW43439 WiFi) — full
      suite passing on Ethernet and WiFi nodes simultaneously
- [x] 6-node swarm (1× Nucleo + 5× Pico W): distributed sum, cross-node
      lost-update race demonstrated, lock exactness under contention
- [x] Fleet ergonomics: status LED, UDP `ANNOUNCE` discovery, `IDENTIFY`
      LED strobe, remote `REBOOT` (incl. RP2040 USB-bootloader entry for
      cable-free reflashing)
- [x] Distributed Mandelbrot demo ([docs/demo.md](docs/demo.md)):
      work-stealing via `MEM_ADD`, live tile-by-tile terminal view,
      per-node provenance image, node-loss recovery with work re-issue
      — 256×192 in 4 s on the 6-node swarm
- [ ] WAN discovery — announce to a coordinator outside the LAN.
      Feasibility checked (2026-07): broadcasts can't leave the LAN, but
      the same `ANNOUNCE` datagram can be **unicast** to a configured
      coordinator hostname (`CONFIG_WASP_COORDINATOR_HOST`), resolved
      on-node with Zephyr's DNS client (`CONFIG_DNS_RESOLVER`;
      `CONFIG_NET_DHCPV4_OPTION_DNS_ADDRESS` feeds it the DHCP-supplied
      DNS server automatically — measured cost on the Pico W: ~5 KiB
      RAM + ~9 KiB flash, fits today). Caveat: a coordinator behind the
      internet usually can't TCP-connect *back* through the node's NAT,
      so the follow-on step is a node-initiated "phone home" TCP
      connection — the wire protocol already runs traffic in both
      directions over one socket (the 0x4x range), so only who dials
      changes. mDNS/DNS-SD was considered and rejected: it is
      link-local, same as broadcast.
- [ ] Multiple WAMR instances per node — on RAM-rich MCUs (RP2350:
      520 KiB, STM32H7: ~1 MiB, ESP32-S3: up to 8 MiB PSRAM) one
      physical node could host N module instances and present as N
      virtual workers. Feasible: WAMR already supports concurrent
      modules/instances in one runtime, and each instance is
      independent, so the cost is per-instance RAM (its ≥64 KiB linear
      memory + stack) plus bounded firmware work — instance handles in
      `LOAD_MODULE`/`CALL`/`UNLOAD_MODULE`, one executor thread (and
      exec_env) per running call instead of the single executor, and
      the node→coordinator RPC slot generalized to a small table keyed
      by instance. Today's fleet stays single-instance: the Pico W is
      at ~93% RAM and the F439's pool fits exactly one 64 KiB page.
- [ ] Coordinator (separate effort)
- [ ] More boards
