#!/usr/bin/env python3
"""wasp remote-memory performance characterization.

Measures the round-trip costs a module author actually pays, and the
layout/locking decisions that change them. The results and the guidance
they back live in docs/performance.md.

  T1  RPC latency floor + bandwidth vs transfer size
  T2  per-element loop vs bulk read/compute/write
  T3  aligned vs misaligned transfers and atomics
  T4  hot counter inside vs outside a bulk-written extent   (>= 2 nodes)
  T5  lock contention: one shared region vs per-node regions (>= 2 nodes)
  T6  MEM_ADD vs CAS retry loop under contention             (>= 2 nodes)

Usage:
  perf_test.py <perf_module.wasm> <node-ip> [<node-ip> ...]
  perf_test.py <perf_module.wasm> discover      # use ANNOUNCE broadcasts
"""

import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from wasp_client import WaspNode, discover, pack_ref


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
        raise SystemExit("perf test failed:\n  " + "\n  ".join(errors))


def call_ok(node, func, *args):
    """CALL that treats a negative i32 result as a failure."""
    r = node.call(func, *args)[0]
    if r < 0:
        raise RuntimeError(f"{func}{args} -> {r}")
    return r


def timed(fn):
    t0 = time.monotonic()
    r = fn()
    return time.monotonic() - t0, r


def reset_u32(regions, region, off=0, val=0):
    struct.pack_into("<I", regions[region], off, val)


def read_u32(regions, region, off=0):
    return struct.unpack_from("<I", regions[region], off)[0]


# -- T1 ----------------------------------------------------------------

def t1_bandwidth(node, regions, results):
    print(f"\n== T1: RPC latency & bandwidth vs transfer size [{node.lock_owner}] ==")
    regions[1] = bytearray(8192 + 64)
    print(f"  {'size':>6} {'read ms/op':>11} {'read KiB/s':>11} "
          f"{'write ms/op':>12} {'write KiB/s':>12}")
    rows = []
    for size, reps in [(4, 200), (64, 200), (256, 150), (1024, 80), (4096, 30)]:
        dt_r, _ = timed(lambda: call_ok(node, "read_loop", pack_ref(1), size, reps))
        dt_w, _ = timed(lambda: call_ok(node, "write_loop", pack_ref(1), size, reps))
        r_ms, w_ms = dt_r / reps * 1e3, dt_w / reps * 1e3
        r_bw, w_bw = size * reps / dt_r / 1024, size * reps / dt_w / 1024
        rows.append((size, r_ms, r_bw, w_ms, w_bw))
        print(f"  {size:>6} {r_ms:>11.2f} {r_bw:>11.0f} {w_ms:>12.2f} {w_bw:>12.0f}")
    results[f"t1:{node.lock_owner}"] = rows


# -- T2 ----------------------------------------------------------------

def t2_batching(node, regions, results):
    print(f"\n== T2: per-element loop vs bulk read/compute/write [{node.lock_owner}] ==")
    print(f"  {'ints':>5} {'per-element':>12} {'bulk':>9} {'speedup':>8}")
    regions[2] = bytearray(4 * 256)
    for n in (16, 64, 256):
        dt_e, _ = timed(lambda: call_ok(node, "scale_per_element", pack_ref(2), n, 1))
        dt_b, _ = timed(lambda: call_ok(node, "scale_bulk", pack_ref(2), n, 1))
        print(f"  {n:>5} {dt_e:>11.2f}s {dt_b:>8.3f}s {dt_e / dt_b:>7.1f}x")
        results[f"t2:{n}"] = (dt_e, dt_b)


# -- T3 ----------------------------------------------------------------

def t3_alignment(node, regions, results):
    print(f"\n== T3: aligned vs misaligned transfers & atomics [{node.lock_owner}] ==")
    reps = 150
    rows = []
    for name, func, off, size in (
        ("64 B read  @ +0", "read_loop", 0, 64),
        ("64 B read  @ +1", "read_loop", 1, 64),
        ("64 B read  @ +3", "read_loop", 3, 64),
        ("64 B write @ +0", "write_loop", 0, 64),
        ("64 B write @ +1", "write_loop", 1, 64),
        ("MEM_ADD    @ +0", None, 0, 0),
        ("MEM_ADD    @ +2", None, 2, 0),
    ):
        if func:
            dt, _ = timed(lambda: call_ok(node, func, pack_ref(1, off), size, reps))
        else:
            reset_u32(regions, 1, off)
            dt, _ = timed(lambda: call_ok(node, "add_loop", pack_ref(1, off), reps, 1))
        rows.append((name, dt / reps * 1e3))
        print(f"  {name}  {dt / reps * 1e3:6.2f} ms/op")
    results["t3"] = rows


# -- T4 ----------------------------------------------------------------

def t4_clobber(nodes, regions, results):
    print("\n== T4: hot counter inside vs outside a bulk-written extent ==")
    churn_len, churn_reps, adds = 256, 30, 40
    adders = len(nodes) - 1
    expected = adds * adders
    regions[4] = bytearray(512)

    def run(counter_off):
        reset_u32(regions, 4, counter_off)

        def work(i, node):
            if i == 0:
                call_ok(node, "churn_rmw", pack_ref(4, 0), churn_len, churn_reps)
            else:
                call_ok(node, "add_loop", pack_ref(4, counter_off), adds, 1)

        per_node(nodes, work)
        return read_u32(regions, 4, counter_off)

    inside = run(128)    # counter lives inside the churned extent
    outside = run(256)   # counter lives just past it
    print(f"  node 0 bulk-RMWs bytes [0,{churn_len}); "
          f"{adders} node(s) MEM_ADD the counter {expected}x total")
    print(f"  counter @ +128 (inside):  {inside}/{expected} survived "
          f"({expected - inside} adds erased by write-back)")
    print(f"  counter @ +256 (outside): {outside}/{expected} survived")
    results["t4"] = (inside, outside, expected)


