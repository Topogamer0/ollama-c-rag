/*
 * Indexing: files are read, compared with their indexed version, split into
 * chunks; the chunks of consecutive files are embedded in batches and stored
 * as their embeddings arrive.
 *
 * A file is written with a random pending hash when its first chunk is
 * stored, and gets its real hash with the last one. A file interrupted in the
 * middle never looks up to date, so the next run indexes it again; and if
 * another handle starts writing the same file in the meantime, its pending
 * hash replaces ours, and this run leaves the file to it.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A file whose chunks are waiting for their embeddings. */
typedef struct job {
    char *path;
    char hash[65];
    char pending[34]; /* the hash of the file while it is being written */
    int64_t size;
    int64_t mtime;
    chunk_list_t chunks;
    char **texts; /* the text of every chunk, valid UTF-8; released once stored */
    size_t *lens;
    size_t stored;   /* chunks stored (or dropped) so far */
    int64_t file_id; /* 0 until the file is written */
    bool lost;       /* another handle is writing the file: its chunks are dropped */
    struct job *next;
} job_t;

typedef struct batch_item {
    job_t *job;
    size_t chunk;
} batch_item_t;

typedef struct indexer {
    rag_t *rag;
    rag_index_stats_t *stats;
    char *settings; /* the settings of the files indexed by this run */
    int64_t now;
    size_t examined;    /* files of the directory examined so far, for the progress callback */
    size_t total;       /* files of the directory */
    job_t *head, *tail; /* files with chunks to store, in order */
    batch_item_t *batch;
    size_t batch_count;
} indexer_t;

static void job_free(job_t *j) {
    if (j->texts)
        for (size_t i = 0; i < j->chunks.count; i++) free(j->texts[i]);
    free(j->texts);
    free(j->lens);
    rag_chunk_list_free(&j->chunks);
    free(j->path);
    free(j);
}

static rag_status_t oom(rag_t *rag) { return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory"); }

static rag_status_t indexer_init(indexer_t *ix, rag_t *rag, rag_index_stats_t *stats, size_t total) {
    *ix = (indexer_t){.rag = rag, .stats = stats, .now = (int64_t)time(NULL), .total = total};
    ix->settings = rag_format("chunk_size=%zu chunk_overlap=%zu dimensions=%d document_prefix=%s", rag->chunk_size,
                              rag->chunk_overlap, rag->dimensions, rag->document_prefix);
    /* rag_open() keeps batch_size small enough for this not to overflow */
    ix->batch = malloc(rag->batch_size * sizeof(*ix->batch));
    return ix->settings && ix->batch ? RAG_OK : oom(rag);
}

static void indexer_free(indexer_t *ix) {
    while (ix->head) {
        job_t *next = ix->head->next;
        job_free(ix->head);
        ix->head = next;
    }
    free(ix->settings);
    free(ix->batch);
}

/* The settings become the defaults of the database. Called in every write transaction. */
static rag_status_t save_settings(indexer_t *ix) {
    rag_t *rag = ix->rag;
    rag_status_t st = rag_db_set_meta_int(rag, "chunk_size", (int64_t)rag->chunk_size);
    if (st == RAG_OK) st = rag_db_set_meta_int(rag, "chunk_overlap", (int64_t)rag->chunk_overlap);
    if (st == RAG_OK) st = rag_db_set_meta_int(rag, "embed_dimensions", rag->dimensions);
    if (st == RAG_OK) st = rag_db_set_meta(rag, "query_prefix", rag->query_prefix);
    if (st == RAG_OK) st = rag_db_set_meta(rag, "document_prefix", rag->document_prefix);
    return st;
}

/*
 * Checks, in a write transaction, that the index still belongs to the model of
 * this handle and stores embeddings of `dims` floats (0: none are added), as
 * another handle may have changed it since this one was opened. An index
 * without embeddings takes the model and the size of this handle.
 */
static rag_status_t check_index(indexer_t *ix, size_t dims) {
    rag_t *rag = ix->rag;
    char *model = NULL;
    bool vectors = false;
    int64_t stored = 0;
    bool found = false;
    rag_status_t st = rag_db_get_meta(rag, "embed_model", &model);
    if (st == RAG_OK) st = rag_db_has_vectors(rag, &vectors);
    if (st == RAG_OK) st = rag_db_get_meta_int(rag, "dimensions", &stored, &found);
    bool same = model && rag_same_model(model, rag->embed_model);
    if (st == RAG_OK && vectors && !same)
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the index '%s' now holds embeddings of the model '%s', not '%s': use the same model, or "
                         "another database for the other model",
                         rag->path, model ? model : "?", rag->embed_model);
    else if (st == RAG_OK && vectors && dims && found && stored > 0 && (size_t)stored != dims)
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the embedding model '%s' returned embeddings of %zu dimensions, but the index '%s' stores "
                         "embeddings of %lld: was the model changed?",
                         rag->embed_model, dims, rag->path, (long long)stored);
    free(model);
    if (st == RAG_OK && !same) st = rag_db_set_meta(rag, "embed_model", rag->embed_model);
    if (st == RAG_OK && dims && (!found || stored != (int64_t)dims))
        st = rag_db_set_meta_int(rag, "dimensions", (int64_t)dims);
    if (st == RAG_OK && dims) rag->stored_dimensions = dims;
    return st;
}

