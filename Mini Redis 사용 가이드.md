# Mini Redis 사용 가이드

이 서버는 키와 바이트 값을 메모리에 보관하고 HTTP로 저장·조회·삭제합니다. 밀리초 TTL을 지정할 수 있습니다. 데이터는 서버 프로세스의 메모리에 있으므로 종료하면 사라집니다.

Windows/MinGW에서 실제 서버와 HTTP 호출을 검증했습니다. 최종 통합 검증 결과는 문서 끝에 기록합니다.

## 빌드와 실행

Windows에서 MinGW-w64의 GCC, `mingw32-make`, Python 3가 필요합니다. GCC와 make가 PATH에 있어야 합니다. 생성된 llhttp 9.4.4 소스가 포함되어 있어 별도 패키지 설치는 필요하지 않습니다.

프로젝트 루트의 PowerShell에서 실행하세요.

```powershell
mingw32-make server
.\build\mini-redis.exe
```

기본 주소는 `http://127.0.0.1:8080`입니다. 다른 포트는 `.\build\mini-redis.exe 8081`처럼 지정합니다. 실행한 콘솔에서 Ctrl+C를 누르면 이벤트 루프가 연결·저장소를 해제하고 종료합니다. 외부 공개나 방화벽 변경은 필요하지 않습니다.

실행 중인 서버 때문에 Windows가 기본 실행 파일의 덮어쓰기를 막으면 별도 이름으로 빌드할 수 있습니다. 이번 자동 확장 구현은 아래 별도 바이너리로 최종 검증했습니다. 기존 서버와 함께 실행하려면 사용하지 않는 포트를 지정하세요.

```powershell
mingw32-make server SERVER=build/mini-redis-rehash.exe
.\build\mini-redis-rehash.exe 8081
```

```powershell
mingw32-make unit-test
mingw32-make server-unit-test
mingw32-make functional-test
mingw32-make timeout-test
# 위 검증을 함께 실행합니다.
mingw32-make check
```

`unit-test`는 기존 해시 테이블, `server-unit-test`는 TTL·HTTP·TCP 진행 로직을 검사합니다. `functional-test`는 별도 서버 프로세스를 실행해 실제 HTTP/TCP로 호출합니다. `timeout-test`는 운영 바이너리와 별도로 짧은 제한값의 서버를 빌드해 실제 수신·유휴 시간 초과를 검사합니다. 운영 서버 실행 콘솔과 테스트 콘솔을 구분하세요.

## HTTP API

HTTP/1.1 요청에는 비어 있지 않은 Host 헤더가 정확히 하나 있어야 합니다. 아래 `curl.exe` 명령은 이를 자동으로 전송합니다. `-i`는 상태와 헤더도 출력합니다.

| 메서드·경로 | 요청 | 성공 응답 |
|---|---|---|
| `GET /health` | 본문 없음 | `200`, `{"ok":true}` |
| `PUT /keys/{key}` | Content-Length가 있는 원시 본문, 선택적 X-TTL-Ms | `200`, `{"ok":true}` |
| `GET /keys/{key}` | 본문 없음 | `200`, 저장된 원시 바이트 |
| `DELETE /keys/{key}` | 본문 없음 | `200`, `{"ok":true}` |

먼저 연결을 확인한 뒤 `greeting`에 문자열을 저장하고 조회합니다. PUT은 같은 키가 있으면 값을 교체합니다.

```powershell
curl.exe -i http://127.0.0.1:8080/health
curl.exe -i -X PUT --data-binary 'hello' http://127.0.0.1:8080/keys/greeting
curl.exe -i http://127.0.0.1:8080/keys/greeting
```

삭제 후 다시 조회하면 `404`와 `{"error":"not_found"}`가 반환됩니다.

```powershell
curl.exe -i -X DELETE http://127.0.0.1:8080/keys/greeting
curl.exe -i http://127.0.0.1:8080/keys/greeting
```

빈 값도 저장할 수 있습니다. 본문이 비었더라도 PUT의 Content-Length는 필수입니다.

```powershell
curl.exe -i -X PUT -H 'Content-Length: 0' http://127.0.0.1:8080/keys/empty
```

### TTL

