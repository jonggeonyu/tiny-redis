// 체이닝 해시 테이블. 모든 함수는 동기 함수이다.

// FNV-1a 32비트. 입력은 charCodeAt로 읽은 UTF-16 코드 단위.
export function fnv1a(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

export class HashTable {
  #buckets = new Array(8).fill(null); // 각 칸은 { key, value, next } 연결 리스트의 머리
  #size = 0;

  get size() { return this.#size; }
  get capacity() { return this.#buckets.length; }

  #index(key) { return fnv1a(key) % this.#buckets.length; }

  get(key) {
    for (let n = this.#buckets[this.#index(key)]; n; n = n.next) {
      if (n.key === key) return n.value;
    }
    return undefined;
  }

  set(key, value) {
    const i = this.#index(key);
    for (let n = this.#buckets[i]; n; n = n.next) {
      if (n.key === key) { n.value = value; return; } // 덮어쓰기: size 그대로
    }
    this.#buckets[i] = { key, value, next: this.#buckets[i] }; // 체인 머리에 추가
    this.#size++;
    if (this.#size / this.#buckets.length > 1) this.#resize();
  }

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
