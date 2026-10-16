"""Python binding for libwasp — the coordinator C library.

Drop-in replacement for wasp_client's WaspNode/discover/pack_ref,
backed by libwasp/libwasp.so via ctypes (built automatically on first
use). Differences from the pure-Python client:

- Regions, locks, and the lock queue live in a shared Ctx (the C
  wasp_ctx), not in per-node dicts. WaspNode.register_region and the
  node.regions / node.locks attributes are thin shims over the default
  Ctx, so existing tools keep working unchanged.
- Remote-memory writes are observable as events: Ctx.poll_event()
  returns (tag, type, region, offset, len) tuples, tag being the
  node's user-assigned tag. This replaces subclass hooks like the
  demo's old _node_response override.
"""

import ctypes
import struct
import subprocess
import time
from collections import namedtuple
from pathlib import Path

LIB_DIR = Path(__file__).resolve().parent.parent / "libwasp"

PROTO_VERSION = 1
ANNOUNCE_PORT = 4243

# HELLO_ACK / ANNOUNCE feature bits (protocol.h).
FEAT_REMOTE_MEM = 0x01
FEAT_ATOMICS = 0x02
FEAT_MULTI_INSTANCE = 0x04
EV_MEM_WRITE = 1

ERR_NAMES = {
    0x01: "UNSUPPORTED", 0x02: "BAD_TYPE", 0x03: "TOO_LARGE",
    0x04: "NO_MEM", 0x05: "BUSY", 0x06: "NO_MODULE", 0x07: "INTERNAL",
    0x08: "LOAD_FAILED", 0x09: "TRAP", 0x0A: "BAD_ARGS", 0x0B: "NO_FUNC",
    0x0C: "LOCKED", 0x0D: "NO_REGION",
}

E_TIMEOUT, E_CONN, E_STREAM = -100, -101, -102

Event = namedtuple("Event", "tag type region offset len")


def pack_ref(region: int, offset: int = 0) -> int:
    """Remote reference: (region_id:8 | offset:24)."""
    return (region << 24) | offset


class _CEvent(ctypes.Structure):
    _fields_ = [("tag", ctypes.c_uint32), ("type", ctypes.c_uint8),
                ("region", ctypes.c_uint8), ("offset", ctypes.c_uint32),
                ("len", ctypes.c_uint32)]


class _CInfo(ctypes.Structure):
    # Mirrors wasp_info in libwasp.h — the capacity tail included, or
    # ctypes would read the wrong offsets.
    _fields_ = [("ip", ctypes.c_char * 46), ("tcp_port", ctypes.c_uint16),
                ("version", ctypes.c_uint8), ("features", ctypes.c_uint8),
                ("busy", ctypes.c_uint8), ("board", ctypes.c_char * 33),
                ("slots_total", ctypes.c_uint8),
                ("slots_free", ctypes.c_uint8),
                ("pool_free", ctypes.c_uint32)]


class _CInvModule(ctypes.Structure):
    _fields_ = [("mod", ctypes.c_uint8), ("n_inst", ctypes.c_uint8),
                ("inst", ctypes.c_uint8 * 16)]


class _CInventory(ctypes.Structure):
    _fields_ = [("slots_total", ctypes.c_uint8),
                ("slots_free", ctypes.c_uint8),
                ("pool_free", ctypes.c_uint32),
                ("n_modules", ctypes.c_uint8),
                ("modules", _CInvModule * 16)]