`X-TTL-Ms`는 양의 10진 정수입니다. 0·음수·잘못된 형식·uint64 범위 초과·현재 시각과 덧셈 오버플로는 거부합니다. HTTP에서 헤더 생략은 만료 없음을 뜻합니다.

아래 예제는 100밀리초 TTL로 저장한 뒤 만료를 확인합니다. 첫 조회가 만료를 발견하면 키를 지우고 `{"error":"expired"}`를 반환합니다. 다음 조회는 `{"error":"not_found"}`입니다. 두 응답 모두 `404`입니다.

```powershell
curl.exe -i -X PUT -H 'X-TTL-Ms: 100' --data-binary 'temporary' http://127.0.0.1:8080/keys/session
Start-Sleep -Milliseconds 200
curl.exe -i http://127.0.0.1:8080/keys/session
curl.exe -i http://127.0.0.1:8080/keys/session
```

덮어쓰기에는 새 TTL이 적용됩니다. TTL 없이 덮어쓰면 기존 TTL이 제거됩니다. DELETE도 접근 시 만료를 발견하면 같은 방식으로 `expired`를 한 번 반환합니다. PUT은 만료된 기존 값을 새 값으로 대체합니다.

### 바이너리와 키 인코딩

서버는 요청 Content-Type에 따라 값을 변환하지 않습니다. 조회 성공은 `application/octet-stream`, 다른 응답은 `application/json`입니다. 모든 응답에 `Cache-Control: no-store`와 정확한 Content-Length가 붙습니다.

다음은 NUL을 포함하는 작은 파일을 만들고 그대로 저장·다운로드하는 예제입니다. 두 SHA-256이 같으면 같은 바이트를 받았습니다.

```powershell
[IO.File]::WriteAllBytes((Join-Path $PWD 'sample.bin'), [byte[]](0, 1, 255, 65))
curl.exe -i -X PUT -H 'Content-Type: application/octet-stream' --data-binary '@sample.bin' http://127.0.0.1:8080/keys/file
curl.exe --fail --output downloaded.bin http://127.0.0.1:8080/keys/file
Get-FileHash sample.bin, downloaded.bin -Algorithm SHA256
```

curl이 큰 업로드에 `Expect: 100-continue`를 추가하는 환경에서는 `-H 'Expect:'`를 붙여 해당 헤더를 제거하세요. 이 서버는 Expect를 지원하지 않습니다.

키는 `/keys/` 뒤의 경로 바이트를 퍼센트 디코딩한 결과입니다. 빈 키·NUL·잘못된 `%` 인코딩은 거부합니다. 디코딩은 한 번만 하므로 `%252F`는 문자 `%2F`가 되고 `%2F`는 `/`가 됩니다. `+`는 공백으로 바꾸지 않습니다. 공백은 `%20`, 키에 포함할 `?`는 `%3F`, `#`는 `%23`으로 전송하세요. URL 자체의 query·fragment는 거부합니다.

한글 등은 UTF-8로 인코딩한 뒤 퍼센트 인코딩하세요. 아래는 `user name` 키를 사용합니다.

```powershell
curl.exe -i -X PUT --data-binary 'Alice' http://127.0.0.1:8080/keys/user%20name
curl.exe -i http://127.0.0.1:8080/keys/user%20name
```

## 공개 C 함수

기존 해시 테이블 함수와 소유권은 [README.md](README.md)를 참고하세요. 아래는 이번 서버 계층에 추가한 함수입니다. 각 헤더가 공개 계약이며 내부 static 함수는 직접 호출하지 않습니다.

### 저장소: `include/store.h`

| 함수 | 핵심 인자와 반환값 | 소유권 |
|---|---|---|
| `store_create(bucket_count, &store)` | 양의 초기 버킷 수. 자동 확장. 성공 STORE_OK, 실패 상태 코드 | 성공한 store는 호출자가 destroy |
| `store_destroy(store)` | 저장소 전체 해제, NULL 허용, 반환 없음 | 키·값·TTL·테이블 모두 해제 |
| `store_put(store, key, key_len, value, value_len, ttl_ms, now_ms)` | TTL 0은 만료 없음. 단조 시각 ms. 성공 STORE_OK | 키·값 복사, 실패 시 유효한 기존 값·TTL 보존 |
| `store_get(store, key, key_len, now_ms, &value, &length)` | STORE_OK, STORE_NOT_FOUND, STORE_EXPIRED 등 | 성공 포인터는 borrowed const, 다음 변경 연산 전까지만 유효 |
| `store_delete(store, key, key_len, now_ms)` | STORE_OK, STORE_NOT_FOUND, STORE_EXPIRED 등 | 해당 엔트리 해제 |
| `store_status_message(status)` | 상태에 대응하는 문자열, 알 수 없는 값은 unknown | 정적 문자열, 해제 금지 |