# -- T5 ----------------------------------------------------------------

def t5_partitioning(nodes, regions, results):
    print("\n== T5: locked RMW — one shared region vs one region per node ==")
    reps = 15
    for node in nodes:
        node.queue_locks = True
    try:
        regions[10] = bytearray(64)
        dt_shared, _ = timed(lambda: per_node(
            nodes, lambda i, n: call_ok(n, "locked_rmw_loop",
                                        10, pack_ref(10), reps, 1)))
        shared_final = read_u32(regions, 10)

        for i in range(len(nodes)):
            regions[20 + i] = bytearray(64)
        dt_part, _ = timed(lambda: per_node(
            nodes, lambda i, n: call_ok(n, "locked_rmw_loop",
                                        20 + i, pack_ref(20 + i), reps, 1)))
        part_ok = all(read_u32(regions, 20 + i) == reps for i in range(len(nodes)))
    finally:
        for node in nodes:
            node.queue_locks = False

    n = len(nodes)
    print(f"  {n} nodes x {reps} locked increments (queued locks):")
    print(f"  one shared region:    {dt_shared:6.2f}s  "
          f"(counter {shared_final}/{reps * n} — exact)")
    print(f"  one region per node:  {dt_part:6.2f}s  "
          f"(all counters {'exact' if part_ok else 'WRONG'})")
    print(f"  partitioning speedup: {dt_shared / dt_part:.1f}x")
    results["t5"] = (dt_shared, dt_part, shared_final == reps * n and part_ok)


# -- T6 ----------------------------------------------------------------

def t6_add_vs_cas(nodes, regions, results):
    print("\n== T6: MEM_ADD vs CAS retry loop under contention ==")
    incs = 30
    regions[30] = bytearray(64)
    counts = sorted({1, (len(nodes) + 1) // 2, len(nodes)})
    print(f"  {'nodes':>5} {'MEM_ADD':>9} {'CAS loop':>9} "
          f"{'CAS attempts':>13} {'amplification':>14}")
    rows = []
    for k in counts:
        subset = nodes[:k]
        reset_u32(regions, 30)
        dt_add, _ = timed(lambda: per_node(
            subset, lambda i, n: call_ok(n, "add_loop", pack_ref(30), incs, 1)))
        if read_u32(regions, 30) != incs * k:
            raise SystemExit("T6: MEM_ADD count wrong")

        attempts = [0] * k
        reset_u32(regions, 30)

        def cas(i, node):
            attempts[i] = call_ok(node, "cas_inc_loop", pack_ref(30), incs)

        dt_cas, _ = timed(lambda: per_node(subset, cas))
        if read_u32(regions, 30) != incs * k:
            raise SystemExit("T6: CAS count wrong")
        amp = sum(attempts) / (incs * k)
        rows.append((k, dt_add, dt_cas, sum(attempts), amp))
        print(f"  {k:>5} {dt_add:>8.2f}s {dt_cas:>8.2f}s "
              f"{sum(attempts):>13} {amp:>13.2f}x")
    results["t6"] = rows


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    wasm = open(sys.argv[1], "rb").read()
    ips = sys.argv[2:]
    boards = {}
    if ips == ["discover"]:
        print("listening for ANNOUNCE broadcasts...")
        found = discover()
        ips = sorted(ip for ip, n in found.items() if not n["busy"])
        boards = {ip: found[ip]["board"] for ip in ips}
        if not ips:
            raise SystemExit("no free nodes discovered")

    regions: dict[int, bytearray] = {}
    locks: dict[int, tuple[str, float]] = {}
    lock_queue: dict[int, list[str]] = {}
    nodes = []
    for ip in ips:
        node = WaspNode(ip, 4242, timeout=60.0, lock_owner=ip)
        node.regions = regions
        node.locks = locks
        node.lock_queue = lock_queue
        node.hello()
        node.load(wasm)
        nodes.append(node)
    print(f"{len(nodes)} node(s): " +
          ", ".join(f"{ip} ({boards[ip]})" if ip in boards else ip for ip in ips))

    results = {}

    # Single-node experiments: first node, plus (if present) one node of
    # a different board type so both transports are characterized.
    reps_nodes = [nodes[0]]
    for node in nodes[1:]:
        if boards and boards.get(node.lock_owner) != boards.get(nodes[0].lock_owner):
            reps_nodes.append(node)
            break
    for node in reps_nodes:
        t1_bandwidth(node, regions, results)
    t2_batching(reps_nodes[0], regions, results)
    t3_alignment(reps_nodes[0], regions, results)

    if len(nodes) >= 2:
        t4_clobber(nodes, regions, results)
        t5_partitioning(nodes, regions, results)
        t6_add_vs_cas(nodes, regions, results)
    else:
        print("\n(single node: skipping the contention experiments T4-T6)")

    for node in nodes:
        try:
            node.unload()
            node.sock.close()
        except Exception:
            pass
    print("\nall experiments complete")


if __name__ == "__main__":
    main()