static rag_status_t write_file(indexer_t *ix, job_t *j, const char *hash) {
    db_file_t f = {0, j->path, (char *)hash, ix->settings, j->size, j->mtime, ix->now};
    rag_status_t st = rag_db_put_file(ix->rag, &f);
    j->file_id = f.id;
    return st;
}

/* Embeds the chunks of the batch, stores them and reports the files that are complete. */
static rag_status_t flush(indexer_t *ix) {
    rag_t *rag = ix->rag;
    size_t n = ix->batch_count;
    if (n == 0) return RAG_OK;

    const char **texts = malloc(n * sizeof(*texts));
    if (!texts) return oom(rag);
    for (size_t i = 0; i < n; i++) texts[i] = ix->batch[i].job->texts[ix->batch[i].chunk];
    float *vectors;
    size_t dims;
    rag_status_t st = rag_embed_texts(rag, texts, n, rag->document_prefix, &vectors, &dims);
    free(texts);
    if (st != RAG_OK) return st;

    size_t old_dims = rag->stored_dimensions;
    st = rag_db_begin(rag);
    if (st == RAG_OK) st = check_index(ix, dims);
    if (st == RAG_OK) st = save_settings(ix);
    for (size_t i = 0; st == RAG_OK && i < n; i++) {
        job_t *j = ix->batch[i].job;
        size_t c = ix->batch[i].chunk;
        if (!j->file_id) {
            st = write_file(ix, j, j->pending); /* the real hash comes with the last chunk */
        } else if (!j->lost && (i == 0 || ix->batch[i - 1].job != j)) {
            /* written by an earlier transaction: is it still ours? */
            bool ours = false;
            st = rag_db_file_hash_is(rag, j->file_id, j->pending, &ours);
            j->lost = !ours;
        }
        if (st == RAG_OK && !j->lost)
            st = rag_db_add_chunk(rag, j->file_id, c, &j->chunks.items[c], j->texts[c], j->lens[c], vectors + i * dims,
                                  dims);
        if (st == RAG_OK && ++j->stored == j->chunks.count && !j->lost)
            st = rag_db_set_file_hash(rag, j->file_id, j->hash);
    }
    free(vectors);
    if (st == RAG_OK)
        st = rag_db_commit(rag);
    else
        rag_db_rollback(rag);
    if (st != RAG_OK) {
        rag->stored_dimensions = old_dims;
        return st;
    }

    for (size_t i = 0; i < n; i++) {
        job_t *j = ix->batch[i].job;
        free(j->texts[ix->batch[i].chunk]);
        j->texts[ix->batch[i].chunk] = NULL;
    }
    ix->batch_count = 0;

    /* every complete file is counted, but none is reported after the callback asked to stop */
    while (ix->head && ix->head->stored == ix->head->chunks.count) {
        job_t *j = ix->head;
        ix->head = j->next;
        if (!ix->head) ix->tail = NULL;
        if (j->lost) {
            /* the version of the other handle is the one kept */
            ix->stats->files_unchanged++;
            if (st == RAG_OK) st = rag_report(rag, RAG_FILE_UNCHANGED, j->path, 0, NULL, ix->examined, ix->total);
        } else {
            ix->stats->files_indexed++;
            ix->stats->chunks_added += j->chunks.count;
            if (st == RAG_OK)
                st = rag_report(rag, RAG_FILE_INDEXED, j->path, j->chunks.count, NULL, ix->examined, ix->total);
        }
        job_free(j);
    }
    return st;
}

