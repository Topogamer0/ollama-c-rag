/* Portability layer: the Win32 API on Windows, POSIX elsewhere (see compat.h). */
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* _wfopen, _wgetenv, strerror */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* Vista: GetFinalPathNameByHandleW */
#endif
#else
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE /* getattrlist */
#endif

#include "compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stddef.h>
#include <sys/attr.h>
#endif
#endif

static void set_message(os_error_t *oe, os_error_kind_t kind, const char *message) {
    if (!oe) return;
    oe->kind = kind;
    size_t n = strlen(message);
    if (n >= sizeof(oe->message)) {
        n = sizeof(oe->message) - 1;
        while (n > 0 && ((unsigned char)message[n] & 0xC0) == 0x80) n--; /* not inside a character */
    }
    memcpy(oe->message, message, n);
    oe->message[n] = '\0';
}

static void no_memory(os_error_t *oe) { set_message(oe, RAG_OS_NO_MEMORY, "out of memory"); }

/* a + b + c, to free. */
static char *concat(const char *a, const char *b, const char *c) {
    size_t n = strlen(a), m = strlen(b), k = strlen(c);
    char *s = malloc(n + m + k + 1);
    if (!s) return NULL;
    memcpy(s, a, n);
    memcpy(s + n, b, m);
    memcpy(s + n + m, c, k + 1);
    return s;
}

bool rag_os_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#ifdef _WIN32

/* ======================================================================== */
/* Windows                                                                   */
/* ======================================================================== */

wchar_t *rag_os_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(*w));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

char *rag_os_from_wide(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    return s;
}

static bool is_not_found(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY: return true;
    default: return false;
    }
}

/* Fills *oe from a Win32 error code, with the message of the system ("Access is denied"). */
static void set_error(os_error_t *oe, DWORD code) {
    if (!oe) return;
    if (code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY) {
        no_memory(oe);
        return;
    }
    os_error_kind_t kind = is_not_found(code) ? RAG_OS_NOT_FOUND : RAG_OS_FAILED;
    wchar_t *text = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    /* in English when the system has it, like the other messages */
    DWORD n = FormatMessageW(flags, NULL, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (LPWSTR)&text, 0, NULL);
    if (n == 0) n = FormatMessageW(flags, NULL, code, 0, (LPWSTR)&text, 0, NULL);
    char *message = n ? rag_os_from_wide(text) : NULL;
    if (text) LocalFree(text);
    if (!message) {
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "error %lu", (unsigned long)code);
        set_message(oe, kind, fallback);
        return;
    }
    /* "The system cannot find the file specified.\r\n" */
    size_t len = strlen(message);
    while (len > 0 &&
           (message[len - 1] == '\r' || message[len - 1] == '\n' || message[len - 1] == ' ' || message[len - 1] == '.'))
        len--;
    message[len] = '\0';
    set_message(oe, kind, message);
    free(message);
}

static void set_last_error(os_error_t *oe) { set_error(oe, GetLastError()); }

/* A UTF-8 string in UTF-16, with *oe filled on failure. */
static wchar_t *widen(const char *s, os_error_t *oe) {
    wchar_t *w = rag_os_to_wide(s);
    if (!w) {
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) <= 0)
            set_message(oe, RAG_OS_FAILED, "the path is not valid UTF-8");
        else
            no_memory(oe);
    }
    return w;
}

static bool starts_with(const wchar_t *s, const wchar_t *prefix) { return wcsncmp(s, prefix, wcslen(prefix)) == 0; }

/* The full path of a UTF-16 path, to free (GetFullPathNameW). */
static wchar_t *full_path(const wchar_t *w, os_error_t *oe) {
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        set_last_error(oe);
        return NULL;
    }
    for (;;) {
        wchar_t *full = malloc((size_t)n * sizeof(*full));
        if (!full) {
            no_memory(oe);
            return NULL;
        }
        DWORD m = GetFullPathNameW(w, n, full, NULL);
        if (m == 0) {
            set_last_error(oe);
            free(full);
            return NULL;
        }
        if (m < n) return full;
        free(full); /* the current directory changed meanwhile */
        n = m;
    }
}

