#ifndef KEY_HASH_H
#define KEY_HASH_H

#include <stddef.h>
#include <stdint.h>

#define KEY_HASH_SEED_SIZE 16

/* SipHash-2-4, 128비트 시드, 64비트 출력. 출력 8바이트를 little-endian 정수로 해석한다. */
uint64_t key_hash_siphash24(const uint8_t seed[KEY_HASH_SEED_SIZE], const void *data, size_t data_len);

#endif
