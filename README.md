# hjm-mini-redis — 해시 테이블 핵심

Mini Redis의 키-값 저장소 기반이 되는 C 해시 테이블이다. 이 문서는 해시 테이블 API와 일괄 자동 확장을 설명한다. TTL·HTTP·TCP 서버의 실행과 사용법은 [Mini Redis 사용 가이드.md](Mini%20Redis%20사용%20가이드.md)를 참고한다.

## 디렉터리

| 경로 | 내용 |
|---|---|
| `include/hash_table.h` | 공개 API |
| `src/hash_table/hash_table.c` | 버킷 배열, 체인, 엔트리 관리 |
| `src/hash_table/key_hash.c` | SipHash-2-4 래퍼(키 → 64비트 해시) |
| `src/hash_table/random_seed.c` | OS 난수로 테이블 시드 생성 |
| `third_party/siphash/` | SipHash 참조 구현 원본([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |
| `tests/` | 단위 테스트와 테스트 전용 할당기 |
| `examples/basic_usage.c` | 사용 예제 |
| `test.c` | 기존 Hello World(`make run`) |

## 빌드와 테스트

MinGW-w64 GCC 기준(Windows PowerShell). `make`와 `mingw32-make` 모두 사용 가능하다.

```powershell
mingw32-make unit-test     # 해시 테이블 단위 테스트 빌드 + 실행 (19개)
mingw32-make run-example   # 예제 빌드 + 실행
mingw32-make lib           # build/libhash_table.a
mingw32-make all           # 기존 test.exe + 라이브러리 + 예제 + 단위 테스트 + Windows 서버 빌드
mingw32-make check         # 저장소·HTTP·TCP 및 실제 기능·시간 제한까지 회귀 검증
mingw32-make run           # 기존 test.exe 실행
mingw32-make clean         # test.exe와 build/ 삭제
```

Windows에서는 `-lbcrypt`를 링크한다. Linux(devcontainer)에서는 `/dev/urandom`을 쓰며 추가 라이브러리는 없다.

## 사용 예

```c
#include <stdio.h>
#include <stdlib.h>

#include "hash_table.h"

int main(void)
{
    hash_table_t *table = NULL;
    hash_table_status status = hash_table_create(HASH_TABLE_DEFAULT_BUCKET_COUNT, &table);
    if (status != HASH_TABLE_OK) {
        fprintf(stderr, "create: %s\n", hash_table_status_message(status));
        return EXIT_FAILURE;            /* 실패 시 table은 NULL이므로 해제할 것이 없다 */
    }

    const void *value = NULL;
    size_t value_len = 0;
    status = hash_table_set(table, "user:1", 6, "alice", 5, NULL);
    if (status == HASH_TABLE_OK) {
        status = hash_table_get(table, "user:1", 6, &value, &value_len);
    }
    if (status == HASH_TABLE_OK) {
        printf("%.*s\n", (int)value_len, (const char *)value);   /* alice */
        status = hash_table_delete(table, "user:1", 6);
    }
    if (status != HASH_TABLE_OK) {
        fprintf(stderr, "error: %s\n", hash_table_status_message(status));
    }

    hash_table_destroy(table);          /* 성공/실패와 관계없이 해제 */
    return status == HASH_TABLE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

교체(`INSERTED`/`REPLACED` 구분)와 조회 실패(`NOT_FOUND`) 처리를 포함한 예제는 `examples/basic_usage.c`(`mingw32-make run-example`)에 있다.

빌드: `gcc -std=c11 -Iinclude app.c build/libhash_table.a -lbcrypt` (Linux에서는 `-lbcrypt` 제외).

## 동작 원리: SipHash → 버킷 → 체인

1. 테이블 생성 시 128비트 시드를 한 번 만들고 테이블 수명 동안 유지한다.
2. 키 바이트열을 SipHash-2-4(시드, 키)로 해시해 64비트 값을 얻는다.
3. `hash % bucket_count`로 버킷 인덱스를 구한다(`find_bucket_index`). 버킷 수는 2의 거듭제곱일 필요가 없고 1도 허용한다.
4. 버킷은 단일 연결 리스트의 머리 포인터이다. 엔트리는 `key, key_len, value, value_len, next`를 가진다.
5. 체인을 따라가며 길이와 바이트(`memcmp`)가 모두 같은 키를 찾는다(`find_entry_link`). 이 함수는 엔트리 자체가 아니라 그 엔트리를 가리키는 링크의 주소를 돌려주므로, 삭제 시 머리/중간/꼬리를 같은 코드(`*link = entry->next`)로 처리한다.
6. 신규 삽입으로 개수가 현재 버킷 수를 초과하면 버킷을 정확히 2배로 늘리고 기존 엔트리를 재배치한 뒤 삽입한다. 확장이 없으면 체인 머리 연결은 O(1)이다.

생성 시 버킷 수는 초기 용량이다. `size == bucket_count`까지 확장하지 않고 다음 신규 삽입에서 확장한다. 초기 용량 3은 3→6→12처럼 증가한다. 교체는 확장하지 않고 삭제는 축소하지 않는다. 부하율 기준 1은 이번 단계의 기본 정책이며 성능 최적값으로 정한 것은 아니다. 같은 시드와 나머지 연산을 사용하고 체인 순서는 공개 계약으로 보장하지 않는다.

## API 계약

| 함수 | 성공 | 실패 |
|---|---|---|
| `hash_table_create(n, &t)` | `OK` | `INVALID_ARGUMENT`(n=0, out NULL), `OVERFLOW`(버킷 배열 크기 계산 넘침), `NO_MEMORY`, `RANDOM_SOURCE`(OS 난수 실패. 고정 시드로 대체하지 않음) |
| `hash_table_create_with_seed(n, seed, &t)` | `OK` | 위와 같음(+ seed NULL → `INVALID_ARGUMENT`). 테스트/재현용 |
| `hash_table_set(t, k, kl, v, vl, &outcome)` | `OK` + `INSERTED`/`REPLACED` | `INVALID_ARGUMENT`, `NO_MEMORY`, `OVERFLOW`(개수 증가·버킷 2배·배열 바이트 크기 초과) |
| `hash_table_get(t, k, kl, &v, &vl)` | `OK` | `NOT_FOUND`(출력 인자 미변경), `INVALID_ARGUMENT` |
| `hash_table_delete(t, k, kl)` | `OK` | `NOT_FOUND`, `INVALID_ARGUMENT` |
| `hash_table_size(t)` / `hash_table_bucket_count(t)` | 엔트리 수 / 확장 반영된 현재 버킷 수 | t가 NULL이면 0 |
| `hash_table_destroy(t)` | 전부 해제 | t가 NULL이면 무동작 |

- 실패한 생성은 `*out_table`을 NULL로 만든다.
- 키와 값은 길이 지정 바이트열이다. 0바이트, 중간 NUL 포함 바이너리 모두 허용한다. 포인터는 길이가 0일 때만 NULL일 수 있다. 빈 키도 하나의 유효한 키이다.
- `outcome`은 NULL을 넘겨도 된다. 성공 시에만 갱신하며 실패 시 기존 값을 보존한다.
- 테이블은 스레드 안전하지 않다.

## 메모리 소유권

- `set`은 키와 값을 새로 할당해 복사한다. 호출 후 호출자가 원본 버퍼를 바꾸거나 해제해도 테이블은 영향을 받지 않는다.
- 길이 0인 키/값도 1바이트를 할당하므로, `get`이 돌려주는 값 포인터는 항상 NULL이 아니다(길이 0이면 역참조 금지).
- `get`이 돌려주는 값은 테이블이 소유한 borrowed `const` 포인터이다. 같은 테이블에 대한 다음 `set`, `delete`, `destroy` 호출 전까지만 유효하며, 호출자는 수정하거나 해제하면 안 된다. 오래 쓰려면 복사한다.
- 교체는 새 값 할당이 성공한 뒤에만 기존 값을 해제한다. 할당 실패 시 기존 값, 개수, 버킷 수, 체인은 그대로이다.
- 신규 삽입은 오버플로 검사 후 엔트리/키/값을 준비한다(3회 할당). 확장이 필요하면 새 버킷 배열도 확보하고 NULL로 초기화한다(총 4회). 준비 실패 시 임시 자원만 해제하고 기존 키·값·개수·버킷 수·체인 연결을 모두 보존한다.
- 준비 완료 후 기존 엔트리의 next만 바꿔 새 배열로 옮기며 키·값을 복제하거나 엔트리를 재생성하지 않는다. 이 단계부터는 실패 가능한 할당이 없다. 이동 후 옛 버킷 배열을 정확히 한 번 해제하고 새 배열에서 신규 키의 위치를 다시 계산한다. 입력이 기존 테이블 데이터와 겹쳐도 복사를 먼저 끝낸다.
- `delete`는 엔트리, 키, 값을 해제하고, `destroy`는 모든 엔트리와 버킷 배열, 테이블을 해제한다.

## 복잡도와 한계

- 부하율 α = 개수 / 버킷 수. 조회/교체/삭제와 확장 없는 삽입은 평균 O(1 + α), 최악 O(n)(모든 키가 한 버킷에 몰린 경우)이다.
- 위 복잡도와 별도로 키 길이 L에 대해 해시 O(L)과 비교 O(L), 저장/교체 시 키/값 복사 비용이 든다.
- 일괄 rehash는 옛·새 버킷 초기화/순회와 전체 엔트리 이동이 필요하고 모든 키를 다시 해시한다. 확장하는 삽입 한 번은 O(옛 버킷 수 + 새 버킷 수 + 엔트리 수 + 전체 키 바이트 수)이며 서버의 단일 이벤트 루프에서 다른 요청을 지연시킬 수 있다. 성능 수치는 측정하지 않았다.
- 확장 중에는 기존 B개와 새 2B개 포인터 배열을 함께 보유하므로 버킷 배열에 약 3B개 포인터 공간이 일시적으로 필요하다(신규 엔트리·복사본 별도). 점진적 rehash·축소는 구현하지 않았다.
- 확장 기준은 실제 보유 엔트리 수다. 해시 테이블은 TTL을 모르므로 아직 접근하지 않은 만료 키도 계산에 포함되어 확장을 유발할 수 있다.
- 시드가 비밀로 유지되는 한, 공격자가 의도적으로 한 버킷에 몰리는 키를 만들기 어렵다(SipHash의 목적). `create_with_seed`에 예측 가능한 시드를 쓰면 이 보호가 사라진다.

## 테스트

`tests/test_hash_table.c`는 기존 공개 API와 자동 확장을 검증한다. 고정 시드에서 같은 버킷의 키를 골라 실제 충돌·머리/중간/꼬리 삭제를 보장한다. 초기 1/비2거듭제곱 용량, 정확한 확장 경계, 반복 확장·재분산·빈/바이너리 데이터, borrowed 입력, 네 할당 지점 실패 시 기존 데이터·용량·체인·출력 인자 보존과 재시도, 순수 내부 계산 함수의 오버플로 경계를 포함한다. 테스트 전용 관찰 함수는 운영 라이브러리에 포함되지 않는다.

할당 실패 주입과 주소별 할당 추적(누수, 미추적 주소 해제, 이중 해제 검출. 테스트마다, 그리고 전체 실행 후 살아 있는 할당 0개 확인)은 테스트 빌드에서만 켜진다(`-DHASH_TABLE_TEST_ALLOCATOR`로 `hash_table.c`를 따로 컴파일해 `tests/test_allocator.c`에 연결). 라이브러리 빌드는 표준 `malloc`/`free`를 쓴다.

## 보류 항목(이번 범위 밖)

점진적 리해시, 자동 축소, ListPack, 주기적 TTL 정리, RESP, 영속성, 성능 튜닝, 대규모 퍼징/부하 테스트. TTL 저장소와 HTTP/TCP는 이미 별도 계층으로 구현되어 있다. 계획은 [구현 설계.md](구현%20설계.md)의 향후 계획 참고.
