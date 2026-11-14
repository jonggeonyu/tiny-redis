#include "hash_table.h"

#include <stdlib.h>
#include <string.h>

#include "key_hash.h"
#include "random_seed.h"

#ifdef HASH_TABLE_TEST_ALLOCATOR
#include "hash_table_test_access.h"
#endif

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

static size_t compute_bucket_index(const uint8_t seed[HASH_TABLE_SEED_SIZE], size_t bucket_count,
                                   const void *key, size_t key_len)
{
    const uint64_t hash = key_hash_siphash24(seed, key, key_len);
    return (size_t)(hash % (uint64_t)bucket_count);
}

static size_t find_bucket_index(const hash_table_t *table, const void *key, size_t key_len)
{
    return compute_bucket_index(table->seed, table->bucket_count, key, key_len);
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

static int is_bucket_array_size_valid(size_t bucket_count)
{
    return bucket_count <= SIZE_MAX / sizeof(hash_table_entry *);
}

/* 호출자가 is_bucket_array_size_valid로 바이트 크기 넘침을 먼저 확인해야 한다. */
static hash_table_entry **allocate_empty_buckets(size_t bucket_count)
{
    hash_table_entry **buckets = HASH_TABLE_MALLOC(bucket_count * sizeof(*buckets));

    if (buckets == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < bucket_count; i++) {
        buckets[i] = NULL;
    }
    return buckets;
}

/*
 * 신규 키 하나를 넣은 뒤의 버킷 수를 정한다. size + 1이 버킷 수를 넘으면 정확히 두 배로 늘린다.
 * 부하율 1은 이번 단계의 정책일 뿐 성능 최적값이 아니다.
 * 개수, 두 배 용량, 배열 바이트 크기의 넘침을 할당 전에 모두 검사해 테이블을 건드리기 전에 실패한다.
 */
static hash_table_status plan_bucket_count_for_insert(size_t size, size_t bucket_count, size_t *out_bucket_count)
{
    if (size == SIZE_MAX) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    if (size + 1 <= bucket_count) {
        *out_bucket_count = bucket_count;
        return HASH_TABLE_OK;
    }
    if (bucket_count > SIZE_MAX / 2 || !is_bucket_array_size_valid(bucket_count * 2)) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    *out_bucket_count = bucket_count * 2;
    return HASH_TABLE_OK;
}

static hash_table_status allocate_table(size_t bucket_count, hash_table_t **out_table)
{
    if (!is_bucket_array_size_valid(bucket_count)) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    hash_table_t *table = HASH_TABLE_MALLOC(sizeof(*table));
    if (table == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    table->buckets = allocate_empty_buckets(bucket_count);
    if (table->buckets == NULL) {
        HASH_TABLE_FREE(table);
        return HASH_TABLE_ERR_NO_MEMORY;
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

/* 체인 순서는 의미가 없으므로 O(1)인 머리 삽입을 쓴다. */
static void link_entry_at_head(hash_table_entry **bucket, hash_table_entry *entry)
{
    entry->next = *bucket;
    *bucket = entry;
}

/*
 * 엔트리를 복제하거나 다시 만들지 않고 next 연결만 바꿔 미리 할당한 새 배열로 옮긴 뒤 옛 배열을 해제한다.
 * 실패할 수 있는 작업이 없으므로, 시작한 뒤에는 항상 끝까지 진행된다.
 */
static void move_entries_to_buckets(hash_table_t *table, hash_table_entry **new_buckets, size_t new_bucket_count)
{
    for (size_t i = 0; i < table->bucket_count; i++) {
        hash_table_entry *entry = table->buckets[i];
        while (entry != NULL) {
            hash_table_entry *next = entry->next;
            const size_t index = compute_bucket_index(table->seed, new_bucket_count, entry->key, entry->key_len);
            link_entry_at_head(&new_buckets[index], entry);
            entry = next;
        }
    }
    HASH_TABLE_FREE(table->buckets);
    table->buckets = new_buckets;
    table->bucket_count = new_bucket_count;
}

/*
 * 준비 단계(넘침 검사, 엔트리/키/값 복사, 필요 시 확장 배열 할당)를 모두 마친 뒤에만 체인을 바꾼다.
 * 준비가 실패하면 만든 자원만 해제하므로 테이블은 그대로다.
 * key와 value는 테이블이 빌려준 버퍼와 겹칠 수 있으므로, 체인을 바꾸기 전에 복사를 끝낸다.
 */
static hash_table_status insert_new_entry(hash_table_t *table,
                                          const void *key, size_t key_len,
                                          const void *value, size_t value_len)
{
    size_t target_bucket_count = 0;
    const hash_table_status plan_status =
        plan_bucket_count_for_insert(table->size, table->bucket_count, &target_bucket_count);
    if (plan_status != HASH_TABLE_OK) {
        return plan_status;
    }
    hash_table_entry *entry = create_entry(key, key_len, value, value_len);
    if (entry == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    if (target_bucket_count != table->bucket_count) {
        hash_table_entry **grown_buckets = allocate_empty_buckets(target_bucket_count);
        if (grown_buckets == NULL) {
            destroy_entry(entry);
            return HASH_TABLE_ERR_NO_MEMORY;
        }
        move_entries_to_buckets(table, grown_buckets, target_bucket_count);
    }
    /* 재배치로 옛 버킷 배열이 해제되었을 수 있으므로 현재 배열에서 위치를 다시 구한다. */
    const size_t index = find_bucket_index(table, entry->key, entry->key_len);
    link_entry_at_head(&table->buckets[index], entry);
    table->size++;
    return HASH_TABLE_OK;
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
    hash_table_entry *existing = *find_entry_link(bucket, key, key_len);

    /* 기존 키 교체는 개수가 늘지 않으므로 확장하지 않는다. */
    if (existing != NULL) {
        const hash_table_status status = replace_entry_value(existing, value, value_len);
        if (status == HASH_TABLE_OK && outcome != NULL) {
            *outcome = HASH_TABLE_SET_REPLACED;
        }
        return status;
    }
    const hash_table_status status = insert_new_entry(table, key, key_len, value, value_len);
    if (status == HASH_TABLE_OK && outcome != NULL) {
        *outcome = HASH_TABLE_SET_INSERTED;
    }
    return status;
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

#ifdef HASH_TABLE_TEST_ALLOCATOR
/* 테스트 빌드 전용 관찰 함수. 선언과 설명은 hash_table_test_access.h에 있다. */
hash_table_status hash_table_test_plan_bucket_count(size_t size, size_t bucket_count, size_t *out_bucket_count)
{
    return plan_bucket_count_for_insert(size, bucket_count, out_bucket_count);
}

size_t hash_table_test_collect_links(const hash_table_t *table, hash_table_test_link *out, size_t capacity)
{
    size_t count = 0;

    for (size_t i = 0; i < table->bucket_count; i++) {
        for (const hash_table_entry *entry = table->buckets[i]; entry != NULL; entry = entry->next) {
            if (count < capacity) {
                out[count].bucket_index = i;
                out[count].entry = entry;
                out[count].next = entry->next;
                out[count].key = entry->key;
                out[count].key_len = entry->key_len;
            }
            count++;
        }
    }
    return count;
}
#endif
