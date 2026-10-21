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

bool rag_buf_append(buf_t *b, const void *data, size_t len);
bool rag_buf_puts(buf_t *b, const char *s);
bool rag_buf_putc(buf_t *b, char c);
bool rag_buf_printf(buf_t *b, const char *fmt, ...);
/* Returns a NUL-terminated string (possibly empty), or NULL if the buffer failed. */
const char *rag_buf_cstr(buf_t *b);
/* Hands the string over to the caller, who frees it; NULL if the buffer failed. */
char *rag_buf_take(buf_t *b);
void rag_buf_free(buf_t *b);

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

void rag_err_init(err_t *e);
void rag_err_clear(err_t *e);
void rag_err_free(err_t *e);
rag_status_t rag_err_set(err_t *e, rag_status_t code, const char *fmt, ...);

/* ---- UTF-8 (utf8.c) ---- */

/*
 * A "character" is a valid UTF-8 sequence or a single byte that is not part of
 * one, so every byte string is a sequence of characters.
 */

/* Length of the valid UTF-8 sequence at s[i] (decoded in *cp), 0 if invalid. */
size_t rag_utf8_decode(const char *s, size_t len, size_t i, uint32_t *cp);
/* Length in bytes of the character at s[i] (i < len). */
size_t rag_utf8_char_len(const char *s, size_t len, size_t i);
/* Offset after `n` characters starting from s[i], or len. */
size_t rag_utf8_advance(const char *s, size_t len, size_t i, size_t n);
/* Number of characters in s[i..j), where j is a character boundary. */
size_t rag_utf8_count(const char *s, size_t i, size_t j);
/* Copy of s[0..len) with every invalid byte replaced by U+FFFD; NULL if out of memory. */
char *rag_utf8_sanitize(const char *s, size_t len, size_t *out_len);

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
bool rag_chunk_split(const char *text, size_t len, size_t size, size_t overlap, chunk_list_t *out);
void rag_chunk_list_free(chunk_list_t *list);

/* ---- sha256 (sha256.c) ---- */

