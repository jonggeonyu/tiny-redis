#ifndef MINI_REDIS_STORE_H
#define MINI_REDIS_STORE_H
#include <stddef.h>
#include <stdint.h>
typedef struct store store_t;
typedef enum {
    STORE_OK = 0, STORE_NOT_FOUND, STORE_EXPIRED, STORE_INVALID_ARGUMENT,
    STORE_NO_MEMORY, STORE_OVERFLOW, STORE_RANDOM_SOURCE
} store_status;
store_status store_create(size_t bucket_count, store_t **out_store);
void store_destroy(store_t *store);
/* ttl_ms=0 means no expiry. now_ms is a monotonic timestamp supplied by caller.
 * A key expires when now_ms >= put_now_ms + ttl_ms. Expired keys are removed only on access
 * (get/delete report STORE_EXPIRED once) or overwrite; unaccessed expired keys stay in memory. */
store_status store_put(store_t *store, const void *key, size_t key_len,
                       const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms);
/* Borrowed result, valid until the next mutating store operation. */
store_status store_get(store_t *store, const void *key, size_t key_len,
                       uint64_t now_ms, const void **out_value, size_t *out_value_len);
store_status store_delete(store_t *store, const void *key, size_t key_len, uint64_t now_ms);
const char *store_status_message(store_status status);
#endif
