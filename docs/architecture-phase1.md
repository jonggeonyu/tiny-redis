# mini-redis 1단계 설계

요구사항: [requirements.md](requirements.md). "제안값"은 설계자가 정하고 사용자가 2026-11-09에 승인한 값이다. 10절에 모아 두었다.

## 1. 전체 구조
```
[클라이언트/데모 API] --HTTP(JSON)--> [server.js] --동기 호출--> [store.js] --> [hashtable.js]
                                                    (명령·만료)        (저장)
```

| 파일 | 역할 |
|---|---|
| `src/hashtable.js` | 해시 테이블. `get`, `set`, `delete`, `size`. 만료를 모른다. |
| `src/store.js` | 명령 `set/get/del/expire`. 만료 시각 관리(lazy). 시계 `now()`를 주입받는다. |
| `src/server.js` | HTTP 라우팅, 입력 검증, JSON 응답. `createServer({ store })` 형태로 만들어 테스트에서 포트 0으로 띄운다. |
| `src/client.js` | mini-redis HTTP API를 감싼 작은 클라이언트(`fetch` 사용). 데모 API와 테스트가 쓴다. |
| `demo/fake-db.js` | 느린 원본. 메모리 객체 + 지연(`setTimeout`). 호출 횟수를 센다. |
| `demo/api-server.js` | 데모 API. `GET /users/:id`(cache-aside), `PUT /users/:id`(원본 수정 + `DEL`). 캐시 사용 여부를 옵션으로 받는다. |
| `bench/compare.js` | 성능 비교. 결과를 표로 출력한다. |
| `tests/unit/`, `tests/functional/` | 단위 테스트, 기능 테스트 |

파일 이름과 내부 함수 이름은 구현자가 바꿔도 된다.

## 2. 해시 테이블 (`hashtable.js`)
- **충돌 처리: 체이닝.** 각 버킷은 `{ key, value, next }` 노드의 연결 리스트이다. 삭제가 단순하고 실제 Redis `dict`와 같다.
- **해시 함수: FNV-1a 32비트** (`Math.imul`로 32비트 곱셈, `>>> 0`으로 부호 없는 정수). 모든 글자를 섞는다.
- **칸 번호**: `hash % capacity`
- **리사이즈**: `size / capacity > 1`이 되면 capacity를 2배로 늘리고 모든 노드를 새 배열에 다시 넣는다(한 번에 rehash). 줄이기는 하지 않는다.
- **초기 capacity: 8.** 테스트에서 리사이즈가 자주 일어나게 하려는 값이다.
- 키는 문자열, 값은 아무 값이나 저장한다(`store`가 `{ value, expireAt }`을 넣는다).

## 3. 저장소와 만료 (`store.js`)
- 엔트리: `{ value: string, expireAt: number | null }` (밀리초)
- `isExpired(e) = e.expireAt !== null && now() >= e.expireAt`
- 모든 명령은 키를 찾은 뒤 먼저 만료를 확인한다. 만료되었으면 그 자리에서 삭제하고 "없음"으로 취급한다(**lazy expiration**).

| 명령 | 동작 | 반환 |
|---|---|---|
| `set(key, value, ttlSec?)` | 덮어쓴다. ttl이 없으면 `expireAt = null`(기존 만료 제거) | `true` |
| `get(key)` | 만료 확인 후 값 | `string` 또는 `null` |
| `del(key)` | 만료 확인 후 삭제 | 지웠으면 `true`, 없었으면 `false` |
| `expire(key, ttlSec)` | 만료 확인 후 있으면 `expireAt = now() + ttl*1000` | 설정했으면 `true`, 없으면 `false` |

- 주기적 만료(active expiration)는 1단계에 넣지 않는다. 요구사항은 "만료된 값을 요청받았을 때"이고, lazy로 충족한다. 메모리 정리는 제안으로 남긴다.

## 4. 동시성
- Node는 JavaScript를 단일 스레드 이벤트 루프로 실행한다. 한 요청의 동기 코드가 끝날 때까지 다른 요청은 끼어들지 못한다.
- **규칙: `store`와 `hashtable`의 모든 함수는 동기 함수이다. `async`/`await`를 쓰지 않는다.**
  - `server.js`는 요청 본문을 비동기로 다 읽은 **뒤에** `store`의 함수를 한 번 동기로 호출한다.
  - 그래서 확인(만료 검사)과 변경(삭제·저장) 사이에 다른 요청이 끼어들 틈이 없다.
- 같은 키에 동시 `SET`이 오면 이벤트 루프가 처리한 순서대로 적용되고, 마지막에 처리된 값이 남는다.
- 동시성 테스트(requirements의 1~4)는 기능 테스트에서 `Promise.all`로 실제 HTTP 요청을 동시에 보낸다.
- **보장 범위**: 명령 하나의 확인과 변경은 원자적이다. 여러 명령에 걸친 동작은 보장하지 않는다.
- **한계(갱신 손실)**: 클라이언트가 `GET`으로 읽고 값을 바꿔 `SET`하면, 그 사이에 다른 요청이 끼어들어 한쪽의 갱신이 사라질 수 있다.
  - 예: A와 B가 둘 다 10을 읽고 각각 11을 `SET`한다. 의도는 12인데 결과는 11이다.
  - 서버의 명령에는 읽고-바꾸기가 없으므로 서버 안에서는 생기지 않는다. 클라이언트가 명령을 조합할 때의 한계이다.
  - 6절의 cache-aside 경쟁 조건도 같은 종류이다.
