# valkey-wasm

[Valkey](https://valkey.io) (the BSD-licensed fork of Redis) compiled to
WebAssembly, with its networking wired through Node's `net` module. You get a
`redis://127.0.0.1:6379` server running inside your Node process, with no native
Redis install and no Docker.

If you've used [PGlite](https://pglite.dev), it's the same idea for Redis.

## Why

Sometimes you need Redis for something that shouldn't drag a running Redis into
it. Local development where you'd rather not run Docker. A CI job that shouldn't
have to stand up a service container. A test suite that wants a clean database
on every run. An offline demo, or somewhere you just can't keep a daemon alive.

The usual way to cover that is either a mock or a real server, and each one has
a catch.

A mock like `ioredis-mock` is easy to reach for, but it reimplements Redis in
JavaScript. That works for the common commands and falls short on the rest: Lua
scripting, `EVALSHA`, blocking commands like `BLPOP`, and streams. It also lives
entirely inside one process, so the moment you fork a worker, that worker is
talking to an empty database.

A real Redis, or a container running one, does all of it correctly. But then
you're back to running and managing a daemon and a port, which is usually the
thing you were trying to get out of.

valkey-wasm takes a different route. It's the actual Valkey source compiled to
WebAssembly, running inside your Node process. Since it's the real engine,
there's nothing to keep a checklist of. Scripting, streams, blocking commands,
pub/sub, and transactions behave the way they do on any Redis server.

## Install

```
npm install valkey-wasm
```

Then start a server and point any Redis client at it. See
[docs/usage.md](docs/usage.md) for an example and the API.

## How it works

Valkey keeps its whole event loop, client handling, blocking commands, pub/sub,
and scripting. The only things swapped out are the two spots that touch the
network:

- `ae_wasi.c` replaces the event-loop poll (`epoll`/`select`) with a call into
  the host, because a wasm build can't see the sockets that live on the JS side.
- The socket layer (`read`/`write`/`accept` and friends) is routed to host
  functions instead of real syscalls.

Those host functions live in the JS bridge and are backed by `node:net`. There's
no `fork` (persistence is off) and no threads (background jobs run inline,
io-threads set to 1). The wasm module is a reactor: it boots once and stays
resident, and the bridge steps its event loop forward on each socket event and a
timer.

```
 TCP client ──▶ net.Server (JS) ──▶ host_* imports ──▶ Valkey wasm core
   (ioredis,        bridge/            (fd table)         (RESP + Lua +
    BullMQ)      valkey-server.ts                         data structures)
```
