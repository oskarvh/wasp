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
┌────────────────────────────────────────────────────────────────────┐
│                                                                    │
│  ┌───────────────┐  rx msgq   ┌──────────────┐  exec q  ┌────────┐ │
│  │ network       │ ─────────▶ │ agent        │ ───────▶ │ wasm   │ │
│  │ handler       │            │              │          │ executor│ │
│  │ thread        │ ◀───────── │ • protocol   │ ◀─────── │ thread │ │
│  └───────┬───────┘  tx msgq   │   dispatch   │ results  └───┬────┘ │
│          │                    │ • module     │              │      │
│   Zephyr network stack        │   lifecycle  │        WAMR runtime │
│   (sockets, DHCP/static IP)   └──────────────┘        (interpreter,│
│          │                                             static heap │
│   Ethernet driver (per-board)                          pool)       │
└──────────┴─────────────────────────────────────────────────────────┘
```

### Threads

| Thread              | Responsibility                                                                                                                              |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| **network handler** | Brings up the network interface (DHCP by default), runs the TCP server socket, frames/deframes protocol messages, forwards complete commands to the agent via the rx queue, and drains the tx queue back to the coordinator. Owns the socket exclusively. |
| **agent**           | The node's brain (such as it is). Consumes commands from the rx queue, validates them, manages WASM module lifecycle (load / instantiate / unload) through the WAMR wrapper, dispatches execution requests to the executor, and turns results/errors into response messages on the tx queue. Never blocks on WASM execution. |
| **wasm executor**   | Calls into WAMR to run exported functions of the loaded module. Isolated in its own thread (with its own stack budget) so a long-running or misbehaving module cannot wedge the agent or the network handler. Posts results back to the agent. |

Zephyr's own threads (net stack rx/tx, system workqueue, logging) run
alongside these.

### Message flow

1. Coordinator connects and sends a framed command (e.g. *load module*,
   *call function*, *query status*).
2. Network handler validates the frame and posts it to the **rx msgq**.
3. Agent dispatches: lifecycle commands are handled inline via the WAMR
   wrapper; execution commands are posted to the **exec queue**.
4. Executor runs the WASM function and posts the outcome back to the agent.
5. Agent formats a response frame onto the **tx msgq**; network handler
   writes it to the socket.

### Wire protocol (draft)

Length-prefixed binary frames over a single TCP connection (default port
`4242`, configurable via Kconfig):

```
┌────────┬────────┬───────────┬─────────────┬───────────┐
│ magic  │ type   │ seq       │ payload len │ payload   │
│ 2 B    │ 1 B    │ 1 B       │ 4 B LE      │ n B       │
└────────┴────────┴───────────┴─────────────┴───────────┘
```

Initial command set (to be refined together with the coordinator design):

- `HELLO` / `HELLO_ACK` — version + capability exchange
- `LOAD_MODULE` — payload is a `.wasm` binary; node instantiates it
- `UNLOAD_MODULE`
- `CALL` — invoke an exported function with arguments
- `RESULT` / `ERROR` — node → coordinator
- `PING` / `PONG` — liveness

### Memory strategy

WAMR runs from a **static heap pool** (`Alloc_With_Pool`) so WASM memory is
bounded and isolated from the Zephyr kernel heap. Pool size, thread stack
sizes, and the maximum module size are Kconfig options tuned per board.

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

Note on WAMR integration: WAMR's `zephyr/module.yml` declares external
CMake/Kconfig glue but does not ship it; `app/modules/wamr/` provides that
glue and builds WAMR's `vmlib` as a Zephyr library, enabled by
`CONFIG_WAMR=y`. The WASM heap pool is placed in tightly-coupled memory
(CCM/DTCM) automatically on boards that have it — on the F439 the whole
64 KiB CCM belongs to WAMR, leaving main SRAM for network buffers and
stacks.

## Status / roadmap

- [x] Architecture (this document)
- [x] Workspace scaffolding: west manifest, Zephyr app skeleton, WAMR integration
- [x] Thread + queue skeleton (network handler, agent, executor)
- [ ] Wire protocol implementation
- [ ] WASM module lifecycle (load / call / unload) end to end
- [ ] Coordinator (separate effort)
- [ ] More boards
