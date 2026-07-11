# Writing and running a WASM function on a wasp node

This is the complete walk-through from a C function on your laptop to a
result coming back from a node — no WASM SDK, no Zephyr knowledge
required. Total time: about two minutes.

## Prerequisites

- **clang with a wasm32 backend + wasm-ld** — on Debian/Ubuntu:
  `sudo apt install clang lld`. Verify with:

  ```sh
  clang --print-targets | grep wasm32
  which wasm-ld
  ```

- **python3** (stdlib only — no packages needed) for the client.
- A **flashed wasp node** on your network (see the top-level README for
  building and flashing the firmware).

## Step 1 — write the function

Create `gcd.c`:

```c
/* Greatest common divisor. */
__attribute__((export_name("gcd"))) int gcd(int a, int b)
{
	while (b != 0) {
		int t = b;

		b = a % b;
		a = t;
	}
	return a;
}
```

The rules (v1 calling convention — enforced by the node, violations come
back as clean errors, never crashes):

| Rule | Why |
| ---- | --- |
| Mark every callable function `__attribute__((export_name("...")))` | Only exported functions are visible to `CALL`. The exported name is what you call, regardless of the C name. |
| Parameters and return values must be `int` (i32) | The v1 wire format only carries 32-bit integer cells. `float`, `double`, `long long` and pointers-as-return-API are rejected with `BAD_ARGS`. |
| No external functions (nothing to link against) | The node currently provides no host functions, so a module that *imports* anything fails to instantiate (`LOAD_FAILED`). That means no `printf`, no libc — pure computation. |
| Keep it small | Modules are capped at 32 KiB on the wire (`CONFIG_WASP_MAX_MODULE_SIZE`, advertised in the handshake). |

Within the module you *can* use statics, arrays, recursion, and even
`malloc`-style allocation if you bring your own allocator — the module
gets one 64 KiB page of linear memory with a small app heap. What you
can't do (yet) is talk to the outside world from inside the module.

Multiple functions per module are fine — the test module
(`tools/test_module.c`) exports three.

## Step 2 — compile it

```sh
clang --target=wasm32 -O2 -nostdlib \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
      -z stack-size=1024 \
      -o gcd.wasm gcd.c
```

Flag by flag:

- `--target=wasm32` — cross-compile to 32-bit WASM.
- `-nostdlib` — no libc (there is nothing to link against on the node).
- `-Wl,--no-entry` — a module is a library, not a program; there is no `main`.
- `-Wl,--export-dynamic` — export everything marked `export_name`.
- `-Wl,--strip-all` — drop debug info; keeps the module small.
- `-z stack-size=1024` — the module's internal call stack. The default is
  64 KiB, which would waste most of the linear memory page; 1 KiB is
  plenty for non-recursive code. Deep recursion needs more.

The result is tiny (typically a few hundred bytes):

```sh
$ ls -l gcd.wasm
-rwxrwxr-x 1 you you 100 … gcd.wasm
```

## Step 3 — find your node

The node prints its DHCP address on the serial console (115200 baud) at
boot, and it answers on TCP port 4242:

```sh
picocom -b 115200 /dev/ttyACM0        # …wasp_net: IPv4 address: 10.0.0.154
```

Sanity-check the connection first:

```sh
$ python3 tools/wasp_client.py 10.0.0.154 check
connected to 10.0.0.154:4242
  [ok] HELLO -> HELLO_ACK: v1, max payload 32768 B
  …
all protocol checks passed
```

## Step 4 — push the module and call it

```sh
$ python3 tools/wasp_client.py 10.0.0.154 load gcd.wasm
connected to 10.0.0.154:4242
module loaded

$ python3 tools/wasp_client.py 10.0.0.154 call gcd 462 1071
connected to 10.0.0.154:4242
gcd(462, 1071) -> [21]

$ python3 tools/wasp_client.py 10.0.0.154 unload
connected to 10.0.0.154:4242
module unloaded
```

That's the whole loop: the module stays loaded (and its statics keep
their values between calls) until you `unload` it, the node reboots, or
you push a replacement (`unload` first — loading over a loaded module is
rejected with `BUSY`).

Arguments are i32s and accept any Python-int syntax including negative
and hex (`0x2a`). Results print as a list because WASM functions may
return multiple values.

## Step 5 — reading results and errors

A successful `call` returns the function's results; everything that can
go wrong comes back as a one-line error telling you what to fix:

| You see | Meaning / fix |
| ------- | ------------- |
| `call gcd failed: NO_MODULE` | Nothing loaded — `load` first (also happens after a node reboot). |
| `call gdc failed: NO_FUNC(gdc)` | No export with that name — typo, or you forgot `export_name`. |
| `call gcd failed: BAD_ARGS(argument count mismatch)` | Wrong number of arguments. |
| `call gcd failed: BAD_ARGS(non-i32 parameter)` | The C signature uses something other than `int`. |
| `call boom failed: TRAP(unreachable)` | The module crashed *inside the sandbox* — division by zero, out-of-bounds access, `__builtin_trap()`. The node is unharmed; fix the bug and call again. |
| `load failed: LOAD_FAILED(…)` | Not a valid module, or it imports functions the node doesn't provide, or its memory demands don't fit. The detail text is WAMR's actual diagnostic. |
| `load failed: TOO_LARGE` | Module exceeds the 32 KiB cap. |

## Scripting it

`tools/wasp_client.py` is importable — the same five lines the CLI uses:

```python
import sys; sys.path.insert(0, "tools")   # or copy wasp_client.py next to your script
from wasp_client import WaspNode

node = WaspNode("10.0.0.154", 4242)
node.hello()
node.load(open("gcd.wasm", "rb").read())
print(node.call("gcd", 462, 1071))        # -> [21]
node.unload()
```

`call()` raises `ValueError` with the decoded error name on failure, so
scripts fail loudly.

## Current limitations (v1)

- i32 arguments and results only; max 8 args, 4 results.
- One module loaded at a time per node.
- No host functions yet — modules cannot do I/O or touch peripherals;
  results must come back through return values.
- Function names ≤ 63 bytes; modules ≤ 32 KiB.

The wire format behind all of this is documented in the top-level README
and defined in `app/src/protocol.h`.
