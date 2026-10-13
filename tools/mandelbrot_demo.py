#!/usr/bin/env python3
"""wasp distributed Mandelbrot demo — the swarm renders an image.

The coordinator registers a framebuffer and a work counter as remote
regions and calls `render` once on every node. Each node then claims
tiles off the shared counter (one MEM_ADD each), renders them locally
in Q4.28 fixed point, and ships every finished tile with one bulk
MEM_WRITE. No scheduler: fast nodes claim more tiles, and the live
view + provenance image show exactly who rendered what. If a node dies
mid-render, its lost tiles are detected and re-issued to the survivors.
The coordinator side runs on libwasp (the C coordinator library, via
tools/libwasp.py); framebuffer writes are observed as libwasp events.

Usage:
  mandelbrot_demo.py discover [options]
  mandelbrot_demo.py <ip> [<ip> ...] [options]

Options:
  --size WxH        image size            (default 256x192)
  --iter N          max iterations, <=254 (default 96)
  --tile-rows N     rows per work unit    (default 2)
  --center X Y      complex center        (default -0.5 0.0)
  --span S          real-axis width       (default 3.2)
  --out NAME        output basename       (default mandelbrot)
  --module PATH     wasm module           (default tools/mandelbrot_module.wasm)
  --no-live         plain progress lines instead of the ANSI live view
  --gif             also record NAME.gif of the image appearing tile by tile
  --chaos           kill one node's connection mid-render (resilience demo)

Outputs NAME.png (the fractal) and NAME_nodes.png (each tile colored by
the node that rendered it); with --gif, an animated NAME.gif of the
render as it happened (real-time, looping).
"""

import argparse
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from libwasp import (WaspNode, discover, pack_ref, default_ctx,
                     EV_MEM_WRITE, FEAT_MULTI_INSTANCE)
from wasp_client import print_stats

PARAMS_REGION, CTR_REGION, FB_REGION = 1, 2, 3
Q = 1 << 28  # Q4.28

NODE_COLORS = [
    (231, 76, 60), (52, 152, 219), (46, 204, 113), (241, 196, 15),
    (155, 89, 182), (26, 188, 156), (230, 126, 34), (149, 165, 166),
]


class Demo:
    def __init__(self, args, ips, boards):
        self.w, self.h = args.size
        self.tile_rows = args.tile_rows
        self.max_iter = args.iter
        self.tiles = (self.h + self.tile_rows - 1) // self.tile_rows
        self.tile_owner = [None] * self.tiles
        self.t0 = None
        self.dead = {}
        self.ips = ips
        self.boards = boards

        step = args.span / self.w
        x0 = args.center[0] - step * self.w / 2
        y0 = args.center[1] - step * self.h / 2
        for v in (x0, y0, step):
            if not -8.0 < v < 8.0:
                raise SystemExit("view out of Q4.28 range")
        self.params = bytearray(struct.pack(
            "<7i", self.w, self.h, self.tile_rows, self.max_iter,
            round(x0 * Q), round(y0 * Q), round(step * Q)))
        self.fb = bytearray(b"\xff" * (self.w * self.h))
        self.regions = {PARAMS_REGION: self.params,
                        CTR_REGION: bytearray(8),
                        FB_REGION: self.fb}

    # Called from the event pump / drain only (single writer).
    def record_write(self, node_index, offset):
        tile = offset // (self.tile_rows * self.w)
        if 0 <= tile < self.tiles:
            self.tile_owner[tile] = node_index

    def done(self):
        return sum(o is not None for o in self.tile_owner)

    def counts(self):
        c = [0] * len(self.ips)
        for o in self.tile_owner:
            if o is not None:
                c[o] += 1
        return c


# -- colors -------------------------------------------------------------

def shade(v, max_iter):
    """Iteration byte -> RGB. 255 = unrendered, max_iter = interior."""
    if v == 255:
        return (38, 38, 46)
    if v >= max_iter:
        return (0, 0, 0)
    t = v / max_iter
    stops = [(0, (9, 12, 60)), (0.25, (28, 90, 180)), (0.5, (90, 200, 220)),
             (0.75, (250, 220, 90)), (1.0, (255, 250, 235))]
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0)
            return tuple(int(a + (b - a) * f) for a, b in zip(c0, c1))
    return stops[-1][1]


