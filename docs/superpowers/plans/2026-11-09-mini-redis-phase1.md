# mini-redis 1단계 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**목표:** 직접 만든 해시 테이블 위에 `SET/GET/DEL/EXPIRE` HTTP API를 올리고, 이를 캐시로 쓰는 데모 API와 성능 비교 스크립트를 만든다.

**구조:** `hashtable.js`(저장) → `store.js`(명령·lazy 만료, 시계 주입) → `server.js`(HTTP·검증). `client.js`가 HTTP API를 감싸고, `demo/`와 `bench/`가 그것을 쓴다. 동시성은 "store·hashtable은 동기 함수만" 규칙으로 보장한다.

**기술:** Node.js 22, ES 모듈, 내장 `http`·`fetch`, Vitest

**설계:** `docs/requirements.md`, `docs/architecture-phase1.md` (절 번호는 이 문서 기준)

## 전체 제약
CLAUDE.md의 제약과 설계 10절 제안값을 그대로 따른다. 여기서 반복하지 않는다.

## 계획에서 정한 값 (2026-11-09 승인, 설계 10절 9~17번에 반영)
| # | 값 | 종류 |
|---|---|---|
| V1 | 해시 함수 입력은 `charCodeAt`로 읽은 UTF-16 코드 단위 | 내부 |
| V2 | 키 디코딩 실패(`%E0%A4%A` 등)와 JSON 객체가 아닌 본문(`null`, `[]`, `"x"`)은 `400 bad_request` | 외부 형식 |
| V3 | `ttl: null`은 생략이 아니라 잘못된 값으로 보고 `400` | 외부 형식 |
| V4 | 있는 경로에 다른 메서드(`GET /keys/a/expire`, `PATCH /keys/a`)는 `405` | 외부 형식 |
| V5 | 데모 `PUT /users/:id` 본문은 바꿀 필드의 JSON 객체, 응답은 `200` + 수정된 사용자, 없는 사용자는 `404` | 외부 형식 |
| V6 | 없는 사용자(`404`)는 캐시에 저장하지 않는다 | 외부 동작 |
| V7 | 동시성 테스트 규모: 클라이언트 200개가 키 2,000개 `SET` | 테스트 |
| V8 | 성능 비교 요청 100개는 사용자 10명을 차례로 반복 조회 | 벤치 |
| V9 | 응답 헤더 `Content-Type: application/json; charset=utf-8` | 외부 형식 |

실행 포트, `npm start` 같은 실행 명령은 (미정)이다. 이 계획은 포트 0만 쓴다.

## 검토 초점
테스트가 없으면 놓치기 쉬운 입력이다. 각 줄의 테스트를 담당 작업에 넣어 두었다.
1. 빈 문자열 값 `""`: `GET`이 `404`가 아니라 `200 { "value": "" }`을 돌려준다 (Task 2, 3).
2. 깨진 퍼센트 인코딩 키: 서버가 죽지 않고 `400` (Task 3).
3. `/`가 든 키(`a%2Fb`): 한 개의 키로 저장·조회되고 `expire` 경로로 오인되지 않는다 (Task 3, 4).
4. JSON 객체가 아닌 본문(`null`, `[]`): `400`이고 서버가 죽지 않는다 (Task 3).
5. mini-redis가 꺼진 상태의 `PUT /users/:id`: `DEL` 실패와 관계없이 원본 수정 결과로 응답한다 (Task 5).

---

### Task 1: 프로젝트 틀과 해시 테이블
담당: `builder` / 강도: 높음

**Files:**
- Create: `package.json`, `src/hashtable.js`
- Test: `tests/unit/hashtable.test.js`

**Interfaces:**
- Produces:
  - `export function fnv1a(str: string): number` — 32비트 부호 없는 정수
  - `export class HashTable` — `get(key): any | undefined`, `set(key, value): void`, `delete(key): boolean`, `get size(): number`, `get capacity(): number`

- [ ] **Step 1: 틀 만들기**
`package.json`에 `"type": "module"`, `"scripts": { "test": "vitest run", "bench": "node bench/compare.js" }`. `npm i -D vitest`.

