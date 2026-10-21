/*
 * Temporary files for the tests (POSIX). Define _XOPEN_SOURCE 700
 * before including any header.
 */
#ifndef RAG_TEST_FILES_H
#define RAG_TEST_FILES_H

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Creates a new directory in the current one and returns its absolute, resolved path. */
static inline char *make_temp_dir(void) {
    char name[] = "ragtest-XXXXXX";
    if (!mkdtemp(name)) {
        perror("mkdtemp");
        exit(2);
    }
    char *path = realpath(name, NULL);
    if (!path) {
        perror("realpath");
        exit(2);
    }
    return path;
}

/* Joins path components: path_of(dir, "a.md"). Returns a static buffer (the last 8 results stay valid). */
static inline const char *path_of(const char *dir, const char *name) {
    static char buffers[8][4096];
    static int next;
    char *out = buffers[next++ % 8];
    snprintf(out, sizeof(buffers[0]), "%s/%s", dir, name);
    return out;
}

/* Writes a file, creating the directories above it. */
static inline void write_file(const char *path, const char *content) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static inline bool file_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static inline void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        chmod(path, 0755);
        DIR *d = opendir(path);
        struct dirent *ent;
        while (d && (ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_tree(child);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

#endif