/* True for a full path without empty, "." or ".." components, which Windows can take as it is. */
static bool is_normalized(const char *path) {
    size_t root = rag_os_root_len(path);
    if (root == 0) return false;
    for (const char *p = path + root; *p;) {
        const char *end = p;
        while (*end && !rag_os_is_sep(*end)) end++;
        size_t n = (size_t)(end - p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (*end && !end[1]) return false; /* a separator at the end */
        p = *end ? end + 1 : end;
    }
    return true;
}

wchar_t *rag_os_wide_path(const char *path, os_error_t *oe) {
    wchar_t *w = widen(path, oe);
    if (!w) return NULL;
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    if (starts_with(w, L"\\\\?\\") || starts_with(w, L"\\\\.\\")) return w;
    if (!is_normalized(path)) {
        /* relative, or with "." and "..": read as Windows reads it, and MAX_PATH applies to the full path */
        wchar_t *full = full_path(w, oe);
        free(w);
        if (!full || starts_with(full, L"\\\\?\\") || starts_with(full, L"\\\\.\\")) return full; /* "con" */
        w = full;
    }
    bool unc = starts_with(w, L"\\\\");
    bool drive = w[0] && w[1] == L':' && w[2] == L'\\';
    if (!unc && !drive) return w;
    /*
     * "\\?\" lifts the limit of MAX_PATH, and keeps the names as they are:
     * Windows would otherwise drop a final "." ("v1." is a directory of its
     * own) and read "con" or "nul" as devices.
     */
    const wchar_t *prefix = unc ? L"\\\\?\\UNC" : L"\\\\?\\";
    const wchar_t *rest = unc ? w + 1 : w; /* "\\server\share" -> "\\?\UNC\server\share" */
    size_t n = wcslen(prefix), m = wcslen(rest);
    wchar_t *long_path = malloc((n + m + 1) * sizeof(*long_path));
    if (long_path) {
        memcpy(long_path, prefix, n * sizeof(*long_path));
        memcpy(long_path + n, rest, (m + 1) * sizeof(*long_path));
    } else {
        no_memory(oe);
    }
    free(w);
    return long_path;
}

/* "\\?\C:\x" -> "C:\x", "\\?\UNC\server\share" -> "\\server\share", in place. */
static wchar_t *strip_long_prefix(wchar_t *w) {
    if (starts_with(w, L"\\\\?\\UNC\\")) {
        w[6] = L'\\';
        return w + 6;
    }
    if (starts_with(w, L"\\\\?\\") && w[4] && w[5] == L':') return w + 4;
    return w;
}

size_t rag_os_root_len(const char *path) {
    size_t skip = 0;
    bool unc = false;
    if (strncmp(path, "\\\\?\\UNC\\", 8) == 0) {
        skip = 8; /* "\\?\UNC\server\share\" */
        unc = true;
    } else if (strncmp(path, "\\\\?\\", 4) == 0 || strncmp(path, "\\\\.\\", 4) == 0) {
        skip = 4; /* "\\?\C:\" */
    } else if (rag_os_is_sep(path[0]) && rag_os_is_sep(path[1])) {
        skip = 2; /* "\\server\share\" */
        unc = true;
    }
    const char *p = path + skip;
    if (!unc) {
        bool letter = (p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z');
        return letter && p[1] == ':' && rag_os_is_sep(p[2]) ? skip + 3 : 0;
    }
    /* the server, then the share */
    size_t n = 0;
    for (int part = 0; part < 2; part++) {
        while (p[n] && !rag_os_is_sep(p[n])) n++;
        if (p[n]) n++;
    }
    return skip + n;
}

char *rag_os_absolute(const char *path, os_error_t *oe) {
    wchar_t *w = widen(path, oe);
    if (!w) return NULL;
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    wchar_t *full = full_path(w, oe);
    free(w);
    if (!full) return NULL;
    char *result = rag_os_from_wide(full);
    free(full);
    if (!result) no_memory(oe);
    return result;
}

/* Opens a file or a directory to read its attributes, following links. */
static HANDLE open_handle(const char *path, os_error_t *oe) {
    wchar_t *w = rag_os_wide_path(path, oe);
    if (!w) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) set_last_error(oe);
    free(w);
    return h;
}

/* The full path of an existing file, with every component spelled as on the disk, and the drive letter in capitals. */
static char *disk_spelling(const char *path, os_error_t *oe) {
    char *abs = rag_os_absolute(path, oe);
    if (!abs) return NULL;
    size_t root = rag_os_root_len(abs);
    if (root >= 2 && abs[1] == ':' && abs[0] >= 'a' && abs[0] <= 'z') abs[0] = (char)(abs[0] - 'a' + 'A');
    char *result = malloc(root + 1);
    if (!result) {
        free(abs);
        no_memory(oe);
        return NULL;
    }
    memcpy(result, abs, root);
    result[root] = '\0';
    for (char *p = abs + root; *p && result;) {
        char *end = p;
        while (*end && !rag_os_is_sep(*end)) end++;
        char saved = *end;
        *end = '\0';
        size_t n = strlen(result);
        char *next = concat(result, n > 0 && !rag_os_is_sep(result[n - 1]) ? "\\" : "", p);
        char *name = next ? rag_os_disk_name(next) : NULL;
        if (name) {
            free(next);
            next = concat(result, n > 0 && !rag_os_is_sep(result[n - 1]) ? "\\" : "", name);
            free(name);
        }
        free(result);
        result = next;
        *end = saved;
        p = saved ? end + 1 : end;
    }
    free(abs);
    if (!result) no_memory(oe);
    return result;
}

char *rag_os_realpath(const char *path, os_error_t *oe) {
    HANDLE h = open_handle(path, oe);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, flags);
    wchar_t *final = n ? malloc((size_t)n * sizeof(*final)) : NULL;
    DWORD m = final ? GetFinalPathNameByHandleW(h, final, n, flags) : 0;
    CloseHandle(h);
    if (n && !final) {
        no_memory(oe);
        return NULL;
    }
    char *result = NULL;
    if (m > 0 && m < n) {
        result = rag_os_from_wide(strip_long_prefix(final));
        if (!result) no_memory(oe);
    } else {
        /* some volumes cannot tell (RAM disks, ...): the full path, spelled as on the disk */
        result = disk_spelling(path, oe);
    }
    free(final);
    return result;
}

