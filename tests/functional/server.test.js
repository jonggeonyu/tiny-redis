import { afterEach, beforeEach, describe, expect, test } from 'vitest';
import { createStore } from '../../src/store.js';
import { createServer } from '../../src/server.js';

let server, base, clock;
const put = (key, body, raw = false) =>
  fetch(`${base}/keys/${key}`, { method: 'PUT', body: raw ? body : JSON.stringify(body) });
const get = (key) => fetch(`${base}/keys/${key}`);

beforeEach(async () => {
  clock = 1000;
  server = createServer({ store: createStore({ now: () => clock }) });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  base = `http://127.0.0.1:${server.address().port}`;
});
afterEach(async () => {
  server.closeAllConnections();
  await new Promise((r) => server.close(r));
});

test('SET → GET', async () => {
  const r = await put('a', { value: 'v' });
  expect(r.status).toBe(200);
  expect(r.headers.get('content-type')).toBe('application/json; charset=utf-8');
  expect(await r.json()).toEqual({ ok: true });
  const g = await get('a');
  expect(g.status).toBe(200);
  expect(await g.json()).toEqual({ value: 'v' });
});

test('GET 없는 키 404 not_found', async () => {
  const g = await get('nope');
  expect(g.status).toBe(404);
  expect(await g.json()).toEqual({ error: 'not_found' });
});

test('DEL', async () => {
  await put('a', { value: 'v' });
  const d1 = await fetch(`${base}/keys/a`, { method: 'DELETE' });
  expect(d1.status).toBe(200);
  expect(await d1.json()).toEqual({ deleted: true });
  const d2 = await fetch(`${base}/keys/a`, { method: 'DELETE' });
  expect(await d2.json()).toEqual({ deleted: false });
});

test('EXPIRE와 만료', async () => {
  await put('a', { value: 'v' });
  const r = await fetch(`${base}/keys/a/expire`, { method: 'POST', body: JSON.stringify({ ttl: 10 }) });
  expect(r.status).toBe(200);
  expect(await r.json()).toEqual({ updated: true });
  clock += 10000;
  expect((await get('a')).status).toBe(404);
  const r2 = await fetch(`${base}/keys/zz/expire`, { method: 'POST', body: JSON.stringify({ ttl: 10 }) });
  expect(await r2.json()).toEqual({ updated: false });
});

test('SET ttl 만료', async () => {
  await put('a', { value: 'v', ttl: 1 });
  expect((await get('a')).status).toBe(200);
  clock += 1000;
  expect((await get('a')).status).toBe(404);
});

test.each(['{', 'null', '[]', '"x"'])('본문 %s → 400 bad_request', async (body) => {
  const r = await put('a', body, true);
  expect(r.status).toBe(400);
  expect((await r.json()).error).toBe('bad_request');
});

test.each([
  { value: 1 },
  { value: 'v', ttl: 0 },
  { value: 'v', ttl: -1 },
  { value: 'v', ttl: 1.5 },
  { value: 'v', ttl: '10' },
  { value: 'v', ttl: null },
  { value: 'v', ttl: 9007199254740993 },
])('잘못된 SET 본문 %j → 400', async (body) => {
  await put('a', { value: 'v0' });
  const r = await put('a', body);
  expect(r.status).toBe(400);
  expect((await r.json()).error).toBe('bad_request');
  // store에 닿지 않았다
  expect(await (await get('a')).json()).toEqual({ value: 'v0' });
});

test.each([{}, { ttl: 0 }, { ttl: 1.5 }, { ttl: '10' }, { ttl: null }])(
  'EXPIRE ttl 없음·잘못됨 %j → 400',
  async (body) => {
    await put('a', { value: 'v' });
    const r = await fetch(`${base}/keys/a/expire`, { method: 'POST', body: JSON.stringify(body) });
    expect(r.status).toBe(400);
    expect((await r.json()).error).toBe('bad_request');
    clock += 100000;
    expect((await get('a')).status).toBe(200);
  },
);

test('빈 키 PUT /keys/ → 400', async () => {
  const r = await fetch(`${base}/keys/`, { method: 'PUT', body: JSON.stringify({ value: 'v' }) });
  expect(r.status).toBe(400);
  expect((await r.json()).error).toBe('bad_request');
});

test('깨진 인코딩 /keys/%E0%A4%A → 400', async () => {
  const r = await get('%E0%A4%A');
  expect(r.status).toBe(400);
  expect((await r.json()).error).toBe('bad_request');
});

test('a%2Fb는 한 키이다', async () => {
  await put('a%2Fb', { value: 'v' });
  expect((await get('a%2Fb')).status).toBe(200);
  expect((await get('a')).status).toBe(404);
});

test('한글 키', async () => {
  const k = encodeURIComponent('키 1');
  await put(k, { value: '값' });
  expect(await (await get(k)).json()).toEqual({ value: '값' });
});

test('빈 문자열 값 GET 200 {value:""}', async () => {
  await put('e', { value: '' });
  const g = await get('e');
  expect(g.status).toBe(200);
  expect(await g.json()).toEqual({ value: '' });
});

test('없는 경로 /foo → 404 not_found', async () => {
  const r = await fetch(`${base}/foo`);
  expect(r.status).toBe(404);
  expect(await r.json()).toEqual({ error: 'not_found' });
});

test('PATCH /keys/a, GET /keys/a/expire → 405 method_not_allowed', async () => {
  const a = await fetch(`${base}/keys/a`, { method: 'PATCH' });
  expect(a.status).toBe(405);
  expect(await a.json()).toEqual({ error: 'method_not_allowed' });
  const b = await fetch(`${base}/keys/a/expire`);
  expect(b.status).toBe(405);
  expect(await b.json()).toEqual({ error: 'method_not_allowed' });
});

test('오류 뒤에도 서버가 계속 응답한다', async () => {
  await put('a', { value: 'v' });
  expect((await put('a', '{', true)).status).toBe(400);
  expect((await get('a')).status).toBe(200);
});
