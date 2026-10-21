/* UTF-8 decoding, character counting and sanitization. */
#include "internal.h"

#include <stdlib.h>

size_t utf8_decode(const char *s, size_t len, size_t i, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s + i;
    size_t avail = len - i;
    unsigned char c = u[0];

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t n;
    uint32_t value;
    unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        value = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        value = c & 0x0F;
        if (c == 0xE0) lo = 0xA0; /* overlong */
        if (c == 0xED) hi = 0x9F; /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        value = c & 0x07;
        if (c == 0xF0) lo = 0x90; /* overlong */
        if (c == 0xF4) hi = 0x8F; /* above U+10FFFF */
    } else {
        return 0;
    }
    if (avail < n || u[1] < lo || u[1] > hi) return 0;
    for (size_t k = 1; k < n; k++) {
        if ((u[k] & 0xC0) != 0x80) return 0;
        value = (value << 6) | (u[k] & 0x3F);
    }
    *cp = value;
    return n;
}

size_t utf8_char_len(const char *s, size_t len, size_t i) {
    uint32_t cp;
    size_t n = utf8_decode(s, len, i, &cp);
    return n ? n : 1;
}

size_t utf8_advance(const char *s, size_t len, size_t i, size_t n) {
    while (n > 0 && i < len) {
        i += utf8_char_len(s, len, i);
        n--;
    }
    return i;
}

size_t utf8_count(const char *s, size_t i, size_t j) {
    size_t n = 0;
    while (i < j) {
        i += utf8_char_len(s, j, i);
        n++;
    }
    return n;
}

char *utf8_sanitize(const char *s, size_t len, size_t *out_len) {
    size_t invalid = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t n = utf8_decode(s, len, i, &cp);
        if (!n) invalid++;
        i += n ? n : 1;
    }
    if (invalid > (SIZE_MAX - len - 1) / 2) return NULL;

    /* each invalid byte becomes the 3 bytes of U+FFFD */
    char *out = malloc(len + 2 * invalid + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t n = utf8_decode(s, len, i, &cp);
        if (n) {
            for (size_t k = 0; k < n; k++) out[o++] = s[i + k];
            i += n;
        } else {
            out[o++] = (char)0xEF;
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            i++;
        }
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}
