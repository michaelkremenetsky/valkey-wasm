# Build status

## Milestones
1. **Compiles** — every needed TU builds to `.o` for `wasm32-wasip1`
2. **Links** — reactor `build/valkey.wasm` links (host imports undefined-allowed)
3. **`redis-cli ping`** — `test/ping.mjs` green (server + bridge + Lua libs)
4. **BullMQ** — `test/bullmq.mjs` green (EVALSHA/BZPOPMIN/streams/cmsgpack)

## Done (foundation)
- Vendored pristine Valkey 8.0.1 (first commit unmodified). Reports
  `redis_version:7.2.4` → BullMQ's `≥6.2` gate passes.
- Confirmed the bundled Lua ships `lua_cmsgpack.c` + `lua_cjson.c` +
  `lua_struct.c` — the exact libs ioredis-mock lacks.
- Architecture: Valkey keeps its whole event loop / client / blocking / pubsub /
  scripting engine; only I/O is bridged.
  - `src/wasi/ae_wasi.c` — ae backend over `host_poll` (never blocks; reactor
    inversion). Wired into `ae.c` via `HAVE_WASI_AE`.
  - `src/wasi/sock_shim.c` — libc socket family over host imports, `--wrap`ing
    read/write/close/fcntl; host fds from 4096 (small, fits ae `events[fd]`).
  - `src/wasi/reactor.c` — `rk_boot(port)` / `rk_step()` exports; `main()`
    patched (server.c, `VALKEY_WASM`) to skip `aeMain`.
  - `src/wasi/stubs.c` — fork/rlimit/backtrace/getrusage seeds.
  - `bridge/valkey-server.mjs` — `node:net` host bridge + `ValkeyServer` API.
  - `scripts/build.sh` — wasi-sdk reactor build.

## Remaining grind (the compile→link loop)
Run `scripts/build.sh`, read `build/compile.err` + `build/link.err`, iterate:

1. **Compile errors** per TU — mostly missing platform macros; add to
   `stubs.c` or guard in the compat header. Expect `dict.c`, `zmalloc.c`,
   `server.c`, `syscheck.c`, `debug.c`, `latency.c` to need attention.
2. **Threads** — `bio.c` + `io_threads.c` use pthreads (absent on plain
   wasip1). Preferred: exclude both TUs from `build.sh` and implement the bio
   API synchronously in `stubs.c` (jobs run inline). See the note at the bottom
   of `stubs.c`. Alternative: retarget `wasm32-wasip1-threads` (adds worker +
   shared-memory setup on the JS side — heavier).
3. **`--wrap` flags** — add `-Wl,--wrap=read,--wrap=write,--wrap=close,--wrap=fcntl`
   (and `recv`/`send` if anet uses them) to `LDFLAGS` so sock_shim intercepts.
4. **Link** — resolve remaining undefined symbols into `stubs.c`. Keep the
   `host_*` imports undefined (that's expected; the bridge supplies them).
5. **Bring-up** — `node test/ping.mjs`. Likely first issues: reactor step
   cadence, `poll` readiness for the listener fd, RESP framing across chunks.
6. **BullMQ** — `cd test && npm i bullmq && node bullmq.mjs`.

## Notes
- Kernel is untouched and uninvolved — this runs under stock Node `WASI` +
  the `host` import object, so it works in CI, host Node, and (later) the
  strapkit guest node identically.
- Publishable standalone (like PGlite): `valkey-wasm` (the module) + a
  `valkey-socket`-style bridge. strapkit would consume it the way it consumes
  `@electric-sql/pglite`.
