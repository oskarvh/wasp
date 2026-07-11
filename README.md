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

A module that traps (e.g. `unreachable`, out-of-bounds access) returns
`ERROR(TRAP, exception-text)` and the node carries on — the WASM sandbox
holds, and the next `CALL` works.

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
│       ├── net/            # network handler thread + framing
│       ├── agent/          # agent thread + protocol dispatch
│       └── wamr/           # thin wrapper around the WAMR runtime + executor
├── deps/                   # west-managed dependencies (NOT committed)
│   ├── zephyr/
│   └── modules/            # hal_stm32, cmsis, wasm-micro-runtime, …
├── docs/
│   └── writing-modules.md  # C function -> node -> result, step by step
├── tools/
│   ├── wasp_client.py      # protocol test client (embryonic coordinator)
│   ├── test_module.c       # test WASM module (add/fib/boom)
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

First target: **ST Nucleo-F439ZI** (`nucleo_f439zi`) — Cortex-M4F @ 180 MHz,
192 KiB SRAM + 64 KiB CCM, on-board Ethernet (LAN8742 PHY), which makes it a
convenient first swarm node.

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

# build & flash
west build -b nucleo_f439zi app
west flash                 # needs stlink (openocd or STM32CubeProgrammer)
```

Talking to a node (IP is on the serial console, 115200 baud):

```sh
python3 tools/wasp_client.py <node-ip> check        # protocol self-test
tools/build_test_module.sh                          # needs clang + wasm-ld
python3 tools/wasp_client.py <node-ip> lifecycle tools/test_module.wasm
python3 tools/wasp_client.py <node-ip> load tools/test_module.wasm
python3 tools/wasp_client.py <node-ip> call fib 20
python3 tools/wasp_client.py <node-ip> unload
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

## Status / roadmap

- [x] Architecture (this document)
- [x] Workspace scaffolding: west manifest, Zephyr app skeleton, WAMR integration
- [x] Thread + queue skeleton (network handler, agent, executor)
- [x] Wire protocol implementation (framing, HELLO/PING/ERROR)
- [x] WASM module lifecycle (load / call / unload) end to end — i32-only v1 calling convention
- [ ] Coordinator (separate effort)
- [ ] More boards
