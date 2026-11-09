import { afterEach, beforeEach, expect, test } from 'vitest';
import { createStore } from '../../src/store.js';
import { createServer } from '../../src/server.js';
import { createClient } from '../../src/client.js';

let server, client, clock;

beforeEach(async () => {
  clock = 1000;
  server = createServer({ store: createStore({ now: () => clock }) });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  client = createClient(`http://127.0.0.1:${server.address().port}`);
});
afterEach(async () => {
  server.closeAllConnections();
  await new Promise((r) => server.close(r));
});

test('클라이언트 200개가 키 2,000개 동시 SET 후 전부 조회', async () => {
  const keys = Array.from({ length: 2000 }, (_, i) => `k${i}`);
  // 클라이언트 하나는 자기 키 10개를 차례로 보낸다. 동시에 열리는 연결은 200개.
  const workers = Array.from({ length: 200 }, async (_, w) => {
    for (const k of keys.filter((_, i) => i % 200 === w)) await client.set(k, `v-${k}`);
  });
  await Promise.all(workers);
  const values = [];
  for (let i = 0; i < keys.length; i += 200) {
    values.push(...(await Promise.all(keys.slice(i, i + 200).map((k) => client.get(k)))));
  }
  expect(values).toEqual(keys.map((k) => `v-${k}`));
}, 60000);

test('같은 키 동시 SET 100개 → 최종 값은 보낸 값 중 하나', async () => {
  const sent = Array.from({ length: 100 }, (_, i) => `v${i}`);
  await Promise.all(sent.map((v) => client.set('same', v)));
  expect(sent).toContain(await client.get('same'));
}, 30000);

test('만료 키 조회와 새 SET이 겹쳐도 새 값이 남는다', async () => {
  await client.set('k', 'old', 1);
  clock += 2000;
  await Promise.all([client.get('k'), client.set('k', 'new')]);
  expect(await client.get('k')).toBe('new');
});
