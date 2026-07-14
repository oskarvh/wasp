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
| No external functions, except `wasp.*` | The only host functions the node provides are the remote-memory API (`tools/include/wasp/remote.h` — read/write/lock coordinator RAM; see the README's *Remote memory* section). Importing anything else fails to instantiate (`LOAD_FAILED`). That means no `printf`, no libc — pure computation plus remote memory. |
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

Nodes announce themselves over UDP broadcast every 5 seconds, so no
serial cable is needed — just listen:

```sh
$ python3 tools/wasp_client.py discover
10.0.0.154:4242  v1  features=0x01  free  nucleo_f439zi/stm32f439xx
10.0.0.181:4242  v1  features=0x01  free  rpi_pico/rp2040/w
```

`free`/`busy` tells you whether a coordinator is already connected. With
several identical boards on the bench, strobe one's status LED to see
which is which:

```sh
python3 tools/wasp_client.py 10.0.0.181 identify   # LED strobes for 10 s
```

(Fallback: the node also prints its DHCP address on the serial console,
115200 baud, at boot — `picocom -b 115200 /dev/ttyACM0`.)

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

## Remote memory: working on the coordinator's RAM

Everything above is pure computation — data in, i32s out. Modules can
also work on memory that lives in the **coordinator's** RAM: the node
provides `wasp.*` host functions that fetch, write back, and lock
*regions* the coordinator has explicitly shared, over the same TCP
connection, while the module runs. (Design and rationale: the *Remote
memory* section of the top-level README.)

Three parties are involved; here is what each one needs.

### On the node: nothing

The firmware ships the host functions. A node that supports them
advertises feature bit `0x01` in its handshake — visible in the `check`
output:

```
  [ok] HELLO -> HELLO_ACK: v1, max payload 32768 B, features 0x01
```

### In the module: include the header, use the API

Include `wasp/remote.h` (in `tools/include/`) — five functions, all
returning `WASP_REMOTE_OK` (0) or a negative error:

| Function | Does |
| -------- | ---- |
| `wasp_mem_read(ref, dst, len)` | Copy `len` bytes of coordinator memory at `ref` into the module. |
| `wasp_mem_write(ref, src, len)` | Copy `len` bytes from the module into coordinator memory at `ref`. |
| `wasp_lock(region)` / `wasp_unlock(region)` | Hold the region's mutex across several accesses. |
| `wasp_region_size(region)` | Byte length of a shared region. |

A **remote reference** (`ref`) names coordinator memory as a packed
32-bit value: region id in the top 8 bits, byte offset in the low 24
(`WASP_REF(region, offset)` builds one). Because the offset sits in the
low bits, plain arithmetic walks a region — `ref + 4 * i` is element `i`
of an i32 array, exactly like a pointer.

A worked example — multiply an i32 array *in the coordinator's RAM* by
`k`, in place (`scale.c`):

```c
#include "wasp/remote.h"

/* Multiply n i32s in coordinator RAM by k, in place. The whole
 * read-modify-write is one critical section. */
__attribute__((export_name("scale")))
int scale(unsigned ref, int n, int k)
{
	int buf[64];

	if (n < 1 || n > 64)
		return WASP_REMOTE_EBOUNDS;

	int rc = wasp_lock(WASP_REF_REGION(ref));
	if (rc != WASP_REMOTE_OK)
		return rc;              /* ELOCKED: someone else holds it */

	rc = wasp_mem_read(ref, buf, n * sizeof(int));
	if (rc == WASP_REMOTE_OK) {
		for (int i = 0; i < n; i++)
			buf[i] *= k;
		rc = wasp_mem_write(ref, buf, n * sizeof(int));
	}
	wasp_unlock(WASP_REF_REGION(ref));
	return rc;
}
```

Compile exactly as in Step 2, plus the include path:

```sh
clang --target=wasm32 -O2 -nostdlib -Itools/include \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
      -z stack-size=1024 \
      -o scale.wasm scale.c
```

No linker flags for the imports — the `import_module("wasp")`
attributes in the header are enough, and the node resolves them at
instantiation. (Importing anything *outside* the `wasp` namespace still
fails with `LOAD_FAILED`.)

### On the coordinator: register regions, keep listening

The coordinator is the memory host. It decides what is shared — a
region is just a `bytearray` it registers under an id — and it must
keep servicing the node's requests while a call is in flight.
`WaspNode` does that automatically inside `request()`:

```python
import struct, sys; sys.path.insert(0, "tools")
from wasp_client import WaspNode, pack_ref

node = WaspNode("10.0.0.154", 4242)
node.hello()
node.load(open("scale.wasm", "rb").read())

data = bytearray(struct.pack("<8i", *range(8)))  # the memory being shared
node.register_region(1, data)

print(node.call("scale", pack_ref(1), 8, 3))     # -> [0]  (WASP_REMOTE_OK)
print(struct.unpack("<8i", data))                # -> (0, 3, 6, ..., 21)
```

The `bytearray` *is* the shared memory: the module's `wasp_mem_write`
lands in it directly. Register the region before the call — an
unregistered id comes back as `WASP_REMOTE_ENOREGION`. Memory that was
never registered cannot be named at all.

### Locking rules and error codes

Every single `wasp_mem_read`/`wasp_mem_write` is atomic on its own —
the coordinator serializes all access to a region, and you cannot
observe a torn read or write. Take the lock only around **compound**
operations (read-modify-write, multi-field updates), like `scale()`
above. Locks are leases: if a node dies while holding one, the
coordinator revokes it when the lease expires (5 s in the test client).
While someone else holds a region's lock, *everything* — reads, writes,
lock attempts — fails fast with `WASP_REMOTE_ELOCKED` rather than
blocking; retry or back off in the module.

| Code | Meaning / fix |
| ---- | ------------- |
| `WASP_REMOTE_OK` (0) | Success. |
| `WASP_REMOTE_ELOCKED` (-1) | Another node holds the region's lock — retry later. |
| `WASP_REMOTE_EBOUNDS` (-2) | Access reaches outside the region — check offset + length against `wasp_region_size()`. |
| `WASP_REMOTE_ENOREGION` (-3) | No region with that id — the coordinator didn't `register_region()` it. |
| `WASP_REMOTE_EIO` (-4) | The coordinator refused for another reason. |

If the coordinator stops answering entirely (crash, unplugged cable),
the pending host call gives up after 5 s
(`CONFIG_WASP_RPC_TIMEOUT_MS`) and the module **traps** — the call
returns `ERROR(TRAP, wasp: remote memory RPC failed …)` and the node
carries on, same as any other trap.

### Atomic primitives: skip the lock entirely

For the most common compound operation — read-modify-write of a single
i32 — you don't need a lock at all. The coordinator serializes every
request it services, so these two host functions do the whole
read-modify-write inside that window, race-free against every other
node, in **one round-trip** (nodes advertise them as feature bit
`0x02`):

```c
int old;
wasp_add(ref, 1, &old);                  /* atomic *ref += 1; old value out */

unsigned seen;
wasp_cas(ref, expected, desired, &seen); /* write desired iff *ref == expected */
if (seen == expected) { /* won the swap */ }
```

`wasp_cas` covers arbitrary small updates: read the value, compute the
new one, CAS it back, and repeat only if another node really did change
it in between — each attempt costs one round-trip, not a
lock/read/write/unlock cycle. In C++, `remote_ptr<T>` exposes the same
as `p.fetch_add(n)` and `p.compare_exchange(expected, desired)`.

Both primitives respect explicit locks (they fail with
`WASP_REMOTE_ELOCKED` while another node holds the region), so they
compose with lock-based code. Measured on a 6-node swarm hammering one
counter (150 increments): fail-fast locks took 60 s with ~1000
`ELOCKED` retries; coordinator-queued locks took 10 s with zero
retries; `wasp_add` took **1.5 s** — reach for a lock only when an
invariant genuinely spans multiple words or operations.

One performance rule: **every call is a network round-trip** (~ms,
about a million times slower than local memory). Move data in bulk —
one 64-byte read beats sixteen 4-byte reads sixteen-fold. The
remote-memory self-test (`python3 tools/wasp_client.py <node-ip> remote
tools/remote_module.wasm`) exercises all of the above if you want a
reference transcript.

## Transparent remote pointers (phase 2)

The explicit API above works, but pass-by-reference code shouldn't have
to be rewritten around `wasp_mem_read` calls. Phase 2 adds two ways to
just *dereference* coordinator memory. Nothing changes on the node or
the coordinator — both routes compile down to the same phase-1 host
functions.

### C: the `wasp_remote` qualifier + compiler plugin

Declare a pointer `wasp_remote` (an address-space qualifier from
`wasp/remote.h`) and use it like any pointer:

```c
#include "wasp/remote.h"

__attribute__((export_name("normalize")))
int normalize(unsigned region, int count)
{
    wasp_remote int *v = WASP_REMOTE_PTR(int, region, 0);
    int max = 1;

    for (int i = 0; i < count; i++)
        if (v[i] > max)
            max = v[i];              /* remote loads */
    for (int i = 0; i < count; i++)
        v[i] = v[i] * 100 / max;     /* remote load + store */
    return max;
}
```

Indexing, pointer arithmetic, `*p`, struct member access, and
whole-struct assignment (`struct t x = *rp;` — one bulk RPC, not
field-by-field) all work. Two build ingredients (see
`tools/build_test_module.sh` for the exact commands):

```sh
clang --target=wasm32 -O2 -nostdlib -Itools/include \
      -fpass-plugin=tools/wasp-remote-pass/libWaspRemotePass.so \
      -Wl,--no-entry -Wl,--export-dynamic -z stack-size=1024 \
      -o module.wasm module.c tools/lib/wasp_remote_rt.c
```

- the **LLVM pass plugin** (`tools/wasp-remote-pass/`, built
  automatically by `build_test_module.sh`) rewrites every load/store
  through a `wasp_remote` pointer into a remote-memory call, and
- `tools/lib/wasp_remote_rt.c` provides the runtime those calls land
  in.

Anything the plugin cannot lower faithfully — casting a remote pointer
to a local one, remote `memset`, remote-to-remote `memcpy`, atomics —
**fails the compile** with a `wasp-remote:` error; there is no
silent-miscompile mode.

### C++: `remote_ptr<T>`, no plugin at all

C++ operator overloading achieves the same transparency in a plain
header, `wasp/remote.hpp`:

```cpp
#include <wasp/remote.hpp>

extern "C" __attribute__((export_name("locked_inc")))
int locked_inc(unsigned region)
{
    wasp::remote_lock lock(region);      // RAII lease
    if (!lock)
        return WASP_REMOTE_ELOCKED;      // contended: no trap, report

    wasp::remote_ptr<int> p(region, 0);
    *p += 1;                             // load + store through proxies
    return *p;
}
```

`remote_ptr<T>` supports `*p`, `p[i]`, `p->field` (loads the whole
struct once), pointer arithmetic, and bulk `try_read`/`try_write` for
the batched fast path. Compile with `clang++ --target=wasm32
-fno-exceptions -fno-rtti -nostdlib` — no plugin, no runtime file.

### Error semantics: the remote segfault

A transparent dereference has no return code, so a failed access —
out of bounds, unknown region, region locked by another node,
coordinator gone — **traps the module**, exactly like dereferencing a
bad local pointer, and surfaces to the coordinator as `ERROR(TRAP)`.
The node survives; the next call works. Where failure is *expected*
(lock contention, probing), don't dereference blind: use `wasp_locked`
/ `remote_lock` and the explicit `wasp_mem_*` API, which report
`WASP_REMOTE_*` codes instead.

The performance rule doubles in importance here: transparent syntax
makes it easy to write an RPC per array element without noticing. The
loops in `normalize()` above cost `2 × count` round-trips —
per-element access is for small or sparse data, and anything bigger
should stage through a local buffer (`wasp_mem_read` /
`try_read`, one RPC per direction).

Reference modules: `tools/remote_as_module.c` (C, every access
transparent) and `tools/remote_cpp_module.cpp` (C++); self-test:
`python3 tools/wasp_client.py <node-ip> remote2
tools/remote_as_module.wasm tools/remote_cpp_module.wasm`.

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
- No I/O or peripheral host functions yet — the only window to the
  outside world is the remote-memory API (`#include "wasp/remote.h"`,
  compile with `-Itools/include`): read, write, and lock regions of
  coordinator RAM that the coordinator has exported. See
  `tools/remote_module.c` for a worked example and the top-level
  README's *Remote memory* section for the model.
- Function names ≤ 63 bytes; modules ≤ 32 KiB.

The wire format behind all of this is documented in the top-level README
and defined in `app/src/protocol.h`.
