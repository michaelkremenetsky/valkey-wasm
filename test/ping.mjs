// Milestone 3: redis-cli-level smoke test over the bridge. Boots valkey.wasm,
// then speaks raw RESP (PING / SET / GET / EVAL cjson+cmsgpack) with a plain
// TCP socket — no client library, so it isolates the server + bridge.
import net from 'node:net';
import { ValkeyServer } from '../dist/valkey-server.js';

const port = 6390;
const srv = await new ValkeyServer({ port }).start();

function cmd(sock, ...args) {
  // encode as a RESP array of bulk strings
  let s = `*${args.length}\r\n`;
  for (const a of args) s += `$${Buffer.byteLength(a)}\r\n${a}\r\n`;
  sock.write(s);
}
const sock = net.connect(port, '127.0.0.1');
const replies = [];
let buf = '';
sock.on('data', (d) => { buf += d.toString(); process.stdout.write(d); });

await new Promise((r) => sock.once('connect', r));
cmd(sock, 'PING');
cmd(sock, 'SET', 'foo', 'bar');
cmd(sock, 'GET', 'foo');
cmd(sock, 'EVAL', "return cjson.encode({1,2,3})", '0');
cmd(sock, 'EVAL', "return cmsgpack.unpack(cmsgpack.pack('hi'))", '0');
await new Promise((r) => setTimeout(r, 500));

const ok = buf.includes('+PONG') && buf.includes('bar') && buf.includes('[1,2,3]') && buf.includes('hi');
console.log('\n' + (ok ? 'PING-TEST OK' : 'PING-TEST FAIL'));
sock.destroy();
await srv.stop();
process.exit(ok ? 0 : 1);
