#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#define HASH_TABLE_DEFAULT_BUCKET_COUNT 16
#define HASH_TABLE_SEED_SIZE 16

/*
 * 버킷 배열 + 단일 연결 리스트 체이닝 해시 테이블.
 * 키 해시는 테이블마다 고유한 128비트 시드를 쓰는 SipHash-2-4(64비트 출력)이다.
 * 키와 값은 길이가 지정된 임의의 바이트열이며, 저장 시 복사되어 테이블이 소유한다.
 * 신규 키 삽입으로 개수가 버킷 수를 넘게 되면(size + 1 > bucket_count) 삽입 전에 버킷 수를
 * 정확히 두 배로 늘리고 엔트리를 한 번에 재배치한다. 부하율 1은 현재 단계의 정책이며 성능 최적값이 아니다.
 * 기존 키 교체는 확장하지 않고, 삭제해도 축소하지 않는다. 점진적 리해시는 하지 않는다.
 * 스레드 안전하지 않다. 동시 접근은 호출자가 직렬화해야 한다.
 */
typedef struct hash_table hash_table_t;

typedef enum {
    HASH_TABLE_OK = 0,
    HASH_TABLE_ERR_INVALID_ARGUMENT,
    HASH_TABLE_ERR_NO_MEMORY,
    HASH_TABLE_ERR_NOT_FOUND,
    HASH_TABLE_ERR_RANDOM_SOURCE,
    HASH_TABLE_ERR_OVERFLOW
} hash_table_status;

typedef enum {
    HASH_TABLE_SET_INSERTED,
    HASH_TABLE_SET_REPLACED
} hash_table_set_outcome;

/*
 * 운영체제 난수(Windows: BCryptGenRandom, 그 외: /dev/urandom)로 시드를 만들어 테이블을 생성한다.
 * bucket_count는 초기 버킷 수로 1 이상이어야 한다(2의 거듭제곱일 필요 없음). 기본값은 HASH_TABLE_DEFAULT_BUCKET_COUNT이다.
 * 난수 생성에 실패하면 고정 시드로 대체하지 않고 HASH_TABLE_ERR_RANDOM_SOURCE를 반환한다.
 * 실패 시 *out_table은 NULL이 된다(out_table 자체가 NULL이 아닌 경우).
 */
hash_table_status hash_table_create(size_t bucket_count, hash_table_t **out_table);

/*
 * 호출자가 준 16바이트 시드로 테이블을 생성한다. 테스트와 재현용이다.
 * 외부 입력을 키로 받는 환경에서 예측 가능한 시드를 쓰면 해시 충돌 공격에 취약해진다.
 */
hash_table_status hash_table_create_with_seed(size_t bucket_count,
                                              const uint8_t seed[HASH_TABLE_SEED_SIZE],
                                              hash_table_t **out_table);

/* 모든 엔트리, 키, 값, 버킷 배열을 해제한다. NULL이면 아무것도 하지 않는다. */
void hash_table_destroy(hash_table_t *table);

/*
 * 키에 값을 저장한다. 키가 이미 있으면 값을 교체하고 개수는 유지한다.
 * key는 key_len이 0일 때만 NULL일 수 있다. value도 같다. 빈 키도 하나의 유효한 키이다.
 * outcome이 NULL이 아니면 성공 시 INSERTED 또는 REPLACED를 기록한다.
 * 신규 키로 확장이 필요하면 엔트리와 새 버킷 배열을 모두 할당한 뒤에 재배치와 삽입을 한다.
 * 실패(NO_MEMORY, OVERFLOW 등) 시 테이블의 기존 키, 값, 개수, 버킷 수, 체인과 *outcome은 변하지 않는다.
 * 개수나 확장 후 버킷 배열 크기가 size_t를 넘으면 할당 전에 HASH_TABLE_ERR_OVERFLOW를 반환한다.
 */
hash_table_status hash_table_set(hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 hash_table_set_outcome *outcome);

/*
 * 키를 찾아 테이블이 소유한 값의 borrowed 포인터와 길이를 돌려준다.
 * 값의 길이가 0이어도 *out_value는 NULL이 아닌 포인터이다(역참조 금지).
 * 포인터는 같은 테이블에 대한 다음 hash_table_set, hash_table_delete,
 * hash_table_destroy 호출 전까지만 유효하다. 호출자는 이를 수정하거나 해제하면 안 된다.
 * 키가 없으면 HASH_TABLE_ERR_NOT_FOUND를 반환하고 출력 인자는 건드리지 않는다.
 */
hash_table_status hash_table_get(const hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void **out_value, size_t *out_value_len);

/* 키를 삭제하고 키, 값, 엔트리 메모리를 해제한다. 없으면 HASH_TABLE_ERR_NOT_FOUND. */
hash_table_status hash_table_delete(hash_table_t *table, const void *key, size_t key_len);

/* 저장된 키의 개수. table이 NULL이면 0. */
size_t hash_table_size(const hash_table_t *table);

/* 현재 버킷 수(확장이 반영된 용량). table이 NULL이면 0. */
size_t hash_table_bucket_count(const hash_table_t *table);

/* 상태 코드의 짧은 설명 문자열(정적 수명). */
const char *hash_table_status_message(hash_table_status status);

#endif
