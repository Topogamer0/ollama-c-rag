/* The handle: opening, settings, statistics, removal of files. */
#include "internal.h"

#include <ollama.h>

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

bool rag_same_model(const char *a, const char *b) {
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
    return rag_err_set(&rag->err, RAG_ERROR_ABORTED, "stopped by the progress callback");
}

/* ---- Ollama ---- */

ollama_client_t *rag_client(rag_t *rag) {
    if (!rag->client) {
        rag->client = ollama_client_new(rag->host);
        if (!rag->client) {
            rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
            return NULL;
        }
        if (rag->timeout_ms > 0) ollama_client_set_timeout(rag->client, rag->timeout_ms);
    }
    return rag->client;
}

rag_status_t rag_ollama_error(rag_t *rag, int status, const char *what, const char *model) {
    const ollama_error_t *e = ollama_last_error(rag->client);
    rag_status_t st;
    switch ((ollama_status_t)status) {
    case OLLAMA_ERROR_MEMORY: return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    case OLLAMA_ERROR_CONNECTION:
        st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "%s (%s)", e->message, ollama_client_host(rag->client));
        break;
    case OLLAMA_ERROR_RESPONSE:
        st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "%s '%s': %s%s%s", what, model, e->message,
                         e->status_code == 404 ? "; to download it: ollama pull " : "",
                         e->status_code == 404 ? model : "");
        break;
    default: st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "%s '%s': %s", what, model, e->message); break;
    }
    rag->err.pub.status_code = e->status_code;
    return st;
}

/* ---- opening ---- */

static rag_status_t oom(rag_t *rag) { return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory"); }

/* Stored string setting, or NULL. */
static rag_status_t stored(rag_t *rag, const char *key, char **value) { return rag_db_get_meta(rag, key, value); }

rag_status_t rag_load_dimensions(rag_t *rag) {
    bool vectors = false;
    int64_t dims = 0;
    bool found = false;
    rag_status_t st = rag_db_has_vectors(rag, &vectors);
    if (st == RAG_OK && vectors) st = rag_db_get_meta_int(rag, "dimensions", &dims, &found);
    /* the stored size only matters while embeddings are stored */
    rag->stored_dimensions = st == RAG_OK && vectors && found && dims > 0 ? (size_t)dims : 0;
    return st;
}

/*
 * Sets rag->embed_model: the option, else the database's, else the default.
 * When the database must be updated (a new database, or a new model for an
 * index without embeddings), sets *must_write instead, unless `write` is set.
 */
static rag_status_t resolve_model(rag_t *rag, const rag_options_t *o, bool write, bool *must_write) {
    const char *wanted = o->embed_model && *o->embed_model ? o->embed_model : NULL;
    char *model = NULL;
    bool vectors = false;
    rag_status_t st = stored(rag, "embed_model", &model);
    if (st == RAG_OK) st = rag_db_has_vectors(rag, &vectors);
    if (st == RAG_OK && wanted && model && !rag_same_model(model, wanted) && vectors)
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the index '%s' was built with the embedding model '%s', not '%s': use the same model, or "
                         "another database for the other model",
                         rag->path, model, wanted);
    if (st != RAG_OK) {
        free(model);
        return st;
    }

    /* nothing has been embedded yet: the model can still change */
    bool change = wanted ? !model || !rag_same_model(model, wanted) : !model;
    if (change && !write) {
        free(model);
        *must_write = true;
        return RAG_OK;
    }
    if (change) {
        const char *name = wanted ? wanted : RAG_DEFAULT_EMBED_MODEL;
        /* settings tied to the previous model */
        st = rag_db_set_meta(rag, "embed_model", name);
        if (st == RAG_OK) st = rag_db_set_meta(rag, "dimensions", "0");
        if (st == RAG_OK) st = rag_db_set_meta(rag, "embed_dimensions", "0");
        if (st == RAG_OK) st = rag_db_delete_meta(rag, "query_prefix");
        if (st == RAG_OK) st = rag_db_delete_meta(rag, "document_prefix");
        free(model);
        model = NULL;
        if (st != RAG_OK) return st;
    }
    free(rag->embed_model);
    rag->embed_model = wanted || !model ? rag_strdup(wanted ? wanted : RAG_DEFAULT_EMBED_MODEL) : model;
    if (rag->embed_model != model) free(model);
    return rag->embed_model ? RAG_OK : oom(rag);
}

/* The requested size of the embeddings: the option, else the database's. */
static rag_status_t load_requested_dimensions(rag_t *rag) {
    if (rag->dimensions_option) {
        rag->dimensions = rag->dimensions_option;
        return RAG_OK;
    }
    int64_t requested = 0;
    bool found = false;
    rag_status_t st = rag_db_get_meta_int(rag, "embed_dimensions", &requested, &found);
    rag->dimensions = st == RAG_OK && found && requested > 0 ? (int)requested : 0;
    return st;
}

