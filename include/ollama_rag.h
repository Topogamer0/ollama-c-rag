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
#define RAG_VERSION_MINOR 2
#define RAG_VERSION_PATCH 0
#define RAG_VERSION "0.2.0"

/*
 * The functions of the library. A Windows DLL exports them (RAG_BUILDING_SHARED)
 * and its users import them (RAG_SHARED, set by the CMake target).
 */
#if defined(_WIN32)
#if defined(RAG_BUILDING_SHARED)
#define RAG_API __declspec(dllexport)
#elif defined(RAG_SHARED)
#define RAG_API __declspec(dllimport)
#else
#define RAG_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define RAG_API __attribute__((visibility("default")))
#else
#define RAG_API
#endif

/* Returns the library version, e.g. "0.2.0". */
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
#define RAG_MAX_BATCH_SIZE 65536

/* Value of rag_options_t.chunk_overlap for chunks that do not overlap. */
#define RAG_NO_OVERLAP ((size_t) - 1)

typedef struct rag rag_t;

/* What happened to a file, reported to the progress callback. */
typedef enum rag_event {
    RAG_FILE_INDEXED,   /* a new or modified file has been (re)indexed */
    RAG_FILE_UNCHANGED, /* the file was already up to date (or another handle indexed it meanwhile) */
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
    /* Chunks embedded per request to Ollama, at most RAG_MAX_BATCH_SIZE; 0: RAG_DEFAULT_BATCH_SIZE */
    size_t batch_size;

    /* Chat model used by rag_ask; NULL: RAG_DEFAULT_CHAT_MODEL */
    const char *chat_model;

    /* Maximum characters per chunk; 0: the database's, or RAG_DEFAULT_CHUNK_SIZE */
    size_t chunk_size;
    /*
     * Characters shared by consecutive chunks (less than chunk_size); 0: the
     * database's, or RAG_DEFAULT_CHUNK_OVERLAP but at most a quarter of
     * chunk_size; RAG_NO_OVERLAP: none
     */
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
 *
 * Paths are UTF-8 strings, also on Windows, relative to the current directory
 * or absolute. The index stores them absolute, with the links of their
 * directories resolved; on Windows with "\" separators ("/" is accepted too)
 * and with the names spelled as on the disk.
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
 * Indexes a text file, whatever its extension: splits it into chunks, computes
 * their embeddings and stores them, replacing the previous version of the
 * file. Does nothing if the file has not changed since it was indexed with the
 * same settings. `stats` can be NULL.
 */
RAG_API rag_status_t rag_add_file(rag_t *rag, const char *path, rag_index_stats_t *stats);

/*
 * Indexes the text files (.txt, .md, .markdown, ...) below a directory,
 * recursively, skipping hidden files and directories. Only new and modified
 * files are processed, and the files of the directory that have been deleted
 * are removed from the index. Files that cannot be read are reported to the
 * progress callback (RAG_FILE_FAILED) and skipped. `stats` can be NULL.
 *
 * Embeddings are computed in batches that can span several files. Every file
 * is stored as soon as all its chunks have been embedded: if the call fails or
 * is stopped, the files already stored stay indexed (and counted in `stats`),
 * and a file that was being indexed is indexed again by the next call.
 *
 * Several handles, in the same process or not, can index the same database:
 * the embedding model and size are checked again before anything is stored,
 * and a file written by two of them at the same time is kept from the one
 * that started writing it last.
 */
RAG_API rag_status_t rag_add_dir(rag_t *rag, const char *path, rag_index_stats_t *stats);

/*
 * Removes a file from the index, or all the indexed files below a directory.
 * The file does not need to exist anymore. Files are removed by groups, each
 * in a transaction of its own, then reported: if the progress callback stops
 * the call, the files removed so far stay removed (and counted in `stats`).
 * Fails with RAG_ERROR_NOT_FOUND if nothing is indexed under `path`. `stats`
 * can be NULL.
 */
RAG_API rag_status_t rag_remove_file(rag_t *rag, const char *path, rag_index_stats_t *stats);

/* ===========================================================================
 * Searching
 *
 * The question is embedded with the model of the index and compared with
 * every chunk (cosine similarity, exact search).
 * ======================================================================== */

typedef struct rag_search_options {
    size_t top_k;     /* number of chunks to return; 0: RAG_DEFAULT_TOP_K */
    double min_score; /* chunks with a lower score are dropped; 0: none are */
} rag_search_options_t;

/* A chunk found by a search. */
typedef struct rag_result {
    const char *path;  /* the file it comes from */
    size_t chunk;      /* position of the chunk in the file, from 0 */
    size_t start, end; /* byte offsets of the chunk in the file */
    size_t line_start; /* first line of the chunk, from 1 */
    size_t line_end;   /* last line of the chunk */
    const char *text;  /* the text of the chunk */
    double score;      /* cosine similarity with the question, from -1 to 1 */
} rag_result_t;

typedef struct rag_results {
    const rag_result_t *items; /* the most similar first */
    size_t count;
} rag_results_t;

/*
 * Finds the chunks most similar to `query`. `options` can be NULL. An empty
 * index gives no results. Release *results with rag_results_free().
 */
RAG_API rag_status_t rag_search(rag_t *rag, const char *query, const rag_search_options_t *options,
                                rag_results_t **results);

/* Releases search results. NULL is ignored. */
RAG_API void rag_results_free(rag_results_t *results);

/* ===========================================================================
 * Answering
 *
 * rag_ask() searches the chunks most similar to the question, gives them to
 * the chat model as numbered passages ([1], [2], ...) and streams its answer.
 * ======================================================================== */

/* Receives the answer while it is generated; return nonzero to stop (rag_ask then fails with RAG_ERROR_ABORTED). */
typedef int (*rag_token_fn)(const char *text, void *user_data);

typedef struct rag_ask_options {
    size_t top_k;              /* chunks given to the model; 0: RAG_DEFAULT_TOP_K */
    double min_score;          /* chunks with a lower score are not given to the model; 0: none are dropped */
    const char *chat_model;    /* NULL: the chat model of the handle */
    const char *system_prompt; /* NULL: one asking to answer from the passages only, citing them; "": none */
    rag_token_fn on_token;     /* called with every piece of the answer; NULL: no streaming */
    void *user_data;
} rag_ask_options_t;

typedef struct rag_answer {
    const char *text;            /* the whole answer */
    const char *model;           /* the chat model that wrote it */
    const rag_result_t *sources; /* the passages given to the model: [n] in the answer is sources[n - 1] */
    size_t sources_count;
} rag_answer_t;

/*
 * Answers a question from the indexed documents. `options` can be NULL.
 * Fails with RAG_ERROR_EMPTY if nothing has been indexed. Release *answer
 * with rag_answer_free().
 */
RAG_API rag_status_t rag_ask(rag_t *rag, const char *question, const rag_ask_options_t *options, rag_answer_t **answer);

/* Releases an answer. NULL is ignored. */
RAG_API void rag_answer_free(rag_answer_t *answer);

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

/* Fills `stats`; its strings belong to the handle and are valid until the next rag_stats() or rag_close(). */
RAG_API rag_status_t rag_stats(rag_t *rag, rag_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* OLLAMA_RAG_H */
