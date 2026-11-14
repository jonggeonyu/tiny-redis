#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include "server/tcp_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "server/http_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNECTIONS 128
#define ACCEPT_BUDGET 16
#define IO_BUDGET (64u * 1024u)
#define INPUT_CAPACITY (64u * 1024u)
#ifndef TCP_RECEIVE_TIMEOUT_MS
#define TCP_RECEIVE_TIMEOUT_MS 10000u
#endif
#ifndef TCP_IDLE_TIMEOUT_MS
#define TCP_IDLE_TIMEOUT_MS 30000u
#endif
#ifndef TCP_SEND_TIMEOUT_MS
#define TCP_SEND_TIMEOUT_MS 10000u
#endif

typedef struct {
    SOCKET socket;
    http_connection_t *http;
    unsigned char input[INPUT_CAPACITY];
    size_t input_length;
    http_response output;
    size_t sent;
    uint64_t receive_started;
    uint64_t send_started;
    uint64_t idle_started;
    int receiving;
    int eof;
} client_connection;

static volatile LONG stop_requested;

static BOOL WINAPI request_shutdown(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static int set_nonblocking(SOCKET socket)
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

static void destroy_client(client_connection *client)
{
    if (client == NULL) {
        return;
    }
    closesocket(client->socket);
    http_connection_destroy(client->http);
    free(client->output.data);
    free(client);
}

static client_connection *create_client(SOCKET socket, uint64_t now)
{
    client_connection *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        closesocket(socket);
        return NULL;
    }
    client->socket = socket;
    client->idle_started = now;
    client->http = http_connection_create();
    if (client->http == NULL || !set_nonblocking(socket)) {
        destroy_client(client);
        return NULL;
    }
    return client;
}

/* Reading while a response is pending is bounded by this staging buffer. */
static int receive_client(client_connection *client, uint64_t now)
{
    size_t remaining = IO_BUDGET;
    while (!client->eof && remaining && client->input_length < INPUT_CAPACITY) {
        size_t available = INPUT_CAPACITY - client->input_length;
        int received;
        if (available > remaining) {
            available = remaining;
        }
        received = recv(client->socket, (char *)client->input + client->input_length,
                        (int)available, 0);
        if (received > 0) {
            if (!client->receiving && client->output.data == NULL) {
                client->receiving = 1;
                client->receive_started = now;
            }
            client->input_length += (size_t)received;
            remaining -= (size_t)received;
        } else if (received == 0) {
            client->eof = 1;
        } else {
            return WSAGetLastError() == WSAEWOULDBLOCK;
        }
    }
    return 1;
}

/* Called once per loop: a completed response always blocks further execution. */
static int execute_request(client_connection *client, store_t *store, uint64_t now)
{
    size_t consumed = 0;
    int result;
    if (client->output.data != NULL) {
        return 1;
    }
    if (client->input_length == 0) {
        return !client->eof;
    }
    if (!client->receiving) {
        client->receiving = 1;
        client->receive_started = now;
    }
    result = http_connection_feed(client->http, client->input, client->input_length,
                                  &consumed, store, now, &client->output);
    /* An incomplete feed must consume input, otherwise internal work spins. */
    if (consumed > client->input_length || result < 0 ||
        (result == 0 && consumed == 0)) {
        return 0;
    }
    client->input_length -= consumed;
    memmove(client->input, client->input + consumed, client->input_length);
    if (result == 1) {
        if (client->output.data == NULL || client->output.length == 0) {
            return 0;
        }
        client->receiving = 0;
        client->sent = 0;
        client->send_started = now;
        return 1;
    }
    /* EOF with an incomplete message cannot become complete later. */
    return !client->eof;
}

static int send_response(client_connection *client, uint64_t now)
{
    size_t remaining = IO_BUDGET;
    while (client->output.data != NULL && remaining) {
        size_t count = client->output.length - client->sent;
        int sent;
        if (count > remaining) {
            count = remaining;
        }
        sent = send(client->socket, (const char *)client->output.data + client->sent,
                    (int)count, 0);
        if (sent == SOCKET_ERROR) {
            return WSAGetLastError() == WSAEWOULDBLOCK;
        }
        if (sent == 0) {
            return 0;
        }
        client->sent += (size_t)sent;
        remaining -= (size_t)sent;
        if (client->sent == client->output.length) {
            int close_after = client->output.close_after;
            free(client->output.data);
            memset(&client->output, 0, sizeof(client->output));
            client->sent = 0;
            http_connection_reset(client->http);
            client->idle_started = now;
            return !close_after && !(client->eof && client->input_length == 0);
        }
    }
    return 1;
}

static uint64_t time_remaining(uint64_t now, uint64_t started, uint64_t limit)
{
    uint64_t elapsed = now - started;
    return elapsed >= limit ? 0 : limit - elapsed;
}

