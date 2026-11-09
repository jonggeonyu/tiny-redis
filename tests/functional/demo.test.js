import { afterEach, beforeEach, expect, test } from 'vitest';
import { createStore } from '../../src/store.js';
import { createServer } from '../../src/server.js';
import { createClient } from '../../src/client.js';
import { createFakeDb } from '../../demo/fake-db.js';
import { createApiServer } from '../../demo/api-server.js';

let redis, redisBase, clock, cache, db, api, apiBase;
const users = () => [{ id: '1', name: 'kim', age: 20 }];

const listen = async (s) => {
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  return `http://127.0.0.1:${s.address().port}`;
};
const close = async (s) => {
  if (!s) return;
  s.closeAllConnections();
  await new Promise((r) => (s.listening ? s.close(r) : r()));
};
const startApi = async (opts) => {
  api = createApiServer({ db, ...opts });
  apiBase = await listen(api);
};
const getUser = (id) => fetch(`${apiBase}/users/${id}`);
const putUser = (id, body) =>
  fetch(`${apiBase}/users/${id}`, { method: 'PUT', body: typeof body === 'string' ? body : JSON.stringify(body) });

beforeEach(async () => {
  clock = 1000;
  redis = createServer({ store: createStore({ now: () => clock }) });
  redisBase = await listen(redis);
  cache = createClient(redisBase);
  db = createFakeDb({ users: users(), delayMs: 0 });
});
afterEach(async () => {
  await close(api);
  await close(redis);
  api = undefined;
});

test('첫 GET은 miss(원본 1회), 두 번째는 hit(원본 그대로 1회)', async () => {
  await startApi({ cache });
  const r1 = await getUser(1);
  expect(r1.status).toBe(200);
  expect(r1.headers.get('content-type')).toBe('application/json; charset=utf-8');
  expect(await r1.json()).toEqual({ id: '1', name: 'kim', age: 20 });
  expect(db.calls).toBe(1);
  expect(await (await getUser(1)).json()).toEqual({ id: '1', name: 'kim', age: 20 });
  expect(db.calls).toBe(1);
});

test('캐시에는 user:<id> 키로 JSON 문자열이 들어간다', async () => {
  await startApi({ cache });
  await getUser(1);
  const v = await cache.get('user:1');
  expect(typeof v).toBe('string');
  expect(JSON.parse(v)).toEqual({ id: '1', name: 'kim', age: 20 });
});

test('PUT 뒤 GET은 새 값, 원본 다시 읽음', async () => {
  await startApi({ cache });
  await getUser(1);
  const p = await putUser(1, { name: 'lee' });
  expect(p.status).toBe(200);
  expect(await cache.get('user:1')).toBeNull();
  const before = db.calls;
  const r = await getUser(1);
  expect((await r.json()).name).toBe('lee');
  expect(db.calls).toBe(before + 1);
});

test('ttl 60초 지나면 원본에서 다시 읽음', async () => {
  await startApi({ cache });
  await getUser(1);
  clock += 59000;
  await getUser(1);
  expect(db.calls).toBe(1);
  clock += 1000;
  await getUser(1);
  expect(db.calls).toBe(2);
});

test('없는 사용자 GET·PUT → 404, 캐시에 남지 않음', async () => {
  await startApi({ cache });
  const g = await getUser(99);
  expect(g.status).toBe(404);
  expect(await g.json()).toEqual({ error: 'not_found' });
  const p = await putUser(99, { name: 'x' });
  expect(p.status).toBe(404);
  expect(await cache.get('user:99')).toBeNull();
});

test('mini-redis가 꺼져도 GET·PUT은 원본으로 200', async () => {
  const dead = createServer({ store: createStore({ now: () => clock }) });
  const deadBase = await listen(dead);
  await close(dead);
  await startApi({ cache: createClient(deadBase) });
  expect((await getUser(1)).status).toBe(200);
  const p = await putUser(1, { name: 'lee' });
  expect(p.status).toBe(200);
  expect((await (await getUser(1)).json()).name).toBe('lee');
});

test('cache: null이면 매번 원본', async () => {
  await startApi({ cache: null });
  await getUser(1);
  await getUser(1);
  expect(db.calls).toBe(2);
});

test('캐시 빈 상태에서 같은 GET 100개 동시 → 원본 호출 수를 기록만 한다', async () => {
  await startApi({ cache });
  const rs = await Promise.all(Array.from({ length: 100 }, () => getUser(1)));
  expect(rs.every((r) => r.status === 200)).toBe(true);
  console.log(`stampede: 원본 호출 ${db.calls}회 / 100`);
  expect(db.calls).toBeGreaterThanOrEqual(1);
  expect(db.calls).toBeLessThanOrEqual(100);
});

test('PUT 본문이 JSON 객체가 아니면 400', async () => {
  await startApi({ cache });
  for (const body of ['{oops', 'null', '[]', '"x"']) {
    const r = await putUser(1, body);
    expect(r.status).toBe(400);
    expect(await r.json()).toEqual({ error: 'bad_request' });
  }
});

test('없는 경로 404, 다른 메서드 405', async () => {
  await startApi({ cache });
  const a = await fetch(`${apiBase}/nope`);
  expect(a.status).toBe(404);
  expect(await a.json()).toEqual({ error: 'not_found' });
  const b = await fetch(`${apiBase}/users/1`, { method: 'DELETE' });
  expect(b.status).toBe(405);
  expect(await b.json()).toEqual({ error: 'method_not_allowed' });
});
