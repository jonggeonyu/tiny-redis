/**
 * mini-redis HTTP API 클라이언트를 만든다. 데모 서버가 캐시로 쓴다.
 * 2xx와 404 외의 응답, 네트워크 오류는 Error를 던진다.
 * @param {string} baseUrl 서버 주소. 예: `http://127.0.0.1:3000`
 * @returns {{
 *   set: function(string, string, number=): Promise<boolean>,
 *   get: function(string): Promise<(string|null)>,
 *   del: function(string): Promise<boolean>,
 *   expire: function(string, number): Promise<boolean>
 * }} set, get, del, expire 메서드를 가진 객체
 */
export function createClient(baseUrl) {
  const url = (key, suffix = '') => `${baseUrl}/keys/${encodeURIComponent(key)}${suffix}`;

  async function call(method, path, body) {
    const res = await fetch(path, {
      method,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok && res.status !== 404) {
      throw new Error(`mini-redis ${res.status} ${data.error ?? ''} ${data.message ?? ''}`.trim());
    }
    return { status: res.status, data };
  }

  return {
    /**
     * 키에 값을 저장한다. ttl(초, 1 이상 정수)이 없으면 만료되지 않는다.
     * @param {string} key 키
     * @param {string} value 값
     * @param {number} [ttl] 만료까지 초
     * @returns {Promise<boolean>} 성공하면 true
     */
    async set(key, value, ttl) {
      const body = ttl === undefined ? { value } : { value, ttl };
      await call('PUT', url(key), body);
      return true;
    },

    /**
     * 키의 값을 읽는다.
     * @param {string} key 키
     * @returns {Promise<(string|null)>} 값. 없거나 만료되었으면 null
     */
    async get(key) {
      const { status, data } = await call('GET', url(key));
      return status === 404 ? null : data.value;
    },

    /**
     * 키를 삭제한다.
     * @param {string} key 키
     * @returns {Promise<boolean>} 지워졌으면 true
     */
    async del(key) {
      return (await call('DELETE', url(key))).data.deleted;
    },

    /**
     * 키의 만료 시간을 다시 정한다.
     * @param {string} key 키
     * @param {number} ttl 만료까지 초(1 이상 정수)
     * @returns {Promise<boolean>} 키가 있어 바꿨으면 true
     */
    async expire(key, ttl) {
      return (await call('POST', url(key, '/expire'), { ttl })).data.updated;
    },
  };
}
