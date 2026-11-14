#include "server/http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llhttp.h"

/* Only these header names affect request handling; longer names are never interesting. */
#define HTTP_FIELD_NAME_CAPACITY 32
#define HTTP_RESPONSE_HEAD_CAPACITY 256

typedef enum {
    HEADER_OTHER,
    HEADER_HOST,
    HEADER_CONTENT_LENGTH,
    HEADER_TRANSFER_ENCODING,
    HEADER_EXPECT,
    HEADER_UPGRADE,
    HEADER_TTL
} header_kind;

typedef enum {
    ROUTE_HEALTH,
    ROUTE_KEY
} route_kind;

typedef struct {
    int status;
    const char *code;
    const char *allow;
} http_error;

static const http_error ERROR_BAD_REQUEST = {400, "bad_request", NULL};
static const http_error ERROR_INVALID_HOST = {400, "invalid_host", NULL};
static const http_error ERROR_EXPECT = {400, "expect_not_supported", NULL};
static const http_error ERROR_TRANSFER_ENCODING = {400, "unsupported_transfer_encoding", NULL};
static const http_error ERROR_DUPLICATE_LENGTH = {400, "invalid_content_length", NULL};
static const http_error ERROR_LENGTH_REQUIRED = {400, "content_length_required", NULL};
static const http_error ERROR_BODY_NOT_ALLOWED = {400, "body_not_allowed", NULL};
static const http_error ERROR_UPGRADE = {400, "upgrade_not_supported", NULL};
static const http_error ERROR_INVALID_TARGET = {400, "invalid_target", NULL};
static const http_error ERROR_QUERY = {400, "query_not_supported", NULL};
static const http_error ERROR_INVALID_KEY = {400, "invalid_key", NULL};
static const http_error ERROR_INVALID_TTL = {400, "invalid_ttl", NULL};
static const http_error ERROR_TTL_NOT_ALLOWED = {400, "ttl_not_allowed", NULL};
static const http_error ERROR_INVALID_ARGUMENT = {400, "invalid_argument", NULL};
static const http_error ERROR_ROUTE_NOT_FOUND = {404, "route_not_found", NULL};
static const http_error ERROR_HEALTH_METHOD = {405, "method_not_allowed", "GET"};
static const http_error ERROR_KEY_METHOD = {405, "method_not_allowed", "GET, PUT, DELETE"};
static const http_error ERROR_BODY_TOO_LARGE = {413, "payload_too_large", NULL};
static const http_error ERROR_KEY_TOO_LONG = {414, "key_too_long", NULL};
static const http_error ERROR_HEADERS_TOO_LARGE = {431, "headers_too_large", NULL};
static const http_error ERROR_INTERNAL = {500, "internal_error", NULL};
static const http_error ERROR_VERSION = {505, "http_version_not_supported", NULL};

static const char JSON_CONTENT_TYPE[] = "application/json";
static const char BINARY_CONTENT_TYPE[] = "application/octet-stream";
static const char JSON_OK[] = "{\"ok\":true}";

struct http_connection {
    llhttp_t parser;
    llhttp_settings_t settings;
    uint64_t now_ms;
    size_t header_bytes;
    int headers_complete;
    int message_complete;
    int responded;
    int keep_alive;
    const http_error *error;

    char url[HTTP_MAX_HEADER_BYTES];
    size_t url_length;
    char field[HTTP_FIELD_NAME_CAPACITY];
    size_t field_length;
    int field_too_long;
    header_kind current_header;
    char value[HTTP_MAX_HEADER_BYTES];
    size_t value_length;

    unsigned int host_count;
    int host_empty;
    unsigned int content_length_count;
    unsigned int ttl_count;
    int ttl_valid;
    uint64_t ttl_ms;

    route_kind route;
    uint8_t method;
    unsigned char key[HTTP_MAX_KEY_BYTES];
    size_t key_length;
    unsigned char *body;
    size_t body_length;
    size_t body_capacity;
};

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

