/* The handle: opening, settings, statistics, removal of files. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Prefixes recommended by the authors of some embedding models, matched on
 * the start of the model name (without namespace and tag).
 */
static const struct {
    const char *model;
    const char *query;
    const char *document;
} KNOWN_PREFIXES[] = {
    {"embeddinggemma", "task: search result | query: ", "title: none | text: "},
    {"nomic-embed-text", "search_query: ", "search_document: "},
    {"mxbai-embed-large", "Represent this sentence for searching relevant passages: ", ""},
    {"snowflake-arctic-embed2", "query: ", ""},
    {"snowflake-arctic-embed", "Represent this sentence for searching relevant passages: ", ""},
    {"qwen3-embedding",
     "Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery: ", ""},
};

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* Length of a model name without its tag (":latest" ...). */
static size_t model_name_len(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *colon = strrchr(slash ? slash : name, ':');
    return colon ? (size_t)(colon - name) : strlen(name);
}

/* Tag of a model name, "latest" if it has none. */
static const char *model_tag(const char *name) {
    size_t n = model_name_len(name);
    return name[n] == ':' ? name + n + 1 : "latest";
}

static bool ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool same_model(const char *a, const char *b) {
    size_t na = model_name_len(a), nb = model_name_len(b);
    const char *ta = model_tag(a), *tb = model_tag(b);
    return na == nb && ascii_ieq_n(a, b, na) && strlen(ta) == strlen(tb) && ascii_ieq_n(ta, tb, strlen(ta));
}

static void default_prefixes(const char *model, const char **query, const char **document) {
    const char *slash = strrchr(model, '/');
    const char *base = slash ? slash + 1 : model;
    *query = *document = "";
    for (size_t i = 0; i < sizeof(KNOWN_PREFIXES) / sizeof(KNOWN_PREFIXES[0]); i++) {
        size_t n = strlen(KNOWN_PREFIXES[i].model);
        if (strlen(base) >= n && ascii_ieq_n(base, KNOWN_PREFIXES[i].model, n)) {
            *query = KNOWN_PREFIXES[i].query;
            *document = KNOWN_PREFIXES[i].document;
            return;
        }
    }
}

rag_status_t rag_report(rag_t *rag, rag_event_t event, const char *path, size_t chunks, const char *message,
                        size_t done, size_t total) {
    if (!rag->progress) return RAG_OK;
    rag_progress_t p = {event, path, chunks, message, done, total};
    if (rag->progress(&p, rag->progress_data) == 0) return RAG_OK;
    return err_set(&rag->err, RAG_ERROR_ABORTED, "stopped by the progress callback");
}

/* ---- opening ---- */

