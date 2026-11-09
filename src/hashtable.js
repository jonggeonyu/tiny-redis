/**
 * 체이닝 해시 테이블. 모든 함수는 동기 함수이다.
 * @module hashtable
 */

/**
 * FNV-1a 32비트 해시를 계산한다.
 * 입력은 `charCodeAt`로 읽은 UTF-16 코드 단위이다.
 * @param {string} str 해시할 문자열
 * @returns {number} 부호 없는 32비트 정수 (0 ~ 2^32-1)
 */
export function fnv1a(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

/**
 * 체이닝 방식 해시 테이블. 키는 문자열이다.
 * 초기 capacity는 8이고, 추가 후 `size / capacity > 1`이면
 * capacity를 2배로 늘리고 모든 노드를 한 번에 rehash한다.
 */
export class HashTable {
  #buckets = new Array(8).fill(null); // 각 칸은 { key, value, next } 연결 리스트의 머리
  #size = 0;

  /**
   * 저장된 키의 개수.
   * @type {number}
   */
  get size() { return this.#size; }

  /**
   * 현재 버킷 개수. 초기값은 8이다.
   * @type {number}
   */
  get capacity() { return this.#buckets.length; }

  #index(key) { return fnv1a(key) % this.#buckets.length; }

  /**
   * 키의 값을 찾는다.
   * @param {string} key 찾을 키
   * @returns {*} 값. 키가 없으면 `undefined`
   */
  get(key) {
    for (let n = this.#buckets[this.#index(key)]; n; n = n.next) {
      if (n.key === key) return n.value;
    }
    return undefined;
  }

  /**
   * 키에 값을 저장한다. 이미 있는 키는 값을 덮어쓰며 size를 늘리지 않는다.
   * 새 키는 체인 머리에 추가하고, 부하율이 1을 넘으면 확장한다.
   * @param {string} key 저장할 키
   * @param {*} value 저장할 값
   * @returns {void}
   */
  set(key, value) {
    const i = this.#index(key);
    for (let n = this.#buckets[i]; n; n = n.next) {
      if (n.key === key) { n.value = value; return; } // 덮어쓰기: size 그대로
    }
    this.#buckets[i] = { key, value, next: this.#buckets[i] }; // 체인 머리에 추가
    this.#size++;
    if (this.#size / this.#buckets.length > 1) this.#resize();
  }

  /**
   * 키를 삭제한다.
   * @param {string} key 삭제할 키
   * @returns {boolean} 지웠으면 `true`, 키가 없었으면 `false`
   */
  delete(key) {
    const i = this.#index(key);
    let prev = null;
    for (let n = this.#buckets[i]; n; prev = n, n = n.next) {
      if (n.key !== key) continue;
      if (prev) prev.next = n.next;
      else this.#buckets[i] = n.next;
      this.#size--;
      return true;
    }
    return false;
  }

  // capacity를 2배로 늘리고 모든 노드를 한 번에 다시 넣는다.
  #resize() {
    const old = this.#buckets;
    this.#buckets = new Array(old.length * 2).fill(null);
    for (let head of old) {
      while (head) {
        const next = head.next;
        const i = this.#index(head.key);
        head.next = this.#buckets[i];
        this.#buckets[i] = head;
        head = next;
      }
    }
  }
}
