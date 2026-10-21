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
    /* another handle may have indexed, or removed, files since this one was opened */
    rag_status_t st = rag_refresh(rag);
    if (st != RAG_OK || rag->stored_dimensions == 0) {
        /* nothing is indexed: no need to ask Ollama */
        if (st == RAG_OK)
            *out = results;
        else
            free(results);
        return st;
    }

    const char *texts[1] = {query};
    float *q;
    size_t dims;
    st = rag_embed_texts(rag, texts, 1, rag->query_prefix, &q, &dims);
    if (st != RAG_OK) {
        free(results);
        return st;
    }

    scan_t s = {q, dims, options->min_score, {0}};
    rag_topk_init(&s.best, k);
    /* one read transaction, so that the chunks found are still there when they are read */
    st = rag_db_begin_read(rag);
    bool reading = st == RAG_OK;
    /* again, now that the index cannot change: it may have been emptied or rebuilt meanwhile */
    if (st == RAG_OK) st = rag_refresh(rag);
    if (st == RAG_OK && rag->stored_dimensions && rag->stored_dimensions != dims)
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the index '%s' was rebuilt with embeddings of %zu dimensions during the search: try again",
                         rag->path, rag->stored_dimensions);
    if (st == RAG_OK && rag->stored_dimensions) st = rag_db_scan_vectors(rag, dims, visit, &s);
    if (st == RAG_OK && s.best.failed) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
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
