# Future hardware: a purpose-built wasp node board

Parked notes (2026-07-18, revised 2026-08-25) for a custom node board.
The point of the board is to run **wasp** well: enough RAM to host N
WAMR instances on one physical node (the "multiple instances per node"
roadmap item), and an Ethernet switch on every node so a fleet cables
itself together instead of fanning out to a desk switch. Nothing here
is started; this is the design rationale so it doesn't have to be
re-derived later.

Settled:

- **STM32H723ZGT6** — ~$11 @ qty 10, DigiKey (quoted 2026-08).
- **Octal PSRAM over OCTOSPI** for WAMR linear memory.
- **KSZ8794CNX switch** — $6.22/unit, DigiKey (quoted 2026-08) —
  three copper ports (one upstream, two downstream) on an RMII link to
  the MCU. QFN-64 and its extra 1.2 V rail are both accepted.
- **No WiFi.** This is a wired board.
- **Self-powered nodes**: USB-C (5 V, no PD) and an **XT30** 5 V DC
  connector, OR-ed. No PoE.
- **USB-C carries data**, not just power: CDC-ACM console and DFU
  flashing over the same cable.
- **The MCU does not participate in forwarding.** The switch is
  configured once at boot and then runs autonomously — which, per the
  datasheet findings below, means **RSTP is out** and hardware storm
  protection is in.

Switch details below are verified against the KSZ8794CNX datasheet,
Microchip DS00002134C (2016–2021), unless marked otherwise.

![block diagram](diagrams/node-board.svg)

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
and 1.4 MB SRAM but dropped the Ethernet MAC — disqualified, and the
switch doesn't rescue them: the switch replaces the *PHY*, not the
MAC, so the MCU still needs one.

**Pick: STM32H723ZGT6** — LQFP144, 550 MHz M7 (I/D-cache), 1 MB
flash, 564 KB internal RAM, Ethernet MAC, OCTOSPI.

- **LQFP144 is the system-cost sweet spot**: 0.5 mm pitch, no
  via-in-pad, plain 4-layer stackup, hand-reworkable. BGA saves board
  area we don't need and costs HDI/6-layer + assembly yield.
- **RMII to the switch is the right interface**, and the reason the
  pin budget is comfortable: 7 signals against MII's 16, natively
  supported by the H7 MAC, and already the interface both fleet
  Nucleos run — so the `st,stm32h7-ethernet` side is proven silicon
  and proven driver. Budget: RMII (7) + switch SPI (4) + reset (1) +
  octal PSRAM (11) + USB D+/D− (2) + SWD + UART console. Dropping the
  PHY's MDIO/MDC pair
  pays for the switch's SPI, and dropping WiFi returns a UART.
  LQFP144 is not close to full; LQFP100 (H723VGT6) would now fit too,
  but saves only ~$1–2 and removes all headroom — stay at 144.
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
  breakout — then the custom board is essentially the Nucleo, minus
  what we don't need, plus the switch. Stays on the existing gnuarmemb
  toolchain.

## Every node is a switch

Each node carries a **KSZ8794CNX** managed Ethernet switch. The
datasheet describes it as "intended for cost-sensitive applications
requiring three 10/100 Mbps copper ports and one 10/100/1000 Mbps
Gigabit uplink port" — concretely, three integrated 10/100BASE-T/TX
MAC/PHYs (ports 1–3) plus one GMAC on **port 4** with selectable
RGMII, MII, or RMII. Three copper ports go to RJ45 jacks; port 4 is
the internal RMII link to the H723's MAC. The node is therefore *both*
a switch in the fabric and a host on it — one MAC, one IP, exactly as
today.

**Three copper ports is the requirement, not a compromise: one
upstream, two downstream.** The part matches it exactly. Nothing
smaller in the family reaches three copper (KSZ8863/8873 and LAN9354
are 2 PHY + 1 MAC); the 5-port KSZ8795CLX buys a fourth we don't want.

> **Datasheet erratum to be aware of:** the Features list on page 1
> claims "Four Integrated IEEE 802.3/802.3u-Compliant Ethernet
> Transceivers." The general description, the MAC list ("Three
> internal MAC1–MAC3 units and one internal GMAC4"), and the pin
> table all say three. It is copy-paste from the 5-port KSZ8795;
> Microchip has a support article on KSZ8765/8775/8794/8795
> documentation updates. Three is correct.