- [ ] **Step 2: 실패하는 테스트 작성**
```js
test('fnv1a 알려진 값', () => {
  expect(fnv1a('')).toBe(0x811c9dc5);
  expect(fnv1a('a')).toBe(0xe40c292c);
});
test('없는 키', () => { const t = new HashTable(); expect(t.get('x')).toBeUndefined(); expect(t.delete('x')).toBe(false); });
test('덮어쓰기는 size를 늘리지 않는다', () => { /* set a=1, set a=2 → get a === 2, size === 1 */ });
test('같은 칸 충돌 키 저장·조회·삭제(체인 머리·중간·끝)', () => {
  // capacity 8에서 fnv1a(k) % 8 이 같은 키 3개를 찾아 넣고, 중간 → 머리 → 끝 순으로 delete
  // 매 삭제 뒤 남은 키 get이 맞고 지운 키는 undefined
});
test('리사이즈: 9번째 키에서 capacity 16, 모든 키 조회', () => {
  // 키 8개 → capacity 8, 9개 → 16. 1,000개 넣은 뒤 전부 get 일치, size 1000
});
test('한글·공백·특수문자·긴 키(10,000자), 빈 문자열 키', () => { /* set 후 get 일치 */ });
```

- [ ] **Step 3: 실패 확인** — `npm test -- tests/unit/hashtable.test.js` → FAIL (모듈 없음)

- [ ] **Step 4: 구현**
설계 2절대로: 버킷 배열 + `{ key, value, next }` 체인, `fnv1a(key) % capacity`, `size / capacity > 1`이면 2배 확장 후 전체 rehash. FNV-1a: `h = 0x811c9dc5`, 글자마다 `h ^= code; h = Math.imul(h, 0x01000193)`, 끝에 `>>> 0`.

- [ ] **Step 5: 통과 확인** — 같은 명령 → PASS

- [ ] **Step 6: 커밋** — `feat: 해시 테이블(체이닝, FNV-1a, 리사이즈)`

### Task 2: 저장소와 lazy 만료
담당: `builder` / 강도: 높음

**Files:**
- Create: `src/store.js`
- Test: `tests/unit/store.test.js`

**Interfaces:**
- Consumes: `HashTable`
- Produces: `export function createStore({ now = Date.now } = {})` → `{ set(key, value, ttlSec?): true, get(key): string | null, del(key): boolean, expire(key, ttlSec): boolean }` (모두 동기)

- [ ] **Step 1: 실패하는 테스트 작성** (가짜 시계 `let t = 0; const s = createStore({ now: () => t })`)
```js
test('ttl 없는 값은 만료되지 않는다', ...)          // set a, t += 1e9 → get 'v'
test('만료 직전·직후', ...)                         // set a v 10 → t=9999: 'v', t=10000: null
test('ttl 없이 덮어쓰면 기존 만료가 사라진다', ...)   // set a v 10, set a w → t=20000: 'w'
test('만료된 키에 expire는 false, del은 false', ...)
test('없는 키 get null, del false, expire false', ...)
test('expire는 now 기준으로 다시 잡는다', ...)        // set a v, t=5000, expire a 10 → t=14999 'v', t=15000 null
test('빈 문자열 값은 null이 아니다', ...)            // set a '' → get === ''
test('만료 키는 조회 시 테이블에서 지워진다', ...)    // 만료 뒤 get → 내부 HashTable size 0 (테스트용으로 내부 테이블 접근 방법은 구현자가 정함)
```

- [ ] **Step 2: 실패 확인** — `npm test -- tests/unit/store.test.js` → FAIL

- [ ] **Step 3: 구현** — 설계 3절 표와 `isExpired` 정의 그대로. 엔트리 `{ value, expireAt }`.

- [ ] **Step 4: 통과 확인** → PASS

- [ ] **Step 5: 커밋** — `feat: 저장소 명령과 lazy 만료`

### Task 3: HTTP 서버
담당: `builder` / 강도: 기본

**Files:**
- Create: `src/server.js`
- Test: `tests/functional/server.test.js`

**Interfaces:**
- Consumes: `createStore`
- Produces: `export function createServer({ store }): http.Server` (listen 하지 않은 상태로 반환)