/* Builds one owned buffer so the response survives later store mutations. */
static int build_response(http_response *response, int status, const char *content_type,
                          const void *body, size_t body_length, const char *allow,
                          int close_after)
{
    char head[HTTP_RESPONSE_HEAD_CAPACITY];
    int head_length = snprintf(head, sizeof(head),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %llu\r\n"
                               "Cache-Control: no-store\r\n"
                               "%s%s%s"
                               "%s"
                               "\r\n",
                               status, reason_phrase(status), content_type,
                               (unsigned long long)body_length,
                               allow != NULL ? "Allow: " : "",
                               allow != NULL ? allow : "",
                               allow != NULL ? "\r\n" : "",
                               close_after ? "Connection: close\r\n" : "");
    unsigned char *data;
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) {
        return -1;
    }
    if (body_length > SIZE_MAX - (size_t)head_length) {
        return -1;
    }
    data = malloc((size_t)head_length + body_length);
    if (data == NULL) {
        return -1;
    }
    memcpy(data, head, (size_t)head_length);
    if (body_length > 0) {
        memcpy(data + head_length, body, body_length);
    }
    response->data = data;
    response->length = (size_t)head_length + body_length;
    response->close_after = close_after;
    return 1;
}

static int build_json_response(http_response *response, int status, const char *json,
                               const char *allow, int close_after)
{
    return build_response(response, status, JSON_CONTENT_TYPE, json, strlen(json), allow,
                          close_after);
}

/* Every protocol or validation error closes the connection after the response. */
static int build_error_response(http_response *response, const http_error *error)
{
    char json[64];
    int json_length = snprintf(json, sizeof(json), "{\"error\":\"%s\"}", error->code);
    if (json_length < 0 || (size_t)json_length >= sizeof(json)) {
        return -1;
    }
    return build_json_response(response, error->status, json, error->allow, 1);
}

static int fail_request(http_connection_t *connection, const http_error *error)
{
    if (connection->error == NULL) {
        connection->error = error;
    }
    return -1;
}

static int is_space_or_tab(char c)
{
    return c == ' ' || c == '\t';
}

