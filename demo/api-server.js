/**
 * mini-redis를 캐시(cache-aside)로 쓰는 데모 API 서버.
 * @module api-server
 */
import http from 'node:http';

const send = (res, status, body) => {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(body));
};

// 캐시 호출이 실패해도 요청은 원본으로 계속 처리한다.
async function safe(fn) {
  try {
    return await fn();
  } catch {
    return undefined;
  }
}

/**
 * 데모 API 서버를 만든다. listen하지 않은 상태로 돌려준다.
 * GET /users/:id 는 캐시를 먼저 보고 miss면 원본을 읽어 캐시에 넣는다.
 * PUT /users/:id 는 원본을 고친 뒤 캐시 키를 지운다.
 * @param {object} options
 * @param {{getUser: Function, updateUser: Function}} options.db 원본 저장소. createFakeDb() 결과
 * @param {?{get: Function, set: Function, del: Function}} [options.cache=null] createClient() 결과. null이면 캐시를 쓰지 않는다
 * @param {number} [options.ttl=60] 캐시 ttl(초)
 * @returns {http.Server} listen하지 않은 HTTP 서버
 */
export function createApiServer({ db, cache = null, ttl = 60 }) {
  async function handleGet(res, id) {
    const key = `user:${id}`;
    if (cache) {
      const hit = await safe(() => cache.get(key));
      if (typeof hit === 'string') {
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
        return res.end(hit);
      }
    }
    const user = await db.getUser(id);
    if (!user) return send(res, 404, { error: 'not_found' });
    const json = JSON.stringify(user);
    if (cache) await safe(() => cache.set(key, json, ttl));
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(json);
  }

  async function handlePut(req, res, id) {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    let body;
    try {
      body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    } catch {
      return send(res, 400, { error: 'bad_request' });
    }
    if (body === null || typeof body !== 'object' || Array.isArray(body)) {
      return send(res, 400, { error: 'bad_request' });
    }
    const user = await db.updateUser(id, body);
    if (!user) return send(res, 404, { error: 'not_found' });
    if (cache) await safe(() => cache.del(`user:${id}`));
    send(res, 200, user);
  }

  return http.createServer((req, res) => {
    const parts = req.url.split('?')[0].split('/'); // ['', 'users', id]
    if (parts[1] !== 'users' || parts.length !== 3 || parts[2] === '') {
      return send(res, 404, { error: 'not_found' });
    }
    let id;
    try {
      id = decodeURIComponent(parts[2]);
    } catch {
      return send(res, 400, { error: 'bad_request' });
    }
    const run = (p) =>
      p.catch(() => {
        if (!res.headersSent) send(res, 500, { error: 'internal' });
        else res.end();
      });
    if (req.method === 'GET') return run(handleGet(res, id));
    if (req.method === 'PUT') return run(handlePut(req, res, id));
    send(res, 405, { error: 'method_not_allowed' });
  });
}
