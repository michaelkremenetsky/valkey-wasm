// node:net host bridge for valkey.wasm. Implements the "host" import surface
// (see src/wasi/host.h) — sockets + the event-loop poll — and drives the
// reactor. Public API mirrors pglite-socket's shape:
//
//   import { ValkeyServer } from 'valkey-wasm';
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

// WebAssembly.Tag (the exception-handling proposal, used here for Lua's
// setjmp/longjmp bridge) isn't in TypeScript's bundled lib types yet; declare it.
declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace WebAssembly {
    class Tag {
      constructor(type: { parameters: string[] });
    }
  }
}

const HOST_FD_BASE = 4096; // must match src/wasi/host.h
const AE_READABLE = 1; // must match src/src/ae.h
const AE_WRITABLE = 2;
const DEFAULT_WASM_PATH = fileURLToPath(new URL('../build/valkey.wasm', import.meta.url));

export interface ValkeyServerOptions {
  /** TCP port to listen on. Default 6379. */
  port?: number;
  /** Host/interface to bind. Default '127.0.0.1'. */
  host?: string;
  /** Override the path to valkey.wasm. Defaults to the bundled build. */
  wasmPath?: string;
}

/** The reactor entry points exported by valkey.wasm (see src/wasi/reactor.c). */
interface ValkeyExports extends WebAssembly.Exports {
  memory: WebAssembly.Memory;
  rk_boot(port: number): number;
  rk_step(): number;
  rk_next_timeout_ms(): number;
}

/** Per-connection state: buffered inbound chunks + liveness/backpressure flags. */
interface SockRecord {
  socket: net.Socket;
  chunks: Buffer[];
  off: number; // read offset into chunks[0]
  eof: boolean;
  backpressured: boolean;
}

/** Per-listener state: the net.Server + a queue of accepted-but-not-yet-accept()ed fds. */
interface ListenerRecord {
  server: net.Server;
  pending: number[];
}

/** The set of functions imported by the wasm module under the "host" namespace. */
interface HostImports {
  socket(): number;
  listen(fd: number, port: number): number;
  accept(lfd: number): number;
  read(fd: number, ptr: number, len: number): number;
  write(fd: number, ptr: number, len: number): number;
  close(fd: number): void;
  poll(interestPtr: number, count: number, firedPtr: number, max: number, timeoutMs: number): number;
}

export class ValkeyServer {
  readonly port: number;
  readonly host: string;
  readonly wasmPath: string;

  private instance!: WebAssembly.Instance & { exports: ValkeyExports };
  private nextFd = HOST_FD_BASE;
  private socks = new Map<number, SockRecord>();
  private listeners = new Map<number, ListenerRecord>();
  private stepScheduled = false;
  private closed = false;
  private timer?: ReturnType<typeof setInterval>;

  constructor(options: ValkeyServerOptions = {}) {
    this.port = options.port ?? 6379;
    this.host = options.host ?? '127.0.0.1';
    this.wasmPath = options.wasmPath ?? DEFAULT_WASM_PATH;
  }

  private allocFd(): number {
    return this.nextFd++;
  }

  private dbg(...a: unknown[]): void {
    if (process.env.VALKEY_DEBUG) console.error('[vk]', ...a);
  }

  private mem(): Buffer {
    // memory.buffer can be detached/resized by wasm growth, so re-wrap each time.
    return Buffer.from(this.instance.exports.memory.buffer);
  }

  // Coalesce many events into one synchronous reactor pass on the next tick.
  private scheduleStep(): void {
    if (this.stepScheduled || this.closed) return;
    this.stepScheduled = true;
    queueMicrotask(() => {
      this.stepScheduled = false;
      this.step();
    });
  }

  private step(): void {
    if (this.closed) return;
    try {
      this.instance.exports.rk_step();
    } catch (e) {
      console.error('[valkey-wasm] rk_step threw:', e);
    }
  }

