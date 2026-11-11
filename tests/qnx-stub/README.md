# QNX stub headers

These headers let the **PC** build compile the QNX glue of the drivers
(`drivers/**/*_qnx.c`) without a QNX SDP installation. They are used by the
`pgw_qnx_syntax` CMake targets, which build an *object* library: nothing is
linked, so the PC build can never fail because of a missing QNX symbol.

## What this is, and what it is not

**It is** a set of declarations - types, prototypes and a handful of
constants - faithful enough that a typo, a wrong prototype, a wrong argument
type or a wrong structure field is caught by the PC compiler.

**It is not** a simulation. Nothing here emulates QNX: the stub functions have
no body, no message is ever dispatched, no interrupt is ever delivered, no
filesystem is ever mounted. A test that passes against these headers proves
only that the file *compiles*.

Concretely, the following are **not** verified by the stub build:

- the behaviour of the resource manager (a real QNX target is needed);
- the numeric values of the message numbers and of the `IOFUNC_*` / `_MOUNT_*`
  bits, which the drivers only pass along (they are checked on target by
  `drivers/tests/qnx_abi_assert.c`, and documented in `docs/qnx-abi.md`);
- the ability checks of the kernel: `procmgr_ability()` always succeeds here,
  so the privilege drop is *not* really exercised on the PC.

## Rule: substitute for building on the PC only

Every stub header starts with the same sentence:

> substitute for building on the PC only; the real QNX header is used on
> target

It is there so that a reader who finds one of these files in an include path
knows immediately that it is a build-time stand-in, not something to ship.

## Where the values come from

Two kinds of values live here, and they are treated differently:

1. **Values the project depends on numerically** are the ones the drivers
   compare or index with. They are documented in `docs/qnx-abi.md` with their
   source (a QNX 8.0 documentation page), and they are checked on the target
   by `drivers/tests/qnx_abi_assert.c` with `_Static_assert`. The errno
   constants are *not* in the stubs at all: the drivers include the real
   `<errno.h>` of the build, which is the right thing on both the PC and the
   target.
2. **Values the project only passes along** (message numbers, `IOFUNC_*`
   bits, `PROCMGR_AID_*` numbers) are declared symbolically here, with a
   comment saying so. Their exact value is irrelevant to the PC build.

## The headers

| Stub | Real QNX 8.0 header | What it declares |
|---|---|---|
| `sys/neutrino.h` | `<sys/neutrino.h>` | pulses, channel and message calls, sync mutexes, threads, clocks, the interrupt interface, `TraceEvent()` |
| `sys/iomsg.h` | `<sys/iomsg.h>` | the connect message and its subtypes, the I/O messages, the extended types, the file types |
| `sys/resmgr.h` | `<sys/resmgr.h>` | `resmgr_context_t`, the connect and io handler tables, `resmgr_attach()` / `resmgr_detach()` |
| `sys/iofunc.h` | `<sys/iofunc.h>` | the POSIX-layer structures (`iofunc_attr_t`, `iofunc_ocb_t`), the mount configuration, the `iofunc_*()` entry points |
| `sys/dispatch.h` | `<sys/dispatch.h>` | the dispatch channel, the thread pool |
| `sys/trace.h` | `<sys/trace.h>` | the user trace events |
| `sys/slog2.h` | `<sys/slog2.h>` | the slog2 buffer set and the record call |
| `sys/procmgr.h` | `<sys/procmgr.h>` | `procmgr_daemon()`, `procmgr_ability()` and the ability names |
| `sys/mount.h` | `<sys/mount.h>` | the `_MOUNT_*` flags |
| `sys/ftype.h` | `<sys/ftype.h>` | the `_FTYPE_*` file types |
| `sys/dcmd_blk.h` | `<sys/dcmd_blk.h>` | the filesystem devctl commands |
| `pci/pci.h` | `<pci/pci.h>` | the PCI server client API |
| `pci/cap_msix.h` | `<pci/cap_msix.h>` | the MSI-X capability plug-in |
| `hw/pci.h` | (QNX 7.0 spelling) | forwards to `<pci/pci.h>`: QNX 8.0 replaced the bus layer with the PCI server client library |

## What is deliberately *not* stubbed

`<errno.h>`, `<fcntl.h>`, `<sys/stat.h>`, `<string.h>`, `<unistd.h>` and the
other POSIX headers are **not** stubbed: the host's own are good enough, and
using them is what makes the PC build meaningful (the drivers translate
between the wire's Linux numbers and the host's numbers through
`common/src/err.c`, exactly as they do on a target).

## How to keep the stubs honest

When a driver starts using a new QNX symbol:

1. add it to the matching stub header, with the same shape as the real one;
2. note in the header comment where the real declaration lives;
3. if the value matters numerically, add it to `docs/qnx-abi.md` **and** to
   `drivers/tests/qnx_abi_assert.c`;
4. run the build: `cmake -B build-drivers "-DPGW_MODULES=common;drivers" &&
   cmake --build build-drivers -j4`.

The PC build then stays green, and the target build is the one that proves the
value is right.
