/* SQLite storage of the index. */
#include "internal.h"

#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCHEMA_VERSION 1

/*
 * meta:    settings of the index (schema_version, embed_model, dimensions, ...)
 * files:   indexed files, with the hash of their content when they were indexed
 * chunks:  pieces of the files, with their position
 * vectors: the embedding of every chunk, normalized, as little-endian float32;
 *          a table of its own so that searches scan the vectors only
 */
static const char SCHEMA[] = "CREATE TABLE meta ("
                             "  key TEXT PRIMARY KEY,"
                             "  value TEXT NOT NULL);"
                             "CREATE TABLE files ("
                             "  id INTEGER PRIMARY KEY,"
                             "  path TEXT NOT NULL UNIQUE,"
                             "  hash TEXT NOT NULL,"
                             "  settings TEXT NOT NULL,"
                             "  size INTEGER NOT NULL,"
                             "  mtime INTEGER NOT NULL,"
                             "  indexed_at INTEGER NOT NULL);"
                             "CREATE TABLE chunks ("
                             "  id INTEGER PRIMARY KEY,"
                             "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
                             "  seq INTEGER NOT NULL,"
                             "  start_offset INTEGER NOT NULL,"
                             "  end_offset INTEGER NOT NULL,"
                             "  line_start INTEGER NOT NULL,"
                             "  line_end INTEGER NOT NULL,"
                             "  text TEXT NOT NULL);"
                             "CREATE INDEX chunks_by_file ON chunks(file_id, seq);"
                             "CREATE TABLE vectors ("
                             "  chunk_id INTEGER PRIMARY KEY REFERENCES chunks(id) ON DELETE CASCADE,"
                             "  embedding BLOB NOT NULL);";

enum { STMT_ADD_CHUNK, STMT_ADD_VECTOR };

static rag_status_t db_error(rag_t *rag) {
    int code = sqlite3_errcode(rag->db);
    if (code == SQLITE_NOMEM) return err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    return err_set(&rag->err, RAG_ERROR_DATABASE, "database '%s': %s", rag->path, sqlite3_errmsg(rag->db));
}

static rag_status_t exec(rag_t *rag, const char *sql) {
    return sqlite3_exec(rag->db, sql, NULL, NULL, NULL) == SQLITE_OK ? RAG_OK : db_error(rag);
}

static rag_status_t prepare(rag_t *rag, const char *sql, sqlite3_stmt **stmt) {
    return sqlite3_prepare_v2(rag->db, sql, -1, stmt, NULL) == SQLITE_OK ? RAG_OK : db_error(rag);
}

/* A statement kept for the life of the handle. */
static rag_status_t cached(rag_t *rag, int which, const char *sql, sqlite3_stmt **stmt) {
    if (!rag->stmts[which]) {
        rag_status_t st = prepare(rag, sql, &rag->stmts[which]);
        if (st != RAG_OK) return st;
    }
    *stmt = rag->stmts[which];
    return RAG_OK;
}

/* Steps a statement that returns no row, then resets it. */
static rag_status_t run(rag_t *rag, sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    rag_status_t st = rc == SQLITE_DONE || rc == SQLITE_ROW ? RAG_OK : db_error(rag);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return st;
}

static int bind_text(sqlite3_stmt *stmt, int i, const char *s) {
    return sqlite3_bind_text64(stmt, i, s, strlen(s), SQLITE_STATIC, SQLITE_UTF8);
}

/* Runs a query returning a single integer. */
static rag_status_t query_int(rag_t *rag, const char *sql, int64_t *value) {
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, sql, &stmt);
    if (st != RAG_OK) return st;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *value = sqlite3_column_int64(stmt, 0);
    else
        st = rc == SQLITE_DONE ? err_set(&rag->err, RAG_ERROR_DATABASE, "no result for %s", sql) : db_error(rag);
    sqlite3_finalize(stmt);
    return st;
}

static char *column_text(sqlite3_stmt *stmt, int i) {
    const unsigned char *s = sqlite3_column_text(stmt, i);
    return rag_strndup(s ? (const char *)s : "", (size_t)sqlite3_column_bytes(stmt, i));
}

/* ---- opening ---- */

static rag_status_t create_schema(rag_t *rag) {
    rag_status_t st = exec(rag, "BEGIN IMMEDIATE");
    if (st != RAG_OK) return st;
    int64_t tables = 0;
    st = query_int(rag, "SELECT count(*) FROM sqlite_master", &tables);
    if (st == RAG_OK && tables == 0) {
        /* nobody created it in the meantime */
        st = exec(rag, SCHEMA);
        if (st == RAG_OK) st = db_set_meta_int(rag, "schema_version", SCHEMA_VERSION);
        if (st == RAG_OK) st = db_set_meta_int(rag, "created_at", (int64_t)time(NULL));
    }
    if (st == RAG_OK) return exec(rag, "COMMIT");
    db_rollback(rag);
    return st;
}