  private hostImports(): HostImports {
    return {
      socket: () => this.allocFd(),

      listen: (fd: number, port: number): number => {
        const rec: ListenerRecord = { server: null as unknown as net.Server, pending: [] };
        const server = net.createServer((socket: net.Socket) => {
          const cfd = this.allocFd();
          const s: SockRecord = { socket, chunks: [], off: 0, eof: false, backpressured: false };
          socket.on('data', (buf: Buffer) => {
            this.dbg('data', cfd, buf.length, 'bytes');
            s.chunks.push(buf);
            this.scheduleStep();
          });
          socket.on('end', () => { s.eof = true; this.scheduleStep(); });
          socket.on('close', () => { s.eof = true; this.scheduleStep(); });
          socket.on('error', () => { s.eof = true; });
          socket.on('drain', () => { s.backpressured = false; this.scheduleStep(); });
          this.socks.set(cfd, s);
          rec.pending.push(cfd);
          this.scheduleStep();
        });
        server.on('error', (e: Error) => console.error('[valkey-wasm] listen error:', e.message));
        server.listen(port || this.port, this.host);
        rec.server = server;
        this.listeners.set(fd, rec);
        return 0;
      },

      accept: (lfd: number): number => {
        const rec = this.listeners.get(lfd);
        if (!rec || rec.pending.length === 0) return -1;
        const cfd = rec.pending.shift()!;
        this.dbg('accept', lfd, '->', cfd);
        return cfd;
      },

      read: (fd: number, ptr: number, len: number): number => {
        const s = this.socks.get(fd);
        if (!s) { this.dbg('read', fd, 'no-sock'); return -1; }
        if (s.chunks.length === 0) return s.eof ? 0 : -1;
        const mem = this.mem();
        let written = 0;
        while (written < len && s.chunks.length) {
          const head = s.chunks[0];
          const avail = head.length - s.off;
          const n = Math.min(avail, len - written);
          head.copy(mem, ptr + written, s.off, s.off + n);
          written += n;
          s.off += n;
          if (s.off >= head.length) { s.chunks.shift(); s.off = 0; }
        }
        this.dbg('read', fd, written, 'bytes');
        return written;
      },

      write: (fd: number, ptr: number, len: number): number => {
        const s = this.socks.get(fd);
        if (!s || s.eof) return -1;
        const mem = this.mem();
        // copy out: the memory may be detached/reused after this returns
        const out = Buffer.allocUnsafe(len);
        mem.copy(out, 0, ptr, ptr + len);
        const ok = s.socket.write(out);
        if (!ok) s.backpressured = true;
        this.dbg('write', fd, len, 'bytes');
        return len;
      },

      close: (fd: number): void => {
        const s = this.socks.get(fd);
        if (s) { try { s.socket.destroy(); } catch { /* already gone */ } this.socks.delete(fd); }
        const l = this.listeners.get(fd);
        if (l) { try { l.server.close(); } catch { /* already gone */ } this.listeners.delete(fd); }
      },

      poll: (interestPtr: number, count: number, firedPtr: number, max: number, _timeoutMs: number): number => {
        const mem = this.mem();
        let fired = 0;
        for (let i = 0; i < count && fired < max; i++) {
          const fd = mem.readInt32LE(interestPtr + i * 8);
          const mask = mem.readInt32LE(interestPtr + i * 8 + 4);
          let ready = 0;
          const lis = this.listeners.get(fd);
          if (lis) {
            if (mask & AE_READABLE && lis.pending.length) ready |= AE_READABLE;
          } else {
            const s = this.socks.get(fd);
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

  /** Boot the wasm instance and start listening. Resolves once the server is live. */
  async start(): Promise<this> {
    const bytes = fs.readFileSync(this.wasmPath);
    const wasi = new WASI({ version: 'preview1', args: ['valkey-server'], env: {}, preopens: {} });
    const module = await WebAssembly.compile(bytes);
    const imports: WebAssembly.Imports = {
      wasi_snapshot_preview1: wasi.wasiImport as unknown as WebAssembly.ModuleImports,
      host: this.hostImports() as unknown as WebAssembly.ModuleImports,
      // Lua is built with wasm setjmp/longjmp (-wasm-enable-sjlj); its longjmp
      // throws the `__c_longjmp` exception tag, which — as in Emscripten — is an
      // imported tag the host provides. It carries one i32 (a pointer to the
      // longjmp args in wasm linear memory).
      env: { __c_longjmp: new WebAssembly.Tag({ parameters: ['i32'] }) } as unknown as WebAssembly.ModuleImports,
    };
    this.instance = (await WebAssembly.instantiate(module, imports)) as WebAssembly.Instance & {
      exports: ValkeyExports;
    };
    wasi.initialize(this.instance); // reactor: runs _initialize (ctors)
    this.instance.exports.rk_boot(this.port); // full Valkey init, returns (no aeMain)

    // Periodic pass for serverCron / client + blocked-client timeouts.
    let hzMs = 100;
    try {
      hzMs = this.instance.exports.rk_next_timeout_ms();
    } catch {
      /* fall back to 100ms */
    }
    this.timer = setInterval(() => this.step(), Math.max(20, hzMs || 100));
    return this;
  }

  /** Stop the server: close listeners, destroy connections, halt the reactor timer. */
  async stop(): Promise<void> {
    this.closed = true;
    if (this.timer) clearInterval(this.timer);
    for (const l of this.listeners.values()) { try { l.server.close(); } catch { /* already gone */ } }
    for (const s of this.socks.values()) { try { s.socket.destroy(); } catch { /* already gone */ } }
    this.listeners.clear();
    this.socks.clear();
  }
}
