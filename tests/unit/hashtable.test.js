import { test, expect } from 'vitest';
import { fnv1a, HashTable } from '../../src/hashtable.js';

test('fnv1a 알려진 값', () => {
  expect(fnv1a('')).toBe(0x811c9dc5);
  expect(fnv1a('a')).toBe(0xe40c292c);
});

test('없는 키', () => {
  const t = new HashTable();
  expect(t.get('x')).toBeUndefined();
  expect(t.delete('x')).toBe(false);
});

test('덮어쓰기는 size를 늘리지 않는다', () => {
  const t = new HashTable();
  t.set('a', 1);
  t.set('a', 2);
  expect(t.get('a')).toBe(2);
  expect(t.size).toBe(1);
});

test('같은 칸 충돌 키 저장·조회·삭제(체인 머리·중간·끝)', () => {
  const t = new HashTable();
  expect(t.capacity).toBe(8);
  // capacity 8에서 같은 칸에 들어가는 키 3개를 찾는다
  const keys = [];
  for (let i = 0; keys.length < 3; i++) {
    const k = 'k' + i;
    if (fnv1a(k) % 8 === fnv1a('k0') % 8) keys.push(k);
  }
  const [k1, k2, k3] = keys;
  t.set(k1, 1);
  t.set(k2, 2);
  t.set(k3, 3);
  expect(t.capacity).toBe(8);
  expect(t.get(k1)).toBe(1);
  expect(t.get(k2)).toBe(2);
  expect(t.get(k3)).toBe(3);

  // 체인에서 가운데에 있는 키를 먼저 지운다 (삽입 순서와 무관하게 k2는 가운데)
  expect(t.delete(k2)).toBe(true);
  expect(t.get(k2)).toBeUndefined();
  expect(t.get(k1)).toBe(1);
  expect(t.get(k3)).toBe(3);

  // 남은 두 키 중 하나는 머리, 하나는 끝이다. 둘 다 차례로 지운다
  expect(t.delete(k3)).toBe(true);
  expect(t.get(k3)).toBeUndefined();
  expect(t.get(k1)).toBe(1);

  expect(t.delete(k1)).toBe(true);
  expect(t.get(k1)).toBeUndefined();
  expect(t.size).toBe(0);
  expect(t.delete(k1)).toBe(false);
});

test('리사이즈: 9번째 키에서 capacity 16, 모든 키 조회', () => {
  const t = new HashTable();
  for (let i = 0; i < 8; i++) t.set('key' + i, i);
  expect(t.capacity).toBe(8);
  t.set('key8', 8);
  expect(t.capacity).toBe(16);
  for (let i = 9; i < 1000; i++) t.set('key' + i, i);
  expect(t.size).toBe(1000);
  for (let i = 0; i < 1000; i++) expect(t.get('key' + i)).toBe(i);
});

test('한글·공백·특수문자·긴 키(10,000자), 빈 문자열 키', () => {
  const t = new HashTable();
  const keys = ['한글 키', 'a b c', '!@#$%^&*()/?\\"\'', 'x'.repeat(10000), ''];
  keys.forEach((k, i) => t.set(k, { i }));
  keys.forEach((k, i) => expect(t.get(k)).toEqual({ i }));
  expect(t.size).toBe(keys.length);
});