### What it buys

- **No infrastructure.** Nodes chain node-to-node; the root's upstream
  port plugs into the house network for DHCP and the coordinator. A
  15-node fleet stops needing a 16-port switch and 15 cables back to
  it.
- **The switch outlives the firmware.** Switch silicon forwards
  independently of the MCU, so a node that is crashed, reflashing, or
  sitting in the bootloader **still passes traffic for the nodes
  behind it**. Only a power cut partitions the tree. This is a real
  property for a project whose demo deliberately kills workers
  mid-render (`CONFIG_WASP_CONDUCTOR_CHAOS`).
- **Discovery is unchanged.** ANNOUNCE is a UDP broadcast; the switch
  floods broadcast to every port, so `wasp_discover` sees the whole
  tree exactly as it sees a flat desk switch today.
- **One crystal for the pair.** The switch runs from a single 25 MHz
  ±50 ppm reference and can *output* the 50 MHz RMII reference clock
  to the MAC (strap: 25 MHz OSC in, 50 MHz RMII clock out on RXC4).
  No second oscillator, no clock-source negotiation with the H723.

### Latency: the fan-out of two is what saves it

The fabric is confirmed "non-blocking store-and-forward" — no
cut-through — so each hop re-serializes the whole frame. At 100 Mbit/s
that is 80 ns per byte plus ~2 µs of fabric: ~10 µs per hop for a
small RPC frame, ~86 µs for a 1 KiB one. Against the baselines in
[performance.md](performance.md) (~2.8 ms per Ethernet round-trip,
~3.2 ms for 1 KiB):

| fleet | max hops, coordinator at root | small RPC | 1 KiB bulk |
| --- | ---: | ---: | ---: |
| 7 nodes (depth 3) | 3 | +2 % | +16 % |
| 15 nodes (depth 4) | 4 | +3 % | +21 % |
| 15 nodes, cabled as one chain | 15 | +11 % | +81 % |

**This is the argument for two downstream ports.** A bus puts depth at
O(N) and makes bulk transfers pay for it; a binary tree puts depth at
O(log N) and the penalty stops mattering. Small RPCs — the thing that
actually governs wasp performance, per performance.md's T1 — are
unaffected either way, because they are overwhelmingly host-stack
time. But T2's "batch, don't peek" advice produces exactly the large
frames that pay per hop, so the topology rule earns its keep: **keep
the tree balanced, and put the coordinator at the root.**

Trunk bandwidth is *not* the binding constraint yet: a node issuing
1 KiB RPCs back-to-back at 2.8 ms each generates ~3 Mbit/s, so even
eight of them through a shared 100 Mbit/s trunk sit around a quarter
of the link. Module pushes and framebuffer dumps are the bursts to
watch, not steady RPC traffic. Port 4 is gigabit-capable over
RGMII/GMII, but the H723's MAC is 10/100, so RMII at 100 Mbit is the
matched choice and costs us nothing we had.

### RSTP: real, but it runs on the MCU

The datasheet does list it — "Rapid Spanning Tree Support (RSTP) for
Topology Management and Ring/Linear Recovery." What that means in
practice is settled by **Table 3-13, titled "Port Setting and Software
Actions for Rapid Spanning Tree"**: for each RSTP port state the table
gives a register setting *and* a required processor action. The
Discarding state is "Transmit enable = 0, Receive enable = 0, Learning
disable = 1," annotated "**The processor** should not send any packets
to the port… the processor should discard those packets," with the
note "processor is connected to Port 4." Disabling learning means the
host writes Register 2 Bit[5]/Bit[4] to flush the port's dynamic and
static MAC-table entries.

