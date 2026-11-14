#include "server/tcp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char **argv)
{
    unsigned long port = 8080;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        if (argv[1][0] < '0' || argv[1][0] > '9') {
            fprintf(stderr, "Invalid port\n");
            return EXIT_FAILURE;
        }
        port = strtoul(argv[1], &end, 10);
        if (errno || *end || port == 0 || port > 65535) {
            fprintf(stderr, "Invalid port\n");
            return EXIT_FAILURE;
        }
    }
    return tcp_server_run((uint16_t)port);
}
