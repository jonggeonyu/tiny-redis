# tiny-redis (mini-redis)

해시 테이블을 직접 구현한 키-값 저장소를 HTTP API로 제공한다.
이 저장소를 캐시로 쓰는 데모 API로 기능과 성능(캐시 사용/미사용)을 검증한다.

- Node.js 22, 실행 의존성 없음. HTTP는 Node 내장 `http` 모듈만 쓴다.
- 명령: `SET`(ttl 옵션), `GET`, `DEL`, `EXPIRE`

## 빠르게 시작하기

```bash
npm install
npm start          # http://localhost:6380 (PORT 환경 변수로 변경)
```

다른 터미널에서 요청을 보낸다. 아래 예시는 macOS·Linux·Git Bash의 `curl`이다.

```bash
curl -X PUT  localhost:6380/keys/greeting -d '{"value":"hello","ttl":10}'   # {"ok":true}
curl         localhost:6380/keys/greeting                                     # {"value":"hello"}
curl -X POST localhost:6380/keys/greeting/expire -d '{"ttl":30}'              # {"updated":true}
curl -X DELETE localhost:6380/keys/greeting                                   # {"deleted":true}
```

Windows PowerShell 5.1에서는 `curl`이 `Invoke-WebRequest`의 별칭이라 `Invoke-RestMethod`를 쓴다.

```powershell
Invoke-RestMethod -Method Put -Uri http://localhost:6380/keys/greeting -Body '{"value":"hello","ttl":10}'
Invoke-RestMethod http://localhost:6380/keys/greeting
```

Node 코드에서는 클라이언트를 쓴다. 키는 자동으로 인코딩된다.

```js
import { createClient } from './src/client.js';
const redis = createClient('http://localhost:6380');
await redis.set('user:1', '{"name":"kim"}', 60); // ttl 60초
await redis.get('user:1');                       // 문자열, 없으면 null
```

## 명령어

| 명령 | 하는 일 |
|---|---|
| `npm start` | mini-redis 서버 실행 (포트 6380) |
| `npm test` | 단위·기능 테스트 (Vitest) |
| `npm run bench` | 캐시 사용/미사용 성능 비교 |
| `npm run docs` | JSDoc → [docs/api.md](docs/api.md) 생성 |

## HTTP API

| 명령 | 요청 | 성공 응답 |
|---|---|---|
| SET | `PUT /keys/:key` 본문 `{ "value": "문자열", "ttl": 10 }` (`ttl` 생략 가능, 단위 초) | `200 { "ok": true }` |
| GET | `GET /keys/:key` | `200 { "value": "..." }`, 없으면 `404 { "error": "not_found" }` |
| DEL | `DELETE /keys/:key` | `200 { "deleted": true \| false }` |
| EXPIRE | `POST /keys/:key/expire` 본문 `{ "ttl": 10 }` | `200 { "updated": true \| false }` |

- 잘못된 JSON, 문자열이 아닌 `value`, 1 이상의 정수가 아닌 `ttl`, 빈 키, 깨진 퍼센트 인코딩: `400 bad_request`
- 없는 경로: `404 not_found`. 있는 경로에 다른 메서드: `405 method_not_allowed`
- 키는 URL 경로에서 읽는다. `/`, `?`, `#`이 든 키는 `encodeURIComponent`로 보낸다(클라이언트가 처리). 키 `.`과 `..`은 쓸 수 없다.

## 구조

```
[클라이언트 / 데모 API] --HTTP(JSON)--> server.js --동기 호출--> store.js --> hashtable.js
                                         (검증)        (명령·만료)     (저장)
```

| 파일 | 역할 |
|---|---|
| [src/hashtable.js](src/hashtable.js) | 해시 테이블. 체이닝, FNV-1a, 리사이즈 |
| [src/store.js](src/store.js) | `set/get/del/expire`와 lazy 만료. 시계 `now()`를 주입받는다 |
| [src/server.js](src/server.js) | HTTP 라우팅, 입력 검증, JSON 응답 |
| [src/client.js](src/client.js) | HTTP API를 감싼 클라이언트 (`fetch`) |
| [bin/start.js](bin/start.js) | `npm start` 진입점 |
| [demo/](demo/) | 느린 원본(`fake-db.js`)과 cache-aside 데모 API(`api-server.js`) |
| [bench/compare.js](bench/compare.js) | 성능 비교 스크립트 |

