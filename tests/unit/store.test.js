import { test, expect } from 'vitest';
import { createStore } from '../../src/store.js';
import { HashTable } from '../../src/hashtable.js';

function setup() {
  const clock = { t: 0 };
  const table = new HashTable();
  const s = createStore({ now: () => clock.t, table });
  return { clock, table, s };
}

test('ttl 없는 값은 만료되지 않는다', () => {
  const { clock, s } = setup();
  expect(s.set('a', 'v')).toBe(true);
  clock.t += 1e9;
  expect(s.get('a')).toBe('v');
});

test('만료 직전·직후', () => {
  const { clock, s } = setup();
  s.set('a', 'v', 10);
  clock.t = 9999;
  expect(s.get('a')).toBe('v');
  clock.t = 10000;
  expect(s.get('a')).toBeNull();
});

test('ttl 없이 덮어쓰면 기존 만료가 사라진다', () => {
  const { clock, s } = setup();
  s.set('a', 'v', 10);
  s.set('a', 'w');
  clock.t = 20000;
  expect(s.get('a')).toBe('w');
});

test('만료된 키에 expire는 false, del은 false', () => {
  const { clock, s } = setup();
  s.set('a', 'v', 10);
  s.set('b', 'v', 10);
  clock.t = 10000;
  expect(s.expire('a', 10)).toBe(false);
  expect(s.get('a')).toBeNull();
  expect(s.del('b')).toBe(false);
});

test('없는 키 get null, del false, expire false', () => {
  const { s } = setup();
  expect(s.get('x')).toBeNull();
  expect(s.del('x')).toBe(false);
  expect(s.expire('x', 10)).toBe(false);
});

test('있는 키 del은 true이고 이후 get은 null', () => {
  const { s } = setup();
  s.set('a', 'v');
  expect(s.del('a')).toBe(true);
  expect(s.get('a')).toBeNull();
});

test('expire는 now 기준으로 다시 잡는다', () => {
  const { clock, s } = setup();
  s.set('a', 'v');
  clock.t = 5000;
  expect(s.expire('a', 10)).toBe(true);
  clock.t = 14999;
  expect(s.get('a')).toBe('v');
  clock.t = 15000;
  expect(s.get('a')).toBeNull();
});

test('빈 문자열 값은 null이 아니다', () => {
  const { s } = setup();
  s.set('a', '');
  expect(s.get('a')).toBe('');
});

test('만료 키는 조회 시 테이블에서 지워진다', () => {
  const { clock, table, s } = setup();
  s.set('a', 'v', 1);
  expect(table.size).toBe(1);
  clock.t = 1000;
  expect(s.get('a')).toBeNull();
  expect(table.size).toBe(0);
});
