#include "hash_table.h"

#include <stdlib.h>
#include <string.h>

#include "key_hash.h"
#include "random_seed.h"

/* 단위 테스트 빌드에서만 할당 실패 주입과 누수 추적용 할당기로 바꾼다. */
#ifdef HASH_TABLE_TEST_ALLOCATOR
void *test_allocator_malloc(size_t size);
void test_allocator_free(void *ptr);
#define HASH_TABLE_MALLOC(size) test_allocator_malloc(size)
#define HASH_TABLE_FREE(ptr) test_allocator_free(ptr)
#else
#define HASH_TABLE_MALLOC(size) malloc(size)
#define HASH_TABLE_FREE(ptr) free(ptr)
#endif

typedef struct hash_table_entry {
    uint8_t *key;
    size_t key_len;
    uint8_t *value;
    size_t value_len;
    struct hash_table_entry *next;
} hash_table_entry;

struct hash_table {
    hash_table_entry **buckets;
    size_t bucket_count;
    size_t size;
    uint8_t seed[HASH_TABLE_SEED_SIZE];
};

static int is_valid_bytes(const void *data, size_t data_len)
{
    return data != NULL || data_len == 0;
}

/* 길이 0도 1바이트를 할당해, 저장된 키/값 포인터가 항상 NULL이 아니도록 한다. */
static uint8_t *copy_bytes(const void *data, size_t data_len)
{
    uint8_t *copy = HASH_TABLE_MALLOC(data_len == 0 ? 1 : data_len);

    if (copy == NULL) {
        return NULL;
    }
    if (data_len > 0) {
        memcpy(copy, data, data_len);
    }
    return copy;
}

static size_t find_bucket_index(const hash_table_t *table, const void *key, size_t key_len)
{
    const uint64_t hash = key_hash_siphash24(table->seed, key, key_len);
    return (size_t)(hash % (uint64_t)table->bucket_count);
}

static int entry_has_key(const hash_table_entry *entry, const void *key, size_t key_len)
{
    if (entry->key_len != key_len) {
        return 0;
    }
    return key_len == 0 || memcmp(entry->key, key, key_len) == 0;
}

/*
 * 체인에서 키를 찾아, 그 엔트리를 가리키는 링크(버킷 슬롯 또는 이전 엔트리의 next)의 주소를 돌려준다.
 * 키가 없으면 체인 끝의 NULL 링크 주소를 돌려준다. 링크 주소를 쓰면 머리/중간/꼬리 삭제를 같은 방식으로 처리할 수 있다.
 */
static hash_table_entry **find_entry_link(hash_table_entry **bucket, const void *key, size_t key_len)
{
    hash_table_entry **link = bucket;

    while (*link != NULL && !entry_has_key(*link, key, key_len)) {
        link = &(*link)->next;
    }
    return link;
}

static void destroy_entry(hash_table_entry *entry)
{
    HASH_TABLE_FREE(entry->key);
    HASH_TABLE_FREE(entry->value);
    HASH_TABLE_FREE(entry);
}

static hash_table_entry *create_entry(const void *key, size_t key_len, const void *value, size_t value_len)
{
    hash_table_entry *entry = HASH_TABLE_MALLOC(sizeof(*entry));

    if (entry == NULL) {
        return NULL;
    }
    entry->key = copy_bytes(key, key_len);
    entry->value = copy_bytes(value, value_len);
    if (entry->key == NULL || entry->value == NULL) {
        destroy_entry(entry);
        return NULL;
    }
    entry->key_len = key_len;
    entry->value_len = value_len;
    entry->next = NULL;
    return entry;
}

