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
    RAG_ERROR_NOT_FOUND = 6,      /* the file is not in the index */
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

#ifdef __cplusplus
}
#endif

#endif /* OLLAMA_RAG_H */
