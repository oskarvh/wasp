#!/usr/bin/env python3
"""Minimal wasp test client.

Exercises the wire protocol against a live node: handshake, ping echo,
and (deliberately) an unknown frame type to check error handling.
Frame format must match app/src/protocol.h.

Usage: wasp_client.py <node-ip> [--port 4242]
"""

import argparse
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
}


class WaspNode:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.seq = 0

    def request(self, msg_type: int, payload: bytes = b"") -> tuple[int, int, bytes]:
        """Send one frame and return the (type, seq, payload) response."""
        seq = self.seq = (self.seq + 1) & 0xFF
        self.sock.sendall(HDR.pack(MAGIC, msg_type, seq, len(payload)) + payload)
        return self._recv_frame()

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


def check(name: str, ok: bool, detail: str = ""):
    print(f"  [{'ok' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))
    if not ok:
        raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host", help="node IP address")
    ap.add_argument("--port", type=int, default=4242)
    args = ap.parse_args()

    node = WaspNode(args.host, args.port)
    print(f"connected to {args.host}:{args.port}")

    t, seq, payload = node.request(HELLO, bytes([PROTO_VERSION]))
    check("HELLO -> HELLO_ACK", t == HELLO_ACK and len(payload) == 5)
    version, max_payload = payload[0], struct.unpack("<I", payload[1:5])[0]
    check("protocol version", version == PROTO_VERSION,
          f"v{version}, max payload {max_payload} B")

    probe = b"wasp probe \x00\x01\x02"
    t, seq, payload = node.request(PING, probe)
    check("PING -> PONG echo", t == PONG and payload == probe)

    t, seq, payload = node.request(0x7F)
    check("unknown type -> ERROR(BAD_TYPE)",
          t == ERROR and payload and ERR_NAMES.get(payload[0]) == "BAD_TYPE")

    t, seq, payload = node.request(CALL)
    check("CALL -> ERROR(UNSUPPORTED) for now",
          t == ERROR and payload and ERR_NAMES.get(payload[0]) == "UNSUPPORTED")

    print("all protocol checks passed")


if __name__ == "__main__":
    main()