typedef struct sha256 {
    uint32_t state[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} sha256_t;

void rag_sha256_init(sha256_t *s);
void rag_sha256_update(sha256_t *s, const void *data, size_t len);
void rag_sha256_final(sha256_t *s, unsigned char digest[32]);
/* Hexadecimal digest of a buffer. */
void rag_sha256_hex(const void *data, size_t len, char hex[65]);

/* ---- files (fs.c) ---- */

typedef struct fs_info {
    bool is_dir;
    bool is_file; /* a regular file */
    int64_t size;
    int64_t mtime;
} fs_info_t;

typedef struct fs_entry {
    char *path;
    char *error; /* NULL, or why the file or directory could not be visited */
} fs_entry_t;

typedef struct fs_list {
    fs_entry_t *items;
    size_t count;
    size_t cap;
} fs_list_t;

/*
 * Absolute path, without "." and "..", with the links of the directories
 * resolved. The last component of a file is kept even if it is a link, and
 * does not need to exist. NULL if out of memory.
 */
char *rag_fs_canonical(const char *path);
/* True for the extensions of text files: .txt, .md, .markdown, ... */
bool rag_fs_is_text_file(const char *name);
/* stat(); RAG_ERROR_NOT_FOUND if the path does not exist. */
rag_status_t rag_fs_stat(const char *path, fs_info_t *info, err_t *e);
/* Reads a whole file. */
rag_status_t rag_fs_read_file(const char *path, buf_t *out, err_t *e);
/*
 * Lists the text files below a directory, recursively, sorted by path.
 * Hidden files and directories (".git", ...) and links to directories are
 * skipped. Subdirectories that cannot be read are listed with an error.
 */
rag_status_t rag_fs_list_dir(const char *dir, fs_list_t *out, err_t *e);
void rag_fs_list_free(fs_list_t *list);

/* ---- the handle (rag.c) ---- */

struct sqlite3;
struct sqlite3_stmt;
struct ollama_client;

struct rag {
    struct sqlite3 *db;
    char *path;
    err_t err;
    struct ollama_client *client; /* created when first needed */

    /* settings */
    char *host;
    long timeout_ms;
    char *embed_model;
    int dimensions;           /* requested size of the embeddings, 0: the model's */
    size_t stored_dimensions; /* size of the stored embeddings, 0 if there are none */
    char *query_prefix;
    char *document_prefix;
    size_t batch_size;
    char *chat_model;
    size_t chunk_size;
    size_t chunk_overlap;
    rag_progress_fn progress;
    void *progress_data;

    struct sqlite3_stmt *stmts[8]; /* prepared statements, see db.c */
};

/* Starts a public call: checks the handle and clears the last error. */
rag_status_t rag_begin_call(rag_t *rag);

/* Reports a progress event; returns RAG_ERROR_ABORTED if the callback asks to stop. */
rag_status_t rag_report(rag_t *rag, rag_event_t event, const char *path, size_t chunks, const char *message,
                        size_t done, size_t total);

/* The Ollama client of the handle, created on first use; NULL if out of memory (the error is set). */
struct ollama_client *rag_client(rag_t *rag);

/* Sets the error of a failed Ollama call. `what` is e.g. "embedding model". */
rag_status_t rag_ollama_error(rag_t *rag, int status, const char *what, const char *model);

/* True if two model names designate the same model ("name" is "name:latest"). */
bool rag_same_model(const char *a, const char *b);

/* ---- database (db.c) ---- */

typedef struct db_file {
    int64_t id;
    char *path;
    char *hash;     /* sha256 of the content */
    char *settings; /* chunking and embedding settings the file was indexed with */
    int64_t size;
    int64_t mtime;
    int64_t indexed_at;
} db_file_t;

typedef struct db_file_list {
    db_file_t *items;
    size_t count;
    size_t cap;
} db_file_list_t;

void rag_db_file_release(db_file_t *f);
void rag_db_file_list_free(db_file_list_t *list);

/* Opens or creates the database, checks or creates the schema. */
rag_status_t rag_db_open(rag_t *rag, const char *path, bool must_exist);
void rag_db_close(rag_t *rag);

rag_status_t rag_db_begin(rag_t *rag);
/* Starts a read transaction, for consistent reads; end it with rag_db_commit(). */
rag_status_t rag_db_begin_read(rag_t *rag);
rag_status_t rag_db_commit(rag_t *rag);
void rag_db_rollback(rag_t *rag);

/* *value is NULL if the key is missing. */
rag_status_t rag_db_get_meta(rag_t *rag, const char *key, char **value);
rag_status_t rag_db_set_meta(rag_t *rag, const char *key, const char *value);
rag_status_t rag_db_delete_meta(rag_t *rag, const char *key);
rag_status_t rag_db_get_meta_int(rag_t *rag, const char *key, int64_t *value, bool *found);
rag_status_t rag_db_set_meta_int(rag_t *rag, const char *key, int64_t value);

/* Looks up a file by path; *found tells whether it exists. */
rag_status_t rag_db_find_file(rag_t *rag, const char *path, db_file_t *out, bool *found);
/* Files whose path is `dir` or starts with `dir` + "/", sorted by path. */
rag_status_t rag_db_files_under(rag_t *rag, const char *dir, db_file_list_t *out);
/* Inserts a file (its id is set) or replaces the one with the same path, dropping its chunks. */
rag_status_t rag_db_put_file(rag_t *rag, db_file_t *file);
/* Sets the hash of a file, once all its chunks are stored. */
rag_status_t rag_db_set_file_hash(rag_t *rag, int64_t id, const char *hash);
/* Deletes a file with its chunks; *chunks is the number of chunks deleted. */
rag_status_t rag_db_delete_file(rag_t *rag, int64_t id, size_t *chunks);
/* Adds a chunk and its embedding (`dims` floats, normalized) to a file. */
rag_status_t rag_db_add_chunk(rag_t *rag, int64_t file_id, size_t seq, const chunk_span_t *span, const char *text,
                              size_t text_len, const float *embedding, size_t dims);

typedef struct rag_db_counts {
    size_t files;
    size_t chunks;
    size_t vectors;
    uint64_t text_bytes;
    uint64_t database_bytes;
    int64_t last_indexed;
} db_counts_t;

rag_status_t rag_db_counts(rag_t *rag, db_counts_t *counts);

/* Calls `fn` for every stored embedding (`dims` floats, from little-endian storage). */
typedef void (*db_vector_fn)(int64_t chunk_id, const float *embedding, void *data);
rag_status_t rag_db_scan_vectors(rag_t *rag, size_t dims, db_vector_fn fn, void *data);
/* Reads a chunk: its file, position and text (allocated, released by the caller). */
rag_status_t rag_db_get_chunk(rag_t *rag, int64_t chunk_id, rag_result_t *out, bool *found);

/* ---- vectors (vector.c) ---- */

/* Embeddings are stored as little-endian float32 values. */
void rag_vec_to_blob(const float *v, size_t n, unsigned char *out);
void rag_vec_from_blob(const unsigned char *blob, size_t n, float *out);
/* Scales v to a length of 1. Returns false if v is zero (left as is). */
bool rag_vec_normalize(float *v, size_t n);
/* Dot product: the cosine similarity of normalized vectors. */
double rag_vec_dot(const float *a, const float *b, size_t n);

/* The k best scores seen, for searches. */
typedef struct topk_item {
    double score;
    int64_t id;
} topk_item_t;

typedef struct topk {
    topk_item_t *items; /* a min-heap: the worst kept score first */
    size_t count;
    size_t k;
} topk_t;

bool rag_topk_init(topk_t *t, size_t k);
/* Keeps (score, id) if it is among the k best; ties go to the smallest id. */
void rag_topk_push(topk_t *t, double score, int64_t id);
/* Sorts the items, the best first. */
void rag_topk_sort(topk_t *t);
void rag_topk_free(topk_t *t);

/* ---- search (search.c) ---- */

/* rag_search() without the checks of a public call. */
rag_status_t rag_find(rag_t *rag, const char *query, const rag_search_options_t *options, rag_results_t **results);

/* ---- embeddings (embed.c) ---- */

/*
 * Computes the embeddings of `n` texts, each preceded by `prefix`, with the
 * model of the index. On success *out holds n * *dims normalized floats, to
 * release with free(). The size must match the one of the stored embeddings.
 */
rag_status_t rag_embed_texts(rag_t *rag, const char *const *texts, size_t n, const char *prefix, float **out,
                             size_t *dims);

#endif /* RAG_INTERNAL_H */