## 핵심 로직

### 해시 테이블
- **충돌 처리는 체이닝**이다. 버킷마다 `{ key, value, next }` 노드의 연결 리스트를 두고, 새 노드는 체인 머리에 넣는다.
- **해시 함수는 FNV-1a 32비트**이다. `h = 0x811c9dc5`에서 시작해 글자(UTF-16 코드 단위)마다 `h ^= code; h = Math.imul(h, 0x01000193)`을 하고, 끝에 `>>> 0`으로 부호 없는 정수를 만든다. 칸 번호는 `hash % capacity`이다.
- **리사이즈**: 초기 capacity는 8이다. `size / capacity > 1`이 되면 capacity를 2배로 늘리고 모든 노드를 한 번에 다시 넣는다.

### 만료 (lazy expiration)
- 엔트리는 `{ value, expireAt }`(밀리초, 만료 없으면 `null`)이다.
- 모든 명령은 키를 찾은 뒤 **만료를 먼저 확인**한다. `now() >= expireAt`이면 그 자리에서 지우고 "없음"으로 응답한다.
- ttl 없이 `SET`으로 덮어쓰면 기존 만료가 사라진다(Redis와 같다).

### 동시성
- Node는 JavaScript를 단일 스레드 이벤트 루프로 실행한다.
- `store`와 `hashtable`의 함수는 **모두 동기 함수**이다. 서버는 요청 본문을 다 읽은 **뒤에** store 함수를 한 번 동기로 호출한다.
- 그래서 "만료 확인 → 삭제·저장" 사이에 다른 요청이 끼어들 수 없다. 테스트로 확인한 시나리오:
  - 클라이언트 200개가 키 2,000개를 동시에 SET한 뒤 전부 조회
  - 같은 키에 동시 SET 100개
  - 만료된 키 조회와 새 SET이 겹치는 경우

### 무효화
- 즉시 무효화: `DEL`
- 시간 기반 무효화: `SET`의 `ttl`, `EXPIRE`
- 데모 API는 원본을 수정한 뒤 `DEL user:<id>`로 캐시를 지운다. ttl(60초)은 안전망이다.

## 데모 API와 성능 비교

[demo/api-server.js](demo/api-server.js)는 cache-aside 방식이다.
- `GET /users/:id`: 캐시에 있으면 그대로 응답한다. 없으면 원본(100ms 지연)에서 읽고 캐시에 60초 저장한다.
- `PUT /users/:id`: 원본을 수정하고 캐시를 지운다.
- mini-redis가 꺼져 있어도 원본으로 응답한다.

`npm run bench`는 사용자 10명을 차례로 100번 조회한다. 아래는 한 번 실행한 예시 결과이다.

| | 총 시간 | 평균 응답 | 원본 호출 | hit 비율 |
|---|---|---|---|---|
| 캐시 미사용 | 약 10.9초 | 약 109ms | 100 | 0% |
| 캐시 사용 | 약 1.3초 | 약 13ms | 10 | 90% |

빈 캐시에 같은 요청 100개를 동시에 보내면 원본을 100번 호출한다(cache stampede). 1단계에서는 막지 않고 측정만 한다.

## 테스트

- 단위: [tests/unit/](tests/unit/) (해시 테이블, store와 만료)
- 기능: [tests/functional/](tests/functional/) (실제 HTTP로 서버·클라이언트·동시성·데모 API·`npm start`)
- 만료 테스트는 가짜 시계를 주입하므로 실제로 기다리지 않는다. 서버 테스트는 포트 0을 쓴다.

## 하지 않은 것

- 영속성(서버가 꺼져도 데이터 보존)은 2단계로 미뤘다. 스냅샷과 명령 로그 방식의 비교는 [설계 9절](docs/architecture-phase1.md)에 있다.
- 제안만 한 기능: active expiration, cache stampede 방지, 요청 본문 크기 제한과 타임아웃, `TTL`·`EXISTS` 같은 다른 명령

## 문서

- [docs/requirements.md](docs/requirements.md): 요구사항
- [docs/architecture-phase1.md](docs/architecture-phase1.md): 설계와 결정 이유
- [docs/api.md](docs/api.md): 함수 문서 (자동 생성)