테스트 공통: `beforeEach`에서 가짜 시계 store로 서버를 만들고 `listen(0)`, `afterEach`에서 `close()`. 요청은 `fetch`로 직접 보낸다.

- [ ] **Step 1: 실패하는 테스트 작성** — 설계 5절 두 표의 모든 행을 하나씩 검사한다.
```js
test('SET → GET', ...)          // PUT /keys/a {"value":"v"} → 200 {ok:true}; GET → 200 {value:'v'}
test('GET 없는 키 404 not_found', ...)
test('DEL', ...)                // 있으면 {deleted:true}, 다시 하면 {deleted:false}
test('EXPIRE와 만료', ...)       // POST /keys/a/expire {"ttl":10} → {updated:true}; 시계 +10000 → GET 404
test('SET ttl 만료', ...)        // PUT {"value":"v","ttl":1} → 시계 +1000 → GET 404
test.each(['{', 'null', '[]', '"x"'])('본문 %s → 400 bad_request', ...)
test.each([{ value: 1 }, { value: 'v', ttl: 0 }, { value: 'v', ttl: -1 }, { value: 'v', ttl: 1.5 }, { value: 'v', ttl: '10' }, { value: 'v', ttl: null }])('잘못된 SET 본문 → 400', ...)
test('EXPIRE ttl 없음·잘못됨 → 400', ...)
test('빈 키 PUT /keys/ → 400', ...)
test('깨진 인코딩 /keys/%E0%A4%A → 400', ...)
test('a%2Fb는 한 키이다', ...)    // PUT /keys/a%2Fb → GET /keys/a%2Fb 200, GET /keys/a 404
test('한글 키', ...)             // encodeURIComponent('키 1') 저장·조회
test('빈 문자열 값 GET 200 {value:""}', ...)
test('없는 경로 /foo → 404 not_found', ...)
test('PATCH /keys/a, GET /keys/a/expire → 405 method_not_allowed', ...)
test('오류 뒤에도 서버가 계속 응답한다', ...) // 깨진 JSON 다음 정상 GET 200
```

- [ ] **Step 2: 실패 확인** — `npm test -- tests/functional/server.test.js` → FAIL

- [ ] **Step 3: 구현** — 경로를 `/`로 나눈 뒤 각 조각을 `decodeURIComponent`. 본문을 모두 읽고 검증한 다음 store 함수를 한 번 동기로 호출(설계 4절). 오류 응답은 설계 5절 형식, `message`는 자유.

- [ ] **Step 4: 통과 확인** → PASS

- [ ] **Step 5: 커밋** — `feat: HTTP API 서버`

### Task 4: 클라이언트와 동시성 테스트
담당: `builder` / 강도: 기본

**Files:**
- Create: `src/client.js`
- Test: `tests/functional/concurrency.test.js`, `tests/functional/client.test.js`

**Interfaces:**
- Consumes: `createServer`, `createStore`
- Produces: `export function createClient(baseUrl: string)` → `{ set(key, value, ttl?): Promise<true>, get(key): Promise<string | null>, del(key): Promise<boolean>, expire(key, ttl): Promise<boolean> }`. 키는 `encodeURIComponent`로 보낸다. 2xx·404 외 응답과 네트워크 오류는 예외를 던진다.

- [ ] **Step 1: 실패하는 테스트 작성**
```js
// client.test.js
test('네 명령 왕복', ...)
test('/·?·# 가 든 키', ...)            // 'a/b?c#d' set → get 일치
test('서버가 꺼져 있으면 예외', ...)     // 닫힌 포트 → rejects
// concurrency.test.js (requirements 동시성 1~3)
test('클라이언트 200개가 키 2,000개 동시 SET 후 전부 조회', ...) // 리사이즈가 여러 번 일어남
test('같은 키 동시 SET 100개 → 최종 값은 보낸 값 중 하나', ...)
test('만료 키 조회와 새 SET이 겹쳐도 새 값이 남는다', ...)      // 가짜 시계로 만료 → Promise.all([get, set new]) → get === new
```

