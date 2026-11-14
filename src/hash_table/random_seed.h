#ifndef RANDOM_SEED_H
#define RANDOM_SEED_H

#include <stddef.h>
#include <stdint.h>

/* 운영체제 난수원으로 buffer를 채운다. 성공하면 1, 실패하면 0. 실패 시 대체 값을 만들지 않는다. */
int random_seed_fill(uint8_t *buffer, size_t buffer_len);

#endif
