/**
 * HTTP API 서버. store 명령을 JSON over HTTP로 내보낸다.
 * @module server
 */
import http from 'node:http';

const send = (res, status, body) => {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(body));
};
const badRequest = (res, message) => send(res, 400, { error: 'bad_request', message });
const isTtl = (t) => Number.isSafeInteger(t) && t >= 1;

// 본문을 모두 읽은 뒤 JSON 객체로 파싱해 cb에 넘긴다. 실패하면 400을 보내고 cb를 부르지 않는다.
function readJsonObject(req, res, cb) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    let body;
    try {
      body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    } catch {
      return badRequest(res, 'invalid JSON');
    }
    if (body === null || typeof body !== 'object' || Array.isArray(body)) {
      return badRequest(res, 'body must be a JSON object');
    }
    cb(body); // 여기서 store를 한 번 동기로 호출한다 (설계 4절)
  });
}

/**
 * HTTP 서버를 만든다. listen하지 않은 상태로 돌려준다.
 * 본문을 다 읽고 검증한 뒤 store 함수를 한 번 동기로 호출한다.
 * @param {object} options
 * @param {{set: Function, get: Function, del: Function, expire: Function}} options.store createStore()가 돌려준 저장소
 * @returns {http.Server} listen하지 않은 HTTP 서버
 */
export function createServer({ store }) {
  return http.createServer((req, res) => {
    // 쿼리를 떼고 `/`로 먼저 나눈 뒤 각 조각을 디코딩한다.
    const rawPath = req.url.split('?')[0];
    const raw = rawPath.split('/'); // ['', 'keys', key, 'expire'?]
    if (raw[1] !== 'keys' || raw.length < 3 || raw.length > 4 || (raw.length === 4 && raw[3] !== 'expire')) {
      return send(res, 404, { error: 'not_found' });
    }
    let key;
    try {
      key = decodeURIComponent(raw[2]);
    } catch {
      return badRequest(res, 'invalid key encoding');
    }
    const isExpire = raw.length === 4;
    const method = req.method;

    if (isExpire) {
      if (method !== 'POST') return send(res, 405, { error: 'method_not_allowed' });
    } else if (!['PUT', 'GET', 'DELETE'].includes(method)) {
      return send(res, 405, { error: 'method_not_allowed' });
    }
    if (key === '') return badRequest(res, 'empty key');

    if (method === 'GET') {
      const value = store.get(key);
      return value === null ? send(res, 404, { error: 'not_found' }) : send(res, 200, { value });
    }
    if (method === 'DELETE') return send(res, 200, { deleted: store.del(key) });

    if (isExpire) {
      return readJsonObject(req, res, (body) => {
        if (!isTtl(body.ttl)) return badRequest(res, 'ttl must be an integer >= 1');
        send(res, 200, { updated: store.expire(key, body.ttl) });
      });
    }
    // PUT: ttl이 없을 때만 인자를 생략한다. store는 null·0을 검증하지 않는다.
    readJsonObject(req, res, (body) => {
      if (typeof body.value !== 'string') return badRequest(res, 'value must be a string');
      if ('ttl' in body && !isTtl(body.ttl)) return badRequest(res, 'ttl must be an integer >= 1');
      if ('ttl' in body) store.set(key, body.value, body.ttl);
      else store.set(key, body.value);
      send(res, 200, { ok: true });
    });
  });
}
