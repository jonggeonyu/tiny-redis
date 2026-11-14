#ifndef MINI_REDIS_TCP_SERVER_H
#define MINI_REDIS_TCP_SERVER_H

#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#elif FD_SETSIZE != 256
#error FD_SETSIZE must be 256 in every Winsock translation unit
#endif

#include <stdint.h>

/* Owns Winsock, the store, and all connections until shutdown. 0 on clean exit. */
int tcp_server_run(uint16_t port);

#endif
