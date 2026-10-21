/* Search: exact cosine similarity between the question and every chunk. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct scan {
    const float *query;
    size_t dims;
    double min_score;
    topk_t best;
} scan_t;

static void visit(int64_t chunk_id, const float *embedding, void *data) {
    scan_t *s = data;
    double score = rag_vec_dot(s->query, embedding, s->dims);
    if (s->min_score != 0 && score < s->min_score) return;
    rag_topk_push(&s->best, score, chunk_id);
}

void rag_results_free(rag_results_t *results) {
    if (!results) return;
    for (size_t i = 0; i < results->count; i++) {
        free((char *)results->items[i].path);
        free((char *)results->items[i].text);
    }
    free((rag_result_t *)results->items);
    free(results);
}

rag_status_t rag_find(rag_t *rag, const char *query, const rag_search_options_t *options, rag_results_t **out) {
    static const rag_search_options_t defaults = {0};
    if (!options) options = &defaults;
    size_t k = options->top_k ? options->top_k : RAG_DEFAULT_TOP_K;

    rag_results_t *results = calloc(1, sizeof(*results));
    if (!results) return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    if (rag->stored_dimensions == 0) {
        /* maybe another process indexed something in the meantime */
        int64_t dims = 0;
        bool found = false;
        rag_status_t st = rag_db_get_meta_int(rag, "dimensions", &dims, &found);
        if (st != RAG_OK) {
            free(results);
            return st;
        }
        if (found && dims > 0) rag->stored_dimensions = (size_t)dims;
    }
    if (rag->stored_dimensions == 0) {
        /* nothing has been indexed */
        *out = results;
        return RAG_OK;
    }

    const char *texts[1] = {query};
    float *q;
    size_t dims;
    rag_status_t st = rag_embed_texts(rag, texts, 1, rag->query_prefix, &q, &dims);
    if (st != RAG_OK) {
        free(results);
        return st;
    }

    scan_t s = {q, dims, options->min_score, {0}};
    if (!rag_topk_init(&s.best, k)) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    /* one read transaction, so that the chunks found are still there when they are read */
    bool reading = false;
    if (st == RAG_OK) st = rag_db_begin_read(rag);
    reading = st == RAG_OK;
    if (st == RAG_OK) st = rag_db_scan_vectors(rag, dims, visit, &s);
    free(q);
    rag_topk_sort(&s.best);

    rag_result_t *items = st == RAG_OK && s.best.count ? calloc(s.best.count, sizeof(*items)) : NULL;
    if (st == RAG_OK && s.best.count && !items) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    results->items = items;
    for (size_t i = 0; st == RAG_OK && i < s.best.count; i++) {
        bool found = false;
        st = rag_db_get_chunk(rag, s.best.items[i].id, &items[results->count], &found);
        if (st == RAG_OK && found) items[results->count++].score = s.best.items[i].score;
    }
    if (reading) {
        if (st == RAG_OK)
            st = rag_db_commit(rag);
        else
            rag_db_rollback(rag);
    }
    rag_topk_free(&s.best);
    if (st != RAG_OK) {
        rag_results_free(results);
        return st;
    }
    *out = results;
    return RAG_OK;
}

rag_status_t rag_search(rag_t *rag, const char *query, const rag_search_options_t *options, rag_results_t **results) {
    if (results) *results = NULL;
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!results) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no results");
    if (!query || !*query) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "empty query");
    return rag_find(rag, query, options, results);
}
