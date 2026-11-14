#include "random_seed.h"

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>

int random_seed_fill(uint8_t *buffer, size_t buffer_len)
{
    if (buffer_len > (size_t)ULONG_MAX) {
        return 0;
    }
    const NTSTATUS status = BCryptGenRandom(NULL, buffer, (ULONG)buffer_len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
}

#else

#include <stdio.h>

int random_seed_fill(uint8_t *buffer, size_t buffer_len)
{
    FILE *source = fopen("/dev/urandom", "rb");

    if (source == NULL) {
        return 0;
    }
    const size_t read_len = fread(buffer, 1, buffer_len, source);
    fclose(source);
    return read_len == buffer_len;
}

#endif
