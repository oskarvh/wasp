#!/usr/bin/env python3
"""Minimal wasp test client / embryonic coordinator.

Frame format must match app/src/protocol.h.

Usage:
  wasp_client.py <node-ip> check                    # protocol self-test
  wasp_client.py <node-ip> lifecycle mod.wasm       # load/call/unload self-test
  wasp_client.py <node-ip> remote mod.wasm          # remote-memory self-test
  wasp_client.py <node-ip> load mod.wasm
  wasp_client.py <node-ip> call <func> [i32 args...]
  wasp_client.py <node-ip> unload

Remote memory: this client is also the authoritative memory host. Regions
registered via WaspNode.register_region() are served to the node while any
request is in flight; every access runs under the region's lock check, and
explicit LOCKs are granted as expiring leases.
"""

import argparse
import random
import socket
import struct
import time

MAGIC = b"WA"
PROTO_VERSION = 1
HDR = struct.Struct("<2sBBI")  # magic, type, seq, payload len

HELLO = 0x01
HELLO_ACK = 0x02
LOAD_MODULE = 0x03
UNLOAD_MODULE = 0x04
CALL = 0x05
RESULT = 0x06
ERROR = 0x07
PING = 0x08
PONG = 0x09
MEM_READ = 0x40
MEM_DATA = 0x41
MEM_WRITE = 0x42
MEM_ACK = 0x43
LOCK = 0x44
LOCK_GRANT = 0x45
UNLOCK = 0x46
REGION_INFO = 0x47
REGION_DESC = 0x48

NODE_REQUESTS = {MEM_READ, MEM_WRITE, LOCK, UNLOCK, REGION_INFO}

FEAT_REMOTE_MEM = 0x01
LOCK_LEASE_S = 5.0

ERR_BAD_ARGS = 0x0A
ERR_LOCKED = 0x0C
ERR_NO_REGION = 0x0D

ERR_NAMES = {
    0x01: "UNSUPPORTED",
    0x02: "BAD_TYPE",
    0x03: "TOO_LARGE",
    0x04: "NO_MEM",
    0x05: "BUSY",
    0x06: "NO_MODULE",
    0x07: "INTERNAL",
    0x08: "LOAD_FAILED",
    0x09: "TRAP",
    0x0A: "BAD_ARGS",
    0x0B: "NO_FUNC",
    0x0C: "LOCKED",
    0x0D: "NO_REGION",
}

# wasp.* host function return codes as seen by the module
# (WASP_REMOTE_* in tools/include/wasp/remote.h).
R_OK, R_ELOCKED, R_EBOUNDS, R_ENOREGION = 0, -1, -2, -3


def pack_ref(region: int, offset: int = 0) -> int:
    """Remote reference: (region_id:8 | offset:24)."""
    return (region << 24) | offset


def err_name(payload: bytes) -> str:
    """Decode an ERROR payload: [code u8][optional utf8 detail]."""
    if not payload:
        return "<empty>"
    name = ERR_NAMES.get(payload[0], f"0x{payload[0]:02x}")
    detail = payload[1:].decode(errors="replace")
    return f"{name}({detail})" if detail else name


