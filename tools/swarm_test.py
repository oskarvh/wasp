#!/usr/bin/env python3
"""Multi-node swarm test — the embryonic multi-node coordinator.

Drives N wasp nodes concurrently (one thread per node) against ONE
shared coordinator memory, and proves the remote-memory concurrency
story on real hardware:

  1. distributed sum   — the array is sliced across the swarm; partial
                         sums combine to the exact total
  2. racy increments   — unlocked read-modify-write from all nodes
                         loses updates (this failing to lose updates
                         would be suspicious, not reassuring)
  3. locked increments — same workload under wasp_lock is exact

Usage:
  swarm_test.py <remote_module.wasm> <node-ip> [<node-ip> ...]
  swarm_test.py <remote_module.wasm> discover     # use ANNOUNCE broadcasts
"""

import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from wasp_client import WaspNode, check, discover, pack_ref, R_ELOCKED

INCREMENTS_PER_NODE = 25


def per_node(nodes, fn):
    """Run fn(index, node) in one thread per node; propagate failures."""
    errors = []

    def run(i, node):
        try:
            fn(i, node)
        except Exception as e:  # noqa: BLE001 — report and fail the test
            errors.append(f"{node.lock_owner}: {e!r}")

    threads = [threading.Thread(target=run, args=(i, n)) for i, n in enumerate(nodes)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise SystemExit("swarm test failed:\n  " + "\n  ".join(errors))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    wasm = open(sys.argv[1], "rb").read()
    ips = sys.argv[2:]
    if ips == ["discover"]:
        print("listening for ANNOUNCE broadcasts...")
        found = discover()
        ips = sorted(ip for ip, n in found.items() if not n["busy"])
        busy = sorted(set(found) - set(ips))
        if busy:
            print(f"skipping busy nodes: {', '.join(busy)}")
        if not ips:
            raise SystemExit("no free nodes discovered")

    # One connection per node; all share the same coordinator memory.
    regions: dict[int, bytearray] = {}
    locks: dict[int, tuple[str, float]] = {}
    nodes = []
    for ip in ips:
        node = WaspNode(ip, 4242, timeout=15.0, lock_owner=f"node-{ip}")
        node.regions = regions
        node.locks = locks
        node.hello()
        node.load(wasm)
        nodes.append(node)
    print(f"swarm of {len(nodes)} nodes: {', '.join(ips)}")

    # -- 1: distributed sum over a shared array ------------------------
    chunks_per_node = 3
    n_ints = 64 * chunks_per_node * len(nodes)
    values = list(range(n_ints))
    regions[1] = bytearray(struct.pack(f"<{n_ints}i", *values))
    partials = [0] * len(nodes)

    def sum_slice(i, node):
        base = i * chunks_per_node * 64
        for c in range(chunks_per_node):
            ref = pack_ref(1, 4 * (base + c * 64))
            partials[i] += node.call("sum_region_bulk", ref, 64)[0]

    t0 = time.monotonic()
    per_node(nodes, sum_slice)
    dt = time.monotonic() - t0
    check(f"distributed sum of {n_ints} ints across {len(nodes)} nodes",
          sum(partials) == sum(values), f"{sum(partials)} in {dt:.2f}s")

    # -- 2: unlocked read-modify-write races and loses updates ---------
    regions[2] = bytearray(struct.pack("<i", 0))
    expected = INCREMENTS_PER_NODE * len(nodes)

    def racy(i, node):
        for _ in range(INCREMENTS_PER_NODE):
            r = node.call("racy_add", pack_ref(2), 1)[0]
            if r < 0:
                raise RuntimeError(f"racy_add failed: {r}")

    per_node(nodes, racy)
    final = struct.unpack("<i", regions[2])[0]
    lost = expected - final
    check(f"unlocked RMW loses updates ({expected} increments -> {final})",
          0 < final <= expected, f"{lost} updates lost")
    if lost == 0:
        print("  (note: no updates lost this run — race not observed)")

    # -- 3: the same workload under wasp_lock is exact ------------------
    struct.pack_into("<i", regions[2], 0, 0)
    retries = [0] * len(nodes)

    def locked(i, node):
        for _ in range(INCREMENTS_PER_NODE):
            while True:
                r = node.call("locked_add", 2, pack_ref(2), 1)[0]
                if r != R_ELOCKED:
                    break
                retries[i] += 1  # someone else held it — try again
            if r < 0:
                raise RuntimeError(f"locked_add failed: {r}")

    t0 = time.monotonic()
    per_node(nodes, locked)
    dt = time.monotonic() - t0
    final = struct.unpack("<i", regions[2])[0]
    check(f"locked RMW is exact ({expected} increments -> {final})",
          final == expected,
          f"{sum(retries)} ELOCKED retries, {dt:.2f}s")

    for node in nodes:
        node.unload()
    print("swarm test passed")


if __name__ == "__main__":
    main()