rag_status_t rag_refresh(rag_t *rag) {
    rag_status_t st = rag_load_dimensions(rag);
    if (st == RAG_OK) st = load_requested_dimensions(rag);
    if (st != RAG_OK || !rag->stored_dimensions) return st;
    char *model = NULL;
    st = stored(rag, "embed_model", &model);
    if (st == RAG_OK && (!model || !rag_same_model(model, rag->embed_model)))
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the index '%s' now holds embeddings of the model '%s', not '%s': open it again, or use "
                         "another database for the other model",
                         rag->path, model ? model : "?", rag->embed_model);
    free(model);
    return st;
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

/* Chunking settings: the options, else the database's, else the defaults. */
static rag_status_t load_chunking(rag_t *rag, const rag_options_t *o) {
    int64_t size = 0, overlap = 0;
    bool size_found = false, overlap_found = false;
    rag_status_t st = rag_db_get_meta_int(rag, "chunk_size", &size, &size_found);
    if (st == RAG_OK) st = rag_db_get_meta_int(rag, "chunk_overlap", &overlap, &overlap_found);
    if (st != RAG_OK) return st;
    rag->chunk_size = o->chunk_size ? o->chunk_size : size_found && size > 0 ? (size_t)size : RAG_DEFAULT_CHUNK_SIZE;
    if (o->chunk_overlap == RAG_NO_OVERLAP) {
        rag->chunk_overlap = 0;
    } else if (o->chunk_overlap) {
        if (o->chunk_overlap >= rag->chunk_size)
            return rag_err_set(&rag->err, RAG_ERROR_INVALID,
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
    return RAG_OK;
}

/* Everything is checked before the database is changed, so that a failure leaves it as it was. */
static rag_status_t load_settings(rag_t *rag, const rag_options_t *o) {
    bool must_write = false;
    rag_status_t st = resolve_model(rag, o, false, &must_write);
    if (st == RAG_OK) st = load_chunking(rag, o);
    if (st != RAG_OK) return st;

    rag->batch_size = o->batch_size ? o->batch_size : RAG_DEFAULT_BATCH_SIZE;
    rag->chat_model = rag_strdup(o->chat_model && *o->chat_model ? o->chat_model : RAG_DEFAULT_CHAT_MODEL);
    rag->host = o->host && *o->host ? rag_strdup(o->host) : NULL;
    if (!rag->chat_model || (o->host && *o->host && !rag->host)) return oom(rag);
    rag->timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 0;
    rag->progress = o->progress;
    rag->progress_data = o->progress_data;
    rag->dimensions_option = o->dimensions;

    if (must_write) {
        /* again under the write lock, as another process may have stored embeddings meanwhile */
        st = rag_db_begin(rag);
        if (st == RAG_OK) st = resolve_model(rag, o, true, &must_write);
        if (st == RAG_OK)
            st = rag_db_commit(rag);
        else
            rag_db_rollback(rag);
        if (st != RAG_OK) return st;
    }

    const char *query, *document;
    default_prefixes(rag->embed_model, &query, &document);
    st = load_string(rag, o->query_prefix, "query_prefix", query, &rag->query_prefix);
    if (st == RAG_OK) st = load_string(rag, o->document_prefix, "document_prefix", document, &rag->document_prefix);
    if (st == RAG_OK) st = rag_load_dimensions(rag);
    if (st == RAG_OK) st = load_requested_dimensions(rag);
    if (st != RAG_OK) return st;
    if (o->dimensions > 0 && rag->stored_dimensions && (size_t)o->dimensions != rag->stored_dimensions)
        return rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                           "the index '%s' stores embeddings of %zu dimensions, not %d: use the same size, or another "
                           "database",
                           rag->path, rag->stored_dimensions, o->dimensions);
    return RAG_OK;
}

rag_status_t rag_open(const char *path, const rag_options_t *options, rag_t **out) {
    static const rag_options_t defaults = {0};
    if (!out) return RAG_ERROR_INVALID;
    rag_t *rag = *out = calloc(1, sizeof(*rag));
    if (!rag) return RAG_ERROR_MEMORY;
    rag_err_init(&rag->err);
    if (!path || !*path) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no database path");
    rag->path = rag_strdup(path);
    if (!rag->path) return oom(rag);

    /* checked before the database is opened, or created */
    const rag_options_t *o = options ? options : &defaults;
    if (o->dimensions < 0)
        return rag_err_set(&rag->err, RAG_ERROR_INVALID, "invalid number of dimensions: %d", o->dimensions);
    if (o->batch_size > RAG_MAX_BATCH_SIZE)
        return rag_err_set(&rag->err, RAG_ERROR_INVALID, "the batch size (%zu) must be at most %d", o->batch_size,
                           RAG_MAX_BATCH_SIZE);
    if (o->chunk_size && o->chunk_overlap != RAG_NO_OVERLAP && o->chunk_overlap >= o->chunk_size)
        return rag_err_set(&rag->err, RAG_ERROR_INVALID,
                           "the chunk overlap (%zu) must be smaller than the chunk size (%zu)", o->chunk_overlap,
                           o->chunk_size);

    rag_status_t st = rag_db_open(rag, path, o->must_exist);
    if (st == RAG_OK) st = load_settings(rag, o);
    if (st != RAG_OK) rag_db_close(rag);
    return st;
}

void rag_close(rag_t *rag) {
    if (!rag) return;
    if (rag->db) rag_db_close(rag);
    ollama_client_free(rag->client);
    free(rag->path);
    free(rag->host);
    free(rag->embed_model);
    free(rag->query_prefix);
    free(rag->document_prefix);
    free(rag->chat_model);
    free(rag->stats_model);
    rag_err_free(&rag->err);
    free(rag);
}

const rag_error_t *rag_last_error(const rag_t *rag) {
    static const rag_error_t no_handle = {RAG_ERROR_MEMORY, -1, "out of memory"};
    return rag ? &rag->err.pub : &no_handle;
}

/* Starts a public call: checks the handle and clears the last error. */
rag_status_t rag_begin_call(rag_t *rag) {
    if (!rag) return RAG_ERROR_INVALID;
    rag_err_clear(&rag->err);
    if (!rag->db) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "the database is not open");
    return RAG_OK;
}