/* A file without text: stored right away. */
static rag_status_t store_empty(indexer_t *ix, job_t *j) {
    rag_t *rag = ix->rag;
    rag_status_t st = rag_db_begin(rag);
    if (st == RAG_OK) st = check_index(ix, 0);
    if (st == RAG_OK) st = save_settings(ix);
    if (st == RAG_OK) st = write_file(ix, j, j->hash);
    if (st == RAG_OK)
        st = rag_db_commit(rag);
    else
        rag_db_rollback(rag);
    if (st == RAG_OK) {
        ix->stats->files_indexed++;
        st = rag_report(rag, RAG_FILE_INDEXED, j->path, 0, NULL, ix->examined, ix->total);
    }
    return st;
}

/* Splits the content of a job into chunks with their text. */
static rag_status_t make_chunks(rag_t *rag, job_t *j, const buf_t *content) {
    if (!rag_chunk_split(content->data, content->len, rag->chunk_size, rag->chunk_overlap, &j->chunks)) return oom(rag);
    size_t n = j->chunks.count;
    j->texts = calloc(n ? n : 1, sizeof(*j->texts));
    j->lens = calloc(n ? n : 1, sizeof(*j->lens));
    if (!j->texts || !j->lens) return oom(rag);
    for (size_t i = 0; i < n; i++) {
        const chunk_span_t *c = &j->chunks.items[i];
        j->texts[i] = rag_utf8_sanitize(content->data + c->start, c->end - c->start, &j->lens[i]);
        if (!j->texts[i]) return oom(rag);
    }
    return RAG_OK;
}

/*
 * Indexes one file, or queues its chunks. On failure *file_error tells whether
 * the problem only concerns this file (it cannot be read, ...), in which case
 * the error is set but indexing can go on with the other files.
 */
static rag_status_t index_file(indexer_t *ix, const char *path, bool *file_error) {
    rag_t *rag = ix->rag;
    *file_error = false;
    err_t e;
    rag_err_init(&e);
    fs_info_t info;
    buf_t content = {0};
    rag_status_t st = rag_fs_stat(path, &info, &e);
    if (st == RAG_OK && info.is_dir)
        st = rag_err_set(&e, RAG_ERROR_INVALID, "'%s' is a directory", path);
    else if (st == RAG_OK && !info.is_file)
        st = rag_err_set(&e, RAG_ERROR_IO, "'%s' is not a regular file", path);
    if (st == RAG_OK) st = rag_fs_read_file(path, &content, &e);
    if (st == RAG_OK && memchr(content.data, '\0', content.len))
        st = rag_err_set(&e, RAG_ERROR_IO, "'%s' is a binary file", path);
    if (st != RAG_OK) {
        *file_error = st != RAG_ERROR_MEMORY;
        rag_err_set(&rag->err, st, "%s", e.pub.message);
        rag_err_free(&e);
        rag_buf_free(&content);
        return st;
    }
    rag_err_free(&e);

    job_t *j = calloc(1, sizeof(*j));
    if (!j || !(j->path = rag_strdup(path))) {
        free(j);
        rag_buf_free(&content);
        return oom(rag);
    }
    rag_sha256_hex(content.data, content.len, j->hash);
    rag_db_pending_hash(j->pending);
    j->size = info.size;
    j->mtime = info.mtime;

    db_file_t old;
    bool found = false;
    st = rag_db_find_file(rag, path, &old, &found);
    if (st == RAG_OK && found) {
        bool unchanged = strcmp(old.hash, j->hash) == 0 && strcmp(old.settings, ix->settings) == 0;
        rag_db_file_release(&old);
        if (unchanged) {
            rag_buf_free(&content);
            job_free(j);
            ix->stats->files_unchanged++;
            return rag_report(rag, RAG_FILE_UNCHANGED, path, 0, NULL, ix->examined, ix->total);
        }
    }
    if (st == RAG_OK) st = make_chunks(rag, j, &content);
    rag_buf_free(&content);
    if (st != RAG_OK) {
        job_free(j);
        return st;
    }

    size_t count = j->chunks.count;
    if (count == 0) {
        st = store_empty(ix, j);
        job_free(j);
        return st;
    }
    if (ix->tail)
        ix->tail->next = j;
    else
        ix->head = j;
    ix->tail = j;
    /* j is released by the flush that stores its last chunk */
    for (size_t c = 0; c < count; c++) {
        ix->batch[ix->batch_count++] = (batch_item_t){j, c};
        if (ix->batch_count == rag->batch_size) {
            st = flush(ix);
            if (st != RAG_OK) return st;
        }
    }
    return RAG_OK;
}