- **해결 방향(구현하지 않음)**: 명령을 `SET`, `GET`, `DEL`, `EXPIRE`로 제한했으므로 제안만 한다.
  - `INCR`처럼 읽기와 쓰기를 서버 명령 하나로 만든다. store에 동기 함수를 하나 더하면 된다.
  - `WATCH`/`MULTI`처럼 읽은 뒤 값이 바뀌었으면 실패시키고 클라이언트가 다시 시도하게 한다(낙관적 락).

## 5. HTTP API (`server.js`) — 형식 전체가 제안값
모든 요청·응답 본문은 JSON. 키는 URL 경로에서 `decodeURIComponent`로 읽는다. 경로를 `/`로 먼저 나눈 뒤 디코딩하므로, `/`·`?`·`#`이 든 키는 클라이언트가 `encodeURIComponent`로 보내면 된다(`client.js`가 처리).

| 명령     | 요청                                                                      | 성공 응답                                                         |
| ------ | ----------------------------------------------------------------------- | ------------------------------------------------------------- |
| SET    | `PUT /keys/:key` 본문 `{ "value": "문자열", "ttl": 10 }` (`ttl` 생략 가능, 단위 초) | `200 { "ok": true }`                                          |
| GET    | `GET /keys/:key`                                                        | `200 { "value": "..." }` / 없으면 `404 { "error": "not_found" }` |
| DEL    | `DELETE /keys/:key`                                                     | `200 { "deleted": true \| false }`                            |
| EXPIRE | `POST /keys/:key/expire` 본문 `{ "ttl": 10 }`                             | `200 { "updated": true \| false }`                            |

| 오류 | 응답 |
|---|---|
| 잘못된 JSON, `value`가 문자열 아님, `ttl`이 1 이상의 정수 아님, 빈 키 | `400 { "error": "bad_request", "message": "..." }` |
| 없는 경로 | `404 { "error": "not_found" }` |
| 지원하지 않는 메서드 | `405 { "error": "method_not_allowed" }` |

- 쿼리 문자열(`?…`)은 경로에서 떼어 내고 무시한다. `?`가 든 키는 인코딩해서 보낸다.
- 경로 모양이 다르면(`/keys`, `/keys/a/x`) `404`, 빈 키(`/keys/`)는 `400`이다.
- 제약: 키 `.`과 `..`은 HTTP로 다룰 수 없다. URL 정규화가 `%2e`까지 점 경로로 바꾸기 때문이다(2026-11-09 최종 리뷰에서 확인).
- `client.js`는 `GET`의 `404`만 "없음"(`null`)으로 받는다. `SET`·`DEL`·`EXPIRE`의 `404`와 형식이 맞지 않는 응답은 예외로 던진다(2026-11-09 최종 리뷰 반영: 잘못된 경로가 조용히 성공으로 보이는 것을 막는다).
- 실행: `npm start`(`bin/start.js`)가 실제 시계 store로 서버를 띄운다. 포트는 6380이고 환경 변수 `PORT`로 바꿀 수 있다(2026-11-09 사용자 결정, 10절 18번). 테스트는 포트 0(빈 포트 자동 선택)을 쓴다.
- HTTP는 Node 내장 `http` 모듈로 만든다. 프레임워크를 쓰지 않는다.

## 6. 데모 API와 캐시
- `GET /users/:id`: mini-redis `GET user:<id>` → hit면 그대로 응답. miss면 `fake-db` 조회 → `SET user:<id> <JSON 문자열> ttl` → 응답.
- `PUT /users/:id`: `fake-db` 수정 → mini-redis `DEL user:<id>` (즉시 무효화).
- 캐시 ttl은 안전망이다. mini-redis 호출이 실패하면 캐시를 건너뛰고 원본으로 응답한다.
- 응답 형식: `200 { "id", "name", ... }`, 없는 사용자 `404`.
- 오류 응답은 mini-redis 서버와 같은 `{ "error": ... }` 형식이고, 헤더도 같다(`Content-Type: application/json; charset=utf-8`). 구현 중 메인 세션이 정했다(2026-11-09).
  - 잘못된 id 인코딩, `PUT` 본문이 JSON 객체가 아님: `400 bad_request`
  - 없는 경로: `404 not_found`. 있는 경로의 다른 메서드: `405 method_not_allowed`
  - 예상하지 못한 예외(원본 오류 등): `500 internal`
- 알려진 한계: miss로 옛 값을 읽은 `GET`이 `PUT`의 `DEL`보다 늦게 `SET`하면 옛 값이 최대 ttl(60초)까지 남는다. ttl이 이 경우의 안전망이다. 1단계에서는 막지 않는다.

