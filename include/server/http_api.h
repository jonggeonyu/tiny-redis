#ifndef MINI_REDIS_HTTP_API_H
#define MINI_REDIS_HTTP_API_H
#include <stddef.h>
#include <stdint.h>
#include "store.h"
/* Request limits from 구현 설계.md. The header area includes the request line. */
#define HTTP_MAX_HEADER_BYTES 8192
#define HTTP_MAX_KEY_BYTES 1024
#define HTTP_MAX_BODY_BYTES (1024 * 1024)
typedef struct http_connection http_connection_t;
typedef struct {
    unsigned char *data;
    size_t length;
    int close_after;
} http_response;
/* Connection owns parser/request. Response owns copied data; caller frees data. */
http_connection_t *http_connection_create(void);
void http_connection_destroy(http_connection_t *connection);
/* 0=incomplete, 1=response ready, -1=fatal allocation error. Consumed always set.
 * Pause at one complete message; no next request until reset after response send. */
int http_connection_feed(http_connection_t *connection, const void *data, size_t length,
                         size_t *consumed, store_t *store, uint64_t now_ms,
                         http_response *response);
void http_connection_reset(http_connection_t *connection);
#endif