static int equals_ignore_case(const char *text, size_t length, const char *expected)
{
    size_t i;
    if (strlen(expected) != length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static header_kind classify_header(const http_connection_t *connection)
{
    const char *name = connection->field;
    size_t length = connection->field_length;
    if (connection->field_too_long) {
        return HEADER_OTHER;
    }
    if (equals_ignore_case(name, length, "host")) {
        return HEADER_HOST;
    }
    if (equals_ignore_case(name, length, "content-length")) {
        return HEADER_CONTENT_LENGTH;
    }
    if (equals_ignore_case(name, length, "transfer-encoding")) {
        return HEADER_TRANSFER_ENCODING;
    }
    if (equals_ignore_case(name, length, "expect")) {
        return HEADER_EXPECT;
    }
    if (equals_ignore_case(name, length, "upgrade")) {
        return HEADER_UPGRADE;
    }
    if (equals_ignore_case(name, length, "x-ttl-ms")) {
        return HEADER_TTL;
    }
    return HEADER_OTHER;
}

/* Accepts only a positive decimal integer that fits in uint64_t. */
static int parse_ttl(const char *text, size_t length, uint64_t *out_ttl)
{
    uint64_t ttl = 0;
    size_t i;
    if (length == 0) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        unsigned int digit;
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        digit = (unsigned int)(text[i] - '0');
        if (ttl > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        ttl = ttl * 10 + digit;
    }
    if (ttl == 0) {
        return 0;
    }
    *out_ttl = ttl;
    return 1;
}

static int on_url(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (length > sizeof(connection->url) - connection->url_length) {
        return fail_request(connection, &ERROR_HEADERS_TOO_LARGE);
    }
    memcpy(connection->url + connection->url_length, at, length);
    connection->url_length += length;
    return 0;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (connection->field_too_long ||
        length > sizeof(connection->field) - connection->field_length) {
        connection->field_too_long = 1;
        return 0;
    }
    memcpy(connection->field + connection->field_length, at, length);
    connection->field_length += length;
    return 0;
}

static int on_header_field_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    connection->current_header = classify_header(connection);
    connection->field_length = 0;
    connection->field_too_long = 0;
    connection->value_length = 0;
    switch (connection->current_header) {
    case HEADER_TRANSFER_ENCODING:
        return fail_request(connection, &ERROR_TRANSFER_ENCODING);
    case HEADER_EXPECT:
        return fail_request(connection, &ERROR_EXPECT);
    case HEADER_UPGRADE:
        /* llhttp flags upgrade only with "Connection: upgrade"; any Upgrade is refused. */
        return fail_request(connection, &ERROR_UPGRADE);
    case HEADER_CONTENT_LENGTH:
        connection->content_length_count++;
        if (connection->content_length_count > 1) {
            return fail_request(connection, &ERROR_DUPLICATE_LENGTH);
        }
        return 0;
    case HEADER_HOST:
        connection->host_count++;
        if (connection->host_count > 1) {
            return fail_request(connection, &ERROR_INVALID_HOST);
        }
        return 0;
    case HEADER_TTL:
        connection->ttl_count++;
        if (connection->ttl_count > 1) {
            return fail_request(connection, &ERROR_INVALID_TTL);
        }
        return 0;
    default:
        return 0;
    }
}

static int on_header_value(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (connection->current_header != HEADER_HOST && connection->current_header != HEADER_TTL) {
        return 0;
    }
    if (length > sizeof(connection->value) - connection->value_length) {
        return fail_request(connection, &ERROR_HEADERS_TOO_LARGE);
    }
    memcpy(connection->value + connection->value_length, at, length);
    connection->value_length += length;
    return 0;
}

static int on_header_value_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    const char *value = connection->value;
    size_t length = connection->value_length;
    while (length > 0 && is_space_or_tab(value[0])) {
        value++;
        length--;
    }
    while (length > 0 && is_space_or_tab(value[length - 1])) {
        length--;
    }
    if (connection->current_header == HEADER_HOST) {
        connection->host_empty = length == 0;
    } else if (connection->current_header == HEADER_TTL) {
        connection->ttl_valid = parse_ttl(value, length, &connection->ttl_ms);
    }
    connection->current_header = HEADER_OTHER;
    connection->value_length = 0;
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Percent-decodes exactly once; a decoded "%25" stays a literal '%'. */
static const http_error *decode_key(http_connection_t *connection, const char *encoded,
                                    size_t length)
{
    size_t i = 0;
    connection->key_length = 0;
    if (length == 0) {
        return &ERROR_INVALID_KEY;
    }
    while (i < length) {
        unsigned char byte = (unsigned char)encoded[i];
        if (byte == '%') {
            int high;
            int low;
            if (length - i < 3) {
                return &ERROR_INVALID_KEY;
            }
            high = hex_value(encoded[i + 1]);
            low = hex_value(encoded[i + 2]);
            if (high < 0 || low < 0) {
                return &ERROR_INVALID_KEY;
            }
            byte = (unsigned char)(high * 16 + low);
            i += 3;
        } else {
            i++;
        }
        if (byte == '\0') {
            return &ERROR_INVALID_KEY;
        }
        if (connection->key_length == sizeof(connection->key)) {
            return &ERROR_KEY_TOO_LONG;
        }
        connection->key[connection->key_length++] = byte;
    }
    return NULL;
}

static const http_error *resolve_route(http_connection_t *connection)
{
    static const char key_prefix[] = "/keys/";
    const size_t prefix_length = sizeof(key_prefix) - 1;
    const char *url = connection->url;
    size_t length = connection->url_length;
    uint8_t method = connection->method;
    if (length == 0 || url[0] != '/') {
        return &ERROR_INVALID_TARGET;
    }
    if (memchr(url, '?', length) != NULL || memchr(url, '#', length) != NULL) {
        return &ERROR_QUERY;
    }
    if (length == strlen("/health") && memcmp(url, "/health", length) == 0) {
        connection->route = ROUTE_HEALTH;
        return method == HTTP_GET ? NULL : &ERROR_HEALTH_METHOD;
    }
    if (length < prefix_length || memcmp(url, key_prefix, prefix_length) != 0) {
        return &ERROR_ROUTE_NOT_FOUND;
    }
    connection->route = ROUTE_KEY;
    if (method != HTTP_GET && method != HTTP_PUT && method != HTTP_DELETE) {
        return &ERROR_KEY_METHOD;
    }
    return decode_key(connection, url + prefix_length, length - prefix_length);
}

static const http_error *validate_ttl(const http_connection_t *connection)
{
    if (connection->ttl_count == 0) {
        return NULL;
    }
    if (connection->method != HTTP_PUT) {
        return &ERROR_TTL_NOT_ALLOWED;
    }
    if (!connection->ttl_valid || connection->ttl_ms > UINT64_MAX - connection->now_ms) {
        return &ERROR_INVALID_TTL;
    }
    return NULL;
}

static const http_error *validate_body_length(const http_connection_t *connection)
{
    uint64_t content_length = connection->parser.content_length;
    if (connection->method == HTTP_PUT && connection->content_length_count == 0) {
        return &ERROR_LENGTH_REQUIRED;
    }
    if (connection->method != HTTP_PUT && content_length > 0) {
        return &ERROR_BODY_NOT_ALLOWED;
    }
    if (content_length > HTTP_MAX_BODY_BYTES) {
        return &ERROR_BODY_TOO_LARGE;
    }
    return NULL;
}

static const http_error *validate_request(http_connection_t *connection)
{
    llhttp_t *parser = &connection->parser;
    const http_error *error;
    if (llhttp_get_http_major(parser) != 1 || llhttp_get_http_minor(parser) != 1) {
        return &ERROR_VERSION;
    }
    if (llhttp_get_upgrade(parser)) {
        return &ERROR_UPGRADE;
    }
    if (connection->host_count != 1 || connection->host_empty) {
        return &ERROR_INVALID_HOST;
    }
    connection->method = llhttp_get_method(parser);
    error = resolve_route(connection);
    if (error == NULL) {
        error = validate_ttl(connection);
    }
    if (error == NULL) {
        error = validate_body_length(connection);
    }
    return error;
}

/* The whole body is bounded by HTTP_MAX_BODY_BYTES, so it is allocated once up front. */
static int on_headers_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    const http_error *error = validate_request(connection);
    connection->headers_complete = 1;
    if (error != NULL) {
        return fail_request(connection, error);
    }
    if (parser->content_length > 0) {
        connection->body = malloc((size_t)parser->content_length);
        if (connection->body == NULL) {
            return fail_request(connection, &ERROR_INTERNAL);
        }
        connection->body_capacity = (size_t)parser->content_length;
    }
    return 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (length > connection->body_capacity - connection->body_length) {
        return fail_request(connection, &ERROR_BAD_REQUEST);
    }
    memcpy(connection->body + connection->body_length, at, length);
    connection->body_length += length;
    return 0;
}

