# valkey-wasm

Real [Valkey](https://valkey.io) (the BSD-licensed Redis fork) compiled to
`wasm32-wasip1`, with networking bridged to `node:net` — a drop-in
`redis://127.0.0.1:6379` server that runs anywhere Node runs, no native Redis
and no Docker. The Redis twin of what [PGlite](https://pglite.dev) is for
Postgres.

## Why

Sometimes you want Redis without running Redis: a dev setup with no Docker, a CI
job that shouldn't spin up a service container, tests that want a fresh instance
per run, an offline demo, an in-browser sandbox, or an edge/serverless context
where you can't keep a daemon around.

There are two common ways to get there today, and each fits plenty of cases
well. A mock like `ioredis-mock` is wonderfully quick to drop into a unit test.
Native Redis or a container gives you the genuine article when you can afford a
daemon and a port. `valkey-wasm` aims at the middle ground between them: the real
engine, in-process, with nothing external to manage.

Because it *is* Valkey — the same C, compiled to WebAssembly — you don't have to
reason about which features are supported. Lua scripting with `cjson`/`cmsgpack`,
`EVALSHA` script caching, blocking commands (`BLPOP`/`BZPOPMIN`), stream consumer
groups, pub/sub, transactions: they behave as they do on a server, because they
*are* the server. Any Redis client (`ioredis`, `node-redis`) connects unmodified
over a real loopback TCP socket, so state is shared across every process that
dials the port — which is what lets multi-process tools like **BullMQ** and
Sidekiq-style workers work the same as they would against a standalone instance.

It also stays low-maintenance: the Redis wire protocol and Lua ABI have been
stable for years, so one port keeps working across client and framework
versions. Valkey 9.1.1 reports `redis_version:7.2.4`, so version-gating clients
(e.g. BullMQ, which needs ≥ 6.2) see a stock 7.2 server — and every Valkey line
from 8.0 through 9.1 reports that same field, so the port can follow the latest
upstream release with no client-visible difference.

Think of it as [PGlite](https://pglite.dev) for Redis.

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
