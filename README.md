# valkey-wasm

Real [Valkey](https://valkey.io) (the BSD-licensed Redis fork) compiled to
`wasm32-wasip1`, with networking bridged to `node:net` — a drop-in
`redis://127.0.0.1:6379` server that runs anywhere Node runs, no native Redis
and no Docker. The Redis twin of what [PGlite](https://pglite.dev) is for
Postgres.

## Why

Mocks (`ioredis-mock`) and reimplementations cover the easy 80% of Redis and
fall over on the exact things production apps need: **BullMQ**'s `EVALSHA`
script cache, `BZPOPMIN` blocking pops, stream consumer groups (`XREADGROUP`),
and the Lua `cmsgpack`/`cjson` libraries its scripts serialize with. They also
only ever hold state inside one JS process, so a spawned worker process sees an
empty store.

Porting the real engine solves all of it at once and permanently: the RESP
protocol + Lua 5.1 + `cmsgpack` are a frozen target, so one port serves every
Redis client library and every version of BullMQ, unmodified, across processes.

Valkey 9.1.1 reports `redis_version:7.2.4`, so version-gating clients
(BullMQ requires ≥ 6.2) see a stock 7.2 server. (Every Valkey line from 8.0
through 9.1 reports the same 7.2.4 compat field, so the port tracks the latest
release with no client-visible difference.)

## Architecture

Valkey keeps its **entire** event loop, client machinery, blocking-command
engine, pub/sub, and scripting intact — only the two layers that touch real I/O
are swapped:

- **`ae_wasi.c`** — an `ae.c` poll backend that calls an imported `host_poll`
  instead of `epoll`/`select` (whose WASI implementations can't see our
  JS-backed socket fds).
- **`conn_wasi`** — the socket `connection` type + `anet` calls rerouted to
  imported `host_socket/bind/listen/accept/read/write/close`.

Those imports are implemented in JS over `node:net` (`bridge/`). No `fork`
(persistence off: `save "" / appendonly no`), no threads (`bio` jobs inlined,
`io-threads 1`). The module is a **WASI reactor**: it stays resident, JS drives
its event loop via exported `step()`/timer entry points.

```
 TCP client ──▶ net.Server (JS) ──▶ host_* imports ──▶ Valkey wasm core
   (ioredis,        bridge/            (fd table)         (RESP + Lua +
    BullMQ)      valkey-server.mjs                         data structures)
```

## Layout

- `src/`            — vendored pristine Valkey 9.1.1 (first commit is unmodified)
- `src/wasi/`       — the WASI compat layer (ae backend, conn shim, stubs, reactor entry)
- `scripts/build.sh`— wasi-sdk build → `build/valkey.wasm`
- `bridge/`         — the `node:net` host bridge + public JS API
- `test/`           — `ping` smoke test + the BullMQ Queue→Worker→completed acceptance test

## Status

See `BUILD-STATUS.md`. Milestone order: (1) compiles, (2) links as a reactor,
(3) `redis-cli ping` over the bridge, (4) BullMQ acceptance test green.