So the switch supplies the *primitives* — per-port transmit/receive
enable, learning disable, MAC-table flush, plus **tail tagging** on
port 4 (one byte before the FCS telling the processor which ingress
port a frame arrived on, which is exactly what a BPDU handler needs).
The 802.1w state machine itself — port roles, timers, the
proposal/agreement handshake, topology-change propagation — is host
software. Zephyr has none (verified: no STP/RSTP/BPDU code in 4.4.1
outside gPTP's unrelated state machine), and Zephyr 4.4 additionally
*deleted* the `microchip,ksz8794` DSA driver that would have supplied
tail-tag handling, because nobody migrated it to the reworked DSA
subsystem (upstream PR 105926, `doc/releases/migration-guide-4.4.rst`).

**This is incompatible with the goal.** Running RSTP would put the MCU
in the forwarding path — tail-tag insert/strip on every frame through
the Ethernet hot path, plus a protocol state machine — which is
precisely what this board is supposed to avoid. **RSTP is therefore
out**, and the KSZ8794 stays: no other 10/100 switch IC runs 802.1w
autonomously either, so this is not a part-selection problem. (The
chips that do are switch SoCs with an application core running Linux
and `mstpd` — a different board and a different price class.)

**Deferring this forecloses nothing.** Everything 802.1w would need is
already on the board because it is needed for other reasons: port 4 is
the CPU port (tail tagging is a register setting away), the SPI link
reaches the port-state and static-ALU registers, and the cabling itself
permits closing a ring — plugging a leaf's downstream port into another
leaf is a patch cable, not a respin. RSTP here is a pure software
decision, deferrable indefinitely at zero hardware cost, and the
topology is stable enough (this is wired Ethernet on a desk, not a mesh
radio) that convergence speed was never going to be the deciding factor.

**And with a tree, none of this is needed anyway.** RSTP breaks loops
in topologies that have them; one upstream and two downstream ports,
cabled as intended, has no cycle to break. Committing to the tree
means committing to *zero* lines of spanning-tree code — not a reduced
amount. What remains is a boot-time SPI configuration routine, which is
a different kind of thing entirely (see
[below](#software-configure-once-then-get-out-of-the-way)). The
primitives Table 3-13 describes still get used, just not by a protocol
— see the port watchdog in the next section.

Worth noting what we give up is smaller than it looks: RSTP converges
in **seconds**. Against 2.8 ms RPCs, any topology change has already
killed every TCP connection through the failed link, so the
coordinator sees `E_CONN` and marks those workers dead long before the
fabric heals. RSTP would restore the fabric for the *next*
`wasp_discover`; it would not rescue an in-flight render. The chaos
recovery path already handles that case and already works.

### Loop protection without the MCU

Two hardware features cover the actual risk, which is miscabling
rather than the intended topology (a tree has no cycle to storm
around):

- **Broadcast Storm Protection with Percentage Control**, global and
  per-port. Rate parameters are programmed globally and the feature is
  enabled or disabled per port; after that it is enforced in hardware.
- **Self-address filtering** — Global Control 18 Register Bit[6], with
  the switch's own MAC in Registers 104–109. Packets sourced by the
  switch itself get filtered at the egress port, so a frame that
  loops back around is dropped rather than circulated.

Both are set once at boot and then run without the processor. There is
**no autonomous loop-detect-and-disable-port feature** in this part, so
on hardware alone a closed ring is still a fault — it degrades instead
of melting. **Topology rule: build a tree, do not close the ring.**

If that ceiling is too low, there is a third tier that costs far less
than RSTP and keeps the MCU out of the data path entirely: an
**out-of-band port watchdog**. The part carries 36 MIB counters per
port; a low-priority thread can poll them over SPI every few seconds,
and a port whose broadcast/multicast rate has the signature of a loop
gets shut down by writing the *same* registers RSTP would have used —
transmit enable = 0, receive enable = 0 — followed by a log line and,
naturally, a report over the wasp protocol.

That is loop detection with none of RSTP's costs: no BPDUs, no tail
tagging, no state machine, no per-frame involvement, nothing in the
Ethernet hot path. It is slow (seconds, like RSTP's own convergence)
and it is unilateral rather than a negotiated topology — which is
exactly right for a tree, where there is no alternate path to
negotiate toward. Call it a day of work rather than weeks, and it is
strictly optional: the two hardware features above are the v1 story.

### Software: configure once, then get out of the way

The switch is managed over **4-wire SPI up to 25 MHz** (client mode
selected by pulling SPIQ down; MDC/MDIO is the alternative, but it
reaches only the MIIM PHY registers, not the switch registers we
need). There is also an I/O pin strapping facility that sets certain
register bits at reset — enough for basic operation, but storm
protection and self-address filtering need real register writes, so
SPI it is.

That gives the v1 firmware story, and it is a small one:

- a **boot-time SPI config routine** — enable broadcast storm
  protection and self-address filtering, set port enables, done. On
  the order of twenty register writes. This is configuration, not
  forwarding: after it returns, the MCU is a plain node on its own
  switch and never touches a frame it didn't send.
- the MCU's RMII link declared as a **fixed link** — Zephyr 4.4's
  `ethernet-phy-fixed-link` compatible with `default-speeds`, since
  there is no PHY to auto-negotiate with on that side — on top of the
  stock `st,stm32h7-ethernet` driver.
- **no tail tagging, no DSA, no per-port netdevs.** All three exist to
  put the CPU in the forwarding path. We don't want it there.

The SPI link earns its keep beyond boot anyway: the part has 36 MIB
counters per port, which are a natural thing for a wasp node to report
over the protocol and the cheapest way to see the fabric from
software. Port mirroring is also available, which makes packet-capture
debugging of the wasp protocol possible without a tap.

### What this breaks or constrains

- **No ring redundancy.** Settled above, and a deliberate trade.
- **Power is now the partition risk.** The "a dead node still
  forwards" property holds only while its switch is powered. Worth
  deciding whether the switch rail is independent of anything the
  firmware can brown out.
- **The power tree grows a rail** (accepted). The KSZ8794CNX needs
  3.3 V analog and **an external 1.2 V LDO for its analog and digital
  cores**; VDDIO can be 3.3/2.5/1.8 V. So: 3.3 V buck + a 1.2 V LDO,
  where the single-PHY design had one rail — a cheap, easy part.
- **The switch is QFN-64, not LQFP** (accepted). Lead-free RoHS 64-pin
  QFN — no BGA, no HDI, but leadless, so it wants hot air or a stencil
  rather than an iron, and the thermal pad wants a via array. Ordinary
  4-layer fab, but the "hand-reworkable" claim covers the MCU, not
  this part.
- **Board area and BOM grow.** Three RJ45 jacks need ~60 mm of board
  edge and roughly double the BOM (below).
- **The proven-PHY argument is gone.** The LAN8742A was chosen partly
  because the Nucleos already use it, so the driver was known-good.
  The switch replaces it, and the MCU-side link becomes a fixed link
  with no PHY driver at all — simpler, but new.
- **No WiFi means no mixed-transport story.** performance.md's ~8×
  WiFi-vs-Ethernet RPC comparison stays valid as measured history, but
  this board can't reproduce it. Existing Pico W nodes still can.
- **The ARP-table gotcha still applies**, and is independent of
  topology: a conductor holding N peer connections needs
  `CONFIG_NET_ARP_TABLE_SIZE` raised from Zephyr's default of *two*
  (see `app/conductor.conf`).

## Power

**Each node powers itself**, from either of two 5 V inputs:

- a **USB-C receptacle**, 5 V only — no PD, no negotiation;
- a **5 V DC connector** for bench and rack use.

### Why not PoE

Tempting on a self-cabling board, and rejected on cost and topology.
A tree node would need to *receive* power and *pass it on*, meaning a
PD **and** a PSE per node — two sets of PoE magnetics plus controllers,
on a board whose entire premise is being cheap. Passive injection down
the spare pairs (100BASE-TX leaves 4-5 and 7-8 free) avoids that, but
not physics: 5 V over 24AWG is ~0.17 Ω/m round trip, so even a 10 m run
at 400 mA costs ~0.7 V, and a 100 m run is hopeless. Passive injection
only works at a higher voltage with a local buck per node, which is a
different design. Self-powered nodes it is.

### Budget

Rough, pending the measurement in the open questions:

| | |
| --- | ---: |
| STM32H723 @ 550 MHz | ~0.8 W |
| KSZ8794CNX, three links up | ~0.5 W |
| PSRAM + LEDs + misc | ~0.3 W |
| **total** | **~1.6 W** |

At 5 V with buck losses that is roughly **0.5 A** — comfortable against
USB-C's usual 5 V/1.5 A or 3 A. The one marginal case is a host port
that only offers default USB current (500 mA); that is an argument for
treating the DC jack as primary and USB-C as convenience, not for
adding PD.

### Two gotchas worth designing in now

1. **USB-C needs two 5.1 kΩ pulldowns, one on each CC pin.** Without
   them a compliant C-to-C source detects no sink and supplies
   *nothing* — the port looks dead, and it is the single most common
   USB-C power-input mistake. They are two resistors; there is no
   excuse for getting this wrong on a fleet of fifteen boards.
2. **OR the two inputs.** With both connected, whichever is higher
   would otherwise backfeed the other. A Schottky pair is the cheap
   answer and the ~0.4 V drop is irrelevant ahead of a 5 V→3.3 V buck;
   a power-mux IC (TPS2116 class, ~$1) buys deterministic priority —
   prefer the DC jack, fall back to USB — if that is worth a dollar.

Add **reverse-polarity protection** on the DC input regardless: a
series P-FET is cents, and hand-made pigtails get miswired by humans.
A PTC and a TVS on both inputs are cheap insurance at fleet scale.

### Connector: XT30

**Decided: XT30** for the DC input. The de facto robotics/RC standard
at this scale — polarized and keyed so it cannot be mated backwards,
~$0.30, rated 30 A (absurd headroom for 0.5 A, but headroom is not a
cost), and the pigtails and 5 V supplies already exist in any robotics
drawer. Needs mating pigtails made up, which is the only real downside.

Considered and passed over: a 2-pin 5.08 mm pluggable terminal block
(Phoenix MC style), field-wireable with a screwdriver and no crimping;
and a 5.5×2.1 mm barrel jack, which is unpolarized by convention only
and the source of a thousand reverse-polarity stories. The
reverse-polarity FET stays in either case — XT30 pigtails are made by
humans.

### USB: power, console, and flashing

The connector is going in for power anyway, so it carries data too.
Two pins and a protection part buy three things:

- **A CDC-ACM console.** Zephyr's USB device stack (the
  `usb_device_next` stack on 4.4.1) exposes a serial console over the
  same cable that powers the board.
- **DFU flashing with no programmer.** The H723's ROM system
  bootloader speaks USB DFU, so `dfu-util` flashes a bare board.
  Bring a **BOOT0 button or test point** out to enter it; once Zephyr
  is up, a software jump can do the same without the button.
- **One cable per node on the bench.** On a fleet of fifteen that is
  the difference between a workbench and a nest.

Mechanics worth getting right the first time:

- **USB 2.0 Full Speed only** — the H723's OTG_HS has an internal FS
  PHY, so no external ULPI PHY. D+/D− land on PA11/PA12 (confirm
  against RMII and OCTOSPI assignments in CubeMX before layout).
- **48 MHz comes free.** HSI48 with the clock recovery system trimming
  against USB SOF meets FS accuracy without another crystal, so USB
  does not constrain the HSE choice below.
- **Bridge the two D+/D− pin pairs.** On a 16-pin receptacle, tie
  A6↔B6 and A7↔B7 so the cable works either way up. USB 2.0-only
  receptacles do it internally; the 16-pin ones do not, and a
  one-orientation USB port is a maddening bug to chase.
- **ESD protection on the data pair** — a USBLC6-2SC6 or equivalent,
  ~$0.15, non-negotiable on a connector humans touch.
- **Keep a UART console on a header.** A USB console needs the USB
  stack up, so it tells you nothing about a hardfault during early
  boot — exactly when a networking board is most likely to die. Three
  pins and a header preserve the always-works path the current fleet
  debugs with.

### Clocking: pick it with 0 Ω resistors

The switch's optional **REFCLKO (25 MHz)** output could drive the
MCU's HSE, leaving the whole board on a single crystal. Whether its
jitter suits a PLL source is not something the datasheet answers
cleanly, and it is not worth a respin to find out.

**So don't choose at design time — choose at build time.** Lay out
both sources into an 0 Ω select:

- **R_A** (fitted by default) connects the switch's REFCLKO to the
  MCU's HSE input;
- **R_B** (DNP) connects a **dedicated 25 MHz oscillator**, itself
  DNP, in its place.

PCB area is the cheapest thing on this board. A do-not-place line in
the BOM is a strictly better outcome than a respin, and if REFCLKO
turns out to be clean enough, the oscillator footprint simply stays
empty forever at a cost of a few square millimetres. If it doesn't,
populating two parts fixes it without touching copper. Same reasoning
applies anywhere else a cheap footprint buys out a risk.

Note that USB does not enter into this: HSI48 + CRS covers the 48 MHz
domain regardless of which HSE source wins.

### Rails

5 V in → **3.3 V buck** (MCU, PSRAM, switch analog and VDDIO) → **1.2 V
LDO** for the switch cores. Two regulators, both ordinary.

## Rest of system

| block | part | ~cost | note |
| --- | --- | ---: | --- |
| MCU | STM32H723ZGT6 | $11 | DigiKey, qty 10, 2026-08 |
| PSRAM | APS6408L-OBM (8 MB, octal) | $2–3 | 11 signals, easy routing, memory-mapped R/W via OCTOSPI |
| Switch | KSZ8794CNX | $6.22 | DigiKey, 2026-08; 3 copper + port 4 RMII; QFN-64; SPI-managed |
| Jacks | 3 × RJ45 w/ integrated magnetics | $6 | the real area cost; LEDs off the switch |
| Clock | 25 MHz ±50 ppm crystal | <$1 | switch sources the 50 MHz RMII refclk from it |
| USB | USB-C receptacle + USBLC6-2SC6 ESD | ~$0.30 | 5 V in, CDC-ACM console, DFU; 5.1 kΩ CC pulldowns |
| DC in | XT30 | ~$0.30 | polarized; reverse-polarity FET behind it |
| Power OR-ing | Schottky pair or TPS2116 mux | $0.20–1 | mux only if priority is worth a dollar |
| Power | 3.3 V buck + 1.2 V LDO for the switch core | $2–3 | two rails, not one |

BOM lands around **$29–31** plus a 4-layer PCBA run — up from the
~$20–25 single-jack estimate, essentially all of it jacks and switch,
less the WiFi module. Both major parts are now real quotes rather than
ballparks. No BGA anywhere, so still the cheap assembly tier, with QFN
as the only leadless part.

## Open questions

Most of the original list is now answered — from DS00002134C, or by
decision. What's left:

1. **Real power draw with three links up**, to replace the estimated
   ~1.6 W budget above and size the buck and LDO. The datasheet's
   power tables need reading properly rather than grepping.
2. **Storm-protection tuning.** The rate is a percentage of bandwidth;
   what percentage still lets ANNOUNCE broadcasts and ARP through
   comfortably at fleet scale is an empirical question, and the wrong
   answer breaks discovery in a way that will look like a wasp bug.
3. **Whether the switch's 25 MHz REFCLKO is clean enough for the
   MCU's HSE.** No longer gates layout — the 0 Ω select above defers
   it to bring-up — but it does decide which two parts get populated,
   so measure it on the Nucleo + switch breakout before the first
   build.

## Firmware work implied

Unchanged from the roadmap entry in the main README: instance handles
in `LOAD_MODULE`/`CALL`/`UNLOAD_MODULE`, one executor thread +
exec_env per running call, the node→coordinator RPC slot generalized
to a table keyed by instance — plus, for this board:

- the OCTOSPI PSRAM devicetree/linker plumbing, and pointing the WAMR
  pool at it;
- a fixed-link Ethernet devicetree node for the MCU↔switch RMII link
  (no PHY driver);
- the boot-time SPI config routine for the switch (storm protection,
  self-address filtering, port enables), and reading the per-port MIB
  counters out over the wasp protocol;
- USB CDC-ACM console on Zephyr's `usb_device_next` stack, keeping
  the UART console as the early-boot and hardfault path;
- optionally, the out-of-band port watchdog described above — the only
  place any RSTP-adjacent logic lives, and it is a polling loop over
  SPI, not a protocol.

Note what is *absent*: no DSA driver, no tail-tag shim, no spanning
tree, nothing of ours in the Ethernet hot path. That is the whole
point of the design.