static rag_status_t check_schema(rag_t *rag) {
    int64_t tables = 0;
    rag_status_t st = query_int(rag, "SELECT count(*) FROM sqlite_master", &tables);
    if (st != RAG_OK) return st;
    if (tables == 0) return create_schema(rag);

    int64_t has_meta = 0;
    st = query_int(rag, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'meta'", &has_meta);
    if (st != RAG_OK) return st;
    int64_t version = 0;
    bool found = false;
    if (has_meta) st = db_get_meta_int(rag, "schema_version", &version, &found);
    if (st != RAG_OK) return st;
    if (!found) return err_set(&rag->err, RAG_ERROR_DATABASE, "'%s' is not an ollama-rag database", rag->path);
    if (version > SCHEMA_VERSION)
        return err_set(&rag->err, RAG_ERROR_DATABASE,
                       "'%s' was created by a newer version of ollama-rag-c (schema %lld, this version reads %d)",
                       rag->path, (long long)version, SCHEMA_VERSION);
    return RAG_OK;
}

rag_status_t db_open(rag_t *rag, const char *path, bool must_exist) {
    if (must_exist && strcmp(path, ":memory:") != 0) {
        fs_info_t info;
        err_t e;
        err_init(&e);
        rag_status_t st = fs_stat(path, &info, &e);
        err_free(&e);
        if (st == RAG_ERROR_NOT_FOUND)
            return err_set(&rag->err, RAG_ERROR_NOT_FOUND, "the database '%s' does not exist", path);
    }
    int flags = SQLITE_OPEN_READWRITE | (must_exist ? 0 : SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(path, &rag->db, flags, NULL);
    if (rc != SQLITE_OK) {
        rag_status_t st = rag->db ? db_error(rag) : err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
        db_close(rag);
        return st;
    }
    sqlite3_busy_timeout(rag->db, 5000);
    rag_status_t st = exec(rag, "PRAGMA foreign_keys = ON");
    if (st == RAG_OK) st = check_schema(rag);
    if (st != RAG_OK) db_close(rag);
    return st;
}

void db_close(rag_t *rag) {
    for (size_t i = 0; i < sizeof(rag->stmts) / sizeof(rag->stmts[0]); i++) {
        sqlite3_finalize(rag->stmts[i]);
        rag->stmts[i] = NULL;
    }
    sqlite3_close(rag->db);
    rag->db = NULL;
}

rag_status_t db_begin(rag_t *rag) { return exec(rag, "BEGIN IMMEDIATE"); }

rag_status_t db_commit(rag_t *rag) {
    rag_status_t st = exec(rag, "COMMIT");
    if (st != RAG_OK) db_rollback(rag);
    return st;
}

void db_rollback(rag_t *rag) {
    if (!sqlite3_get_autocommit(rag->db)) sqlite3_exec(rag->db, "ROLLBACK", NULL, NULL, NULL);
}

/* ---- meta ---- */

rag_status_t db_get_meta(rag_t *rag, const char *key, char **value) {
    *value = NULL;
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, "SELECT value FROM meta WHERE key = ?1", &stmt);
    if (st != RAG_OK) return st;
    bind_text(stmt, 1, key);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *value = column_text(stmt, 0);
        if (!*value) st = err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    } else if (rc != SQLITE_DONE) {
        st = db_error(rag);
    }
    sqlite3_finalize(stmt);
    return st;
}

rag_status_t db_set_meta(rag_t *rag, const char *key, const char *value) {
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, "INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2)", &stmt);
    if (st != RAG_OK) return st;
    bind_text(stmt, 1, key);
    bind_text(stmt, 2, value);
    st = run(rag, stmt);
    sqlite3_finalize(stmt);
    return st;
}

rag_status_t db_delete_meta(rag_t *rag, const char *key) {
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, "DELETE FROM meta WHERE key = ?1", &stmt);
    if (st != RAG_OK) return st;
    bind_text(stmt, 1, key);
    st = run(rag, stmt);
    sqlite3_finalize(stmt);
    return st;
}