C 저장소는 빈 키도 허용하지만 HTTP API에서는 거부합니다. 포인터는 길이가 0일 때만 NULL을 허용합니다. TTL·시각은 uint64_t이며 `now_ms >= 저장 시각 + ttl_ms`에서 만료됩니다. get도 만료 키를 삭제할 수 있으므로 반환 포인터를 장기간 보관하지 마세요. 실패한 get의 출력 인자는 사용하지 마세요.

다음은 100ms TTL을 지정하고 만료 경계를 확인하는 호출 예제입니다. 실제 서버는 `GetTickCount64()`를 사용하며 단위 테스트는 시각을 직접 전달합니다.

```c
#include "store.h"
#include <stdio.h>

store_t *store = NULL;
if (store_create(16, &store) == STORE_OK) {
    store_status result = store_put(store, "k", 1, "v", 1, 100, 1000);
    const void *value = NULL;
    size_t length = 0;
    if (result == STORE_OK) {
        result = store_get(store, "k", 1, 1099, &value, &length);
        /* STORE_OK: value는 'v' 한 바이트입니다. */
    }
    result = store_get(store, "k", 1, 1100, &value, &length);
    printf("%s\n", store_status_message(result)); /* expired */
    result = store_delete(store, "k", 1, 1100);   /* not_found */
    store_destroy(store);
}
```

다른 오류 상태는 `STORE_INVALID_ARGUMENT`, `STORE_NO_MEMORY`, `STORE_OVERFLOW`, `STORE_RANDOM_SOURCE`입니다. 저장소 자체는 스레드 안전하지 않으며 서버의 단일 이벤트 루프가 순차 호출합니다.

### HTTP 파서: `include/server/http_api.h`

| 함수 | 용도·반환값 | 소유권 |
|---|---|---|
| `http_connection_create()` | 새 연결 파서 생성, 실패 NULL | destroy 필요 |
| `http_connection_destroy(connection)` | 파서와 수집 중인 요청 해제 | 반환된 응답 버퍼는 별도로 free |
| `http_connection_feed(connection, data, length, &consumed, store, now_ms, &response)` | 0 미완성, 1 응답 준비, -1 치명적 할당/호출 실패 | 입력은 호출자 소유. 응답 data는 복사된 버퍼로 호출자가 free |
| `http_connection_reset(connection)` | 이전 응답을 모두 전송한 후 다음 요청 준비 | 이전 요청 상태 해제, 응답 data는 별도 관리 |

feed는 한 메시지까지만 처리합니다. `consumed` 뒤의 바이트는 호출자가 보관하고, 응답 전체를 전송한 다음 reset하고 다시 전달해야 합니다. `response.close_after`가 참이면 다음 요청을 처리하지 말고 닫으세요. -1이면 연결을 종료하세요.

다음은 완성된 health 요청 하나를 파서에 전달하는 예제입니다. 소켓 연결에서는 전송 위치를 유지하며 부분 송신을 처리해야 합니다.

```c
#include "server/http_api.h"
#include <stdlib.h>
#include <string.h>

http_connection_t *connection = http_connection_create();
const char request[] = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
http_response response = {0};
size_t consumed = 0;
if (connection != NULL) {
    int result = http_connection_feed(connection, request, strlen(request),
                                      &consumed, store, 1000, &response);
    if (result == 1) {
        /* response.data의 response.length 바이트를 모두 전송합니다. */
        int close_after = response.close_after;
        free(response.data);
        if (!close_after) {
            http_connection_reset(connection);
        }
    }
    http_connection_destroy(connection);
}
```

### TCP 서버: `include/server/tcp_server.h`