def _load_lib():
    so = LIB_DIR / "libwasp.so"
    if not so.exists():
        subprocess.run(["make", "-C", str(LIB_DIR), "libwasp.so"],
                       check=True, capture_output=True)
    lib = ctypes.CDLL(str(so))

    def sig(name, restype, *argtypes):
        f = getattr(lib, name)
        f.restype = restype
        f.argtypes = list(argtypes)

    p, u8, u16, u32 = (ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint16,
                       ctypes.c_uint32)
    sig("wasp_strerror", ctypes.c_char_p, ctypes.c_int)
    sig("wasp_ctx_new", p)
    sig("wasp_ctx_free", None, p)
    sig("wasp_region_register", ctypes.c_int, p, u8, ctypes.c_void_p, u32)
    sig("wasp_region_unregister", ctypes.c_int, p, u8)
    sig("wasp_lock_force", ctypes.c_int, p, u8, ctypes.c_char_p, u32)
    sig("wasp_lock_clear", ctypes.c_int, p, u8)
    sig("wasp_lock_holder", ctypes.c_int, p, u8, ctypes.c_char_p, u32)
    sig("wasp_poll_event", ctypes.c_int, p, ctypes.POINTER(_CEvent), u32)
    sig("wasp_connect", p, p, ctypes.c_char_p, u16, u32)
    sig("wasp_node_free", None, p)
    sig("wasp_node_kill", None, p)
    sig("wasp_node_set_owner", None, p, ctypes.c_char_p)
    sig("wasp_node_set_queue_locks", None, p, ctypes.c_int)
    sig("wasp_node_set_tag", None, p, u32)
    sig("wasp_node_error_detail", ctypes.c_char_p, p)
    sig("wasp_request", ctypes.c_int, p, u8, ctypes.c_char_p, u32,
        ctypes.POINTER(u8), ctypes.POINTER(u8),
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(u32))
    sig("wasp_hello", ctypes.c_int, p, ctypes.POINTER(u8),
        ctypes.POINTER(u32), ctypes.POINTER(u8))
    sig("wasp_load", ctypes.c_int, p, ctypes.c_char_p, u32)
    sig("wasp_unload", ctypes.c_int, p)
    sig("wasp_call", ctypes.c_int, p, ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int32), u32, ctypes.POINTER(ctypes.c_int32))
    sig("wasp_upload", ctypes.c_int, p, ctypes.c_char_p, u32, u8,
        ctypes.POINTER(u8), ctypes.POINTER(u8), u8)
    sig("wasp_instantiate", ctypes.c_int, p, u8, u8, ctypes.POINTER(u8), u8)
    sig("wasp_call_inst", ctypes.c_int, p, u8, ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int32), u32, ctypes.POINTER(ctypes.c_int32))
    sig("wasp_drop_instance", ctypes.c_int, p, u8)
    sig("wasp_drop_module", ctypes.c_int, p, u8)
    sig("wasp_inventory_get", ctypes.c_int, p, ctypes.POINTER(_CInventory))
    sig("wasp_call_inst_async", ctypes.c_int, p, u8, ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int32), u32)
    sig("wasp_call_poll_any", ctypes.c_int, p, ctypes.POINTER(u8),
        ctypes.POINTER(ctypes.c_int32))
    sig("wasp_call_wait_any", ctypes.c_int, p, ctypes.POINTER(u8),
        ctypes.POINTER(ctypes.c_int32), u32)
    sig("wasp_call_inflight", ctypes.c_int, p)
    sig("wasp_discover", ctypes.c_int, ctypes.POINTER(_CInfo), u32, u32, u16)
    return lib


_lib = _load_lib()


def _err_text(rc: int, detail: str = "") -> str:
    name = ERR_NAMES.get(-rc)
    if name is None:
        name = _lib.wasp_strerror(rc).decode()
    return f"{name}({detail})" if detail else name


def _raise_transport(rc: int):
    if rc == E_TIMEOUT:
        raise TimeoutError("timed out waiting for node")
    if rc == E_CONN:
        raise ConnectionError("node closed connection")
    if rc == E_STREAM:
        raise ValueError("protocol stream violation — stale or corrupt stream")
    raise OSError(f"libwasp: {_lib.wasp_strerror(rc).decode()}")


class Ctx:
    """Shared coordinator state: regions, locks, and the event queue.
    All WaspNodes on the same Ctx share memory — this IS the
    'coordinator serializes every access' guarantee."""

    def __init__(self):
        self._c = _lib.wasp_ctx_new()
        if not self._c:
            raise MemoryError("wasp_ctx_new failed")
        self.regions: dict[int, bytearray] = {}
        self._cbufs: dict[int, object] = {}  # keeps from_buffer views alive

    def register_region(self, region_id: int, buf: bytearray):
        cbuf = (ctypes.c_ubyte * len(buf)).from_buffer(buf)
        rc = _lib.wasp_region_register(self._c, region_id, cbuf, len(buf))
        if rc != 0:
            raise ValueError(f"register_region: {_err_text(rc)}")
        self.regions[region_id] = buf
        self._cbufs[region_id] = cbuf

    def unregister_region(self, region_id: int):
        _lib.wasp_region_unregister(self._c, region_id)
        self.regions.pop(region_id, None)
        self._cbufs.pop(region_id, None)

    def lock_force(self, region_id: int, owner: str, lease_s: float):
        _lib.wasp_lock_force(self._c, region_id, owner.encode(),
                             max(1, int(lease_s * 1000)))

    def lock_clear(self, region_id: int):
        _lib.wasp_lock_clear(self._c, region_id)

    def lock_holder(self, region_id: int):
        buf = ctypes.create_string_buffer(32)
        if _lib.wasp_lock_holder(self._c, region_id, buf, 32) == 1:
            return buf.value.decode()
        return None

    def poll_event(self, timeout: float = 0.0):
        """Next remote-memory event, or None on timeout."""
        ev = _CEvent()
        got = _lib.wasp_poll_event(self._c, ctypes.byref(ev),
                                   int(timeout * 1000))
        if got != 1:
            return None
        return Event(ev.tag, ev.type, ev.region, ev.offset, ev.len)