rag_status_t db_get_meta_int(rag_t *rag, const char *key, int64_t *value, bool *found) {
    char *text;
    rag_status_t st = db_get_meta(rag, key, &text);
    *found = false;
    if (st != RAG_OK || !text) return st;
    char *end;
    long long v = strtoll(text, &end, 10);
    if (end != text && *end == '\0') {
        *value = v;
        *found = true;
    }
    free(text);
    return RAG_OK;
}

rag_status_t db_set_meta_int(rag_t *rag, const char *key, int64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%lld", (long long)value);
    return db_set_meta(rag, key, text);
}

/* ---- files ---- */

#define FILE_COLUMNS "id, path, hash, settings, size, mtime, indexed_at"

void db_file_release(db_file_t *f) {
    free(f->path);
    free(f->hash);
    free(f->settings);
    *f = (db_file_t){0};
}

void db_file_list_free(db_file_list_t *list) {
    for (size_t i = 0; i < list->count; i++) db_file_release(&list->items[i]);
    free(list->items);
    *list = (db_file_list_t){0};
}

static bool read_file_row(sqlite3_stmt *stmt, db_file_t *f) {
    f->id = sqlite3_column_int64(stmt, 0);
    f->path = column_text(stmt, 1);
    f->hash = column_text(stmt, 2);
    f->settings = column_text(stmt, 3);
    f->size = sqlite3_column_int64(stmt, 4);
    f->mtime = sqlite3_column_int64(stmt, 5);
    f->indexed_at = sqlite3_column_int64(stmt, 6);
    if (f->path && f->hash && f->settings) return true;
    db_file_release(f);
    return false;
}

rag_status_t db_find_file(rag_t *rag, const char *path, db_file_t *out, bool *found) {
    *found = false;
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, "SELECT " FILE_COLUMNS " FROM files WHERE path = ?1", &stmt);
    if (st != RAG_OK) return st;
    bind_text(stmt, 1, path);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (read_file_row(stmt, out))
            *found = true;
        else
            st = err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    } else if (rc != SQLITE_DONE) {
        st = db_error(rag);
    }
    sqlite3_finalize(stmt);
    return st;
}

rag_status_t db_files_under(rag_t *rag, const char *dir, db_file_list_t *out) {
    /* paths starting with "dir/" are the ones in ["dir/", "dir0"): '0' follows '/' */
    size_t n = strlen(dir);
    char *lower = n > 0 && dir[n - 1] == '/' ? rag_strdup(dir) : rag_format("%s/", dir);
    char *upper = rag_strdup(lower);
    if (!lower || !upper) {
        free(lower);
        free(upper);
        return err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    }
    upper[strlen(upper) - 1] = '0';

    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag,
                              "SELECT " FILE_COLUMNS " FROM files "
                              "WHERE path = ?1 OR (path >= ?2 AND path < ?3) ORDER BY path",
                              &stmt);
    if (st == RAG_OK) {
        bind_text(stmt, 1, dir);
        bind_text(stmt, 2, lower);
        bind_text(stmt, 3, upper);
        int rc = SQLITE_DONE;
        while (st == RAG_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (out->count == out->cap) {
                size_t cap = out->cap ? out->cap * 2 : 16;
                db_file_t *items = realloc(out->items, cap * sizeof(*items));
                if (!items) {
                    st = err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
                    break;
                }
                out->items = items;
                out->cap = cap;
            }
            if (read_file_row(stmt, &out->items[out->count]))
                out->count++;
            else
                st = err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
        }
        if (st == RAG_OK && rc != SQLITE_DONE) st = db_error(rag);
        sqlite3_finalize(stmt);
    }
    free(lower);
    free(upper);
    return st;
}

static rag_status_t delete_chunks(rag_t *rag, int64_t file_id) {
    static const char *const sql[] = {
        "DELETE FROM vectors WHERE chunk_id IN (SELECT id FROM chunks WHERE file_id = ?1)",
        "DELETE FROM chunks WHERE file_id = ?1",
    };
    for (size_t i = 0; i < 2; i++) {
        sqlite3_stmt *stmt;
        rag_status_t st = prepare(rag, sql[i], &stmt);
        if (st != RAG_OK) return st;
        sqlite3_bind_int64(stmt, 1, file_id);
        st = run(rag, stmt);
        sqlite3_finalize(stmt);
        if (st != RAG_OK) return st;
    }
    return RAG_OK;
}

