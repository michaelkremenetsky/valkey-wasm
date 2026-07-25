# Usage

Start a server, then connect with any Redis client:

```ts
import { ValkeyServer } from 'valkey-wasm';
import Redis from 'ioredis';

const server = await new ValkeyServer({ port: 6379 }).start();

const redis = new Redis(6379);
await redis.set('hello', 'world');
console.log(await redis.get('hello')); // 'world'

await redis.quit();
await server.stop();
```

The server speaks the normal Redis wire protocol, so `node-redis`, `bullmq`, and
anything else that talks to Redis works the same way — point it at the port.

## API

### `new ValkeyServer(options?)`

| option     | type     | default         | notes                                  |
| ---------- | -------- | --------------- | -------------------------------------- |
| `port`     | `number` | `6379`          | TCP port to listen on                  |
| `host`     | `string` | `'127.0.0.1'`   | interface to bind                      |
| `wasmPath` | `string` | bundled `.wasm` | override the path to `valkey.wasm`     |

### `server.start(): Promise<this>`

Boots the wasm instance and starts listening. Resolves once the server is live.

### `server.stop(): Promise<void>`

Closes the listener and any open connections and stops the reactor.

## Notes

- One `ValkeyServer` is one Valkey instance. Run several on different ports if
  you need isolated stores.
- Persistence is off (`save "" / appendonly no`), so data lives for the lifetime
  of the process. That's the point for dev, tests, and demos.
- Set `VALKEY_DEBUG=1` to log the bridge's socket and reactor activity.
