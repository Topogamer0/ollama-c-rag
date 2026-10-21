/*
 * Portability layer for the file system and the process environment: the
 * Win32 API on Windows, POSIX elsewhere. Every string is UTF-8: on Windows,
 * paths are converted to UTF-16 for the "W" functions, so that any name
 * works, and both "\" and "/" separate the components of a path.
 *
 * Used by the library (fs.c) and by ragc. Not installed.
 */
#ifndef RAG_COMPAT_H
#define RAG_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <wchar.h>
#define RAG_PATH_SEP '\\'
#else
#define RAG_PATH_SEP '/'
#endif

/* Why a call failed. */
typedef enum os_error_kind {
    RAG_OS_FAILED,    /* see the message */
    RAG_OS_NOT_FOUND, /* the path, or a directory above it, does not exist */
    RAG_OS_NO_MEMORY
} os_error_kind_t;

typedef struct os_error {
    os_error_kind_t kind;
    char message[256]; /* e.g. "No such file or directory" */
} os_error_t;

typedef enum os_type { RAG_OS_OTHER, RAG_OS_FILE, RAG_OS_DIR } os_type_t;

typedef struct os_stat {
    os_type_t type;
    bool is_link; /* a symbolic link (or a junction), when links are not followed */
    int64_t size;
    int64_t mtime; /* seconds since 1970 */
} os_stat_t;

/* True if c separates the components of a path: "/", and "\" on Windows. */
bool rag_os_is_sep(char c);

/*
 * Length of the root of an absolute path, separator included: "/", "C:\",
 * "\\server\share\"; 0 for a relative path.
 */
size_t rag_os_root_len(const char *path);

/*
 * The absolute form of a path, to free. On Windows the path is also
 * normalized ("." and ".." removed, "/" replaced by "\"); elsewhere it is
 * the current directory joined with the path.
 */
char *rag_os_absolute(const char *path, os_error_t *oe);

/* The absolute path of an existing file or directory, every link resolved, to free. */
char *rag_os_realpath(const char *path, os_error_t *oe);

/* The current directory, to free. */
char *rag_os_getcwd(os_error_t *oe);

/* Type, size and time of a path; `follow` follows links. */
bool rag_os_stat(const char *path, bool follow, os_stat_t *st, os_error_t *oe);

/* fopen() of a UTF-8 path. */
FILE *rag_os_fopen(const char *path, const char *mode, os_error_t *oe);

/*
 * The name of the last component of an existing path as it is written on the
 * disk, to free: on Windows, where names do not depend on case, "A.MD" may be
 * "a.md". NULL elsewhere, or when it is not known.
 */
char *rag_os_disk_name(const char *path);

/* A copy of an environment variable, to free; NULL if it is not set. */
char *rag_os_getenv(const char *name);

/* ---- directories ---- */

typedef struct os_dir os_dir_t;

typedef struct os_dirent {
    const char *name; /* valid until the next call */
    bool hidden;      /* the name starts with "." or, on Windows, it has the hidden or system attribute */
    bool has_stat;    /* st is filled (Windows), else stat the entry (POSIX) */
    os_stat_t st;     /* of the entry itself: links are not followed */
} os_dirent_t;

os_dir_t *rag_os_opendir(const char *path, os_error_t *oe);
/* Sets *ent to the next entry, or to NULL at the end ("." and ".." are skipped); false on error. */
bool rag_os_readdir(os_dir_t *d, const os_dirent_t **ent, os_error_t *oe);
void rag_os_closedir(os_dir_t *d);

#ifdef _WIN32
/* Conversions between UTF-8 and UTF-16, to free; NULL if out of memory or not valid UTF-8. */
wchar_t *rag_os_to_wide(const char *s);
char *rag_os_from_wide(const wchar_t *s);
#endif

#endif