rag_status_t db_put_file(rag_t *rag, db_file_t *file) {
    db_file_t old = {0};
    bool found;
    rag_status_t st = db_find_file(rag, file->path, &old, &found);
    if (st != RAG_OK) return st;
    if (found) {
        file->id = old.id;
        db_file_release(&old);
        st = delete_chunks(rag, file->id);
        if (st != RAG_OK) return st;
    }

    sqlite3_stmt *stmt;
    st = prepare(rag,
                 found ? "UPDATE files SET path = ?2, hash = ?3, settings = ?4, size = ?5, mtime = ?6, "
                         "indexed_at = ?7 WHERE id = ?1"
                       : "INSERT INTO files (path, hash, settings, size, mtime, indexed_at) "
                         "VALUES (?2, ?3, ?4, ?5, ?6, ?7)",
                 &stmt);
    if (st != RAG_OK) return st;
    if (found) sqlite3_bind_int64(stmt, 1, file->id);
    bind_text(stmt, 2, file->path);
    bind_text(stmt, 3, file->hash);
    bind_text(stmt, 4, file->settings);
    sqlite3_bind_int64(stmt, 5, file->size);
    sqlite3_bind_int64(stmt, 6, file->mtime);
    sqlite3_bind_int64(stmt, 7, file->indexed_at);
    st = run(rag, stmt);
    sqlite3_finalize(stmt);
    if (st == RAG_OK && !found) file->id = sqlite3_last_insert_rowid(rag->db);
    return st;
}

rag_status_t db_delete_file(rag_t *rag, int64_t id, size_t *chunks) {
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag, "SELECT count(*) FROM chunks WHERE file_id = ?1", &stmt);
    if (st != RAG_OK) return st;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *chunks = (size_t)sqlite3_column_int64(stmt, 0);
    else
        st = db_error(rag);
    sqlite3_finalize(stmt);
    if (st == RAG_OK) st = delete_chunks(rag, id);
    if (st != RAG_OK) return st;

    st = prepare(rag, "DELETE FROM files WHERE id = ?1", &stmt);
    if (st != RAG_OK) return st;
    sqlite3_bind_int64(stmt, 1, id);
    st = run(rag, stmt);
    sqlite3_finalize(stmt);
    return st;
}

/* ---- chunks ---- */

rag_status_t db_add_chunk(rag_t *rag, int64_t file_id, size_t seq, const chunk_span_t *span, const char *text,
                          size_t text_len, const float *embedding, size_t dims) {
    sqlite3_stmt *stmt;
    rag_status_t st = cached(rag, STMT_ADD_CHUNK,
                             "INSERT INTO chunks (file_id, seq, start_offset, end_offset, line_start, line_end, text) "
                             "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                             &stmt);
    if (st != RAG_OK) return st;
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)seq);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)span->start);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)span->end);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)span->line_start);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)span->line_end);
    sqlite3_bind_text64(stmt, 7, text, text_len, SQLITE_STATIC, SQLITE_UTF8);
    st = run(rag, stmt);
    if (st != RAG_OK) return st;
    sqlite3_int64 chunk_id = sqlite3_last_insert_rowid(rag->db);

    unsigned char *blob = malloc(dims * 4);
    if (!blob) return err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    vec_to_blob(embedding, dims, blob);
    st = cached(rag, STMT_ADD_VECTOR, "INSERT INTO vectors (chunk_id, embedding) VALUES (?1, ?2)", &stmt);
    if (st == RAG_OK) {
        sqlite3_bind_int64(stmt, 1, chunk_id);
        sqlite3_bind_blob64(stmt, 2, blob, dims * 4, SQLITE_STATIC);
        st = run(rag, stmt);
    }
    free(blob);
    return st;
}

/* ---- statistics ---- */

rag_status_t db_counts(rag_t *rag, db_counts_t *c) {
    sqlite3_stmt *stmt;
    rag_status_t st = prepare(rag,
                              "SELECT (SELECT count(*) FROM files), (SELECT count(*) FROM chunks), "
                              "(SELECT count(*) FROM vectors), "
                              "(SELECT coalesce(sum(length(CAST(text AS BLOB))), 0) FROM chunks), "
                              "(SELECT coalesce(max(indexed_at), 0) FROM files), "
                              "(SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size())",
                              &stmt);
    if (st != RAG_OK) return st;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        c->files = (size_t)sqlite3_column_int64(stmt, 0);
        c->chunks = (size_t)sqlite3_column_int64(stmt, 1);
        c->vectors = (size_t)sqlite3_column_int64(stmt, 2);
        c->text_bytes = (uint64_t)sqlite3_column_int64(stmt, 3);
        c->last_indexed = sqlite3_column_int64(stmt, 4);
        c->database_bytes = (uint64_t)sqlite3_column_int64(stmt, 5);
    } else {
        st = db_error(rag);
    }
    sqlite3_finalize(stmt);
    return st;
}
