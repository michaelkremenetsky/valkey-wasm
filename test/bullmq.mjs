// Milestone 4 (acceptance): the exact BullMQ Queue -> Worker -> completed
// lifecycle that ioredis-mock cannot run (needs EVALSHA script cache, BZPOPMIN
// blocking, cmsgpack). Green here means real BullMQ works against valkey-wasm
// with zero app changes. Run: `npm i bullmq` in this dir first.
import { ValkeyServer } from '../dist/valkey-server.js';
import { Queue, Worker } from 'bullmq';

const port = 6391;
const srv = await new ValkeyServer({ port }).start();
const connection = { host: '127.0.0.1', port };
const deadline = setTimeout(() => { console.log('TIMEOUT — job never completed'); process.exit(1); }, 20000);

try {
  const queue = new Queue('t', { connection });
  const done = new Promise((resolve, reject) => {
    const worker = new Worker('t', async (job) => {
      console.log('worker got job:', job.name, JSON.stringify(job.data));
      return { ok: true };
    }, { connection });
    worker.on('completed', (job, result) => resolve({ id: job.id, result }));
    worker.on('failed', (job, e) => reject(new Error('job failed: ' + (e && e.message))));
  });
  const job = await queue.add('hello', { x: 1 });
  console.log('added job', job.id);
  const r = await done;
  console.log('BULLMQ-TEST OK', JSON.stringify(r));
  clearTimeout(deadline);
  await srv.stop();
  process.exit(0);
} catch (e) {
  console.log('BULLMQ-TEST FAIL:', (e && e.stack || e));
  process.exit(1);
}