static rag_status_t oom(rag_t *rag) { return err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory"); }

/* Stored string setting, or NULL. */
static rag_status_t stored(rag_t *rag, const char *key, char **value) { return db_get_meta(rag, key, value); }

static rag_status_t has_vectors(rag_t *rag, bool *result) {
    db_counts_t c;
    rag_status_t st = db_counts(rag, &c);
    *result = st == RAG_OK && c.vectors > 0;
    return st;
}

static rag_status_t load_model(rag_t *rag, const rag_options_t *o) {
    if (o->dimensions < 0)
        return err_set(&rag->err, RAG_ERROR_INVALID, "invalid number of dimensions: %d", o->dimensions);
    char *model = NULL;
    rag_status_t st = stored(rag, "embed_model", &model);
    if (st != RAG_OK) return st;
    int64_t dims = 0, requested = 0;
    bool dims_found = false, requested_found = false;
    st = db_get_meta_int(rag, "dimensions", &dims, &dims_found);
    if (st == RAG_OK) st = db_get_meta_int(rag, "embed_dimensions", &requested, &requested_found);
    bool vectors = false;
    if (st == RAG_OK) st = has_vectors(rag, &vectors);
    if (st != RAG_OK) {
        free(model);
        return st;
    }

    bool changed = false;
    if (o->embed_model && *o->embed_model) {
        if (model && !same_model(model, o->embed_model)) {
            if (vectors) {
                st = err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                             "the index '%s' was built with the embedding model '%s', not '%s': use the same "
                             "model, or another database for the other model",
                             rag->path, model, o->embed_model);
                free(model);
                return st;
            }
            changed = true; /* nothing has been embedded yet: the model can still change */
        }
        if (!model) changed = true;
        free(model);
        model = rag_strdup(o->embed_model);
        if (!model) return oom(rag);
    } else if (!model) {
        model = rag_strdup(RAG_DEFAULT_EMBED_MODEL);
        if (!model) return oom(rag);
        changed = true;
    }
    rag->embed_model = model;

    if (changed) {
        /* settings tied to the previous model */
        dims_found = requested_found = false;
        st = db_set_meta(rag, "embed_model", model);
        if (st == RAG_OK) st = db_set_meta(rag, "dimensions", "0");
        if (st == RAG_OK) st = db_set_meta(rag, "embed_dimensions", "0");
        if (st == RAG_OK) st = db_delete_meta(rag, "query_prefix");
        if (st == RAG_OK) st = db_delete_meta(rag, "document_prefix");
        if (st != RAG_OK) return st;
    }
    rag->stored_dimensions = dims_found && dims > 0 ? (size_t)dims : 0;
    rag->dimensions = o->dimensions ? o->dimensions : requested_found && requested > 0 ? (int)requested : 0;
    if (o->dimensions > 0 && vectors && rag->stored_dimensions && (size_t)o->dimensions != rag->stored_dimensions)
        return err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                       "the index '%s' stores embeddings of %zu dimensions, not %d: use the same size, or another "
                       "database",
                       rag->path, rag->stored_dimensions, o->dimensions);
    return RAG_OK;
}

/* String setting: the option, else the stored value, else the default. */
static rag_status_t load_string(rag_t *rag, const char *option, const char *key, const char *fallback, char **out) {
    if (option) {
        *out = rag_strdup(option);
        return *out ? RAG_OK : oom(rag);
    }
    rag_status_t st = stored(rag, key, out);
    if (st == RAG_OK && !*out) {
        *out = rag_strdup(fallback);
        if (!*out) st = oom(rag);
    }
    return st;
}

static rag_status_t load_settings(rag_t *rag, const rag_options_t *o) {
    rag_status_t st = load_model(rag, o);
    if (st != RAG_OK) return st;

    const char *query, *document;
    default_prefixes(rag->embed_model, &query, &document);
    st = load_string(rag, o->query_prefix, "query_prefix", query, &rag->query_prefix);
    if (st == RAG_OK) st = load_string(rag, o->document_prefix, "document_prefix", document, &rag->document_prefix);
    if (st != RAG_OK) return st;

    int64_t size = 0, overlap = 0;
    bool size_found = false, overlap_found = false;
    st = db_get_meta_int(rag, "chunk_size", &size, &size_found);
    if (st == RAG_OK) st = db_get_meta_int(rag, "chunk_overlap", &overlap, &overlap_found);
    if (st != RAG_OK) return st;
    rag->chunk_size = o->chunk_size ? o->chunk_size : size_found && size > 0 ? (size_t)size : RAG_DEFAULT_CHUNK_SIZE;
    if (o->chunk_overlap == RAG_NO_OVERLAP) {
        rag->chunk_overlap = 0;
    } else if (o->chunk_overlap) {
        if (o->chunk_overlap >= rag->chunk_size)
            return err_set(&rag->err, RAG_ERROR_INVALID,
                           "the chunk overlap (%zu) must be smaller than the chunk size (%zu)", o->chunk_overlap,
                           rag->chunk_size);
        rag->chunk_overlap = o->chunk_overlap;
    } else if (overlap_found && overlap >= 0 && (size_t)overlap < rag->chunk_size) {
        rag->chunk_overlap = (size_t)overlap;
    } else {
        /* the default overlap, at most a quarter of the chunks */
        rag->chunk_overlap =
            RAG_DEFAULT_CHUNK_OVERLAP < rag->chunk_size / 4 ? RAG_DEFAULT_CHUNK_OVERLAP : rag->chunk_size / 4;
    }

    rag->batch_size = o->batch_size ? o->batch_size : RAG_DEFAULT_BATCH_SIZE;
    rag->chat_model = rag_strdup(o->chat_model && *o->chat_model ? o->chat_model : RAG_DEFAULT_CHAT_MODEL);
    rag->host = o->host && *o->host ? rag_strdup(o->host) : NULL;
    if (!rag->chat_model || (o->host && *o->host && !rag->host)) return oom(rag);
    rag->timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 0;
    rag->progress = o->progress;
    rag->progress_data = o->progress_data;
    return RAG_OK;
}

