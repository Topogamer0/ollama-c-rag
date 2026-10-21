/* Embedding vectors: storage format. */
#include "internal.h"

#include <string.h>

void vec_to_blob(const float *v, size_t n, unsigned char *out) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], sizeof(bits));
        out[4 * i] = (unsigned char)bits;
        out[4 * i + 1] = (unsigned char)(bits >> 8);
        out[4 * i + 2] = (unsigned char)(bits >> 16);
        out[4 * i + 3] = (unsigned char)(bits >> 24);
    }
}

void vec_from_blob(const unsigned char *blob, size_t n, float *out) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits = (uint32_t)blob[4 * i] | (uint32_t)blob[4 * i + 1] << 8 | (uint32_t)blob[4 * i + 2] << 16 |
                        (uint32_t)blob[4 * i + 3] << 24;
        memcpy(&out[i], &bits, sizeof(bits));
    }
}
