/* Internal declarations shared by the library sources. Not installed. */
#ifndef RAG_INTERNAL_H
#define RAG_INTERNAL_H

#include "ollama_rag.h"

#include <stdarg.h>

/* ---- growable byte buffer (util.c) ---- */

typedef struct buf {
    char *data;
    size_t len;
    size_t cap;
    bool oom; /* set when an append failed */
} buf_t;

bool buf_append(buf_t *b, const void *data, size_t len);
bool buf_puts(buf_t *b, const char *s);
bool buf_putc(buf_t *b, char c);
bool buf_printf(buf_t *b, const char *fmt, ...);
/* Returns a NUL-terminated string (possibly empty), or NULL if the buffer failed. */
const char *buf_cstr(buf_t *b);
/* Hands the string over to the caller, who frees it; NULL if the buffer failed. */
char *buf_take(buf_t *b);
void buf_free(buf_t *b);

/* ---- strings (util.c) ---- */

char *rag_strdup(const char *s);
char *rag_strndup(const char *s, size_t n);
char *rag_vformat(const char *fmt, va_list ap);
char *rag_format(const char *fmt, ...);

/* ---- errors (util.c) ---- */

typedef struct err {
    rag_error_t pub;
    char *msg;
} err_t;

void err_init(err_t *e);
void err_clear(err_t *e);
void err_free(err_t *e);
rag_status_t err_set(err_t *e, rag_status_t code, const char *fmt, ...);

/* ---- UTF-8 (utf8.c) ---- */

/*
 * A "character" is a valid UTF-8 sequence or a single byte that is not part of
 * one, so every byte string is a sequence of characters.
 */

/* Length of the valid UTF-8 sequence at s[i] (decoded in *cp), 0 if invalid. */
size_t utf8_decode(const char *s, size_t len, size_t i, uint32_t *cp);
/* Length in bytes of the character at s[i] (i < len). */
size_t utf8_char_len(const char *s, size_t len, size_t i);
/* Offset after `n` characters starting from s[i], or len. */
size_t utf8_advance(const char *s, size_t len, size_t i, size_t n);
/* Number of characters in s[i..j), where j is a character boundary. */
size_t utf8_count(const char *s, size_t i, size_t j);
/* Copy of s[0..len) with every invalid byte replaced by U+FFFD; NULL if out of memory. */
char *utf8_sanitize(const char *s, size_t len, size_t *out_len);

/* ---- chunking (chunk.c) ---- */

typedef struct chunk_span {
    size_t start, end;           /* byte offsets: the chunk is text[start..end) */
    size_t line_start, line_end; /* first and last line, from 1 */
} chunk_span_t;

typedef struct chunk_list {
    chunk_span_t *items;
    size_t count;
    size_t cap;
} chunk_list_t;

/*
 * Splits a text into chunks of at most `size` characters (size >= 1), where
 * consecutive chunks share about `overlap` characters (overlap < size).
 * Chunks never start or end with whitespace; cuts are made between paragraphs
 * when possible, then after sentences, at line breaks, between words, and as a
 * last resort inside a word. Appends to `out`; returns false if out of memory.
 */
bool chunk_split(const char *text, size_t len, size_t size, size_t overlap, chunk_list_t *out);
void chunk_list_free(chunk_list_t *list);

#endif /* RAG_INTERNAL_H */
