#!/usr/bin/env python3
"""Run wasp_client's self-test suites through the libwasp C library.

The C-backed WaspNode is API-compatible, so the pure-Python client's
own check/lifecycle/remote/remote2 test functions run against it
unchanged — every assertion that passes here proves the C
coordinator behaves exactly like the Python reference.

Usage:
  libwasp_selftest.py <node-ip> [more ips...]
"""

import sys
import time
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).parent))
import wasp_client as wc
import libwasp

TOOLS = Path(__file__).parent


def connect_retry(ip: str, attempts: int = 20) -> libwasp.WaspNode:
    """The node serves one coordinator at a time and needs a moment to
    notice the previous connection closed before accepting again."""
    for i in range(attempts):
        try:
            return libwasp.WaspNode(ip, 4242)
        except ConnectionError:
            if i == attempts - 1:
                raise
            time.sleep(0.5)


def run(ip: str) -> None:
    suites = [
        ("check", wc.cmd_check, SimpleNamespace()),
        ("lifecycle", wc.cmd_lifecycle,
         SimpleNamespace(module=str(TOOLS / "test_module.wasm"))),
        ("remote", wc.cmd_remote,
         SimpleNamespace(module=str(TOOLS / "remote_module.wasm"))),
        ("remote2", wc.cmd_remote2,
         SimpleNamespace(as_module=str(TOOLS / "remote_as_module.wasm"),
                         cpp_module=str(TOOLS / "remote_cpp_module.wasm"))),
        # The same multi-instance suite wasp_client runs, driven through C:
        # upload with a count, per-instance calls, handle generations and
        # the v1 compatibility path all have to behave identically.
        ("instances", wc.cmd_instances,
         SimpleNamespace(module=str(TOOLS / "test_module.wasm"),
                         module2=str(TOOLS / "remote_module.wasm"))),
    ]
    for name, fn, args in suites:
        print(f"[{ip}] {name} (via libwasp)")
        node = connect_retry(ip)
        try:
            fn(node, args)
        finally:
            node.close()


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for ip in sys.argv[1:]:
        run(ip)
    print("\nall libwasp fleet self-tests passed")


if __name__ == "__main__":
    main()
