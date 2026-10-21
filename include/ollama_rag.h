/*
 * ollama-rag-c - local Retrieval-Augmented Generation in C, on top of Ollama.
 *
 * Conventions (the same as ollama-c):
 *   - Functions return RAG_OK (0) on success or an error code. Details about the
 *     last error are available from rag_last_error().
 *   - Option structs use designated initializers: fields left at zero / NULL take
 *     their default value.
 *   - Everything the library allocates is released by a matching *_free() or
 *     close function. There are no global variables.
 */
#ifndef OLLAMA_RAG_H
#define OLLAMA_RAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAG_VERSION_MAJOR 0
#define RAG_VERSION_MINOR 1
#define RAG_VERSION_PATCH 0
#define RAG_VERSION "0.1.0"

#if defined(__GNUC__) && __GNUC__ >= 4
#define RAG_API __attribute__((visibility("default")))
#else
#define RAG_API
#endif

/* Returns the library version, e.g. "0.1.0". */
RAG_API const char *rag_version(void);

/* ===========================================================================
 * Errors
 * ======================================================================== */

typedef enum rag_status {
    RAG_OK = 0,
    RAG_ERROR_INVALID = 1,        /* invalid arguments or options */
    RAG_ERROR_IO = 2,             /* a file or directory could not be read */
    RAG_ERROR_DATABASE = 3,       /* SQLite error, or not an ollama-rag database */
    RAG_ERROR_MODEL_MISMATCH = 4, /* the database was built with another embedding model */
    RAG_ERROR_OLLAMA = 5,         /* a request to Ollama failed */
    RAG_ERROR_NOT_FOUND = 6,      /* the file or database does not exist, or is not indexed */
    RAG_ERROR_EMPTY = 7,          /* the index contains no documents */
    RAG_ERROR_ABORTED = 8,        /* a callback asked to stop */
    RAG_ERROR_MEMORY = 9          /* out of memory */
} rag_status_t;

typedef struct rag_error {
    rag_status_t code;
    int status_code;     /* HTTP status code of a failed Ollama request, -1 otherwise */
    const char *message; /* human readable reason, "" on success; never NULL */
} rag_error_t;

/* Returns a static description of a status code. */
RAG_API const char *rag_status_string(rag_status_t status);

/* ===========================================================================
 * Opening an index
 *
 * An index is a SQLite database. It remembers the embedding model it was
 * built with and refuses to mix embeddings of another model, and it remembers
 * the chunking settings, so that later calls reuse them by default.
 * ======================================================================== */

#define RAG_DEFAULT_EMBED_MODEL "embeddinggemma"
#define RAG_DEFAULT_CHAT_MODEL "gemma3"
#define RAG_DEFAULT_CHUNK_SIZE 800
#define RAG_DEFAULT_CHUNK_OVERLAP 100
#define RAG_DEFAULT_BATCH_SIZE 32
#define RAG_DEFAULT_TOP_K 5

/* Value of rag_options_t.chunk_overlap for chunks that do not overlap. */
#define RAG_NO_OVERLAP ((size_t) - 1)

typedef struct rag rag_t;

/* What happened to a file, reported to the progress callback. */
typedef enum rag_event {
    RAG_FILE_INDEXED,   /* a new or modified file has been (re)indexed */
    RAG_FILE_UNCHANGED, /* the file was already up to date */
    RAG_FILE_REMOVED,   /* the file has been removed from the index */
    RAG_FILE_FAILED     /* the file could not be indexed and has been skipped */
} rag_event_t;

typedef struct rag_progress {
    rag_event_t event;
    const char *path;    /* the file, as stored in the index (an absolute path) */
    size_t chunks;       /* chunks added (RAG_FILE_INDEXED) or removed (RAG_FILE_REMOVED) */
    const char *message; /* the reason, for RAG_FILE_FAILED; NULL otherwise */
    size_t done;         /* files of the directory examined so far, by rag_add_dir */
    size_t total;        /* files found in the directory, by rag_add_dir; 0 otherwise */
} rag_progress_t;

