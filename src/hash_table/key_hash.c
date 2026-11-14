#include "key_hash.h"

#include "siphash.h"

#define SIPHASH_64_OUTPUT_SIZE 8

static uint64_t decode_u64_little_endian(const uint8_t bytes[SIPHASH_64_OUTPUT_SIZE])
{
    uint64_t value = 0;

    for (int i = SIPHASH_64_OUTPUT_SIZE - 1; i >= 0; i--) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

uint64_t key_hash_siphash24(const uint8_t seed[KEY_HASH_SEED_SIZE], const void *data, size_t data_len)
{
    /* 참조 구현은 in + inlen 포인터 연산을 하므로, 빈 입력의 NULL은 유효한 주소로 바꿔 넘긴다. */
    static const uint8_t empty_input[1] = {0};
    const void *input = data_len == 0 ? empty_input : data;
    uint8_t output[SIPHASH_64_OUTPUT_SIZE];

    siphash(input, data_len, seed, output, SIPHASH_64_OUTPUT_SIZE);
    return decode_u64_little_endian(output);
}
