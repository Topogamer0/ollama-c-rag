/* Uses the installed library: creates an index in memory and prints its settings. */
#include <ollama_rag.h>
#include <stdio.h>

int main(void) {
    rag_t *rag;
    rag_stats_t stats;
    if (rag_open(":memory:", NULL, &rag) != RAG_OK || rag_stats(rag, &stats) != RAG_OK) {
        fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
        rag_close(rag);
        return 1;
    }
    printf("ollama-rag-c %s: %s, chunks of %zu characters\n", rag_version(), stats.embed_model, stats.chunk_size);
    rag_close(rag);
    return 0;
}
