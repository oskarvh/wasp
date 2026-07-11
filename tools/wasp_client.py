#!/usr/bin/env python3
"""Minimal wasp test client / embryonic coordinator.

Frame format must match app/src/protocol.h.

Usage:
  wasp_client.py <node-ip> check                    # protocol self-test
  wasp_client.py <node-ip> lifecycle mod.wasm       # load/call/unload self-test
  wasp_client.py <node-ip> load mod.wasm
  wasp_client.py <node-ip> call <func> [i32 args...]
  wasp_client.py <node-ip> unload
"""

import argparse
import random
import socket
import struct

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
}


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

    def request(self, msg_type: int, payload: bytes = b"") -> tuple[int, int, bytes]:
        """Send one frame and return the (type, seq, payload) response."""
        seq = self.seq = (self.seq + 1) & 0xFF
        self.sock.sendall(HDR.pack(MAGIC, msg_type, seq, len(payload)) + payload)
        rtype, rseq, rpayload = self._recv_frame()
        if rseq != seq:
            raise ValueError(
                f"response seq {rseq} does not match request seq {seq} "
                f"(type 0x{rtype:02x}) — stale or corrupt stream")
        return rtype, rseq, rpayload

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

    def hello(self) -> tuple[int, int]:
        """Handshake; returns (node proto version, max payload size)."""
        t, _, payload = self.request(HELLO, bytes([PROTO_VERSION]))
        if t != HELLO_ACK or len(payload) != 5:
            raise ValueError(f"bad HELLO_ACK: type {t}, {payload!r}")
        return payload[0], struct.unpack("<I", payload[1:5])[0]

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
    version, max_payload = node.hello()
    check("HELLO -> HELLO_ACK", version == PROTO_VERSION,
          f"v{version}, max payload {max_payload} B")

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
    p = sub.add_parser("load", help="load a module")
    p.add_argument("module", help="path to .wasm file")
    sub.add_parser("unload", help="unload the module")
    p = sub.add_parser("call", help="call an exported i32 function")
    p.add_argument("func")
    p.add_argument("args", nargs="*", help="i32 arguments")

    args = ap.parse_args()
    node = WaspNode(args.host, args.port)
    print(f"connected to {args.host}:{args.port}")

    handlers = {"check": cmd_check, "lifecycle": cmd_lifecycle, "load": cmd_load,
                "unload": cmd_unload, "call": cmd_call, None: cmd_check}
    handlers[args.cmd](node, args)


if __name__ == "__main__":
    main()