char *rag_os_getcwd(os_error_t *oe) {
    DWORD n = GetCurrentDirectoryW(0, NULL);
    for (;;) {
        if (n == 0) {
            set_last_error(oe);
            return NULL;
        }
        wchar_t *w = malloc((size_t)n * sizeof(*w));
        if (!w) {
            no_memory(oe);
            return NULL;
        }
        DWORD m = GetCurrentDirectoryW(n, w);
        if (m > 0 && m < n) {
            char *result = rag_os_from_wide(w);
            free(w);
            if (!result) no_memory(oe);
            return result;
        }
        free(w);
        if (m == 0) {
            set_last_error(oe);
            return NULL;
        }
        n = m; /* it changed meanwhile */
    }
}

static int64_t unix_time(FILETIME t) {
    uint64_t ticks = (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime; /* 100 ns since 1601 */
    return (int64_t)(ticks / 10000000) - INT64_C(11644473600);
}

static void fill_stat(os_stat_t *st, DWORD attributes, DWORD size_high, DWORD size_low, FILETIME mtime) {
    st->type = attributes & FILE_ATTRIBUTE_DIRECTORY ? RAG_OS_DIR
               : attributes & FILE_ATTRIBUTE_DEVICE  ? RAG_OS_OTHER
                                                     : RAG_OS_FILE;
    st->is_link = false;
    st->size = (int64_t)((uint64_t)size_high << 32 | size_low);
    st->mtime = unix_time(mtime);
}

/* Symbolic links and junctions; other reparse points (OneDrive files, ...) are ordinary files and directories. */
static bool is_link_tag(DWORD attributes, DWORD tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

static bool has_wildcards(const char *path) { return strchr(path, '*') || strchr(path, '?'); }

bool rag_os_stat(const char *path, bool follow, os_stat_t *st, os_error_t *oe) {
    wchar_t *w = rag_os_wide_path(path, oe);
    if (!w) return false;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    DWORD code = GetLastError();
    if (!ok) {
        free(w);
        set_error(oe, code);
        return false;
    }
    fill_stat(st, a.dwFileAttributes, a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime);
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(w);
        return true;
    }
    if (!follow) {
        /* only FindFirstFileW tells the kind of reparse point */
        WIN32_FIND_DATAW fd = {0};
        HANDLE h = has_wildcards(path) ? INVALID_HANDLE_VALUE : FindFirstFileW(w, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            st->is_link = is_link_tag(fd.dwFileAttributes, fd.dwReserved0);
            FindClose(h);
        }
        free(w);
        return true;
    }
    free(w);
    /* the attributes are the link's: ask the target's */
    HANDLE h = open_handle(path, oe);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    ok = GetFileInformationByHandle(h, &info);
    code = GetLastError();
    CloseHandle(h);
    if (!ok) {
        set_error(oe, code);
        return false;
    }
    fill_stat(st, info.dwFileAttributes, info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime);
    return true;
}

FILE *rag_os_fopen(const char *path, const char *mode, os_error_t *oe) {
    wchar_t wmode[8];
    size_t n = strlen(mode);
    if (n >= sizeof(wmode) / sizeof(wmode[0])) {
        set_message(oe, RAG_OS_FAILED, "invalid mode");
        return NULL;
    }
    for (size_t i = 0; i <= n; i++) wmode[i] = (wchar_t)(unsigned char)mode[i];
    wchar_t *w = rag_os_wide_path(path, oe);
    if (!w) return NULL;
    FILE *f = _wfopen(w, wmode);
    int code = errno;
    free(w);
    if (!f) set_message(oe, code == ENOENT ? RAG_OS_NOT_FOUND : RAG_OS_FAILED, strerror(code));
    return f;
}

char *rag_os_disk_name(const char *path) {
    if (has_wildcards(path)) return NULL;
    wchar_t *w = rag_os_wide_path(path, NULL);
    if (!w) return NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    FindClose(h);
    return rag_os_from_wide(fd.cFileName);
}

char *rag_os_getenv(const char *name) {
    wchar_t *w = rag_os_to_wide(name);
    if (!w) return NULL;
    const wchar_t *value = _wgetenv(w);
    free(w);
    return value ? rag_os_from_wide(value) : NULL;
}

struct os_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    bool pending; /* data holds an entry not returned yet */
    char *name;
    os_dirent_t ent;
};