def node_shade(owner, v, max_iter):
    """Provenance pixel: the owning node's color, dimmed inside the set."""
    if owner is None:
        return (30, 30, 30)
    r, g, b = NODE_COLORS[owner % len(NODE_COLORS)]
    f = 0.35 if v >= max_iter else 1.0
    return (int(r * f), int(g * f), int(b * f))


# -- live terminal view --------------------------------------------------

def draw(demo, final=False):
    import shutil
    cols, lines = shutil.get_terminal_size()
    avail_rows = max(8, lines - len(demo.ips) - 4) * 2
    sx = max(1, -(-demo.w // max(cols, 20)), -(-demo.h // avail_rows))
    pw, ph = demo.w // sx, (demo.h // sx) & ~1
    out = ["\x1b[H"]
    for py in range(0, ph, 2):
        row = []
        for px in range(pw):
            top = shade(demo.fb[(py * sx) * demo.w + px * sx], demo.max_iter)
            bot = shade(demo.fb[((py + 1) * sx) * demo.w + px * sx], demo.max_iter)
            row.append(f"\x1b[38;2;{top[0]};{top[1]};{top[2]}m"
                       f"\x1b[48;2;{bot[0]};{bot[1]};{bot[2]}m▀")
        out.append("".join(row) + "\x1b[0m\x1b[K")
    dt = time.monotonic() - demo.t0
    out.append(f"\x1b[0m\x1b[K")
    out.append(f" {demo.done()}/{demo.tiles} tiles   {dt:5.1f}s\x1b[K")
    for i, (ip, n) in enumerate(zip(demo.ips, demo.counts())):
        r, g, b = NODE_COLORS[i % len(NODE_COLORS)]
        state = " DEAD" if i in demo.dead else ""
        out.append(f" \x1b[38;2;{r};{g};{b}m█\x1b[0m {ip:15s} "
                   f"{demo.boards.get(ip, ''):24s} {n:4d} tiles{state}\x1b[K")
    print("\n".join(out), flush=True)


def live_view(demo, workers, rec=None):
    """Live terminal view. Redraws at REDRAW but polls at POLL, so the
    loop notices the workers finishing promptly: the redraw rate is a
    display choice and should not become the resolution of the reported
    render time (see plain_view).
    """
    REDRAW = 0.25
    POLL = 0.02
    print("\x1b[2J\x1b[?25l", end="")
    try:
        last_draw = 0.0
        while any(t.is_alive() for t in workers):
            if rec:
                rec.snap(demo.fb)
            if time.monotonic() - last_draw >= REDRAW:
                last_draw = time.monotonic()
                draw(demo)
            time.sleep(0.25 if rec else POLL)
        draw(demo)
    finally:
        print("\x1b[?25h", end="", flush=True)


def plain_view(demo, workers, rec=None):
    """Watch progress without the live view.

    The poll interval and the print interval are deliberately separate.
    They used to be one `time.sleep(2.0)`, which quantised the reported
    render time to two seconds: a 0.4 s render and a 1.9 s one both came
    out as "2.1s", and anything faster than the sleep was invisible. Any
    conclusion about where the time goes needs a finer ruler than the
    thing being measured.
    """
    POLL = 0.02
    PRINT_EVERY = 2.0
    last_print = 0.0
    while any(t.is_alive() for t in workers):
        if rec:
            rec.snap(demo.fb)
        time.sleep(0.25 if rec else POLL)
        if time.monotonic() - last_print >= PRINT_EVERY:
            last_print = time.monotonic()
            print(f"  {demo.done()}/{demo.tiles} tiles, per node {demo.counts()}",
                  flush=True)


# -- output -------------------------------------------------------------

def _gif_lzw(data, min_code=8):
    """GIF-flavor LZW: variable code width, LSB-first bit packing."""
    clear, end = 1 << min_code, (1 << min_code) + 1
    out = bytearray()
    acc = nbits = 0

    def put(code, size):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8

    table = {bytes([i]): i for i in range(clear)}
    next_code, code_size = end + 1, min_code + 1
    put(clear, code_size)
    w = data[:1]
    for i in range(1, len(data)):
        b = data[i:i + 1]
        wb = w + b
        if wb in table:
            w = wb
            continue
        put(table[w], code_size)
        table[wb] = next_code
        next_code += 1
        # The decoder's table lags one entry: it widens its code size
        # exactly when we have just assigned index 1 << code_size.
        if next_code == (1 << code_size) + 1 and code_size < 12:
            code_size += 1
        if next_code == 4096:
            put(clear, code_size)
            table = {bytes([j]): j for j in range(clear)}
            next_code, code_size = end + 1, min_code + 1
        w = b
    put(table[w], code_size)
    put(end, code_size)
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


class GifRecorder:
    """Records framebuffer snapshots and writes an animated GIF. The
    framebuffer's iteration bytes are used directly as palette indices
    (palette[i] = shade(i)), so a frame costs one copy, no conversion."""

    def __init__(self, path, w, h, max_iter, interval=0.25):
        self.path, self.w, self.h, self.max_iter = path, w, h, max_iter
        self.interval = interval
        self.frames = []  # (monotonic time, fb copy)
        self._last = 0.0

    def snap(self, fb, force=False):
        now = time.monotonic()
        if not force and now - self._last < self.interval:
            return
        f = bytes(fb)
        if self.frames and self.frames[-1][1] == f and not force:
            return
        self.frames.append((now, f))
        self._last = now

    def save(self):
        frames = self.frames
        step = max(1, (len(frames) + 239) // 240)  # cap ~240 frames
        frames = frames[::step]
        palette = b"".join(bytes(shade(i, self.max_iter)) for i in range(256))
        out = [b"GIF89a",
               struct.pack("<HHBBB", self.w, self.h, 0xF7, 0, 0), palette,
               b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"]  # loop forever
        for i, (t, fb) in enumerate(frames):
            delay_cs = (int((frames[i + 1][0] - t) * 100)
                        if i + 1 < len(frames) else 300)  # hold the last frame
            out.append(b"\x21\xF9\x04\x04" +
                       struct.pack("<H", max(2, delay_cs)) + b"\x00\x00")
            out.append(b"\x2C" + struct.pack("<HHHH", 0, 0, self.w, self.h) +
                       b"\x00\x08")
            lzw = _gif_lzw(fb)
            out.extend(bytes([len(lzw[o:o + 255])]) + lzw[o:o + 255]
                       for o in range(0, len(lzw), 255))
            out.append(b"\x00")
        out.append(b"\x3B")
        Path(self.path).write_bytes(b"".join(out))
        return len(frames)


def write_png(path, w, h, pixel):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(
        b"\x00" + bytes(c for x in range(w) for c in pixel(x, y))
        for y in range(h))
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# -- main ---------------------------------------------------------------

def parse_args():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument("nodes", nargs="+")
    ap.add_argument("--size", default="256x192")
    ap.add_argument("--iter", type=int, default=96)
    ap.add_argument("--tile-rows", type=int, default=2)
    ap.add_argument("--center", nargs=2, type=float, default=[-0.5, 0.0])
    ap.add_argument("--span", type=float, default=3.2)
    ap.add_argument("--out", default="mandelbrot")
    ap.add_argument("--module", default=str(Path(__file__).parent / "mandelbrot_module.wasm"))
    ap.add_argument("--instances", type=int, default=0,
                   help="worker instances per node (0 = every free slot; "
                        "1 = one, as before)")
    ap.add_argument("--stats", action="store_true",
                   help="per-instance compute vs network breakdown from "
                        "each node, read before the modules are released")
    ap.add_argument("--no-live", action="store_true")
    ap.add_argument("--gif", action="store_true")
    ap.add_argument("--chaos", action="store_true")
    args = ap.parse_args()
    args.size = tuple(int(v) for v in args.size.lower().split("x"))
    if args.iter > 254:
        raise SystemExit("--iter must be <= 254 (255 is the unrendered sentinel)")
    if args.size[0] * args.tile_rows > 8192:
        raise SystemExit("width * tile-rows must fit the module's 8 KiB buffer")
    return args


def main():
    args = parse_args()
    ips, boards = args.nodes, {}
    if ips == ["discover"]:
        print("listening for ANNOUNCE broadcasts...")
        found = discover()
        ips = sorted(ip for ip, n in found.items() if not n["busy"])
        boards = {ip: found[ip]["board"] for ip in ips}
        if not ips:
            raise SystemExit("no free nodes discovered")

    demo = Demo(args, ips, boards)
    wasm = open(args.module, "rb").read()
    ctx = default_ctx()
    for rid, buf in demo.regions.items():
        ctx.register_region(rid, buf)

    # Framebuffer MEM_WRITEs surface as libwasp events; turning them
    # into completion + provenance records replaces the _node_response
    # override the pure-Python client needed.
    stop_pump = threading.Event()

    def on_event(ev):
        if ev.type == EV_MEM_WRITE and ev.region == FB_REGION:
            demo.record_write(ev.tag, ev.offset)

    def pump():
        while not stop_pump.is_set():
            ev = ctx.poll_event(0.2)
            if ev:
                on_event(ev)

    def drain():
        """Consume already-queued events (all writes are acked before a
        call returns, so post-join the queue is complete)."""
        while (ev := ctx.poll_event(0.05)) is not None:
            on_event(ev)

    # daemon=True so an exception anywhere below still lets the process
    # exit: this thread outlives main() otherwise and the only symptom is
    # a hang with a traceback already printed, which reads like the demo
    # froze rather than failed.
    pump_thread = threading.Thread(target=pump, daemon=True)
    pump_thread.start()

    nodes = []
    for i, ip in enumerate(ips):
        node = WaspNode(ip, 4242, timeout=120.0, lock_owner=ip, tag=i)
        node.index = i
        _, _, features = node.hello()

        # Several instances per node, each an independent worker stealing
        # from the same tile counter. The work-stealing design needs no
        # changes for this — MEM_ADD is atomic at the coordinator, so the
        # number of claimants was never baked in. What it buys is latency
        # hiding: while one instance waits on a round trip, the others
        # compute, and round trips are a real fraction of the run.
        node.insts = None
        #
        # --chaos deliberately kills a node's connection mid-render. With
        # one instance per node that is a clean test of work re-issue;
        # with several it also leaves the victim's other instances looping
        # on a dead link until their RPCs time out, and recovery from that
        # is not yet worked out — the run stalls rather than re-issuing.
        # Until it is, chaos runs one instance per node, which is the
        # configuration the re-issue path was built against.
        #
        if args.chaos and (features & FEAT_MULTI_INSTANCE) and args.instances != 1:
            if i == 0:
                print("  --chaos: using 1 instance per node "
                      "(multi-instance node loss is not handled yet)")
        elif features & FEAT_MULTI_INSTANCE:
            # Clear anything a previous run left resident. A client that
            # died mid-render leaves its instances loaded — the node has
            # no idea the coordinator is gone — and the slots stay taken,
            # so the next run gets NO_SLOTS and no amount of retrying
            # helps. The v1 path has always handled this by unloading on
            # BUSY; the instance path needs the same courtesy.
            inv = node.inventory()
            for m in inv["modules"]:
                for attempt in range(5):
                    try:
                        node.drop_module(m["mod"])
                        break
                    except ValueError as e:
                        # An instance still finishing its last call.
                        if "BUSY" not in str(e) or attempt == 4:
                            raise
                        time.sleep(0.2)
            if inv["modules"]:
                inv = node.inventory()
                print(f"  {ip}: released {len(inv['modules'])==0 and 'all' or 'some'} "
                      f"stale module(s) from an earlier run")

            want = args.instances or inv["slots_free"]
            want = max(1, min(want, inv["slots_free"] or 1))
            try:
                node.mod, node.insts = node.upload(wasm, want)
            except ValueError as e:
                if "NO_SLOTS" not in str(e) and "NO_MEM" not in str(e):
                    raise
                # Asked for more than the pool can back; take one.
                node.mod, node.insts = node.upload(wasm, 1)

        if node.insts is None:
            # Node predates the instance API: the v1 single-module path.
            try:
                node.load(wasm)
            except ValueError as e:
                if "BUSY" not in str(e):
                    raise
                node.unload()  # stale module from an earlier (crashed) run
                node.load(wasm)
        nodes.append(node)

    workers_per_node = [len(n.insts) if n.insts else 1 for n in nodes]
    print(f"rendering {demo.w}x{demo.h}, {demo.tiles} tiles, "
          f"{len(nodes)} node(s), {sum(workers_per_node)} worker(s) "
          f"{workers_per_node}")

    if args.stats:
        for node in nodes:
            try:
                node.stats(reset=True)   # bracket this run
            except Exception:            # noqa: BLE001 — older firmware
                args.stats = False

    demo.t0 = time.monotonic()
    rec = (GifRecorder(f"{args.out}.gif", demo.w, demo.h, demo.max_iter)
           if args.gif else None)
    if rec:
        rec.snap(demo.fb, force=True)  # the all-unrendered first frame

    def worker(node):
        """Run every instance of one node, over its single connection.

        One thread per node, not per instance: the connection carries all
        of them, so the calls go out together and are collected as they
        finish. call_wait_any blocks on the socket, so the node's
        remote-memory RPCs are served the moment they arrive — polling
        with a sleep instead would cost about what a round trip costs and
        give back the latency this is meant to recover.
        """
        refs = (pack_ref(PARAMS_REGION), pack_ref(CTR_REGION),
                pack_ref(FB_REGION))
        try:
            if not node.insts:
                r = node.call("render", *refs)[0]
                if r < 0:
                    raise RuntimeError(f"render -> {r}")
                return

            for inst in node.insts:
                node.call_inst_async(inst, "render", *refs)
            while node.inflight():
                got = node.call_wait_any()
                if got is None:
                    raise RuntimeError("render timed out")
                inst, res = got
                if res and res[0] < 0:
                    raise RuntimeError(f"render on instance {inst} -> {res[0]}")
        except Exception as e:  # noqa: BLE001 — node loss is expected under --chaos
            demo.dead[node.index] = repr(e)

    workers = [threading.Thread(target=worker, args=(n,)) for n in nodes]
    for t in workers:
        t.start()

    if args.chaos:
        def chaos():
            while demo.done() < demo.tiles // 4:
                time.sleep(0.1)
            victim = nodes[-1]
            print(f"\x1b[0m--chaos: killing {victim.lock_owner}'s connection")
            victim.sock.close()
        threading.Thread(target=chaos, daemon=True).start()

    (plain_view if args.no_live else live_view)(demo, workers, rec)
    for t in workers:
        t.join()
    drain()

    # Re-issue tiles lost with dead nodes (claimed but never written).
    missing = [t for t, o in enumerate(demo.tile_owner) if o is None]
    alive = [n for n in nodes if n.index not in demo.dead]
    if missing:
        if not alive:
            raise SystemExit("all nodes died; image incomplete")
        print(f"re-issuing {len(missing)} lost tile(s) to "
              f"{len(alive)} surviving node(s)")
        for j, tile in enumerate(missing):
            node = alive[j % len(alive)]
            # call() addresses the v1 default instance, which a run using
            # the instance API does not have — it would answer NO_MODULE.
            args_ = (pack_ref(PARAMS_REGION), pack_ref(FB_REGION), tile)
            r = (node.call_inst(node.insts[0], "render_tile", *args_)
                 if node.insts else node.call("render_tile", *args_))[0]
            if r != 1:
                raise SystemExit(f"re-issue of tile {tile} failed: {r}")
            if rec:
                rec.snap(demo.fb)

    drain()
    stop_pump.set()
    pump_thread.join()

    dt = time.monotonic() - demo.t0
    if args.stats:
        # Before teardown, not after: dropping a module destroys its
        # instances, and the counters live on the instance.
        print()
        for node in alive:
            try:
                rows = node.stats()
            except Exception as e:  # noqa: BLE001
                print(f"  {node.lock_owner}: stats unavailable ({e})")
                continue
            print(f"  {node.lock_owner}")
            print_stats(rows, "    ")

    for node in alive:
        try:
            # Release the right thing: unload() addresses the single v1
            # instance and fails with NO_MODULE when the run used the
            # instance API, and since this is wrapped in a bare except
            # that failure is silent — which is how a run used to leave
            # all three slots occupied for whatever came next.
            if node.insts:
                node.drop_module(node.mod)
            else:
                node.unload()
            node.sock.close()
        except Exception:
            pass

    write_png(f"{args.out}.png", demo.w, demo.h,
              lambda x, y: shade(demo.fb[y * demo.w + x], demo.max_iter))
    write_png(f"{args.out}_nodes.png", demo.w, demo.h,
              lambda x, y: node_shade(
                  demo.tile_owner[y // demo.tile_rows],
                  demo.fb[y * demo.w + x], demo.max_iter))

    if rec:
        rec.snap(demo.fb, force=True)
        n = rec.save()
        print(f"wrote {rec.path} ({n} frames)")

    print(f"\n{demo.w}x{demo.h}, {demo.max_iter} iterations, "
          f"{demo.tiles} tiles in {dt:.1f}s "
          f"({demo.w * demo.h / dt / 1000:.0f} kpix/s)")
    for i, (ip, n) in enumerate(zip(ips, demo.counts())):
        state = "  [died mid-render]" if i in demo.dead else ""
        print(f"  {ip:15s} {boards.get(ip, ''):24s} "
              f"{n:4d} tiles ({100 * n / demo.tiles:4.1f}%){state}")
    print(f"wrote {args.out}.png and {args.out}_nodes.png")


if __name__ == "__main__":
    main()
