/* Files and directories (POSIX). */
#define _XOPEN_SOURCE 700

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Directories deeper than this are not visited. */
#define MAX_DEPTH 64

static const char *const TEXT_EXTENSIONS[] = {".txt", ".md", ".markdown", ".text", ".mdown", ".mkd"};

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

bool rag_fs_is_text_file(const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(TEXT_EXTENSIONS) / sizeof(TEXT_EXTENSIONS[0]); i++) {
        size_t m = strlen(TEXT_EXTENSIONS[i]);
        if (n <= m) continue;
        size_t k = 0;
        while (k < m && lower(name[n - m + k]) == TEXT_EXTENSIONS[i][k]) k++;
        if (k == m) return true;
    }
    return false;
}

/* Joins a directory and a name, with a single "/" between them. */
static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    return n > 0 && dir[n - 1] == '/' ? rag_format("%s%s", dir, name) : rag_format("%s/%s", dir, name);
}

/* Removes ".", ".." and repeated slashes from an absolute path, in place, without looking at the disk. */
static void normalize(char *path) {
    size_t out = 0; /* out never overtakes in */
    for (size_t in = 0; path[in];) {
        while (path[in] == '/') in++;
        size_t start = in;
        while (path[in] && path[in] != '/') in++;
        size_t n = in - start;
        if (n == 0 || (n == 1 && path[start] == '.')) continue;
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (out > 0 && path[out - 1] != '/') out--;
            if (out > 0) out--; /* the slash */
            continue;
        }
        path[out++] = '/';
        memmove(path + out, path + start, n);
        out += n;
    }
    if (out == 0) path[out++] = '/';
    path[out] = '\0';
}

char *rag_fs_canonical(const char *path, err_t *e) {
    if (!path || !*path) {
        rag_err_set(e, RAG_ERROR_INVALID, "no path");
        return NULL;
    }
    char *abs;
    if (path[0] == '/') {
        abs = rag_strdup(path);
    } else {
        char *cwd = getcwd(NULL, 0);
        if (!cwd && errno != ENOMEM) {
            rag_err_set(e, RAG_ERROR_IO, "cannot resolve '%s': cannot get the current directory: %s", path,
                        strerror(errno));
            return NULL;
        }
        abs = cwd ? join(cwd, path) : NULL;
        free(cwd);
    }
    if (!abs) {
        rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return NULL;
    }

    /* directories are resolved completely */
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode)) {
        char *real = realpath(abs, NULL);
        if (real) {
            free(abs);
            return real;
        }
    }
    /*
     * Files keep their name, even when they are links, and do not have to
     * exist: resolve the closest directory above them that exists.
     */
    for (size_t slash = strlen(abs); slash-- > 0;) {
        if (abs[slash] != '/') continue;
        abs[slash] = '\0';
        char *real = realpath(slash ? abs : "/", NULL);
        abs[slash] = '/';
        if (!real) continue;
        char *result = rag_format("%s%s", strcmp(real, "/") == 0 ? "" : real, abs + slash);
        free(real);
        free(abs);
        if (result)
            normalize(result);
        else
            rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return result;
    }
    normalize(abs);
    return abs;
}

rag_status_t rag_fs_stat(const char *path, fs_info_t *info, err_t *e) {
    struct stat st;
    if (stat(path, &st) != 0) {
        int code = errno;
        return rag_err_set(e, code == ENOENT || code == ENOTDIR ? RAG_ERROR_NOT_FOUND : RAG_ERROR_IO,
                           "cannot access '%s': %s", path, strerror(code));
    }
    info->is_dir = S_ISDIR(st.st_mode);
    info->is_file = S_ISREG(st.st_mode);
    info->size = (int64_t)st.st_size;
    info->mtime = (int64_t)st.st_mtime;
    return RAG_OK;
}

rag_status_t rag_fs_read_file(const char *path, buf_t *out, err_t *e) {
    FILE *f = fopen(path, "rb");
    if (!f) return rag_err_set(e, RAG_ERROR_IO, "cannot read '%s': %s", path, strerror(errno));
    char chunk[16384];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        if (!rag_buf_append(out, chunk, n)) break;
    int failed = ferror(f);
    int code = errno;
    fclose(f);
    if (out->oom || !rag_buf_cstr(out)) return rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
    if (failed) return rag_err_set(e, RAG_ERROR_IO, "cannot read '%s': %s", path, strerror(code));
    return RAG_OK;
}

/* ---- directory walk ---- */

static bool add_entry(fs_list_t *list, char *path, char *error) {
    if (!path) {
        free(error);
        return false;
    }
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 64;
        fs_entry_t *items = realloc(list->items, cap * sizeof(*items));
        if (!items) {
            free(path);
            free(error);
            return false;
        }
        list->items = items;
        list->cap = cap;
    }
    list->items[list->count].path = path;
    list->items[list->count].error = error;
    list->count++;
    return true;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const fs_entry_t *)a)->path, ((const fs_entry_t *)b)->path);
}

/* Returns false if out of memory. Unreadable directories are added as errors. */
static bool walk(const char *dir, int depth, fs_list_t *out) {
    if (depth > MAX_DEPTH) return add_entry(out, rag_strdup(dir), rag_strdup("too many nested directories"));
    DIR *d = opendir(dir);
    if (!d) return add_entry(out, rag_strdup(dir), rag_format("cannot open directory: %s", strerror(errno)));

    bool ok = true;
    struct dirent *ent;
    while (ok && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* ".", ".." and hidden files: .git, .obsidian, ... */
        char *path = join(dir, ent->d_name);
        if (!path) {
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(path, &st) != 0) {
            ok = add_entry(out, path, rag_format("cannot access: %s", strerror(errno)));
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            /* links to files are followed, links to directories are not: they could make cycles */
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                free(path);
                continue;
            }
        }
        if (S_ISDIR(st.st_mode)) {
            ok = walk(path, depth + 1, out);
            free(path);
        } else if (S_ISREG(st.st_mode) && rag_fs_is_text_file(ent->d_name)) {
            ok = add_entry(out, path, NULL);
        } else {
            free(path);
        }
    }
    closedir(d);
    return ok;
}

rag_status_t rag_fs_list_dir(const char *dir, fs_list_t *out, err_t *e) {
    fs_info_t info;
    rag_status_t st = rag_fs_stat(dir, &info, e);
    if (st != RAG_OK) return st;
    if (!info.is_dir) return rag_err_set(e, RAG_ERROR_INVALID, "'%s' is not a directory", dir);
    DIR *d = opendir(dir);
    if (!d) return rag_err_set(e, RAG_ERROR_IO, "cannot open directory '%s': %s", dir, strerror(errno));
    closedir(d);

    if (!walk(dir, 0, out)) return rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
    if (out->count > 1) qsort(out->items, out->count, sizeof(*out->items), compare_entries);
    return RAG_OK;
}

void rag_fs_list_free(fs_list_t *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].path);
        free(list->items[i].error);
    }
    free(list->items);
    *list = (fs_list_t){0};
}