- [ ] **Step 2: 실패 확인** — `npm test -- tests/functional` → FAIL

- [ ] **Step 3: 구현** — `fetch` 사용.

- [ ] **Step 4: 통과 확인** — `npm test` → PASS

- [ ] **Step 5: 커밋** — `feat: 클라이언트와 동시성 테스트`

### Task 5: 데모 원본과 데모 API (cache-aside)
담당: `builder` / 강도: 기본

**Files:**
- Create: `demo/fake-db.js`, `demo/api-server.js`
- Test: `tests/functional/demo.test.js`

**Interfaces:**
- Consumes: `createClient`
- Produces:
  - `export function createFakeDb({ users, delayMs = 100 })` → `{ getUser(id): Promise<object | null>, updateUser(id, fields): Promise<object | null>, get calls(): number }` — `calls`는 두 함수 호출 합계
  - `export function createApiServer({ db, cache = null, ttl = 60 }): http.Server` — `cache`는 `createClient` 결과, `null`이면 캐시 미사용

- [ ] **Step 1: 실패하는 테스트 작성** (`delayMs: 0`, mini-redis는 가짜 시계 store로 띄운다)
```js
test('첫 GET은 miss(원본 1회), 두 번째는 hit(원본 그대로 1회)', ...)
test('캐시에는 user:<id> 키로 JSON 문자열이 들어간다', ...)
test('PUT 뒤 GET은 새 값, 원본 다시 읽음', ...)
test('ttl 60초 지나면 원본에서 다시 읽음', ...)   // mini-redis 시계 +60000
test('없는 사용자 GET·PUT → 404, 캐시에 남지 않음', ...)
test('mini-redis가 꺼져도 GET·PUT은 원본으로 200', ...)
test('cache: null이면 매번 원본', ...)
test('캐시 빈 상태에서 같은 GET 100개 동시 → 원본 호출 수를 기록만 한다', ...) // 기대값은 1 이상 100 이하. 방지 기능 없음(설계 10-5)
```

- [ ] **Step 2: 실패 확인** — `npm test -- tests/functional/demo.test.js` → FAIL

- [ ] **Step 3: 구현** — 설계 6절. 캐시 호출은 try/catch로 감싸 실패 시 건너뛴다.

- [ ] **Step 4: 통과 확인** — `npm test` → PASS

- [ ] **Step 5: 커밋** — `feat: 데모 API와 cache-aside`

### Task 6: 성능 비교 스크립트
담당: `builder` / 강도: 기본

**Files:**
- Create: `bench/compare.js`

**Interfaces:**
- Consumes: `createServer`, `createStore`, `createClient`, `createFakeDb`, `createApiServer`

- [ ] **Step 1: 구현** — 설계 7절. 원본 지연 100ms, 요청 100개(V8), 캐시 ttl 60초. 미사용/사용 두 경우를 포트 0으로 띄우고 같은 요청을 보낸 뒤 `console.table`로 요청 수, 총 시간, 평균 응답 시간, 원본 호출 횟수, 캐시 hit 비율(`1 - 원본 호출 / 요청 수`)을 출력한다. 이어서 빈 캐시에 같은 요청 100개 동시 전송 → 원본 호출 횟수 출력. 끝나면 서버를 모두 닫고 종료한다. 한 함수가 한 화면 안에 들어오게 쓴다.

- [ ] **Step 2: 실행 확인** — `npm run bench` → 종료 코드 0, 표 2개 출력, 캐시 사용 쪽 원본 호출 횟수 10 (사용자 10명)

- [ ] **Step 3: 전체 확인** — `npm test` → 종료 코드 0

- [ ] **Step 4: 커밋** — `feat: 캐시 사용 유무 성능 비교`

## 작업 담당과 병렬
- 각 Task의 커밋 단계는 `verifier` 검증을 통과한 뒤 메인 세션이 한다.
- 강도 높음(Task 1, 2): 해시 테이블과 만료는 모든 기능이 기대는 핵심 로직이라 `model: opus`, `effort: high`로 호출한다.
- Task 1 → 2 → 3 → 4 순서. Task 5는 Task 4 뒤. Task 6은 Task 5 뒤.