static uint64_t client_wait(client_connection *client, uint64_t now)
{
    if (client->output.data != NULL) {
        return time_remaining(now, client->send_started, TCP_SEND_TIMEOUT_MS);
    }
    if (client->receiving) {
        return time_remaining(now, client->receive_started, TCP_RECEIVE_TIMEOUT_MS);
    }
    return time_remaining(now, client->idle_started, TCP_IDLE_TIMEOUT_MS);
}

static int watch_client(client_connection *client, fd_set *reads, fd_set *writes)
{
    if (!client->eof && client->input_length < INPUT_CAPACITY) {
        FD_SET(client->socket, reads);
    }
    if (client->output.data != NULL) {
        FD_SET(client->socket, writes);
        return 0;
    }
    return client->input_length != 0 || client->eof;
}

static void accept_clients(SOCKET listener, client_connection **clients, uint64_t now)
{
    unsigned int accepted;
    for (accepted = 0; accepted < ACCEPT_BUDGET; accepted++) {
        size_t slot;
        SOCKET socket = accept(listener, NULL, NULL);
        if (socket == INVALID_SOCKET) {
            return;
        }
        for (slot = 0; slot < MAX_CONNECTIONS && clients[slot] != NULL; slot++) {
        }
        if (slot == MAX_CONNECTIONS) {
            closesocket(socket);
        } else {
            clients[slot] = create_client(socket, now);
        }
    }
}

static int run_loop(SOCKET listener, store_t *store, client_connection **clients)
{
    while (!InterlockedCompareExchange(&stop_requested, 0, 0)) {
        fd_set reads;
        fd_set writes;
        struct timeval timeout;
        uint64_t now = GetTickCount64();
        uint64_t wait_ms = 100;
        size_t i;
        int ready;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            client_connection *client = clients[i];
            uint64_t remaining;
            if (client == NULL) {
                continue;
            }
            remaining = client_wait(client, now);
            if (remaining == 0) {
                destroy_client(client);
                clients[i] = NULL;
                continue;
            }
            if (remaining < wait_ms) {
                wait_ms = remaining;
            }
            if (watch_client(client, &reads, &writes)) {
                wait_ms = 0;
            }
        }
        timeout.tv_sec = 0;
        timeout.tv_usec = (long)(wait_ms * 1000);
        ready = select(0, &reads, &writes, NULL, &timeout);
        if (ready == SOCKET_ERROR) {
            fprintf(stderr, "select failed: %d\n", WSAGetLastError());
            return 1;
        }
        now = GetTickCount64();
        if (InterlockedCompareExchange(&stop_requested, 0, 0)) {
            break;
        }
        if (FD_ISSET(listener, &reads)) {
            accept_clients(listener, clients, now);
        }
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            client_connection *client = clients[i];
            int alive = 1;
            if (client == NULL) {
                continue;
            }
            now = GetTickCount64();
            if (client_wait(client, now) == 0) {
                alive = 0;
            }
            if (alive && FD_ISSET(client->socket, &reads)) {
                alive = receive_client(client, now);
            }
            if (alive) {
                alive = execute_request(client, store, now);
            }
            if (alive && client->output.data != NULL && FD_ISSET(client->socket, &writes)) {
                alive = send_response(client, now);
            }
            if (!alive) {
                destroy_client(client);
                clients[i] = NULL;
            }
        }
    }
    return 0;
}

int tcp_server_run(uint16_t port)
{
    WSADATA winsock;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    client_connection *clients[MAX_CONNECTIONS] = {0};
    store_t *store = NULL;
    int result = 1;
    int handler_registered = 0;
    const BOOL exclusive_address = TRUE;
    size_t i;
    if (port == 0 || WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fprintf(stderr, "Winsock initialization failed or invalid port\n");
        return 1;
    }
    InterlockedExchange(&stop_requested, 0);
    handler_registered = SetConsoleCtrlHandler(request_shutdown, TRUE);
    if (!handler_registered || store_create(1024, &store) != STORE_OK) {
        fprintf(stderr, "Server initialization failed\n");
        goto cleanup;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET || !set_nonblocking(listener)) {
        goto socket_error;
    }
    /* Windows otherwise permits another socket to steal a live listening port. */
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive_address, sizeof(exclusive_address)) == SOCKET_ERROR) {
        goto socket_error;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        goto socket_error;
    }
    printf("Mini Redis listening on http://127.0.0.1:%u\n", (unsigned int)port);
    fflush(stdout);
    result = run_loop(listener, store, clients);
    goto cleanup;

socket_error:
    fprintf(stderr, "Socket initialization failed: %d\n", WSAGetLastError());
cleanup:
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        destroy_client(clients[i]);
    }
    store_destroy(store);
    if (handler_registered) {
        SetConsoleCtrlHandler(request_shutdown, FALSE);
    }
    WSACleanup();
    return result;
}
