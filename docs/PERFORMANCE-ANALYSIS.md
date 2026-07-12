# Performance Analysis and Modernization Roadmap

*Analysis of commit `b01d5f1`, July 2026. Codebase reviewed: `lib/`, `fuse/`, `cmdline/`.*

afpfs-ng is dramatically slower than the native macOS AFP client (the project's own
`docs/performance` file records untarring the Linux kernel taking 24 seconds natively
vs. 4.5 minutes over afpfs-ng). This document explains why, records correctness bugs
found during the review, and lays out a fix order and a testing strategy.

The root cause is not a single bug: the architecture pays **one or more full network
round trips for every small operation, with exactly one request in flight at a time**.
The native client pipelines many requests, caches metadata aggressively, and writes in
large chunks. afpfs-ng does none of these.

Reference documentation: Apple's AFP Programming Guide and AFP 3.x protocol reference
(<https://developer.apple.com/library/archive/documentation/Networking/Conceptual/AFP/Introduction/Introduction.html>),
including the DSI chapter for session/quantum negotiation.

## Bottlenecks, ranked by impact

### 1. FUSE is forced single-threaded

`fuse/commands.c:226` unconditionally passes `-s` to `fuse_main()` (a commented-out
`#ifdef USE_SINGLE_THREAD` guard was left hardcoded). Every filesystem operation from
every process is serialized through one thread. Combined with (2), the entire mount can
never have more than one AFP request outstanding, so throughput is bounded by
`request_size / round_trip_time`.

### 2. Strictly synchronous request/response protocol core

`dsi_send()` (`lib/dsi.c`) sends a request and blocks on a condition variable until the
reply arrives. There is no pipelining, no readahead, and no write-behind anywhere in
the stack. The request-queue machinery in `dsi.c` could in principle track multiple
in-flight requests, but nothing exploits it.

### 3. Writes arrive from the kernel in 4 KB pieces

`fuse/fuse_int.c` builds against `FUSE_USE_VERSION 25` and no `-o big_writes` /
`max_write` option is passed, so on FUSE 2.x the kernel hands `fuse_write` 4 KB at a
time. Each 4 KB costs one full network round trip, plus a malloc and a full payload
memcpy per request (`afp_writeext`, `lib/proto_files.c`). At 1 ms LAN RTT this is a
~4 MB/s hard ceiling; on Wi-Fi far worse. This alone explains the untar numbers. The
TODO items "queue writes to be one tx quantum" and "large block writes for FUSE" refer
to exactly this.

### 4. Reads: one 128 KB quantum per round trip, no readahead

`ll_read` (`lib/lowlevel.c`) issues a single `afpRead(Ext)` of at most `rx_quantum`;
`fuse_read` loops around it. `rx_quantum` is hardcoded to 128 KB in `lib/afp.c`
("For now, we'll just set a default" — the latency-based sizing code is stubbed out).
Sequential read throughput is therefore `128KB / RTT` and degrades linearly with
latency.

### 5. Byte-range lock/unlock round trips around every read and write

`ll_read`/`ll_write` (`lib/lowlevel.c`) bracket every data operation with
`afp_byterangelock(ext)` lock + unlock — up to 3x the round trips per operation.
Mitigating fact: both the FUSE client (`fuse/client.c`, `DEFAULT_MOUNT_FLAGS`) and
`afpcmd` set `VOLUME_EXTRA_FLAGS_NO_LOCKING`, so the default paths skip this — but any
library embedder that doesn't set the flag pays the full penalty.

### 6. Metadata operations are round-trip storms with no caching

- `ll_readdir` (`lib/lowlevel.c`) enumerates directories **20 entries per round trip**
  (`reqcount=20`); servers happily return far more per call.
- `fuse_readdir` fills entries with a NULL stat, so `ls -l` then triggers a separate
  `getattr` → `afp_getfiledirparms` round trip **per file**.
- No FUSE `attr_timeout`/`entry_timeout` tuning and no library-side attribute cache.

### 7. Per-packet socket overhead

- The event loop (`lib/loop.c`) does a `pselect()` pass per `read()`, and `dsi_recv()`
  reads the 16-byte DSI header and body in separate syscalls — several wakeups per
  reply (TODO: "don't go back through the select loop").
- `TCP_NODELAY` is never set (no `setsockopt` anywhere in the tree); Nagle/delayed-ACK
  interaction can stall multi-segment requests.
- The non-read receive buffer is a fixed 4 KB (`lib/afp.c`), forcing extra loop
  iterations and memcpy shuffling for larger control replies.

## Correctness bugs found during review

1. **`ll_write` silently discards write errors** (`lib/lowlevel.c`, write loop):
   `ret=afp_writeext(...);` is immediately followed by `ret=0;`, clobbering the return
   code before the error switch. A failed write reports success — data-loss grade.
2. **Latent heap overflow in `dsi_recv`** (`lib/dsi.c`): the incoming buffer is 4096
   bytes, but the code computes `amount_to_read = min(header->length, server->bufsize)`
   and reads to `incoming_buffer + data_read` (16 after the header). A control reply
   with payload ≥ 4081 bytes (a large enumerate reply can get there) writes up to 16
   bytes past the allocation.
3. **Request timeout handling is a stub** (`lib/dsi.c`, `/* FIXME */` after
   `pthread_cond_timedwait`): after the 5 s `DSI_DEFAULT_TIMEOUT` the request is torn
   down while the reply may still arrive later ("Got an unknown reply" path).
4. **`afp_closefork` mislabels its request** (`lib/proto_fork.c`): passes
   `afpFlushFork` as the subcommand tag instead of `afpCloseFork` — copy-paste bug,
   currently benign because the reply handling coincides.

## Suggested fix order (highest win per effort first)

1. **Big writes**: pass `-o big_writes,max_write=131072`, or port to FUSE 3 (which
   defaults to 128 KB writes and adds writeback caching). Likely 10–30x on writes.
2. **Drop `-s`** after a thread-safety audit of the reply path (the TODO's "do DSI
   buffers get trampled?" question must be answered first; the shared
   `incoming_buffer` is the main suspect).
3. **Write-behind coalescing** to a full `tx_quantum` and **readahead pipelining** in
   the library. Architecturally the big one.
4. **Readdir**: larger `reqcount`, cache enumerate results to absorb the getattr
   storm, set FUSE attr/entry timeouts.
5. **Small fixes**: `TCP_NODELAY`; grow/bound-check the 4 KB incoming buffer (also
   fixes the overflow); fix the `ll_write` error-swallowing bug.
6. **Build the test/benchmark harness first** (below) so every step is measured.

## Testing and benchmarking strategy

A full AFP server emulator is not needed to get verified numbers:

1. **Integration tests against a real server**: run netatalk 3.x in Docker (still
   speaks AFP 3.3) in CI on Linux runners (FUSE works there). The existing
   `test/Makefile` mount/read-back checks can grow into a scripted suite.
2. **A small mock DSI/AFP server** (a few hundred lines of e.g. Python): speaks DSI
   (OpenSession, Command, Write, Tickle) with canned AFP replies. Two things a real
   server can't provide:
   - **Fault injection** — short reads, split packets, delayed/dropped replies;
     directly exercises `dsi_recv` reassembly and the overflow above.
   - **Round-trip counting** — performance tests become deterministic assertions
     ("writing 1 MB must take ≤ 5 AFP requests") instead of flaky wall-clock timing.
3. **Latency-scaled benchmarks**: `tc netem` (or a delay knob in the mock server) at
   0.2 / 2 / 20 ms RTT; measure large-file get/put and an untar-like small-file
   workload through both `afpcmd` and the FUSE mount.

## Context: why this matters now

Apple has deprecated the AFP client in recent macOS releases and announced its
removal, so a maintained open client is useful for people with Time Capsules and older
AFP-only NAS devices. On modern macOS a FUSE mount requires macFUSE (kext) or fuse-t
(userspace, NFS-bridge based); the library and `afpcmd` need neither.