/* Pausing here leaves pipelined bytes unparsed until the response has been sent. */
static int on_message_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    connection->message_complete = 1;
    connection->keep_alive = llhttp_should_keep_alive(parser);
    return HPE_PAUSED;
}

static int respond_store_status(http_response *response, store_status status, int has_ttl,
                                int close_after)
{
    switch (status) {
    case STORE_OK:
        return build_json_response(response, 200, JSON_OK, NULL, close_after);
    case STORE_NOT_FOUND:
        return build_json_response(response, 404, "{\"error\":\"not_found\"}", NULL,
                                   close_after);
    case STORE_EXPIRED:
        return build_json_response(response, 404, "{\"error\":\"expired\"}", NULL,
                                   close_after);
    case STORE_INVALID_ARGUMENT:
        return build_error_response(response, &ERROR_INVALID_ARGUMENT);
    case STORE_OVERFLOW:
        return build_error_response(response, has_ttl ? &ERROR_INVALID_TTL : &ERROR_INTERNAL);
    default:
        return build_error_response(response, &ERROR_INTERNAL);
    }
}

static int execute_key_request(http_connection_t *connection, store_t *store,
                               http_response *response)
{
    int close_after = !connection->keep_alive;
    store_status status;
    if (connection->method == HTTP_GET) {
        const void *value = NULL;
        size_t value_length = 0;
        status = store_get(store, connection->key, connection->key_length,
                           connection->now_ms, &value, &value_length);
        if (status == STORE_OK) {
            return build_response(response, 200, BINARY_CONTENT_TYPE, value, value_length,
                                  NULL, close_after);
        }
        return respond_store_status(response, status, 0, close_after);
    }
    if (connection->method == HTTP_PUT) {
        uint64_t ttl_ms = connection->ttl_count > 0 ? connection->ttl_ms : 0;
        status = store_put(store, connection->key, connection->key_length, connection->body,
                           connection->body_length, ttl_ms, connection->now_ms);
        return respond_store_status(response, status, ttl_ms != 0, close_after);
    }
    status = store_delete(store, connection->key, connection->key_length, connection->now_ms);
    return respond_store_status(response, status, 0, close_after);
}

