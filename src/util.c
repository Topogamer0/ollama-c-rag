/* Buffers, strings and error reporting. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- buffer ---- */

static bool buf_reserve(buf_t *b, size_t extra) {
    if (b->oom) return false;
    if (extra < b->cap - b->len) return true; /* keep room for the terminator */
    size_t cap = b->cap ? b->cap : 256;
    while (cap - b->len <= extra) {
        if (cap > SIZE_MAX / 2) {
            b->oom = true;
            return false;
        }
        cap *= 2;
    }
    char *data = realloc(b->data, cap);
    if (!data) {
        b->oom = true;
        return false;
    }
    b->data = data;
    b->cap = cap;
    return true;
}

bool rag_buf_append(buf_t *b, const void *data, size_t len) {
    if (!buf_reserve(b, len)) return false;
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

bool rag_buf_puts(buf_t *b, const char *s) { return rag_buf_append(b, s, strlen(s)); }

bool rag_buf_putc(buf_t *b, char c) { return rag_buf_append(b, &c, 1); }

bool rag_buf_printf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = rag_vformat(fmt, ap);
    va_end(ap);
    if (!s) {
        b->oom = true;
        return false;
    }
    bool ok = rag_buf_puts(b, s);
    free(s);
    return ok;
}

const char *rag_buf_cstr(buf_t *b) {
    if (b->oom) return NULL;
    if (!b->data && !buf_reserve(b, 0)) return NULL;
    b->data[b->len] = '\0';
    return b->data;
}

char *rag_buf_take(buf_t *b) {
    char *s = rag_buf_cstr(b) ? b->data : NULL;
    if (!s) rag_buf_free(b);
    *b = (buf_t){0};
    return s;
}

void rag_buf_free(buf_t *b) {
    free(b->data);
    *b = (buf_t){0};
}

/* ---- strings ---- */

char *rag_strndup(const char *s, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *rag_strdup(const char *s) { return s ? rag_strndup(s, strlen(s)) : NULL; }

char *rag_vformat(const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return NULL;
    char *s = malloc((size_t)n + 1);
    if (s) vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

char *rag_format(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = rag_vformat(fmt, ap);
    va_end(ap);
    return s;
}

/* ---- errors ---- */

void rag_err_init(err_t *e) {
    e->pub.code = RAG_OK;
    e->pub.status_code = -1;
    e->pub.message = "";
    e->msg = NULL;
}

void rag_err_clear(err_t *e) {
    free(e->msg);
    rag_err_init(e);
}

void rag_err_free(err_t *e) { rag_err_clear(e); }

rag_status_t rag_err_set(err_t *e, rag_status_t code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = rag_vformat(fmt, ap);
    va_end(ap);
    free(e->msg);
    e->msg = msg;
    e->pub.code = code;
    e->pub.status_code = -1;
    e->pub.message = msg ? msg : rag_status_string(code);
    return code;
}

/* ---- public ---- */

const char *rag_version(void) { return RAG_VERSION; }

const char *rag_status_string(rag_status_t status) {
    switch (status) {
    case RAG_OK: return "success";
    case RAG_ERROR_INVALID: return "invalid argument";
    case RAG_ERROR_IO: return "input/output error";
    case RAG_ERROR_DATABASE: return "database error";
    case RAG_ERROR_MODEL_MISMATCH: return "embedding model mismatch";
    case RAG_ERROR_OLLAMA: return "Ollama request failed";
    case RAG_ERROR_NOT_FOUND: return "not found";
    case RAG_ERROR_EMPTY: return "the index is empty";
    case RAG_ERROR_ABORTED: return "aborted";
    case RAG_ERROR_MEMORY: return "out of memory";
    }
    return "unknown error";
}