class WaspNode:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        # Random start so a stale response from a previous connection
        # can't accidentally match our first request's seq.
        self.seq = random.randrange(256)
        # Remote memory served to the node: region_id -> bytearray, and
        # region_id -> (holder, lease expiry). Holder "node" is the
        # connected node; tests use other names to simulate rival nodes.
        self.regions: dict[int, bytearray] = {}
        self.locks: dict[int, tuple[str, float]] = {}

    def register_region(self, region_id: int, buf: bytearray):
        """Export buf to the node as remote memory region region_id."""
        self.regions[region_id] = buf

    def request(self, msg_type: int, payload: bytes = b"") -> tuple[int, int, bytes]:
        """Send one frame and return the (type, seq, payload) response.

        Node-initiated remote-memory requests arriving while we wait are
        serviced inline — the node issues them mid-CALL.
        """
        seq = self.seq = (self.seq + 1) & 0xFF
        self.sock.sendall(HDR.pack(MAGIC, msg_type, seq, len(payload)) + payload)
        while True:
            rtype, rseq, rpayload = self._recv_frame()
            if rtype in NODE_REQUESTS:
                self._serve_node_req(rtype, rseq, rpayload)
                continue
            if rseq != seq:
                raise ValueError(
                    f"response seq {rseq} does not match request seq {seq} "
                    f"(type 0x{rtype:02x}) — stale or corrupt stream")
            return rtype, rseq, rpayload

    # -- remote memory host (the coordinator side of wasp.*) ----------

    def _lock_holder(self, region: int):
        """Current unexpired lease holder of region, or None."""
        held = self.locks.get(region)
        if held is not None and held[1] > time.monotonic():
            return held[0]
        self.locks.pop(region, None)  # expired -> revoked
        return None

    def _serve_node_req(self, rtype: int, rseq: int, payload: bytes):
        resp_type, resp = self._node_response(rtype, payload)
        self.sock.sendall(HDR.pack(MAGIC, resp_type, rseq, len(resp)) + resp)

    def _node_response(self, rtype: int, p: bytes) -> tuple[int, bytes]:
        def error(code, detail=""):
            return ERROR, bytes([code]) + detail.encode()

        if rtype in (LOCK, UNLOCK, REGION_INFO):
            if len(p) != 1:
                return error(ERR_BAD_ARGS, "malformed request")
            region = p[0]
            if region not in self.regions:
                return error(ERR_NO_REGION)
            if rtype == REGION_INFO:
                return REGION_DESC, struct.pack("<I", len(self.regions[region]))
            holder = self._lock_holder(region)
            if rtype == LOCK:
                if holder not in (None, "node"):
                    return error(ERR_LOCKED)
                self.locks[region] = ("node", time.monotonic() + LOCK_LEASE_S)
                return LOCK_GRANT, struct.pack("<I", int(LOCK_LEASE_S * 1000))
            if holder != "node":  # UNLOCK
                return error(ERR_BAD_ARGS, "not lock holder")
            del self.locks[region]
            return MEM_ACK, b""

        # MEM_READ [ref u32][len u32] / MEM_WRITE [ref u32][data]
        if len(p) < (8 if rtype == MEM_READ else 5):
            return error(ERR_BAD_ARGS, "malformed request")
        ref = struct.unpack_from("<I", p)[0]
        region, offset = ref >> 24, ref & 0xFFFFFF
        if region not in self.regions:
            return error(ERR_NO_REGION)
        if self._lock_holder(region) not in (None, "node"):
            return error(ERR_LOCKED)
        buf = self.regions[region]
        if rtype == MEM_READ:
            length = struct.unpack_from("<I", p, 4)[0]
            if offset + length > len(buf):
                return error(ERR_BAD_ARGS, "out of bounds")
            return MEM_DATA, bytes(buf[offset:offset + length])
        data = p[4:]
        if offset + len(data) > len(buf):
            return error(ERR_BAD_ARGS, "out of bounds")
        buf[offset:offset + len(data)] = data
        return MEM_ACK, b""

    def _recv_frame(self) -> tuple[int, int, bytes]:
        hdr = self._recv_exact(HDR.size)
        magic, msg_type, seq, length = HDR.unpack(hdr)
        if magic != MAGIC:
            raise ValueError(f"bad magic: {magic!r}")
        return msg_type, seq, self._recv_exact(length)

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("node closed connection")
            buf += chunk
        return buf

    # -- convenience wrappers ------------------------------------------

    def hello(self) -> tuple[int, int, int]:
        """Handshake; returns (node proto version, max payload, features)."""
        t, _, payload = self.request(HELLO, bytes([PROTO_VERSION]))
        if t != HELLO_ACK or len(payload) < 5:
            raise ValueError(f"bad HELLO_ACK: type {t}, {payload!r}")
        features = payload[5] if len(payload) >= 6 else 0
        return payload[0], struct.unpack("<I", payload[1:5])[0], features

    def load(self, wasm: bytes):
        t, _, payload = self.request(LOAD_MODULE, wasm)
        if t != RESULT:
            raise ValueError(f"load failed: {err_name(payload)}")

    def unload(self):
        t, _, payload = self.request(UNLOAD_MODULE)
        if t != RESULT:
            raise ValueError(f"unload failed: {err_name(payload)}")

    def call(self, func: str, *args: int) -> list[int]:
        """Call an exported i32 function; returns its results."""
        name = func.encode()
        payload = (bytes([len(name)]) + name + bytes([len(args)])
                   + b"".join(struct.pack("<i", a) for a in args))
        t, _, payload = self.request(CALL, payload)
        if t != RESULT:
            raise ValueError(f"call {func} failed: {err_name(payload)}")
        n = payload[0]
        return [struct.unpack("<i", payload[1 + 4 * i:5 + 4 * i])[0] for i in range(n)]