/* Progress callback: return nonzero to stop, the call then fails with RAG_ERROR_ABORTED. */
typedef int (*rag_progress_fn)(const rag_progress_t *progress, void *user_data);

typedef struct rag_options {
    /* Ollama server; NULL: $OLLAMA_HOST, or http://127.0.0.1:11434 */
    const char *host;
    /* Network timeout in milliseconds (connecting, waiting for data); 0: none */
    long timeout_ms;

    /* Embedding model; NULL: the one of the database, or RAG_DEFAULT_EMBED_MODEL for a new one */
    const char *embed_model;
    /* Size of the embeddings, for models that can shorten them; 0: the database's, or the model's */
    int dimensions;
    /*
     * Text put before questions / documents when computing their embeddings,
     * as some models expect ("search_query: " for nomic-embed-text, ...).
     * NULL: the database's, or the right one for well-known models; "": none.
     */
    const char *query_prefix;
    const char *document_prefix;
    /* Chunks embedded per request to Ollama; 0: RAG_DEFAULT_BATCH_SIZE */
    size_t batch_size;

    /* Chat model used by rag_ask; NULL: RAG_DEFAULT_CHAT_MODEL */
    const char *chat_model;

    /* Maximum characters per chunk; 0: the database's, or RAG_DEFAULT_CHUNK_SIZE */
    size_t chunk_size;
    /* Characters shared by consecutive chunks; 0: the database's, or RAG_DEFAULT_CHUNK_OVERLAP */
    size_t chunk_overlap;

    /* Fail with RAG_ERROR_NOT_FOUND if the database does not exist, instead of creating it */
    bool must_exist;

    /* Called for every file processed by rag_add_file, rag_add_dir and rag_remove_file */
    rag_progress_fn progress;
    void *progress_data;
} rag_options_t;

/*
 * Opens the index stored in the SQLite database `path`, creating it if needed.
 * `options` can be NULL. On success *rag is the new handle. On failure *rag is
 * still set, unless out of memory, so that rag_last_error(*rag) can tell why;
 * the handle cannot be used for anything else and must be closed.
 */
RAG_API rag_status_t rag_open(const char *path, const rag_options_t *options, rag_t **rag);

/* Closes the database and releases the handle. NULL is ignored. */
RAG_API void rag_close(rag_t *rag);

/* Details of the last error of a handle. Never NULL, also for a NULL handle. */
RAG_API const rag_error_t *rag_last_error(const rag_t *rag);

/* ===========================================================================
 * Indexing
 * ======================================================================== */

/* Counts of what a call did; every call resets them. */
typedef struct rag_index_stats {
    size_t files_indexed;   /* new or modified files, (re)indexed */
    size_t files_unchanged; /* files already up to date */
    size_t files_removed;   /* files removed from the index */
    size_t files_failed;    /* files skipped because they could not be indexed */
    size_t chunks_added;
    size_t chunks_removed;
} rag_index_stats_t;

/*
 * Removes a file from the index, or all the indexed files below a directory.
 * The file does not need to exist anymore. Fails with RAG_ERROR_NOT_FOUND if
 * nothing is indexed under `path`. `stats` can be NULL.
 */
RAG_API rag_status_t rag_remove_file(rag_t *rag, const char *path, rag_index_stats_t *stats);

/* ===========================================================================
 * Information
 * ======================================================================== */

typedef struct rag_stats {
    const char *path;        /* the database file */
    const char *embed_model; /* embedding model of the index */
    size_t dimensions;       /* size of the embeddings, 0 if nothing has been indexed yet */
    size_t chunk_size;       /* chunking settings used for new files */
    size_t chunk_overlap;
    size_t files;            /* indexed files */
    size_t chunks;           /* indexed chunks */
    uint64_t text_bytes;     /* total size of the chunks' text */
    uint64_t database_bytes; /* size of the database */
    int64_t last_indexed;    /* time of the last indexing (Unix time), 0 if never */
} rag_stats_t;

/* Fills `stats`; its strings belong to the handle and are valid until rag_close(). */
RAG_API rag_status_t rag_stats(rag_t *rag, rag_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* OLLAMA_RAG_H */