/* ---- information ---- */

rag_status_t rag_stats(rag_t *rag, rag_stats_t *stats) {
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!stats) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no stats");
    /* one snapshot, and what other handles may have changed */
    db_counts_t c;
    char *model = NULL;
    st = rag_db_begin_read(rag);
    if (st == RAG_OK) st = rag_load_dimensions(rag);
    if (st == RAG_OK) st = rag_db_counts(rag, &c);
    if (st == RAG_OK) st = stored(rag, "embed_model", &model);
    if (st == RAG_OK)
        st = rag_db_commit(rag);
    else
        rag_db_rollback(rag);
    if (st != RAG_OK) {
        free(model);
        return st;
    }
    free(rag->stats_model);
    rag->stats_model = model;
    *stats = (rag_stats_t){
        .path = rag->path,
        .embed_model = model ? model : rag->embed_model,
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

#define REMOVE_GROUP 256

rag_status_t rag_remove_listed(rag_t *rag, const db_file_t *const *files, size_t count, rag_index_stats_t *stats,
                               size_t done, size_t total) {
    rag_status_t st = RAG_OK, stop = RAG_OK;
    for (size_t first = 0; st == RAG_OK && first < count; first += REMOVE_GROUP) {
        size_t n = count - first < REMOVE_GROUP ? count - first : REMOVE_GROUP;
        size_t chunks[REMOVE_GROUP];
        bool deleted[REMOVE_GROUP];
        st = rag_db_begin(rag);
        for (size_t i = 0; st == RAG_OK && i < n; i++)
            st = rag_db_delete_file(rag, files[first + i]->id, files[first + i]->path, &chunks[i], &deleted[i]);
        if (st == RAG_OK)
            st = rag_db_commit(rag);
        else
            rag_db_rollback(rag);
        /* every file of a committed group is counted, but none is reported after the callback asked to stop */
        for (size_t i = 0; st == RAG_OK && i < n; i++) {
            if (!deleted[i]) continue;
            stats->files_removed++;
            stats->chunks_removed += chunks[i];
            if (stop == RAG_OK)
                stop = rag_report(rag, RAG_FILE_REMOVED, files[first + i]->path, chunks[i], NULL, done, total);
        }
        if (st == RAG_OK) st = stop;
    }
    return st;
}

rag_status_t rag_remove_file(rag_t *rag, const char *path, rag_index_stats_t *stats) {
    rag_index_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!path || !*path) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no path");

    char *canonical = rag_fs_canonical(path, &rag->err);
    if (!canonical) return rag->err.pub.code;
    db_file_list_t files = {0};
    st = rag_db_files_under(rag, canonical, &files);
    free(canonical);
    if (st == RAG_OK && files.count == 0)
        st = rag_err_set(&rag->err, RAG_ERROR_NOT_FOUND, "'%s' is not in the index", path);
    const db_file_t **list = st == RAG_OK ? malloc(files.count * sizeof(*list)) : NULL;
    if (st == RAG_OK && !list) st = oom(rag);
    if (st == RAG_OK) {
        for (size_t i = 0; i < files.count; i++) list[i] = &files.items[i];
        st = rag_remove_listed(rag, list, files.count, stats, 0, 0);
    }
    free(list);
    rag_db_file_list_free(&files);
    return st;
}