`tcp_server_run(uint16_t port)`는 로컬 주소에 바인딩하고 종료 요청까지 실행합니다. 0은 정상 종료, 1은 초기화·소켓·이벤트 루프 오류입니다. Winsock, 저장소와 모든 연결을 함수가 소유하고 종료 시 해제합니다.

```c
#include "server/tcp_server.h"

int main(void)
{
    return tcp_server_run(8080);
}
```

Winsock을 포함하는 번역 단위는 관련 시스템 헤더보다 먼저 이 헤더를 포함하고 FD_SETSIZE를 256으로 일치시키세요. 서버는 기존 main.c를 포함해 `mingw32-make server`로 빌드할 수 있습니다.

## 요청 처리와 제한

```text
클라이언트 HTTP → Winsock 수신 → llhttp 증분 파싱·검증
→ 요청 완료 → TTL 저장소 → SipHash 해시 테이블
→ 연결 소유 응답 복사 → 부분 송신 → 다음 요청
```

응답 전송이 끝나기 전에는 같은 연결의 다음 요청을 실행하지 않습니다. 파이프라이닝으로 이미 받은 후속 바이트는 보관했다가 다음 소켓 읽기 이벤트 없이도 처리합니다. 다른 연결의 PUT/DELETE가 진행되어도 전송 중인 응답 데이터는 바뀌지 않습니다.

| 항목 | 제한 |
|---|---|
| 요청 줄 포함 헤더 | 8 KiB |
| 디코딩된 키 | 1 KiB |
| 본문 | 1 MiB |
| 동시 클라이언트 | 128개, 초과 연결은 즉시 종료 |
| 미처리 수신 데이터 | 연결당 64 KiB, 가득 차면 읽기 중단 |
| 요청 수신 / 응답 송신 | 각각 시작 후 10초, 조금씩 진행해도 연장 안 함 |
| 요청 사이 유휴 연결 | 30초 |
| 루프별 연결당 작업 | 읽기·쓰기 각각 64 KiB, 요청 실행 1개 |
| 루프별 연결 수락 / select 대기 | 16개 / 최대 100ms |

| HTTP 상태 | 의미 |
|---|---|
| 400 | 잘못된 요청·키·TTL·Host·길이·지원하지 않는 전송 방식 등 |
| 404 | not_found, expired 또는 route_not_found |
| 405 | 경로에서 지원하지 않는 메서드, Allow 헤더 제공 |
| 413 | 본문 초과 |
| 414 | 디코딩된 키 초과 |
| 431 | 요청 헤더 초과 |
| 500 | 내부 처리 실패 |
| 505 | 지원하지 않는 HTTP 버전 |

잘못된 요청은 오류 응답 후 닫습니다. GET/DELETE 본문, PUT 이외의 TTL 헤더, Expect, Transfer-Encoding, 중복 Content-Length, HTTP upgrade, query·fragment는 지원하지 않습니다. 연결·요청·응답의 시간 초과는 연결을 닫으며 별도 HTTP 오류 응답을 보장하지 않습니다. 정상 응답은 연결을 유지하고 `Connection: close` 요청은 응답 후 닫습니다.

GET/DELETE의 Content-Length: 0은 허용합니다. 앞선 응답 전송 때문에 보류 중인 파이프라인 요청은 실제 처리를 시작할 때 수신 제한 시간이 시작됩니다. 따라서 이미 버퍼에 도착한 후속 바이트에는 앞선 송신 대기와 수신 제한이 합쳐질 수 있습니다. 현재 활성 요청의 최초 수신과 송신 제한 자체는 진행에 따라 연장하지 않습니다.

정상 정리 종료는 실행 콘솔의 Ctrl+C·Ctrl+Break로 확인했습니다. 콘솔 창 닫기·운영체제 종료·프로세스 강제 종료에서는 Windows가 자원을 회수하지만 이벤트 루프의 정리 완료는 보장하지 않습니다.

## 웹서비스에서 캐시로 사용하기

웹서비스의 HTTP 클라이언트가 `GET /keys/{key}`를 호출합니다. 200이면 받은 바이트를 사용합니다. `not_found` 또는 `expired`이면 원본 API나 DB에서 값을 가져오고, `PUT`과 적절한 X-TTL-Ms로 저장한 뒤 사용자에게 반환합니다. 원본을 변경한 뒤에는 DELETE로 캐시를 무효화하거나 새 값으로 PUT하세요.

