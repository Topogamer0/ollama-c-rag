/* Embeddings, computed by Ollama. */
#include "internal.h"

#include <ollama.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

rag_status_t rag_embed_texts(rag_t *rag, const char *const *texts, size_t n, const char *prefix, float **out,
                             size_t *dims) {
    *out = NULL;
    *dims = 0;
    if (n == 0) return RAG_OK;
    ollama_client_t *client = rag_client(rag);
    if (!client) return rag->err.pub.code;

    /* Ollama expects valid UTF-8 */
    char **inputs = calloc(n, sizeof(*inputs));
    if (!inputs) return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    rag_status_t st = RAG_OK;
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < n && st == RAG_OK; i++) {
        buf_t b = {0};
        rag_buf_append(&b, prefix, prefix_len);
        rag_buf_puts(&b, texts[i]);
        inputs[i] = b.oom ? NULL : rag_utf8_sanitize(b.data ? b.data : "", b.len, NULL);
        rag_buf_free(&b);
        if (!inputs[i]) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    }

    ollama_embed_response_t *res = NULL;
    if (st == RAG_OK) {
        ollama_embed_request_t req = {
            .model = rag->embed_model,
            .inputs = (const char *const *)inputs,
            .inputs_count = n,
            .dimensions = rag->dimensions,
        };
        ollama_status_t ost = ollama_embed(client, &req, &res);
        if (ost != OLLAMA_OK) st = rag_ollama_error(rag, ost, "embedding model", rag->embed_model);
    }
    for (size_t i = 0; i < n; i++) free(inputs[i]);
    free(inputs);
    if (st != RAG_OK) return st;

    /* check what came back */
    size_t size = res->embeddings_count > 0 ? res->embeddings[0].size : 0;
    if (res->embeddings_count != n)
        st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "the embedding model '%s' returned %zu embeddings for %zu texts",
                         rag->embed_model, res->embeddings_count, n);
    else if (size == 0)
        st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "the embedding model '%s' returned empty embeddings",
                         rag->embed_model);
    for (size_t i = 0; st == RAG_OK && i < n; i++)
        if (res->embeddings[i].size != size)
            st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA,
                             "the embedding model '%s' returned embeddings of different sizes", rag->embed_model);
    if (st == RAG_OK && rag->stored_dimensions && size != rag->stored_dimensions)
        st = rag_err_set(&rag->err, RAG_ERROR_MODEL_MISMATCH,
                         "the embedding model '%s' returned embeddings of %zu dimensions, but the index '%s' stores "
                         "embeddings of %zu: was the model changed?",
                         rag->embed_model, size, rag->path, rag->stored_dimensions);
    if (st == RAG_OK && size > SIZE_MAX / sizeof(float) / n)
        st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");

    float *vectors = st == RAG_OK ? malloc(n * size * sizeof(float)) : NULL;
    if (st == RAG_OK && !vectors) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    for (size_t i = 0; st == RAG_OK && i < n; i++) {
        float *v = vectors + i * size;
        for (size_t j = 0; j < size; j++) {
            double x = res->embeddings[i].values[j];
            if (!isfinite(x) || fabs(x) > 3e38) {
                st = rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "the embedding model '%s' returned an invalid embedding",
                                 rag->embed_model);
                break;
            }
            v[j] = (float)x;
        }
        rag_vec_normalize(v, size); /* a zero vector stays zero: similar to nothing */
    }
    ollama_response_free(res);
    if (st != RAG_OK) {
        free(vectors);
        return st;
    }
    *out = vectors;
    *dims = size;
    return RAG_OK;
}