def check(name: str, ok: bool, detail: str = ""):
    print(f"  [{'ok' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))
    if not ok:
        raise SystemExit(1)


def expect_error(node: WaspNode, req_type: int, payload: bytes, want: str) -> bool:
    t, _, resp = node.request(req_type, payload)
    return t == ERROR and bool(resp) and ERR_NAMES.get(resp[0]) == want


def cmd_check(node: WaspNode, args):
    version, max_payload, features = node.hello()
    check("HELLO -> HELLO_ACK", version == PROTO_VERSION,
          f"v{version}, max payload {max_payload} B, features 0x{features:02x}")

    probe = b"wasp probe \x00\x01\x02"
    t, _, payload = node.request(PING, probe)
    check("PING -> PONG echo", t == PONG and payload == probe)

    check("unknown type -> ERROR(BAD_TYPE)",
          expect_error(node, 0x7F, b"", "BAD_TYPE"))
    check("CALL w/o module -> ERROR(NO_MODULE)",
          expect_error(node, CALL, b"\x03add\x00", "NO_MODULE"))
    print("all protocol checks passed")


def cmd_lifecycle(node: WaspNode, args):
    wasm = open(args.module, "rb").read()
    node.hello()

    node.load(wasm)
    check("load module", True, f"{len(wasm)} B")

    check("add(2, 3)", node.call("add", 2, 3) == [5])
    check("add(-10, 4)", node.call("add", -10, 4) == [-6])
    check("fib(20)", node.call("fib", 20) == [6765])

    name = b"\x04boom\x00"
    check("boom() -> ERROR(TRAP)", expect_error(node, CALL, name, "TRAP"))
    check("still alive after trap", node.call("add", 1, 1) == [2])

    check("missing export -> ERROR(NO_FUNC)",
          expect_error(node, CALL, b"\x07nothere\x00", "NO_FUNC"))
    check("bad argc -> ERROR(BAD_ARGS)",
          expect_error(node, CALL, b"\x03add\x01" + struct.pack("<i", 1), "BAD_ARGS"))
    check("double load -> ERROR(BUSY)",
          expect_error(node, LOAD_MODULE, wasm, "BUSY"))

    node.unload()
    check("unload module", True)
    check("call after unload -> ERROR(NO_MODULE)",
          expect_error(node, CALL, b"\x03add\x00", "NO_MODULE"))
    check("garbage module -> ERROR(LOAD_FAILED)",
          expect_error(node, LOAD_MODULE, b"\x00asm garbage", "LOAD_FAILED"))
    print("all lifecycle checks passed")


def cmd_remote(node: WaspNode, args):
    """Remote-memory self-test against tools/remote_module.wasm."""
    wasm = open(args.module, "rb").read()
    version, _, features = node.hello()
    check("node advertises remote memory", bool(features & FEAT_REMOTE_MEM),
          f"features 0x{features:02x}")
    node.load(wasm)

    # Region 1: sixteen i32s, values 0..15 (sum 120).
    region = bytearray(struct.pack("<16i", *range(16)))
    node.register_region(1, region)
    ref = pack_ref(1)

    check("region_size from module", node.call("region_len", 1) == [64])
    check("unknown region -> ENOREGION", node.call("region_len", 9) == [R_ENOREGION])

    check("element-wise remote sum", node.call("sum_region", ref, 16) == [120])
    check("bulk remote sum", node.call("sum_region_bulk", ref, 16) == [120])
    check("reference arithmetic (offset ref)",
          node.call("sum_region_bulk", pack_ref(1, 32), 8) == [sum(range(8, 16))])

    check("remote write", node.call("fill_region", ref, 16, 7) == [R_OK])
    check("coordinator memory updated",
          region == bytearray(struct.pack("<16i", *([7] * 16))))

    check("out-of-bounds read -> EBOUNDS",
          node.call("sum_region_bulk", pack_ref(1, 40), 16) == [R_EBOUNDS])
    check("out-of-bounds write -> EBOUNDS",
          node.call("try_write", pack_ref(1, 61), 1) == [R_EBOUNDS])

    struct.pack_into("<i", region, 0, 100)
    check("locked read-modify-write", node.call("locked_add", 1, ref, 5) == [105])
    check("lock released after locked_add", 1 not in node.locks)

    # A rival node holds the lock: everything must fail fast, then the
    # lease must expire and revoke it.
    node.locks[1] = ("rival", time.monotonic() + 1.5)
    check("write to locked region -> ELOCKED",
          node.call("try_write", ref, 1) == [R_ELOCKED])
    check("read of locked region -> ELOCKED",
          node.call("sum_region_bulk", ref, 16) == [R_ELOCKED])
    check("lock while held -> ELOCKED", node.call("locked_add", 1, ref, 1) == [R_ELOCKED])
    time.sleep(1.6)
    check("expired lease is revoked", node.call("try_write", ref, 42) == [R_OK])
    check("write after revocation landed",
          struct.unpack_from("<i", region, 0)[0] == 42)

    node.unload()
    print("all remote memory checks passed")


def cmd_load(node: WaspNode, args):
    node.hello()
    node.load(open(args.module, "rb").read())
    print("module loaded")


def cmd_unload(node: WaspNode, args):
    node.hello()
    node.unload()
    print("module unloaded")


def cmd_call(node: WaspNode, args):
    node.hello()
    results = node.call(args.func, *[int(a, 0) for a in args.args])
    print(f"{args.func}({', '.join(args.args)}) -> {results}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="node IP address")
    ap.add_argument("--port", type=int, default=4242)
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("check", help="protocol self-test")
    p = sub.add_parser("lifecycle", help="load/call/unload self-test")
    p.add_argument("module", help="path to .wasm file")
    p = sub.add_parser("remote", help="remote-memory self-test")
    p.add_argument("module", help="path to remote_module.wasm")
    p = sub.add_parser("load", help="load a module")
    p.add_argument("module", help="path to .wasm file")
    sub.add_parser("unload", help="unload the module")
    p = sub.add_parser("call", help="call an exported i32 function")
    p.add_argument("func")
    p.add_argument("args", nargs="*", help="i32 arguments")

    args = ap.parse_args()
    node = WaspNode(args.host, args.port)
    print(f"connected to {args.host}:{args.port}")

    handlers = {"check": cmd_check, "lifecycle": cmd_lifecycle, "remote": cmd_remote,
                "load": cmd_load, "unload": cmd_unload, "call": cmd_call, None: cmd_check}
    handlers[args.cmd](node, args)


if __name__ == "__main__":
    main()
