# wasp node board

KiCad 9 project for the purpose-built wasp node: STM32H723ZGT6 + octal-bus
PSRAM + a KSZ8794CNX switch giving every node three copper ports, so a fleet
cables itself into a tree instead of fanning out to a desk switch.

The design rationale — part choices, what was rejected and why, the power and
clocking decisions, and the gotchas that cost real time — lives in
[`docs/hardware.md`](../docs/hardware.md). The block diagram is
[`docs/diagrams/node-board.svg`](../docs/diagrams/node-board.svg).

Open `wasp_node_board/wasp_node_board.kicad_pro`. Sheets: `mcu`, `power`,
`eth`.

## External libraries are not committed

`hardware/lib/` is gitignored. The parts in it come from third-party symbol
services whose terms on redistributing the library files are unclear, so the
repo records **where to re-fetch them** instead of vendoring them.

This costs less than it sounds: KiCad caches symbol definitions inline in each
`.kicad_sch`, so the schematic opens, renders and netlists on a fresh clone
with no libraries present. It is **footprints** that go missing, which matters
only once PCB layout starts.

Layout convention, matching the project library tables:

```
hardware/lib/<PART>/<PART>.kicad_sym     # symbol
hardware/lib/<PART>/<PART>.kicad_mod     # footprint (or <PART>.pretty/)
```

The project's `sym-lib-table` and `fp-lib-table` reference these through
`${KIPRJMOD}/../lib/...`, so they resolve wherever the repo is checked out.

### What to re-fetch

| ref | part | source |
| --- | --- | --- |
| U1 (eth) | Microchip `KSZ8794CNXCC` | [SnapEDA](https://www.snapeda.com/parts/KSZ8794CNXCC/Microchip/view-part) |
| FL1 | TDK `ACM2012-361-2P-T002` | [Ultra Librarian](https://app.ultralibrarian.com/details/55ef4603-1073-11e9-ab3a-0a3560a4cccc/TDK/ACM2012-361-2P-T002) |
| J3 | GCT `USB4110-GF-A` | [Ultra Librarian](https://app.ultralibrarian.com/details/3db0fb39-9b49-11ea-b5d0-0aebb021a1ea/Global-Connector-Technology/USB4110-GF-A) |
| U4 | TI `TPSM863253RDXR` | [Ultra Librarian](https://app.ultralibrarian.com/details/232cd2f6-c430-11ee-8cb3-0ae0a3b49db5/Texas-Instruments/TPSM863253RDXR) |

Install each under `hardware/lib/<PART>/` and register it in the project
library tables. Everything else on the board comes from stock KiCad libraries
and needs no fetching.

## Known gaps before layout

**J3's symbol and footprint do not match.** The schematic still uses the
cached 20-pin SamacSys symbol (`A1 A4 … B12`, `MP1`–`MP4`), while the
registered Ultra Librarian footprint uses **merged** pads — `A1-B12`,
`A4-B9`, `A9-B4`, `A12-B1`, plus `13`–`18` for the shell. Every VBUS, ground
and shield pin therefore has no pad to land on, and layout will fail on
twelve of them.

The fix is to drop the external library for J3 entirely and use stock KiCad,
which carries this exact receptacle:

| | |
| --- | --- |
| symbol | `Connector:USB_C_Receptacle_USB2.0_16P` |
| footprint | `Connector_USB:USB_C_Receptacle_GCT_USB4110` |

Both use `A1 A4 A5 A6 A7 A8 A9 A12 B1 B4 B5 B6 B7 B8 B9 B12 S1`, so they pair
exactly — and that is the same separate-pin naming the schematic is already
wired against, so only the shell connection changes (`MP1`–`MP4` → `S1`).
Using Ultra Librarian's own 12-pin symbol with its merged footprint would also
be coherent, but means re-wiring VBUS and ground onto merged pins for no gain.

While J3 is open: `DP1`/`DP2` (A6/B6) and `DN1`/`DN2` (A7/B7) stay separate
pins on the stock symbol, so tying them pairwise for cable-flip independence
is still an explicit job in the schematic.

**FL1 and U4 are cosmetic only.** Both cached symbols were diffed against the
installed libraries pin-by-pin and are identical — 4 pins for the choke, 7 for
the buck module — so the TPSM863**252**-vs-**253** and ACM2012-**201**-vs-**361**
mismatches carry no electrical risk. Values on the schematic are already
correct, so a BOM export is right either way. Registering the two `.kicad_sym`
files and re-linking is worth doing for traceability, not urgency.

The KSZ8794's footprint may not need the external library at all: stock KiCad
ships `Package_DFN_QFN:QFN-64-1EP_8x8mm_P0.4mm_EP6.5x6.5mm` (and a
`_ThermalVias` variant, which a QFN exposed pad wants). If §11 of the datasheet
confirms a 6.5 × 6.5 mm exposed pad, switching to the stock footprint leaves
only the symbol as third-party — and the PCB is still empty, so this is the
cheapest moment to make that swap.
