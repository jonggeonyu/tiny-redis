/**
 * 키-값 저장소 명령 계층. HashTable 위에 SET/GET/DEL/EXPIRE와 lazy 만료를 올린다.
 * 모든 함수는 동기 함수이다(동시성 보장의 근거).
 * @module store
 */
import { HashTable } from './hashtable.js';

/**
 * 저장소를 만든다. 엔트리는 `{ value, expireAt }`(밀리초, 만료 없으면 `null`)이다.
 * 모든 명령은 키를 찾은 뒤 만료를 먼저 확인하고, 만료되었으면 삭제하고 없음으로 취급한다.
 * @param {object} [options]
 * @param {function(): number} [options.now=Date.now] 현재 시각(밀리초)을 돌려주는 시계
 * @param {HashTable} [options.table=new HashTable()] 엔트리를 담을 해시 테이블
 * @returns {{set: Function, get: Function, del: Function, expire: Function}} 명령 객체
 */
export function createStore({ now = Date.now, table = new HashTable() } = {}) {
  const isExpired = (e) => e.expireAt !== null && now() >= e.expireAt;

  // 살아 있는 엔트리를 돌려준다. 만료되었으면 그 자리에서 지우고 undefined.
  function lookup(key) {
    const e = table.get(key);
    if (e === undefined) return undefined;
    if (isExpired(e)) {
      table.delete(key);
      return undefined;
    }
    return e;
  }

  return {
    /**
     * 값을 저장한다. 기존 값은 덮어쓰고, ttl이 없으면 기존 만료도 없앤다.
     * @param {string} key 키
     * @param {string} value 값
     * @param {number} [ttlSec] 만료까지 초. 검증은 호출하는 쪽이 한다
     * @returns {true}
     */
    set(key, value, ttlSec) {
      const expireAt = ttlSec === undefined ? null : now() + ttlSec * 1000;
      table.set(key, { value, expireAt });
      return true;
    },

    /**
     * 값을 조회한다.
     * @param {string} key 키
     * @returns {string|null} 값. 없거나 만료되었으면 `null`
     */
    get(key) {
      const e = lookup(key);
      return e === undefined ? null : e.value;
    },

    /**
     * 키를 지운다.
     * @param {string} key 키
     * @returns {boolean} 지웠으면 `true`, 없거나 만료되었으면 `false`
     */
    del(key) {
      return lookup(key) !== undefined && table.delete(key);
    },

    /**
     * 만료 시각을 `now() + ttlSec * 1000`으로 다시 잡는다.
     * @param {string} key 키
     * @param {number} ttlSec 만료까지 초. 검증은 호출하는 쪽이 한다
     * @returns {boolean} 설정했으면 `true`, 없거나 만료되었으면 `false`
     */
    expire(key, ttlSec) {
      const e = lookup(key);
      if (e === undefined) return false;
      e.expireAt = now() + ttlSec * 1000;
      return true;
    },
  };
}
