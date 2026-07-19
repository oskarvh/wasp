# Future hardware: a purpose-built wasp node board

Parked notes (2026-07-18) for a custom node board targeting the
"multiple WAMR instances per node" roadmap item: Ethernet + WiFi and
enough RAM to host N module instances on one physical node. Nothing
here is started — this is the design rationale so it doesn't have to
be re-derived later.

## Why external RAM works (and its constraints)

WAMR is just C running out of a heap we hand it, so external RAM has
no fundamental blockers. The constraints are performance topology:

1. **The external RAM must sit behind a data cache.** The interpreter
   touches memory several times per opcode; on a core with no D-cache
   (Cortex-M4/M0+ — the F4's ART accelerator only covers flash) every
   access pays full bus latency, roughly a 3–8× interpreter slowdown.
   Behind a real D-cache (Cortex-M7-class) the penalty drops to tens
   of percent. This is the single biggest MCU-selection criterion.
2. **DMA coherence.** Cached external RAM + DMA masters (Ethernet MAC)
   is the classic STM32H7 bug-farm. Sidestep it architecturally:
   network buffers, DMA descriptors, kernel and thread stacks stay in
   internal SRAM; external RAM is CPU-only WAMR territory.
3. **Only the WAMR pool moves out** — module storage, linear memories,
   per-instance structures (~95% of the footprint, and exactly what
   multi-instance needs). Integration is one linker-section /
   devicetree change (Zephyr `shared-multi-heap`). The
   ≥64 KiB-contiguous-per-instance problem that dominates today's RAM
   budget disappears with megabytes of contiguous external RAM.
4. **Atomics are a non-issue.** `LDREX`/`STREX` to external buses can
   be unsupported, but wasp atomics are coordinator-side RPCs and WAMR
   is built without shared memory/threads. Keep OS sync primitives in
   internal SRAM.
5. **Interpreter-only keeps it simple.** No execute-from-external-RAM
   requirement; only AOT/JIT would need an XIP/I-cache path. Don't
   design for that.
6. **Bus bandwidth shared across instances won't bottleneck** before
   the CPU does, given the cache.

Also note: N instances on one core buy *concurrency and isolation*
(host several modules at once), not throughput.

## MCU choice

The deciding constraint: PSRAM-as-transparent-heap needs
**memory-mapped writes**, which means **OCTOSPI** — the H72x/H73x
line. The H743's QUADSPI only memory-maps reads (writes are
indirect-mode), which would force FMC SDRAM: ~45 length-matched pins
and big packages, the opposite of cheap. The H7A3/H7B3 have OCTOSPI
and 1.4 MB SRAM but dropped the Ethernet MAC — disqualified.

**Pick: STM32H723ZGT6** — LQFP144, 550 MHz M7 (I/D-cache), 1 MB
flash, 564 KB internal RAM, Ethernet MAC, OCTOSPI. ~$10–12 @ qty 1–10
(2026 LCSC ballpark; re-quote before committing).

- **LQFP144 is the system-cost sweet spot**: 0.5 mm pitch, no
  via-in-pad, plain 4-layer stackup, hand-reworkable. BGA saves board
  area we don't need and costs HDI/6-layer + assembly yield.
- **144 pins** fit RMII (9) + octal PSRAM (11) + WiFi UART + SWD +
  console without pin-mux gymnastics. LQFP100 (H723VGT6) can fit but
  saves only ~$1–2.
- **1 MB internal flash → no external flash, no custom bootloader.**
  This is why the tempting H730 value line (~$6, 128 KB flash) is a
  trap: Zephyr + net stack + WAMR won't fit, so it needs QSPI NOR +
  XIP boot complexity.
- Skip **H725/H735** (integrated SMPS: adds an inductor, complicates
  the power tree for nothing on a powered node) and **H733/H735**
  (crypto accelerators, unused).
- **Bring-up is free**: `nucleo_h723zg` is a supported Zephyr board in
  the same 144-pin Nucleo format as the F439ZI already in the fleet.
  Buy one (~$30), port the firmware, validate OCTOSPI PSRAM on a
  breakout — then the custom board is essentially the Nucleo minus
  what we don't need. Stays on the existing gnuarmemb toolchain.

Budget alternative considered: RP2350 + QSPI PSRAM on QMI CS1
(cached) + W5500 SPI Ethernet + CYW43 WiFi — closest to the existing
Pico flow, but no native Ethernet MAC. ESP32-S3 rejected (no Ethernet
MAC, new Xtensa toolchain); ESP32-S2 devkit rejected as a supported
board (single core, no PSRAM guarantee, Xtensa toolchain friction).

## Rest of system

| block | part | ~cost | note |
| --- | --- | ---: | --- |
| PSRAM | APS6408L-OBM (8 MB, octal) | $2–3 | 11 signals, easy routing, memory-mapped R/W via OCTOSPI |
| Eth PHY | LAN8742A + integrated-magnetics jack | $3–4 | same PHY as the Nucleo boards → driver already proven |
| WiFi | ESP32-C3-MINI module, ESP-AT over UART | $2–3 | cheapest path with an in-tree Zephyr driver; CYW43439 is the fancier alternative |
| Power | one 3.3 V buck; H723 runs its internal LDO | $1–2 | no SMPS variant needed |

BOM lands around **$20–25** plus a 4-layer PCBA run, with no BGA
anywhere (the PSRAM and ESP module are SOP/castellated) — cheapest
assembly tier.

## Firmware work implied

Unchanged from the roadmap entry in the main README: instance handles
in `LOAD_MODULE`/`CALL`/`UNLOAD_MODULE`, one executor thread +
exec_env per running call, the node→coordinator RPC slot generalized
to a table keyed by instance — plus, for this board, the OCTOSPI
PSRAM devicetree/linker plumbing and pointing the WAMR pool at it.