/* Removes the indexed files below `dir` that do not exist anymore. `seen` lists the files found there. */
static rag_status_t remove_deleted(indexer_t *ix, const char *dir, const fs_list_t *seen) {
    rag_t *rag = ix->rag;
    db_file_list_t files = {0};
    rag_status_t st = rag_db_files_under(rag, dir, &files);
    size_t k = 0;
    for (size_t i = 0; st == RAG_OK && i < files.count; i++) {
        const db_file_t *f = &files.items[i];
        /* both lists are sorted by path */
        while (k < seen->count && strcmp(seen->items[k].path, f->path) < 0) k++;
        if (k < seen->count && strcmp(seen->items[k].path, f->path) == 0) continue;

        err_t e;
        rag_err_init(&e);
        fs_info_t info;
        rag_status_t exists = rag_fs_stat(f->path, &info, &e);
        rag_err_free(&e);
        if (exists == RAG_OK ? info.is_file : exists != RAG_ERROR_NOT_FOUND) continue;

        /* a transaction per file: what has been reported stays removed if the callback stops */
        size_t chunks = 0;
        st = rag_db_begin(rag);
        if (st == RAG_OK) st = rag_db_delete_file(rag, f->id, &chunks);
        if (st == RAG_OK)
            st = rag_db_commit(rag);
        else
            rag_db_rollback(rag);
        if (st == RAG_OK) {
            ix->stats->files_removed++;
            ix->stats->chunks_removed += chunks;
            st = rag_report(rag, RAG_FILE_REMOVED, f->path, chunks, NULL, ix->examined, ix->total);
        }
    }
    rag_db_file_list_free(&files);
    return st;
}

rag_status_t rag_add_file(rag_t *rag, const char *path, rag_index_stats_t *stats) {
    rag_index_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!path || !*path) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no path");

    char *canonical = rag_fs_canonical(path, &rag->err);
    if (!canonical) return rag->err.pub.code;
    indexer_t ix;
    st = indexer_init(&ix, rag, stats, 0);
    bool file_error = false;
    if (st == RAG_OK) st = index_file(&ix, canonical, &file_error);
    if (st == RAG_OK) st = flush(&ix);
    if (file_error) stats->files_failed = 1;
    indexer_free(&ix);
    free(canonical);
    return st;
}

rag_status_t rag_add_dir(rag_t *rag, const char *path, rag_index_stats_t *stats) {
    rag_index_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!path || !*path) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no path");

    char *dir = rag_fs_canonical(path, &rag->err);
    if (!dir) return rag->err.pub.code;
    fs_list_t list = {0};
    err_t e;
    rag_err_init(&e);
    st = rag_fs_list_dir(dir, &list, &e);
    if (st != RAG_OK) {
        rag_err_set(&rag->err, st, "%s", e.pub.message);
        rag_err_free(&e);
        rag_fs_list_free(&list); /* what was listed before the failure */
        free(dir);
        return st;
    }
    rag_err_free(&e);

    indexer_t ix;
    st = indexer_init(&ix, rag, stats, list.count);
    for (size_t i = 0; st == RAG_OK && i < list.count; i++) {
        const fs_entry_t *entry = &list.items[i];
        bool file_error = false;
        char *reason = entry->error ? rag_strdup(entry->error) : NULL;
        ix.examined = i + 1;
        if (!entry->error) {
            st = index_file(&ix, entry->path, &file_error);
            if (st != RAG_OK && file_error) {
                reason = rag_strdup(rag->err.pub.message);
                rag_err_clear(&rag->err);
                st = RAG_OK;
            }
        }
        if (st == RAG_OK && (entry->error || file_error)) {
            stats->files_failed++;
            st = reason ? rag_report(rag, RAG_FILE_FAILED, entry->path, 0, reason, ix.examined, ix.total) : oom(rag);
        }
        free(reason);
    }
    if (st == RAG_OK) st = flush(&ix);
    if (st == RAG_OK) st = remove_deleted(&ix, dir, &list);
    indexer_free(&ix);
    rag_fs_list_free(&list);
    free(dir);
    return st;
}
