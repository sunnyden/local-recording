#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { size_t bytes; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_ALG_SHA_1 1
#define PSA_SUCCESS 0
static inline int psa_hash_setup(psa_hash_operation_t *op, int algorithm)
{ (void)algorithm; op->bytes = 0; return PSA_SUCCESS; }
static inline int psa_hash_update(psa_hash_operation_t *op, const uint8_t *data, size_t length)
{ (void)data; op->bytes += length; return PSA_SUCCESS; }
static inline int psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t capacity, size_t *length)
{
    (void)op;
    if (capacity < 20) return -1;
    for (unsigned i = 0; i < 20; ++i) out[i] = 0xab;
    *length = 20; return PSA_SUCCESS;
}
static inline int psa_hash_abort(psa_hash_operation_t *op) { (void)op; return PSA_SUCCESS; }
