/* Files and directories, on top of the portability layer of compat.h. */
#include "internal.h"

#include "compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Joins a directory and a name, with a single separator between them. */
static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    return n > 0 && rag_os_is_sep(dir[n - 1]) ? rag_format("%s%s", dir, name)
                                              : rag_format("%s%c%s", dir, RAG_PATH_SEP, name);
}

#ifndef _WIN32
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
#endif

/* `real` (a resolved directory) followed by `rest` (a separator and names), cleaned up. */
static char *resolved(const char *real, const char *rest) {
    size_t n = strlen(real);
    if (n > 0 && rag_os_is_sep(real[n - 1])) n--; /* "/" or "C:\" */
    char *path = rag_format("%.*s%s", (int)n, real, rest);
    if (!path) return NULL;
#ifdef _WIN32
    /* names do not depend on case: spell the file as on the disk, so that it has one path only */
    char *name = rag_os_disk_name(path);
    if (name) {
        const char *last = path + strlen(path);
        while (last > path && !rag_os_is_sep(last[-1])) last--;
        char *fixed = rag_format("%.*s%s", (int)(last - path), path, name);
        free(name);
        if (fixed) {
            free(path);
            path = fixed;
        }
    }
#else
    normalize(path);
#endif
    return path;
}

char *rag_fs_canonical(const char *path, err_t *e) {
    if (!path || !*path) {
        rag_err_set(e, RAG_ERROR_INVALID, "no path");
        return NULL;
    }
    os_error_t oe;
    char *abs = rag_os_absolute(path, &oe);
    if (!abs) {
        if (oe.kind == RAG_OS_NO_MEMORY)
            rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        else
            rag_err_set(e, RAG_ERROR_IO, "cannot resolve '%s': %s", path, oe.message);
        return NULL;
    }
    /* "dir/" is "dir", but the root keeps its separator */
    size_t root = rag_os_root_len(abs), len = strlen(abs);
    while (len > root && len > 1 && rag_os_is_sep(abs[len - 1])) abs[--len] = '\0';

    /* directories are resolved completely */
    os_stat_t st;
    if (rag_os_stat(abs, true, &st, NULL) && st.type == RAG_OS_DIR) {
        char *real = rag_os_realpath(abs, NULL);
        if (real) {
            free(abs);
            return real;
        }
    }
    /*
     * Files keep their name, even when they are links, and do not have to
     * exist: resolve the closest directory above them that exists.
     */
    for (size_t sep = len; sep-- > 0;) {
        if (!rag_os_is_sep(abs[sep])) continue;
        if (sep + 1 < root) break;                 /* inside the root, like "\\server\share" */
        size_t end = sep + 1 == root ? root : sep; /* a root keeps its separator */
        char saved = abs[end];
        abs[end] = '\0';
        char *real = rag_os_realpath(abs, NULL);
        abs[end] = saved;
        if (!real) continue;
        char *result = resolved(real, abs + sep);
        free(real);
        free(abs);
        if (!result) rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return result;
    }
#ifndef _WIN32
    normalize(abs);
#endif
    return abs;
}

rag_status_t rag_fs_stat(const char *path, fs_info_t *info, err_t *e) {
    *info = (fs_info_t){0};
    os_stat_t st;
    os_error_t oe;
    if (!rag_os_stat(path, true, &st, &oe)) {
        if (oe.kind == RAG_OS_NO_MEMORY) return rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return rag_err_set(e, oe.kind == RAG_OS_NOT_FOUND ? RAG_ERROR_NOT_FOUND : RAG_ERROR_IO,
                           "cannot access '%s': %s", path, oe.message);
    }
    info->is_dir = st.type == RAG_OS_DIR;
    info->is_file = st.type == RAG_OS_FILE;
    info->size = st.size;
    info->mtime = st.mtime;
    return RAG_OK;
}

rag_status_t rag_fs_read_file(const char *path, buf_t *out, err_t *e) {
    os_error_t oe;
    FILE *f = rag_os_fopen(path, "rb", &oe);
    if (!f) {
        if (oe.kind == RAG_OS_NO_MEMORY) return rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return rag_err_set(e, RAG_ERROR_IO, "cannot read '%s': %s", path, oe.message);
    }
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
    os_error_t oe;
    os_dir_t *d = rag_os_opendir(dir, &oe);
    if (!d) {
        if (oe.kind == RAG_OS_NO_MEMORY) return false;
        return add_entry(out, rag_strdup(dir), rag_format("cannot open directory: %s", oe.message));
    }

    bool ok = true;
    const os_dirent_t *ent;
    while (ok) {
        if (!rag_os_readdir(d, &ent, &oe)) {
            /* reported, so that the files that were not listed are not removed from the index */
            ok = oe.kind != RAG_OS_NO_MEMORY &&
                 add_entry(out, rag_strdup(dir), rag_format("cannot read directory: %s", oe.message));
            break;
        }
        if (!ent) break;
        if (ent->hidden) continue; /* .git, .obsidian, ... */
        char *path = join(dir, ent->name);
        if (!path) {
            ok = false;
            break;
        }
        os_stat_t st;
        if (ent->has_stat) {
            st = ent->st;
        } else if (!rag_os_stat(path, false, &st, &oe)) {
            if (oe.kind == RAG_OS_NO_MEMORY) {
                free(path);
                ok = false;
            } else {
                ok = add_entry(out, path, rag_format("cannot access: %s", oe.message));
            }
            continue;
        }
        if (st.is_link) {
            /* links to files are followed, links to directories are not: they could make cycles */
            if (!rag_os_stat(path, true, &st, NULL) || st.type != RAG_OS_FILE) {
                free(path);
                continue;
            }
        }
        if (st.type == RAG_OS_DIR) {
            ok = walk(path, depth + 1, out);
            free(path);
        } else if (st.type == RAG_OS_FILE && rag_fs_is_text_file(ent->name)) {
            ok = add_entry(out, path, NULL);
        } else {
            free(path);
        }
    }
    rag_os_closedir(d);
    return ok;
}

rag_status_t rag_fs_list_dir(const char *dir, fs_list_t *out, err_t *e) {
    fs_info_t info;
    rag_status_t st = rag_fs_stat(dir, &info, e);
    if (st != RAG_OK) return st;
    if (!info.is_dir) return rag_err_set(e, RAG_ERROR_INVALID, "'%s' is not a directory", dir);
    os_error_t oe;
    os_dir_t *d = rag_os_opendir(dir, &oe);
    if (!d) {
        if (oe.kind == RAG_OS_NO_MEMORY) return rag_err_set(e, RAG_ERROR_MEMORY, "out of memory");
        return rag_err_set(e, RAG_ERROR_IO, "cannot open directory '%s': %s", dir, oe.message);
    }
    rag_os_closedir(d);

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
