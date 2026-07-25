// node:net host bridge for valkey.wasm. Implements the "host" import surface
// (see src/wasi/host.h) — sockets + the event-loop poll — and drives the
// reactor. Public API mirrors pglite-socket's shape:
//
//   import { ValkeyServer } from './valkey-server.mjs';
//   const srv = new ValkeyServer({ port: 6379 });
//   await srv.start();            // now redis://127.0.0.1:6379 is live
//   await srv.stop();
//
// One wasm instance = one Valkey server. Networking is real TCP via node:net,
// so any Redis client (ioredis, node-redis, BullMQ) connects unmodified, and
// state is shared across every process that dials the port.
import net from 'node:net';
import fs from 'node:fs';
import { WASI } from 'node:wasi';
import { fileURLToPath } from 'node:url';

const HOST_FD_BASE = 4096;              // must match src/wasi/host.h
const AE_READABLE = 1, AE_WRITABLE = 2; // must match src/src/ae.h
const WASM_PATH = fileURLToPath(new URL('../build/valkey.wasm', import.meta.url));

export class ValkeyServer {
  constructor({ port = 6379, host = '127.0.0.1', wasmPath = WASM_PATH } = {}) {
    this.port = port;
    this.host = host;
    this.wasmPath = wasmPath;
    this._nextFd = HOST_FD_BASE;
    this._socks = new Map();     // fd -> { socket, chunks:[Buffer], off, eof, backpressured }
    this._listeners = new Map(); // fd -> { server, pending:[fd] }
    this._stepScheduled = false;
    this._closed = false;
  }

  _allocFd() { return this._nextFd++; }
  _dbg(...a) { if (process.env.VALKEY_DEBUG) console.error('[vk]', ...a); }

  _mem() { return Buffer.from(this.instance.exports.memory.buffer); }

  // Coalesce many events into one synchronous reactor pass on the next tick.
  _scheduleStep() {
    if (this._stepScheduled || this._closed) return;
    this._stepScheduled = true;
    queueMicrotask(() => { this._stepScheduled = false; this._step(); });
  }
  _step() {
    if (this._closed) return;
    try { this.instance.exports.rk_step(); }
    catch (e) { console.error('[valkey-wasm] rk_step threw:', e); }
  }

  _hostImports() {
    return {
      socket: () => this._allocFd(),

      listen: (fd, port) => {
        const rec = { server: null, pending: [] };
        const server = net.createServer((socket) => {
          const cfd = this._allocFd();
          const s = { socket, chunks: [], off: 0, eof: false, backpressured: false };
          socket.on('data', (buf) => { this._dbg('data', cfd, buf.length, 'bytes'); s.chunks.push(buf); this._scheduleStep(); });
          socket.on('end', () => { s.eof = true; this._scheduleStep(); });
          socket.on('close', () => { s.eof = true; this._scheduleStep(); });
          socket.on('error', () => { s.eof = true; });
          socket.on('drain', () => { s.backpressured = false; this._scheduleStep(); });
          this._socks.set(cfd, s);
          rec.pending.push(cfd);
          this._scheduleStep();
        });
        server.on('error', (e) => console.error('[valkey-wasm] listen error:', e.message));
        server.listen(port || this.port, this.host);
        rec.server = server;
        this._listeners.set(fd, rec);
        return 0;
      },

      accept: (lfd) => {
        const rec = this._listeners.get(lfd);
        if (!rec || rec.pending.length === 0) return -1;
        const cfd = rec.pending.shift();
        this._dbg('accept', lfd, '->', cfd);
        return cfd;
      },

      read: (fd, ptr, len) => {
        const s = this._socks.get(fd);
        if (!s) { this._dbg('read', fd, 'no-sock'); return -1; }
        if (s.chunks.length === 0) return s.eof ? 0 : -1;
        const mem = this._mem();
        let written = 0;
        while (written < len && s.chunks.length) {
          const head = s.chunks[0];
          const avail = head.length - s.off;
          const n = Math.min(avail, len - written);
          head.copy(mem, ptr + written, s.off, s.off + n);
          written += n; s.off += n;
          if (s.off >= head.length) { s.chunks.shift(); s.off = 0; }
        }
        this._dbg('read', fd, written, 'bytes');
        return written;
      },

      write: (fd, ptr, len) => {
        const s = this._socks.get(fd);
        if (!s || s.eof) return -1;
        const mem = this._mem();
        // copy out: the memory may be detached/reused after this returns
        const out = Buffer.allocUnsafe(len);
        mem.copy(out, 0, ptr, ptr + len);
        const ok = s.socket.write(out);
        if (!ok) s.backpressured = true;
        this._dbg('write', fd, len, 'bytes');
        return len;
      },

      close: (fd) => {
        const s = this._socks.get(fd);
        if (s) { try { s.socket.destroy(); } catch {} this._socks.delete(fd); }
        const l = this._listeners.get(fd);
        if (l) { try { l.server.close(); } catch {} this._listeners.delete(fd); }
      },

      poll: (interestPtr, count, firedPtr, max, _timeoutMs) => {
        const mem = this._mem();
        let fired = 0;
        for (let i = 0; i < count && fired < max; i++) {
          const fd = mem.readInt32LE(interestPtr + i * 8);
          const mask = mem.readInt32LE(interestPtr + i * 8 + 4);
          let ready = 0;
          const lis = this._listeners.get(fd);
          if (lis) {
            if (mask & AE_READABLE && lis.pending.length) ready |= AE_READABLE;
          } else {
            const s = this._socks.get(fd);
            if (s) {
              if (mask & AE_READABLE && (s.chunks.length || s.eof)) ready |= AE_READABLE;
              if (mask & AE_WRITABLE && !s.backpressured) ready |= AE_WRITABLE;
            }
          }
          if (ready) {
            mem.writeInt32LE(fd, firedPtr + fired * 8);
            mem.writeInt32LE(ready, firedPtr + fired * 8 + 4);
            fired++;
          }
        }
        return fired;
      },
    };
  }

  async start() {
    const bytes = fs.readFileSync(this.wasmPath);
    const wasi = new WASI({ version: 'preview1', args: ['valkey-server'], env: {}, preopens: {} });
    const module = await WebAssembly.compile(bytes);
    const imports = {
      wasi_snapshot_preview1: wasi.wasiImport,
      host: this._hostImports(),
      // Lua is built with wasm setjmp/longjmp (-wasm-enable-sjlj); its longjmp
      // throws the `__c_longjmp` exception tag, which — as in Emscripten — is an
      // imported tag the host provides. It carries one i32 (a pointer to the
      // longjmp args in wasm linear memory).
      env: { __c_longjmp: new WebAssembly.Tag({ parameters: ['i32'] }) },
    };
    this.instance = await WebAssembly.instantiate(module, imports);
    wasi.initialize(this.instance);              // reactor: runs _initialize (ctors)
    this.instance.exports.rk_boot(this.port);    // full Valkey init, returns (no aeMain)

    // Periodic pass for serverCron / client + blocked-client timeouts.
    const hzMs = (() => { try { return this.instance.exports.rk_next_timeout_ms(); } catch { return 100; } })();
    this._timer = setInterval(() => this._step(), Math.max(20, hzMs || 100));
    return this;
  }

  async stop() {
    this._closed = true;
    clearInterval(this._timer);
    for (const l of this._listeners.values()) { try { l.server.close(); } catch {} }
    for (const s of this._socks.values()) { try { s.socket.destroy(); } catch {} }
    this._listeners.clear(); this._socks.clear();
  }
}
