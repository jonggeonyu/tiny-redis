/**
 * 캐시 미사용/사용 성능 비교. `npm run bench`로 실행한다.
 * @module compare
 */
import { createStore } from '../src/store.js';
import { createServer } from '../src/server.js';
import { createClient } from '../src/client.js';
import { createFakeDb } from '../demo/fake-db.js';
import { createApiServer } from '../demo/api-server.js';

const REQUESTS = 100;
const USER_COUNT = 10;
const DELAY_MS = 100;
const TTL = 60;
const users = Array.from({ length: USER_COUNT }, (_, i) => ({ id: i + 1, name: `user${i + 1}` }));

const listen = (server) =>
  new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(`http://127.0.0.1:${server.address().port}`)));
const close = (server) => new Promise((resolve) => server.close(resolve));
const userId = (i) => (i % USER_COUNT) + 1; // 사용자 10명을 차례로 반복

/** 빈 mini-redis와 새 fake-db, 데모 API 서버를 띄운다. useCache가 false면 캐시 없이 띄운다. */
async function startDemo(useCache) {
  const db = createFakeDb({ users, delayMs: DELAY_MS });
  const redis = useCache ? createServer({ store: createStore() }) : null;
  const cache = redis ? createClient(await listen(redis)) : null;
  const api = createApiServer({ db, cache, ttl: TTL });
  const apiUrl = await listen(api);
  return { db, apiUrl, stop: () => Promise.all([close(api), redis && close(redis)]) };
}

/**
 * 데모 API를 띄워 요청 100개를 순차로 보내고 결과를 잰다.
 * @param {boolean} useCache mini-redis 캐시를 쓸지 여부
 * @returns {Promise<{요청수: number, 총시간ms: number, 평균응답ms: number, 원본호출: number, hit비율: number}>} 측정 결과
 */
async function runSequential(useCache) {
  const { db, apiUrl, stop } = await startDemo(useCache);
  const start = performance.now();
  for (let i = 0; i < REQUESTS; i++) {
    await (await fetch(`${apiUrl}/users/${userId(i)}`)).text();
  }
  const total = performance.now() - start;
  await stop();
  return {
    요청수: REQUESTS,
    총시간ms: Math.round(total),
    평균응답ms: Number((total / REQUESTS).toFixed(1)),
    원본호출: db.calls,
    hit비율: 1 - db.calls / REQUESTS,
  };
}

/**
 * 빈 캐시에 같은 요청 100개를 동시에 보내 원본 호출 횟수를 센다(stampede 측정).
 * @returns {Promise<number>} 원본(DB) 호출 횟수
 */
async function runStampede() {
  const { db, apiUrl, stop } = await startDemo(true);
  await Promise.all(Array.from({ length: REQUESTS }, () => fetch(`${apiUrl}/users/1`).then((r) => r.text())));
  await stop();
  return db.calls;
}

console.log(`원본 지연 ${DELAY_MS}ms, 요청 ${REQUESTS}개, 사용자 ${USER_COUNT}명 순차 조회, ttl ${TTL}초`);
console.table({ '캐시 미사용': await runSequential(false), '캐시 사용': await runSequential(true) });
console.table({ '빈 캐시에 동시 100개 (같은 키)': { 요청수: REQUESTS, 원본호출: await runStampede() } });