/* 새 값의 할당이 끝난 뒤에만 기존 값을 해제해, 실패 시 기존 값이 남도록 한다. */
static hash_table_status replace_entry_value(hash_table_entry *entry, const void *value, size_t value_len)
{
    uint8_t *new_value = copy_bytes(value, value_len);

    if (new_value == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    HASH_TABLE_FREE(entry->value);
    entry->value = new_value;
    entry->value_len = value_len;
    return HASH_TABLE_OK;
}

static hash_table_status allocate_table(size_t bucket_count, hash_table_t **out_table)
{
    if (bucket_count > SIZE_MAX / sizeof(hash_table_entry *)) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    hash_table_t *table = HASH_TABLE_MALLOC(sizeof(*table));
    if (table == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    table->buckets = HASH_TABLE_MALLOC(bucket_count * sizeof(hash_table_entry *));
    if (table->buckets == NULL) {
        HASH_TABLE_FREE(table);
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < bucket_count; i++) {
        table->buckets[i] = NULL;
    }
    table->bucket_count = bucket_count;
    table->size = 0;
    *out_table = table;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_create_with_seed(size_t bucket_count,
                                              const uint8_t seed[HASH_TABLE_SEED_SIZE],
                                              hash_table_t **out_table)
{
    if (out_table == NULL) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    *out_table = NULL;
    if (bucket_count == 0 || seed == NULL) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    hash_table_t *table = NULL;
    const hash_table_status status = allocate_table(bucket_count, &table);
    if (status != HASH_TABLE_OK) {
        return status;
    }
    memcpy(table->seed, seed, HASH_TABLE_SEED_SIZE);
    *out_table = table;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_create(size_t bucket_count, hash_table_t **out_table)
{
    if (out_table == NULL) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    *out_table = NULL;
    if (bucket_count == 0) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    uint8_t seed[HASH_TABLE_SEED_SIZE];
    if (!random_seed_fill(seed, sizeof(seed))) {
        return HASH_TABLE_ERR_RANDOM_SOURCE;
    }
    return hash_table_create_with_seed(bucket_count, seed, out_table);
}

void hash_table_destroy(hash_table_t *table)
{
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->bucket_count; i++) {
        hash_table_entry *entry = table->buckets[i];
        while (entry != NULL) {
            hash_table_entry *next = entry->next;
            destroy_entry(entry);
            entry = next;
        }
    }
    HASH_TABLE_FREE(table->buckets);
    HASH_TABLE_FREE(table);
}

hash_table_status hash_table_set(hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 hash_table_set_outcome *outcome)
{
    if (table == NULL || !is_valid_bytes(key, key_len) || !is_valid_bytes(value, value_len)) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    hash_table_entry **bucket = &table->buckets[find_bucket_index(table, key, key_len)];
    hash_table_entry **link = find_entry_link(bucket, key, key_len);

    if (*link != NULL) {
        const hash_table_status status = replace_entry_value(*link, value, value_len);
        if (status == HASH_TABLE_OK && outcome != NULL) {
            *outcome = HASH_TABLE_SET_REPLACED;
        }
        return status;
    }
    if (table->size == SIZE_MAX) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    hash_table_entry *entry = create_entry(key, key_len, value, value_len);
    if (entry == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    /* 체인 순서는 의미가 없으므로 O(1)인 머리 삽입을 쓴다. */
    entry->next = *bucket;
    *bucket = entry;
    table->size++;
    if (outcome != NULL) {
        *outcome = HASH_TABLE_SET_INSERTED;
    }
    return HASH_TABLE_OK;
}

hash_table_status hash_table_get(const hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void **out_value, size_t *out_value_len)
{
    if (table == NULL || !is_valid_bytes(key, key_len) || out_value == NULL || out_value_len == NULL) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    hash_table_entry **bucket = &table->buckets[find_bucket_index(table, key, key_len)];
    const hash_table_entry *entry = *find_entry_link(bucket, key, key_len);

    if (entry == NULL) {
        return HASH_TABLE_ERR_NOT_FOUND;
    }
    *out_value = entry->value;
    *out_value_len = entry->value_len;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_delete(hash_table_t *table, const void *key, size_t key_len)
{
    if (table == NULL || !is_valid_bytes(key, key_len)) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    hash_table_entry **bucket = &table->buckets[find_bucket_index(table, key, key_len)];
    hash_table_entry **link = find_entry_link(bucket, key, key_len);
    hash_table_entry *entry = *link;

    if (entry == NULL) {
        return HASH_TABLE_ERR_NOT_FOUND;
    }
    *link = entry->next;
    destroy_entry(entry);
    table->size--;
    return HASH_TABLE_OK;
}

size_t hash_table_size(const hash_table_t *table)
{
    return table == NULL ? 0 : table->size;
}

size_t hash_table_bucket_count(const hash_table_t *table)
{
    return table == NULL ? 0 : table->bucket_count;
}

const char *hash_table_status_message(hash_table_status status)
{
    switch (status) {
    case HASH_TABLE_OK:
        return "ok";
    case HASH_TABLE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case HASH_TABLE_ERR_NO_MEMORY:
        return "out of memory";
    case HASH_TABLE_ERR_NOT_FOUND:
        return "key not found";
    case HASH_TABLE_ERR_RANDOM_SOURCE:
        return "os random source failed";
    case HASH_TABLE_ERR_OVERFLOW:
        return "size overflow";
    }
    return "unknown status";
}
