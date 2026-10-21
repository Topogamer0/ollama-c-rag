/*
 * Temporary files for the tests, on POSIX systems and on Windows, on top of
 * the portability layer of the library (src/compat.h). Paths are UTF-8; on
 * Windows path_of() writes them with "\" separators, like the library.
 * On POSIX systems, define _XOPEN_SOURCE 700 (and _DARWIN_C_SOURCE for macOS)
 * before including any header.
 */
#ifndef RAG_TEST_FILES_H
#define RAG_TEST_FILES_H

#include "compat.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static inline char *dup_string(const char *s) {
    char *copy = malloc(strlen(s) + 1);
    if (!copy) exit(2);
    return strcpy(copy, s);
}

/* A growable string. */
typedef struct string {
    char *data;
    size_t len;
} string_t;

static inline void string_append(string_t *s, const char *text) {
    size_t n = strlen(text);
    char *data = realloc(s->data, s->len + n + 1);
    if (!data) exit(2);
    memcpy(data + s->len, text, n + 1);
    s->data = data;
    s->len += n;
}

#ifdef _WIN32
/* A path for the "W" functions, long ones included. */
static inline wchar_t *test_wide(const char *path) {
    wchar_t *w = rag_os_wide_path(path, NULL);
    if (!w) {
        fprintf(stderr, "invalid path: %s\n", path);
        exit(2);
    }
    return w;
}
#endif

/* mkdir(); false if it fails (it exists, ...). */
static inline bool make_dir(const char *path) {
#ifdef _WIN32
    wchar_t *w = test_wide(path);
    bool ok = CreateDirectoryW(w, NULL) != 0;
    free(w);
    return ok;
#else
    return mkdir(path, 0755) == 0;
#endif
}

/* rmdir() of an empty directory. */
static inline bool remove_dir(const char *path) {
#ifdef _WIN32
    wchar_t *w = test_wide(path);
    bool ok = RemoveDirectoryW(w) != 0;
    free(w);
    return ok;
#else
    return rmdir(path) == 0;
#endif
}

/* unlink() of a file, or of a link. */
static inline bool remove_file(const char *path) {
#ifdef _WIN32
    wchar_t *w = test_wide(path);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL); /* read-only files too */
    bool ok = DeleteFileW(w) != 0;
    free(w);
    return ok;
#else
    return unlink(path) == 0;
#endif
}

/* chdir(). */
static inline bool change_dir(const char *path) {
#ifdef _WIN32
    wchar_t *w = rag_os_to_wide(path); /* without \\?\, which the current directory does not take */
    if (!w) exit(2);
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    bool ok = SetCurrentDirectoryW(w) != 0;
    free(w);
    return ok;
#else
    return chdir(path) == 0;
#endif
}

/* The current directory, to free. */
static inline char *current_dir(void) {
    char *cwd = rag_os_getcwd(NULL);
    if (!cwd) {
        fprintf(stderr, "cannot get the current directory\n");
        exit(2);
    }
    return cwd;
}

/*
 * A symbolic link at `path` to `target` (relative to the link's directory);
 * false where links cannot be made (Windows without the privilege or the
 * developer mode), or do not behave as on Windows (Wine).
 */
static inline bool make_link(const char *target, const char *path, bool to_dir) {
#ifdef _WIN32
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) return false;
    wchar_t *t = rag_os_to_wide(target), *w = test_wide(path);
    if (!t) exit(2);
    for (wchar_t *p = t; *p; p++)
        if (*p == L'/') *p = L'\\';
    DWORD flags = (to_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    bool ok = CreateSymbolicLinkW(w, t, flags) != 0;
    if (!ok && GetLastError() == ERROR_INVALID_PARAMETER) /* Windows before 10 1703 */
        ok = CreateSymbolicLinkW(w, t, flags & ~(DWORD)SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    free(t);
    free(w);
    return ok;
#else
    (void)to_dir;
    return symlink(target, path) == 0;
#endif
}

/* Creates a new directory in the current one and returns its absolute, resolved path. */
static inline char *make_temp_dir(void) {
    char name[64];
    for (unsigned attempt = 0;; attempt++) {
#ifdef _WIN32
        snprintf(name, sizeof(name), "ragtest-%lu-%u", (unsigned long)GetCurrentProcessId(), attempt);
#else
        snprintf(name, sizeof(name), "ragtest-%ld-%u", (long)getpid(), attempt);
#endif
        if (make_dir(name)) break;
        if (attempt == 1000) {
            fprintf(stderr, "cannot create a temporary directory\n");
            exit(2);
        }
    }
    char *path = rag_os_realpath(name, NULL);
    if (!path) {
        fprintf(stderr, "cannot resolve %s\n", name);
        exit(2);
    }
    return path;
}

/*
 * Joins path components: path_of(dir, "sub/a.md"), with the separators of the
 * system. Returns a static buffer (the last 8 results stay valid).
 */
static inline const char *path_of(const char *dir, const char *name) {
    static char buffers[8][4096];
    static int next;
    char *out = buffers[next++ % 8];
    snprintf(out, sizeof(buffers[0]), "%s%c%s", dir, RAG_PATH_SEP, name);
    for (char *p = out; *p; p++)
        if (rag_os_is_sep(*p)) *p = RAG_PATH_SEP;
    return out;
}

/* Writes a file, creating the directories above it. */
static inline void write_file(const char *path, const char *content) {
    char *dir = dup_string(path);
    size_t root = rag_os_root_len(dir);
    for (char *p = dir + (root ? root : 1); *p; p++) {
        if (!rag_os_is_sep(*p)) continue;
        char sep = *p;
        *p = '\0';
        make_dir(dir);
        *p = sep;
    }
    free(dir);
    FILE *f = rag_os_fopen(path, "wb", NULL);
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static inline bool file_exists(const char *path) {
    os_stat_t st;
    return rag_os_stat(path, false, &st, NULL);
}

static inline void remove_tree(const char *path) {
    os_stat_t st;
    if (!rag_os_stat(path, false, &st, NULL)) return;
    if (st.type != RAG_OS_DIR || st.is_link) {
        /* a link to a directory is removed as a directory on Windows */
        if (!remove_file(path)) remove_dir(path);
        return;
    }
#ifndef _WIN32
    chmod(path, 0755);
#endif
    os_dir_t *d = rag_os_opendir(path, NULL);
    const os_dirent_t *ent;
    while (d && rag_os_readdir(d, &ent, NULL) && ent) {
        size_t n = strlen(path) + strlen(ent->name) + 2;
        char *child = malloc(n);
        if (!child) exit(2);
        snprintf(child, n, "%s%c%s", path, RAG_PATH_SEP, ent->name);
        remove_tree(child);
        free(child);
    }
    rag_os_closedir(d);
    remove_dir(path);
}

#endif
