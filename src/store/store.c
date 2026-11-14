#include "store.h"

#include <stdlib.h>
#include <string.h>

#include "hash_table.h"

#ifdef STORE_TEST_ALLOCATOR
void *test_allocator_malloc(size_t size);
void test_allocator_free(void *ptr);
#define STORE_MALLOC(size) test_allocator_malloc(size)
#define STORE_FREE(ptr) test_allocator_free(ptr)
#else
#define STORE_MALLOC(size) malloc(size)
#define STORE_FREE(ptr) free(ptr)
#endif

/*
 * 해시 테이블 값 하나에 [만료 시각 uint64 | 사용자 값]을 함께 넣는다.
 * 값과 TTL을 한 번의 hash_table_set으로 교체하므로 실패 시 둘의 대응이 깨지지 않는다.
 * 만료 시각 0은 만료 없음이다. ttl_ms > 0이면 만료 시각은 항상 1 이상이라 충돌하지 않는다.
 * 접근하지 않는 만료 키는 주기 정리가 없으므로 다음 접근이나 덮어쓰기 전까지 메모리에 남는다.
 */
#define EXPIRY_HEADER_SIZE sizeof(uint64_t)
#define NO_EXPIRY 0

struct store {
    hash_table_t *table;
};

static store_status from_hash_table_status(hash_table_status status)
{
    switch (status) {
    case HASH_TABLE_OK:
        return STORE_OK;
    case HASH_TABLE_ERR_INVALID_ARGUMENT:
        return STORE_INVALID_ARGUMENT;
    case HASH_TABLE_ERR_NO_MEMORY:
        return STORE_NO_MEMORY;
    case HASH_TABLE_ERR_NOT_FOUND:
        return STORE_NOT_FOUND;
    case HASH_TABLE_ERR_RANDOM_SOURCE:
        return STORE_RANDOM_SOURCE;
    case HASH_TABLE_ERR_OVERFLOW:
        return STORE_OVERFLOW;
    }
    return STORE_INVALID_ARGUMENT;
}

static int is_valid_bytes(const void *bytes, size_t len)
{
    return bytes != NULL || len == 0;
}

store_status store_create(size_t bucket_count, store_t **out_store)
{
    if (out_store == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    *out_store = NULL;

    store_t *store = STORE_MALLOC(sizeof(*store));
    if (store == NULL) {
        return STORE_NO_MEMORY;
    }
    const hash_table_status status = hash_table_create(bucket_count, &store->table);
    if (status != HASH_TABLE_OK) {
        STORE_FREE(store);
        return from_hash_table_status(status);
    }
    *out_store = store;
    return STORE_OK;
}

void store_destroy(store_t *store)
{
    if (store == NULL) {
        return;
    }
    hash_table_destroy(store->table);
    STORE_FREE(store);
}

static store_status compute_expiry(uint64_t ttl_ms, uint64_t now_ms, uint64_t *out_expiry)
{
    if (ttl_ms == 0) {
        *out_expiry = NO_EXPIRY;
        return STORE_OK;
    }
    if (ttl_ms > UINT64_MAX - now_ms) {
        return STORE_OVERFLOW;
    }
    *out_expiry = now_ms + ttl_ms;
    return STORE_OK;
}

static int is_expired(uint64_t expiry, uint64_t now_ms)
{
    return expiry != NO_EXPIRY && now_ms >= expiry;
}

store_status store_put(store_t *store, const void *key, size_t key_len,
                       const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms)
{
    if (store == NULL || !is_valid_bytes(key, key_len) || !is_valid_bytes(value, value_len)) {
        return STORE_INVALID_ARGUMENT;
    }
    uint64_t expiry = NO_EXPIRY;
    const store_status expiry_status = compute_expiry(ttl_ms, now_ms, &expiry);
    if (expiry_status != STORE_OK) {
        return expiry_status;
    }
    if (value_len > SIZE_MAX - EXPIRY_HEADER_SIZE) {
        return STORE_OVERFLOW;
    }

    const size_t record_len = EXPIRY_HEADER_SIZE + value_len;
    unsigned char *record = STORE_MALLOC(record_len);
    if (record == NULL) {
        return STORE_NO_MEMORY;
    }
    memcpy(record, &expiry, EXPIRY_HEADER_SIZE);
    if (value_len > 0) {
        memcpy(record + EXPIRY_HEADER_SIZE, value, value_len);
    }
    /* 만료된 기존 키도 hash_table_set의 교체로 원자적으로 대체되므로 별도 삭제가 필요 없다. */
    const hash_table_status status = hash_table_set(store->table, key, key_len, record, record_len, NULL);
    STORE_FREE(record);
    return from_hash_table_status(status);
}

/* 키를 찾고 만료되었으면 제거한다. 살아 있으면 레코드를 돌려준다. */
static store_status find_live_record(store_t *store, const void *key, size_t key_len, uint64_t now_ms,
                                     const unsigned char **out_record, size_t *out_record_len)
{
    const void *record = NULL;
    size_t record_len = 0;
    const hash_table_status status = hash_table_get(store->table, key, key_len, &record, &record_len);
    if (status != HASH_TABLE_OK) {
        return from_hash_table_status(status);
    }

    uint64_t expiry = NO_EXPIRY;
    memcpy(&expiry, record, EXPIRY_HEADER_SIZE);
    if (is_expired(expiry, now_ms)) {
        hash_table_delete(store->table, key, key_len);
        return STORE_EXPIRED;
    }
    *out_record = record;
    *out_record_len = record_len;
    return STORE_OK;
}

store_status store_get(store_t *store, const void *key, size_t key_len,
                       uint64_t now_ms, const void **out_value, size_t *out_value_len)
{
    if (store == NULL || !is_valid_bytes(key, key_len) || out_value == NULL || out_value_len == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    const unsigned char *record = NULL;
    size_t record_len = 0;
    const store_status status = find_live_record(store, key, key_len, now_ms, &record, &record_len);
    if (status != STORE_OK) {
        return status;
    }
    *out_value = record + EXPIRY_HEADER_SIZE;
    *out_value_len = record_len - EXPIRY_HEADER_SIZE;
    return STORE_OK;
}

store_status store_delete(store_t *store, const void *key, size_t key_len, uint64_t now_ms)
{
    if (store == NULL || !is_valid_bytes(key, key_len)) {
        return STORE_INVALID_ARGUMENT;
    }
    const unsigned char *record = NULL;
    size_t record_len = 0;
    const store_status status = find_live_record(store, key, key_len, now_ms, &record, &record_len);
    if (status != STORE_OK) {
        return status;
    }
    return from_hash_table_status(hash_table_delete(store->table, key, key_len));
}

const char *store_status_message(store_status status)
{
    switch (status) {
    case STORE_OK:
        return "ok";
    case STORE_NOT_FOUND:
        return "not_found";
    case STORE_EXPIRED:
        return "expired";
    case STORE_INVALID_ARGUMENT:
        return "invalid_argument";
    case STORE_NO_MEMORY:
        return "no_memory";
    case STORE_OVERFLOW:
        return "overflow";
    case STORE_RANDOM_SOURCE:
        return "random_source";
    }
    return "unknown";
}