_default_ctx: Ctx | None = None


def default_ctx() -> Ctx:
    """The Ctx WaspNode uses unless given another — one per process,
    like the shared dicts of the pure-Python coordinator."""
    global _default_ctx
    if _default_ctx is None:
        _default_ctx = Ctx()
    return _default_ctx


class _LocksShim:
    """Dict-flavored view of the ctx lock table, for tools that poke
    node.locks[region] = (owner, expiry) directly (self-tests)."""

    def __init__(self, ctx: Ctx):
        self._ctx = ctx

    def __setitem__(self, region, value):
        owner, expiry = value
        self._ctx.lock_force(region, owner, expiry - time.monotonic())

    def __delitem__(self, region):
        self._ctx.lock_clear(region)

    def __contains__(self, region):
        return self._ctx.lock_holder(region) is not None

    def get(self, region, default=None):
        holder = self._ctx.lock_holder(region)
        return holder if holder is not None else default


class _SockShim:
    """Just enough socket surface for tools that call node.sock.close()
    (which must only sever the connection, never free the node — other
    threads may be blocked inside it)."""

    def __init__(self, node):
        self._node = node

    def close(self):
        _lib.wasp_node_kill(self._node._n)


class WaspNode:
    """API-compatible with wasp_client.WaspNode, backed by libwasp.
    Use one thread per node; nodes on the same Ctx share regions."""

    def __init__(self, host: str, port: int = 4242, timeout: float = 5.0,
                 lock_owner: str = "node", queue_locks: bool = False,
                 ctx: Ctx | None = None, tag: int = 0):
        self.ctx = ctx or default_ctx()
        self._n = _lib.wasp_connect(self.ctx._c, host.encode(), port,
                                    int(timeout * 1000))
        if not self._n:
            raise ConnectionError(f"cannot connect to {host}:{port}")
        self.host = host
        self._owner = lock_owner
        self._queue_locks = queue_locks
        _lib.wasp_node_set_owner(self._n, lock_owner.encode())
        _lib.wasp_node_set_queue_locks(self._n, int(queue_locks))
        _lib.wasp_node_set_tag(self._n, tag)
        self.regions = self.ctx.regions
        self.locks = _LocksShim(self.ctx)
        self.sock = _SockShim(self)

    # -- knobs ---------------------------------------------------------

    @property
    def lock_owner(self):
        return self._owner

    @lock_owner.setter
    def lock_owner(self, owner: str):
        self._owner = owner
        _lib.wasp_node_set_owner(self._n, owner.encode())

    @property
    def queue_locks(self):
        return self._queue_locks

    @queue_locks.setter
    def queue_locks(self, on: bool):
        self._queue_locks = bool(on)
        _lib.wasp_node_set_queue_locks(self._n, int(on))

    def register_region(self, region_id: int, buf: bytearray):
        """Export buf to every node on this Ctx as region region_id."""
        self.ctx.register_region(region_id, buf)

    def close(self):
        if self._n:
            _lib.wasp_node_free(self._n)
            self._n = None

    # -- protocol ------------------------------------------------------

    def request(self, msg_type: int, payload: bytes = b"") -> tuple[int, int, bytes]:
        """Send one frame, return the (type, seq, payload) response.
        Node-initiated remote-memory RPCs are serviced inline in C."""
        rtype, rseq = ctypes.c_uint8(), ctypes.c_uint8()
        rpay, rlen = ctypes.c_void_p(), ctypes.c_uint32()
        rc = _lib.wasp_request(self._n, msg_type, payload, len(payload),
                               ctypes.byref(rtype), ctypes.byref(rseq),
                               ctypes.byref(rpay), ctypes.byref(rlen))
        if rc != 0:
            _raise_transport(rc)
        data = (ctypes.string_at(rpay.value, rlen.value)
                if rlen.value else b"")
        return rtype.value, rseq.value, data

    def _detail(self) -> str:
        return _lib.wasp_node_error_detail(self._n).decode(errors="replace")

    def hello(self) -> tuple[int, int, int]:
        """Handshake; returns (node proto version, max payload, features)."""
        ver, maxp, feat = (ctypes.c_uint8(), ctypes.c_uint32(),
                           ctypes.c_uint8())
        rc = _lib.wasp_hello(self._n, ctypes.byref(ver), ctypes.byref(maxp),
                             ctypes.byref(feat))
        if rc <= -100:
            _raise_transport(rc)
        if rc != 0:
            raise ValueError(f"hello failed: {_err_text(rc, self._detail())}")
        return ver.value, maxp.value, feat.value

    def load(self, wasm: bytes):
        rc = _lib.wasp_load(self._n, wasm, len(wasm))
        if rc <= -100:
            _raise_transport(rc)
        if rc != 0:
            raise ValueError(f"load failed: {_err_text(rc, self._detail())}")

    def unload(self):
        rc = _lib.wasp_unload(self._n)
        if rc <= -100:
            _raise_transport(rc)
        if rc != 0:
            raise ValueError(f"unload failed: {_err_text(rc, self._detail())}")

    def call(self, func: str, *args: int) -> list[int]:
        """Call an exported i32 function; returns its results."""
        argv = (ctypes.c_int32 * max(1, len(args)))(*args)
        out = (ctypes.c_int32 * 4)()
        rc = _lib.wasp_call(self._n, func.encode(), argv, len(args), out)
        if rc <= -100:
            _raise_transport(rc)
        if rc < 0:
            raise ValueError(
                f"call {func} failed: {_err_text(rc, self._detail())}")
        return [out[i] for i in range(rc)]

    # -- multi-instance ------------------------------------------------
    # Names match tools/wasp_client.py so the shared self-tests run
    # against either implementation unchanged.

    def _check(self, rc: int, what: str) -> int:
        if rc <= -100:
            _raise_transport(rc)
        if rc < 0:
            raise ValueError(f"{what} failed: {_err_text(rc, self._detail())}")
        return rc

    def upload(self, wasm: bytes, n_inst: int = 1) -> tuple[int, list[int]]:
        mod = ctypes.c_uint8()
        insts = (ctypes.c_uint8 * 16)()
        n = self._check(
            _lib.wasp_upload(self._n, wasm, len(wasm), n_inst,
                             ctypes.byref(mod), insts, 16), "upload")
        return mod.value, [insts[i] for i in range(n)]

    def instantiate(self, mod: int, count: int = 1) -> list[int]:
        insts = (ctypes.c_uint8 * 16)()
        n = self._check(_lib.wasp_instantiate(self._n, mod, count, insts, 16),
                        "instantiate")
        return [insts[i] for i in range(n)]

    def call_inst(self, inst: int, func: str, *args: int) -> list[int]:
        argv = (ctypes.c_int32 * max(1, len(args)))(*args)
        out = (ctypes.c_int32 * 4)()
        rc = self._check(
            _lib.wasp_call_inst(self._n, inst, func.encode(), argv,
                                len(args), out), f"call {func}")
        return [out[i] for i in range(rc)]

    def drop_instance(self, inst: int):
        self._check(_lib.wasp_drop_instance(self._n, inst), "drop instance")

    def drop_module(self, mod: int):
        self._check(_lib.wasp_drop_module(self._n, mod), "drop module")

    def stats(self, reset: bool = False) -> list[dict]:
        """Per-instance time and traffic counters from the node.

        Built on request() rather than a C entry point: the parsing is a
        handful of little-endian words and the conductor has no use for
        it yet, so there is nothing for libwasp proper to carry.
        """
        t, _, p = self.request(0x12, bytes([1 if reset else 0]))  # STATS
        if t != 0x06:  # RESULT
            raise ValueError("stats failed")
        out, at = [], 1
        for _ in range(p[0]):
            inst = p[at]
            (calls, traps, exec_us, rpc_count,
             rpc_wait_us, rpc_tx, rpc_rx) = struct.unpack_from("<7I", p, at + 1)
            at += 29
            out.append({
                "inst": inst, "calls": calls, "traps": traps,
                "exec_us": exec_us, "rpc_count": rpc_count,
                "rpc_wait_us": rpc_wait_us, "rpc_tx": rpc_tx, "rpc_rx": rpc_rx,
                "compute_us": max(0, exec_us - rpc_wait_us),
                # See wasp_client.WaspNode.stats: waiting cannot exceed
                # exec, so when it does a counter has overflowed and the
                # derived figures are junk. Both bindings must set this or
                # print_stats, which is shared, cannot warn.
                "bogus": rpc_wait_us > exec_us,
            })
        return out

    def reset(self):
        """Release everything resident, whatever shape it is in.

        Mirrors wasp_client.WaspNode.reset so the shared self-test
        suites run unchanged against either implementation.
        """
        try:
            for m in self.inventory()["modules"]:
                try:
                    self.drop_module(m["mod"])
                except ValueError:
                    pass          # an instance still finishing a call
        except ValueError:
            pass                  # node predates INVENTORY
        try:
            self.unload()         # and the v1 default instance, if any
        except ValueError:
            pass

    def inventory(self) -> dict:
        inv = _CInventory()
        self._check(_lib.wasp_inventory_get(self._n, ctypes.byref(inv)),
                    "inventory")
        return {
            "slots_total": inv.slots_total,
            "slots_free": inv.slots_free,
            "pool_free": inv.pool_free,
            "modules": [
                {"mod": inv.modules[i].mod,
                 "instances": [inv.modules[i].inst[j]
                               for j in range(inv.modules[i].n_inst)]}
                for i in range(inv.n_modules)
            ],
        }

    def call_inst_async(self, inst: int, func: str, *args: int):
        argv = (ctypes.c_int32 * max(1, len(args)))(*args)
        self._check(
            _lib.wasp_call_inst_async(self._n, inst, func.encode(), argv,
                                      len(args)), f"async call {func}")

    def call_poll_any(self):
        """(instance, results) for whichever call finished, or None."""
        inst = ctypes.c_uint8()
        out = (ctypes.c_int32 * 4)()
        rc = _lib.wasp_call_poll_any(self._n, ctypes.byref(inst), out)
        if rc == -107:  # WASP_E_AGAIN
            return None
        rc = self._check(rc, "poll")
        return inst.value, [out[i] for i in range(rc)]

    def call_wait_any(self, timeout_ms: int = 120000):
        """Block until one outstanding call finishes; (instance, results).

        Returns None on timeout. Unlike call_poll_any this waits on the
        socket, so the node's remote-memory RPCs are answered as they
        arrive rather than once per polling interval — which matters when
        a round trip is of the same order as any sleep you would use.
        """
        inst = ctypes.c_uint8()
        out = (ctypes.c_int32 * 4)()
        rc = _lib.wasp_call_wait_any(self._n, ctypes.byref(inst), out, timeout_ms)
        if rc == -100:  # WASP_E_TIMEOUT
            return None
        rc = self._check(rc, "wait")
        return inst.value, [out[i] for i in range(rc)]

    def inflight(self) -> int:
        return _lib.wasp_call_inflight(self._n)


def discover(timeout: float = 6.0, port: int = ANNOUNCE_PORT) -> dict[str, dict]:
    """Listen for ANNOUNCE broadcasts; returns {ip: node info}."""
    infos = (_CInfo * 64)()
    n = _lib.wasp_discover(infos, 64, int(timeout * 1000), port)
    if n < 0:
        raise OSError(f"discover failed: {_lib.wasp_strerror(n).decode()}")
    return {
        infos[i].ip.decode(): {
            "version": infos[i].version,
            "features": infos[i].features,
            "port": infos[i].tcp_port,
            "busy": bool(infos[i].busy),
            "board": infos[i].board.decode(errors="replace"),
            "slots_total": infos[i].slots_total,
            "slots_free": infos[i].slots_free,
            "pool_free": infos[i].pool_free,
        }
        for i in range(n)
    }