rag_status_t rag_open(const char *path, const rag_options_t *options, rag_t **out) {
    static const rag_options_t defaults = {0};
    if (!out) return RAG_ERROR_INVALID;
    rag_t *rag = *out = calloc(1, sizeof(*rag));
    if (!rag) return RAG_ERROR_MEMORY;
    err_init(&rag->err);
    if (!path || !*path) return err_set(&rag->err, RAG_ERROR_INVALID, "no database path");
    rag->path = rag_strdup(path);
    if (!rag->path) return oom(rag);

    rag_status_t st = db_open(rag, path, (options ? options : &defaults)->must_exist);
    if (st == RAG_OK) st = load_settings(rag, options ? options : &defaults);
    if (st != RAG_OK) db_close(rag);
    return st;
}

void rag_close(rag_t *rag) {
    if (!rag) return;
    if (rag->db) db_close(rag);
    free(rag->path);
    free(rag->host);
    free(rag->embed_model);
    free(rag->query_prefix);
    free(rag->document_prefix);
    free(rag->chat_model);
    err_free(&rag->err);
    free(rag);
}

const rag_error_t *rag_last_error(const rag_t *rag) {
    static const rag_error_t no_handle = {RAG_ERROR_MEMORY, -1, "out of memory"};
    return rag ? &rag->err.pub : &no_handle;
}

/* Starts a public call: checks the handle and clears the last error. */
rag_status_t rag_begin_call(rag_t *rag) {
    if (!rag) return RAG_ERROR_INVALID;
    err_clear(&rag->err);
    if (!rag->db) return err_set(&rag->err, RAG_ERROR_INVALID, "the database is not open");
    return RAG_OK;
}

/* ---- information ---- */

rag_status_t rag_stats(rag_t *rag, rag_stats_t *stats) {
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!stats) return err_set(&rag->err, RAG_ERROR_INVALID, "no stats");
    db_counts_t c;
    st = db_counts(rag, &c);
    if (st != RAG_OK) return st;
    *stats = (rag_stats_t){
        .path = rag->path,
        .embed_model = rag->embed_model,
        .dimensions = rag->stored_dimensions,
        .chunk_size = rag->chunk_size,
        .chunk_overlap = rag->chunk_overlap,
        .files = c.files,
        .chunks = c.chunks,
        .text_bytes = c.text_bytes,
        .database_bytes = c.database_bytes,
        .last_indexed = c.last_indexed,
    };
    return RAG_OK;
}

/* ---- removal ---- */

rag_status_t rag_remove_file(rag_t *rag, const char *path, rag_index_stats_t *stats) {
    rag_index_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!path || !*path) return err_set(&rag->err, RAG_ERROR_INVALID, "no path");

    char *canonical = fs_canonical(path);
    if (!canonical) return oom(rag);
    db_file_list_t files = {0};
    st = db_files_under(rag, canonical, &files);
    free(canonical);
    if (st == RAG_OK && files.count == 0)
        st = err_set(&rag->err, RAG_ERROR_NOT_FOUND, "'%s' is not in the index", path);
    if (st == RAG_OK) st = db_begin(rag);
    if (st != RAG_OK) {
        db_file_list_free(&files);
        return st;
    }
    for (size_t i = 0; st == RAG_OK && i < files.count; i++) {
        size_t chunks = 0;
        st = db_delete_file(rag, files.items[i].id, &chunks);
        if (st == RAG_OK) {
            stats->files_removed++;
            stats->chunks_removed += chunks;
            st = rag_report(rag, RAG_FILE_REMOVED, files.items[i].path, chunks, NULL, 0, 0);
        }
    }
    if (st == RAG_OK)
        st = db_commit(rag);
    else
        db_rollback(rag);
    if (st != RAG_OK) memset(stats, 0, sizeof(*stats));
    db_file_list_free(&files);
    return st;
}