## 7. 성능 비교 (`bench/compare.js`)
- 같은 데모 API를 **캐시 미사용 / 사용** 두 가지로 띄우고, 같은 요청 묶음을 보낸다.
- 출력 표: 요청 수, 총 시간, 평균 응답 시간, 원본(DB) 호출 횟수, 캐시 hit 비율
- 추가 시나리오: 캐시가 빈 상태에서 같은 요청 100개 동시 전송 → 원본 호출 횟수 출력(cache stampede 측정)
- 실행: `npm run bench`
- 코드는 발표 화면에 띄울 수 있게 한 함수가 한 화면 안에 들어오도록 쓴다.

## 8. 테스트
- 단위(Vitest): `hashtable`(충돌 키, 체인 중간 삭제, 리사이즈 전후 조회), `store`(가짜 시계로 만료 직전·직후, 덮어쓰기 시 만료 제거, 만료 키에 `expire`/`del`)
- 기능(Vitest): 실제 HTTP로 서버를 띄워 API 형식·오류 응답, 동시성 시나리오 1~3, 데모 API의 hit/miss·`PUT` 뒤 새 값·mini-redis 중단 시 원본 응답
- 만료 기능 테스트는 서버에 가짜 시계를 주입해 실제로 기다리지 않는다.
- 명령: `npm test`

## 9. 2단계 검토: 영속성 (구현 안 함)
| 방식         | 장점              | 단점                       |
| ---------- | --------------- | ------------------------ |
| 스냅샷(RDB)   | 구현이 쉽고 복구가 빠르다  | 마지막 저장 이후 변경을 잃는다        |
| 명령 로그(AOF) | 쓰기마다 기록해 유실이 적다 | 파일이 커진다, 재시작 시 재실행이 필요하다 |

- AOF를 넣는다면 4절 규칙을 지킨다. 테이블을 먼저 동기로 바꾸고, 기록은 큐에 넣어 나중에 쓴다.
- 만료는 상대 시간이 아니라 절대 시각(`expireAt`)으로 기록해야 재시작 후에도 맞다.

## 10. 제안값 (2026-11-09 승인)
설계자가 정하고 사용자가 승인한 값이다. 바꾸면 위 절도 함께 바뀐다.
1. 5절 HTTP API 형식 전체 (경로, 본문 필드 이름, 상태 코드, ttl 단위 = 초, ttl은 1 이상 정수)
2. ttl 없이 덮어쓰는 `SET`은 기존 만료를 없앤다 (실제 Redis와 같음)
3. 체이닝, FNV-1a, 로드 팩터 1에서 2배 확장, 초기 capacity 8, 한 번에 rehash
4. 1단계는 lazy expiration만. active expiration은 제안으로만 남김
5. cache stampede는 측정만 하고 방지 기능은 만들지 않음
6. mini-redis가 꺼지면 데모 API는 원본으로 응답
7. 프레임워크 없이 Node 내장 `http`, 개발 의존성은 Vitest와 jsdoc-to-markdown
   - 2026-11-09 변경: 사용자 요청으로 jsdoc-to-markdown을 추가했다. 함수 설명을 JSDoc에서 `docs/api.md`로 자동 생성해 발표에서 주요 함수의 동작을 설명하는 자료로 쓴다(요구사항의 "주요 함수의 동작을 설명"). 실행 의존성은 여전히 없다.
8. 데모 원본 지연 100ms, 성능 비교 요청 수 100개, 데모 캐시 ttl 60초

구현 계획(`docs/superpowers/plans/2026-11-09-mini-redis-phase1.md`)에서 정하고 2026-11-09에 승인한 값:

9. 해시 함수 입력은 `charCodeAt`로 읽은 UTF-16 코드 단위
10. 키 디코딩 실패(`%E0%A4%A` 등)와 JSON 객체가 아닌 본문(`null`, `[]`, `"x"`)은 `400 bad_request`
11. `ttl: null`은 생략이 아니라 잘못된 값으로 보고 `400`
12. 있는 경로에 다른 메서드(`GET /keys/a/expire`, `PATCH /keys/a`)는 `405`
13. 데모 `PUT /users/:id` 본문은 바꿀 필드의 JSON 객체, 응답은 `200` + 수정된 사용자, 없는 사용자는 `404`
14. 없는 사용자(`404`)는 캐시에 저장하지 않는다
15. 동시성 테스트 규모: 클라이언트 200개가 키 2,000개 `SET`
16. 성능 비교 요청 100개는 사용자 10명을 차례로 반복 조회
17. 응답 헤더 `Content-Type: application/json; charset=utf-8`

1단계 구현 뒤 2026-11-09에 사용자가 정한 값:

18. 실행 명령 `npm start`, 실행 포트 6380(환경 변수 `PORT`로 바꿀 수 있음)

## 11. 제안만 하는 기능 (요구사항에 없음)
- 점진적 rehash, active expiration(표본 추출), `TTL`/`EXISTS`/`FLUSHALL` 명령, cache stampede 방지(같은 키 요청 합치기), 메모리 한도와 삭제 정책