동시에 여러 클라이언트가 캐시 미스를 발견할 때 원본 요청을 하나로 합치는 기능은 제공하지 않습니다. 외부 서비스의 오류와 캐시 연결 오류는 구분하고, 캐시 오류 때 원본을 사용할지는 웹서비스에서 결정하세요.

해시 테이블은 신규 삽입으로 엔트리 수가 현재 버킷 수를 초과하면 정확히 2배로 확장하고 기존 엔트리를 한 번에 재배치합니다. 부하율 1은 기본 정책이며 최적 성능을 주장하는 값은 아닙니다. 교체는 확장하지 않고 삭제는 축소하지 않습니다. C 저장소 생성 인자는 초기 용량이며, 서버의 초기 용량은 1024로 유지합니다. `hash_table_bucket_count()`는 현재 용량을 반환합니다.

확장 시 신규 엔트리·키·값과 새 배열을 모두 준비한 뒤 기존 next 연결만 바꿉니다. 실패 시 기존 값·개수·버킷 수·체인과 성공 출력 인자를 보존합니다. 전체 이동은 단일 이벤트 루프에서 다른 요청을 지연시킬 수 있고, 기존 B개·새 2B개 버킷 배열을 동시에 보유하는 추가 메모리가 필요합니다.

현재 한계는 점진적 rehash·자동 축소 없음, 접근하지 않은 만료 키의 메모리 잔류, 전체 저장량 제한·주기 정리 없음, 단일 스레드, 영속성·인증·TLS·RESP·redis-cli 호환 없음입니다. 접근하지 않은 만료 키도 실제 보유 엔트리로 계산되어 확장을 유발할 수 있습니다. 로컬 개발용 서버이며 성능 비교는 이번 범위에서 제외했습니다. 기존 향후 계획은 `구현 설계.md`에 보존합니다.

## 검증 결과

자동 확장 추가 후 Windows/MinGW에서 `mingw32-make check SERVER=build/mini-redis-rehash.exe`가 종료 코드 0으로 완료됐습니다. 상세한 변경·실패 처리·독립 리뷰 결과는 [일괄 rehash 검증 결과.md](일괄%20rehash%20검증%20결과.md)를 참고하세요.

| 검증 | 결과 |
|---|---|
| 해시 테이블·자동 확장 | 19/19, 테스트 할당기 live 0 / error 0 |
| TTL 저장소 | 11/11, 확장 후 값·TTL 대응 및 실패 시 기존 상태 보존 |
| 실제 저장소에 연결한 HTTP | 307개 검사 통과 |
| TCP 진행 로직 | 11/11 |
| 실제 Windows 서버 기능 | 9/9, 초기 1024를 넘는 1025개 키 검증 포함 |
| 짧은 제한값으로 실제 수신·유휴 시간 초과 | 5/5 |

Claude가 TTL·HTTP를 구현하고 새 Codex 리뷰어들이 검토했습니다. Codex가 TCP와 기능 테스트를 구현하고 새 Claude 리뷰어가 검토했습니다. HTTP 리뷰에서 발견한 Upgrade 헤더 거부 누락은 원 구현자가 수정하고 독립 재검증을 통과했습니다. 상세 분담과 결과는 [서버 구현 검증 결과.md](서버%20구현%20검증%20결과.md)에 기록했습니다.

이 문서의 curl.exe PowerShell 예제 6개 블록은 빈 로컬 포트로 바꾸어 실제 실행했고, 바이너리 파일은 바이트 일치까지 확인했습니다. 저장소·HTTP C 예제도 필요한 main과 저장소 초기화로 감싸 컴파일·실행했습니다. TCP 진입점은 실제 서버 빌드·기능 테스트로 확인했습니다.

송신 시간 초과와 부분 송신·WOULDBLOCK은 OS 버퍼 상태에 의존하지 않는 단위 검사로 확인했습니다. 운영 기본 10초/30초 제한을 기다리는 장시간 테스트, ASan·Valgrind, 대규모 부하·퍼징·벤치마크는 실행하지 않았습니다. 테스트 할당기의 누수 추적은 일반 메모리 오류 검사 도구를 대신하지 않습니다.
