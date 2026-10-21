/*
 * Index a folder of notes (.txt and .md files, recursively).
 *
 *     index-notes <folder> [database] [embedding model]
 *
 * Run it again after editing your notes: only the files that changed are
 * processed, and the deleted ones are removed from the index.
 */
#include <ollama_rag.h>
#include <stdio.h>

static int on_progress(const rag_progress_t *p, void *data) {
    (void)data;
    switch (p->event) {
    case RAG_FILE_INDEXED:
        printf("[%zu/%zu] indexed %s (%zu chunk%s)\n", p->done, p->total, p->path, p->chunks,
               p->chunks == 1 ? "" : "s");
        break;
    case RAG_FILE_UNCHANGED: printf("[%zu/%zu] up to date %s\n", p->done, p->total, p->path); break;
    case RAG_FILE_REMOVED: printf("removed %s\n", p->path); break;
    case RAG_FILE_FAILED: printf("[%zu/%zu] skipped %s: %s\n", p->done, p->total, p->path, p->message); break;
    }
    return 0; /* nonzero would stop */
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s <folder> [database] [embedding model]\n", argv[0]);
        return 2;
    }
    rag_options_t options = {
        .embed_model = argc > 3 ? argv[3] : NULL, /* NULL: the database's, or embeddinggemma */
        .progress = on_progress,
    };
    rag_t *rag;
    if (rag_open(argc > 2 ? argv[2] : "notes.db", &options, &rag) != RAG_OK) {
        fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
        rag_close(rag);
        return 1;
    }

    rag_index_stats_t s;
    if (rag_add_dir(rag, argv[1], &s) != RAG_OK) {
        fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
        rag_close(rag);
        return 1;
    }
    printf("\n%zu files indexed, %zu up to date, %zu removed, %zu skipped; %zu chunks added\n", s.files_indexed,
           s.files_unchanged, s.files_removed, s.files_failed, s.chunks_added);

    rag_stats_t stats;
    if (rag_stats(rag, &stats) == RAG_OK)
        printf("the index has %zu chunks from %zu files, embedded with %s (%zu dimensions)\n", stats.chunks,
               stats.files, stats.embed_model, stats.dimensions);
    rag_close(rag);
    return 0;
}
