#ifndef HASH_TABLE_TEST_ACCESS_H
#define HASH_TABLE_TEST_ACCESS_H

#include <stddef.h>

#include "hash_table.h"

/*
 * 단위 테스트 빌드(HASH_TABLE_TEST_ALLOCATOR)에서만 정의되는 내부 관찰 함수.
 * 공개 API가 아니며 운영 라이브러리에는 들어가지 않는다.
 */

/* 엔트리 하나의 위치. 버킷 오름차순, 각 체인은 연결 순서대로 기록된다. */
typedef struct {
    size_t bucket_index;
    const void *entry;
    const void *next;
    const void *key;
    size_t key_len;
} hash_table_test_link;

/* 신규 키 하나를 넣을 때 필요한 버킷 수 계산을 그대로 노출한다. 실패 시 *out_bucket_count는 바뀌지 않는다. */
hash_table_status hash_table_test_plan_bucket_count(size_t size, size_t bucket_count, size_t *out_bucket_count);

/* 최대 capacity개의 엔트리 위치를 out에 기록하고, 전체 엔트리 수를 돌려준다. */
size_t hash_table_test_collect_links(const hash_table_t *table, hash_table_test_link *out, size_t capacity);

#endif