static int execute_request(http_connection_t *connection, store_t *store,
                           http_response *response)
{
    if (connection->route == ROUTE_HEALTH) {
        return build_json_response(response, 200, JSON_OK, NULL, !connection->keep_alive);
    }
    return execute_key_request(connection, store, response);
}

static void clear_request(http_connection_t *connection)
{
    free(connection->body);
    connection->body = NULL;
    connection->body_length = 0;
    connection->body_capacity = 0;
    connection->header_bytes = 0;
    connection->headers_complete = 0;
    connection->message_complete = 0;
    connection->responded = 0;
    connection->keep_alive = 0;
    connection->error = NULL;
    connection->url_length = 0;
    connection->field_length = 0;
    connection->field_too_long = 0;
    connection->current_header = HEADER_OTHER;
    connection->value_length = 0;
    connection->host_count = 0;
    connection->host_empty = 0;
    connection->content_length_count = 0;
    connection->ttl_count = 0;
    connection->ttl_valid = 0;
    connection->ttl_ms = 0;
    connection->key_length = 0;
}

http_connection_t *http_connection_create(void)
{
    http_connection_t *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) {
        return NULL;
    }
    /* No llhttp_set_lenient_* call: ambiguous framing must stay a parse error. */
    llhttp_settings_init(&connection->settings);
    connection->settings.on_url = on_url;
    connection->settings.on_header_field = on_header_field;
    connection->settings.on_header_field_complete = on_header_field_complete;
    connection->settings.on_header_value = on_header_value;
    connection->settings.on_header_value_complete = on_header_value_complete;
    connection->settings.on_headers_complete = on_headers_complete;
    connection->settings.on_body = on_body;
    connection->settings.on_message_complete = on_message_complete;
    llhttp_init(&connection->parser, HTTP_REQUEST, &connection->settings);
    connection->parser.data = connection;
    clear_request(connection);
    return connection;
}

void http_connection_destroy(http_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    free(connection->body);
    free(connection);
}

void http_connection_reset(http_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    clear_request(connection);
    llhttp_reset(&connection->parser);
}

static int respond_with_error(http_connection_t *connection, http_response *response)
{
    const http_error *error = connection->error != NULL ? connection->error : &ERROR_BAD_REQUEST;
    connection->responded = 1;
    return build_error_response(response, error) < 0 ? -1 : 1;
}

/* Feeds at most the remaining header budget until headers finish, so the 8 KiB limit
 * is enforced on raw bytes regardless of how the request was split. */
static size_t next_chunk_length(const http_connection_t *connection, size_t remaining)
{
    size_t allowed;
    if (connection->headers_complete) {
        return remaining;
    }
    allowed = HTTP_MAX_HEADER_BYTES - connection->header_bytes;
    return remaining < allowed ? remaining : allowed;
}

int http_connection_feed(http_connection_t *connection, const void *data, size_t length,
                         size_t *consumed, store_t *store, uint64_t now_ms,
                         http_response *response)
{
    const char *bytes = data;
    size_t offset = 0;
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (connection == NULL || consumed == NULL || response == NULL ||
        (data == NULL && length > 0)) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (connection->responded) {
        return 0;
    }
    connection->now_ms = now_ms;
    while (offset < length) {
        size_t chunk = next_chunk_length(connection, length - offset);
        int headers_were_complete = connection->headers_complete;
        llhttp_errno_t parse_error = llhttp_execute(&connection->parser, bytes + offset, chunk);
        if (parse_error == HPE_PAUSED && connection->message_complete) {
            int result;
            *consumed = (size_t)(llhttp_get_error_pos(&connection->parser) - bytes);
            connection->responded = 1;
            result = execute_request(connection, store, response);
            return result < 0 ? -1 : 1;
        }
        if (parse_error != HPE_OK) {
            *consumed = length;
            return respond_with_error(connection, response);
        }
        offset += chunk;
        if (!headers_were_complete && !connection->headers_complete) {
            connection->header_bytes += chunk;
            if (connection->header_bytes >= HTTP_MAX_HEADER_BYTES) {
                *consumed = length;
                connection->error = &ERROR_HEADERS_TOO_LARGE;
                return respond_with_error(connection, response);
            }
        }
    }
    *consumed = length;
    return 0;
}