os_dir_t *rag_os_opendir(const char *path, os_error_t *oe) {
    size_t n = strlen(path);
    char *pattern = concat(path, n > 0 && rag_os_is_sep(path[n - 1]) ? "" : "\\", "*");
    if (!pattern) {
        no_memory(oe);
        return NULL;
    }
    wchar_t *w = rag_os_wide_path(pattern, oe);
    free(pattern);
    os_dir_t *d = w ? calloc(1, sizeof(*d)) : NULL;
    if (w && !d) no_memory(oe);
    if (!d) {
        free(w);
        return NULL;
    }
    d->find = FindFirstFileW(w, &d->data);
    DWORD code = GetLastError();
    free(w);
    if (d->find != INVALID_HANDLE_VALUE) {
        d->pending = true;
    } else if (code != ERROR_FILE_NOT_FOUND) { /* an empty root, like "D:\" */
        set_error(oe, code);
        free(d);
        return NULL;
    }
    return d;
}

bool rag_os_readdir(os_dir_t *d, const os_dirent_t **ent, os_error_t *oe) {
    *ent = NULL;
    for (;;) {
        if (!d->pending) {
            if (d->find == INVALID_HANDLE_VALUE) return true;
            if (!FindNextFileW(d->find, &d->data)) {
                DWORD code = GetLastError();
                if (code == ERROR_NO_MORE_FILES) return true;
                set_error(oe, code); /* cut short: the files not listed must not look deleted */
                return false;
            }
        }
        d->pending = false;
        const wchar_t *name = d->data.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
        free(d->name);
        d->name = rag_os_from_wide(name);
        if (!d->name) {
            no_memory(oe);
            return false;
        }
        DWORD attributes = d->data.dwFileAttributes;
        d->ent.name = d->name;
        d->ent.hidden = d->name[0] == '.' || (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        d->ent.has_stat = true;
        fill_stat(&d->ent.st, attributes, d->data.nFileSizeHigh, d->data.nFileSizeLow, d->data.ftLastWriteTime);
        d->ent.st.is_link = is_link_tag(attributes, d->data.dwReserved0);
        *ent = &d->ent;
        return true;
    }
}

void rag_os_closedir(os_dir_t *d) {
    if (!d) return;
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    free(d->name);
    free(d);
}

#else

/* ======================================================================== */
/* POSIX                                                                     */
/* ======================================================================== */

static void set_errno(os_error_t *oe, int code) {
    if (code == ENOMEM)
        no_memory(oe);
    else
        set_message(oe, code == ENOENT || code == ENOTDIR ? RAG_OS_NOT_FOUND : RAG_OS_FAILED, strerror(code));
}

size_t rag_os_root_len(const char *path) { return path[0] == '/' ? 1 : 0; }

char *rag_os_getcwd(os_error_t *oe) {
    char *cwd = getcwd(NULL, 0);
    if (!cwd) set_errno(oe, errno);
    return cwd;
}

char *rag_os_absolute(const char *path, os_error_t *oe) {
    if (path[0] == '/') {
        char *result = concat(path, "", "");
        if (!result) no_memory(oe);
        return result;
    }
    os_error_t cwd_error;
    char *cwd = rag_os_getcwd(&cwd_error);
    if (!cwd) {
        char *message = cwd_error.kind == RAG_OS_NO_MEMORY
                            ? NULL
                            : concat("cannot get the current directory: ", cwd_error.message, "");
        if (message)
            set_message(oe, RAG_OS_FAILED, message);
        else
            no_memory(oe);
        free(message);
        return NULL;
    }
    size_t n = strlen(cwd);
    char *result = concat(cwd, n > 0 && cwd[n - 1] == '/' ? "" : "/", path);
    free(cwd);
    if (!result) no_memory(oe);
    return result;
}

char *rag_os_realpath(const char *path, os_error_t *oe) {
    char *real = realpath(path, NULL);
    if (!real) set_errno(oe, errno);
    return real;
}

static void fill_stat(os_stat_t *st, const struct stat *s) {
    st->type = S_ISDIR(s->st_mode) ? RAG_OS_DIR : S_ISREG(s->st_mode) ? RAG_OS_FILE : RAG_OS_OTHER;
    st->is_link = S_ISLNK(s->st_mode);
    st->size = (int64_t)s->st_size;
    st->mtime = (int64_t)s->st_mtime;
}

bool rag_os_stat(const char *path, bool follow, os_stat_t *st, os_error_t *oe) {
    struct stat s;
    if ((follow ? stat(path, &s) : lstat(path, &s)) != 0) {
        set_errno(oe, errno);
        return false;
    }
    fill_stat(st, &s);
    return true;
}

FILE *rag_os_fopen(const char *path, const char *mode, os_error_t *oe) {
    FILE *f = fopen(path, mode);
    if (!f) set_errno(oe, errno);
    return f;
}

#ifdef __APPLE__
/* What getattrlist() writes for ATTR_CMN_NAME. */
typedef struct name_reply {
    uint32_t length;
    attrreference_t name; /* where the name is, from here */
    char data[1024];      /* 255 characters of UTF-16, in UTF-8 */
} name_reply_t;
#endif

char *rag_os_disk_name(const char *path) {
#ifdef __APPLE__
    /* the file systems of macOS usually ignore case too: ask for the name as it is stored */
    struct attrlist request = {.bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_NAME};
    name_reply_t reply;
    if (getattrlist(path, &request, &reply, sizeof(reply), FSOPT_NOFOLLOW) != 0 || reply.name.attr_dataoffset < 0)
        return NULL;
    size_t start = offsetof(name_reply_t, name) + (size_t)reply.name.attr_dataoffset;
    size_t n = reply.name.attr_length; /* the NUL included */
    if (n < 2 || start > sizeof(reply) || n > sizeof(reply) - start) return NULL;
    const char *name = (const char *)&reply + start;
    if ((const char *)memchr(name, '\0', n) != name + n - 1) return NULL;
    return concat(name, "", "");
#else
    (void)path;
    return NULL;
#endif
}

char *rag_os_getenv(const char *name) {
    const char *value = getenv(name);
    return value ? concat(value, "", "") : NULL;
}

struct os_dir {
    DIR *dir;
    os_dirent_t ent;
};

os_dir_t *rag_os_opendir(const char *path, os_error_t *oe) {
    os_dir_t *d = calloc(1, sizeof(*d));
    if (!d) {
        no_memory(oe);
        return NULL;
    }
    d->dir = opendir(path);
    if (!d->dir) {
        set_errno(oe, errno);
        free(d);
        return NULL;
    }
    return d;
}

bool rag_os_readdir(os_dir_t *d, const os_dirent_t **ent, os_error_t *oe) {
    *ent = NULL;
    struct dirent *e;
    for (;;) {
        errno = 0;
        e = readdir(d->dir);
        if (!e) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        d->ent.name = e->d_name;
        d->ent.hidden = e->d_name[0] == '.';
        d->ent.has_stat = false;
        *ent = &d->ent;
        return true;
    }
    if (errno) {
        set_errno(oe, errno); /* cut short: the files not listed must not look deleted */
        return false;
    }
    return true;
}

void rag_os_closedir(os_dir_t *d) {
    if (!d) return;
    closedir(d->dir);
    free(d);
}

#endif
