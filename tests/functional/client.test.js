import http from 'node:http';
import { afterEach, beforeEach, expect, test } from 'vitest';
import { createStore } from '../../src/store.js';
import { createServer } from '../../src/server.js';
import { createClient } from '../../src/client.js';

let server, base, clock;

beforeEach(async () => {
  clock = 1000;
  server = createServer({ store: createStore({ now: () => clock }) });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  base = `http://127.0.0.1:${server.address().port}`;
});
afterEach(async () => {
  server.closeAllConnections();
  await new Promise((r) => (server.listening ? server.close(r) : r()));
});

test('네 명령 왕복', async () => {
  const c = createClient(base);
  expect(await c.set('a', 'v')).toBe(true);
  expect(await c.get('a')).toBe('v');
  expect(await c.get('nope')).toBeNull();
  expect(await c.expire('a', 10)).toBe(true);
  expect(await c.expire('nope', 10)).toBe(false);
  clock += 11000;
  expect(await c.get('a')).toBeNull();
  await c.set('b', 'x', 5);
  expect(await c.del('b')).toBe(true);
  expect(await c.del('b')).toBe(false);
});

test('/·?·# 가 든 키', async () => {
  const c = createClient(base);
  await c.set('a/b?c#d', 'v');
  expect(await c.get('a/b?c#d')).toBe('v');
});

test('2xx·404 외 응답은 예외', async () => {
  const c = createClient(base);
  await expect(c.set('a', 'v', 0)).rejects.toThrow(/400/);
});

test('서버가 꺼져 있으면 예외', async () => {
  const c = createClient(base);
  server.closeAllConnections();
  await new Promise((r) => server.close(r));
  await expect(c.get('a')).rejects.toThrow(Error);
});

test("키 '..' 는 set·del·expire 가 예외(404), get 은 null", async () => {
  const c = createClient(base);
  await expect(c.set('..', 'v')).rejects.toThrow(/404/);
  await expect(c.del('..')).rejects.toThrow(/404/);
  await expect(c.expire('..', 10)).rejects.toThrow(/404/);
});

test('baseUrl 끝의 / 는 제거', async () => {
  const c = createClient(base + '/');
  expect(await c.set('a', 'v')).toBe(true);
  expect(await c.get('a')).toBe('v');
  expect(await c.expire('a', 10)).toBe(true);
  expect(await c.del('a')).toBe(true);
});

test.each(['<html>no</html>', '{}'])('200 + 잘못된 본문(%s)은 예외', async (payload) => {
  const fake = http.createServer((req, res) => {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end(payload);
  });
  await new Promise((r) => fake.listen(0, '127.0.0.1', r));
  try {
    const c = createClient(`http://127.0.0.1:${fake.address().port}`);
    await expect(c.set('a', 'v')).rejects.toThrow(Error);
    await expect(c.get('a')).rejects.toThrow(Error);
    await expect(c.del('a')).rejects.toThrow(Error);
    await expect(c.expire('a', 1)).rejects.toThrow(Error);
  } finally {
    fake.closeAllConnections();
    await new Promise((r) => fake.close(r));
  }
});
