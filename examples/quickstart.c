/* The example of the README: index a folder of notes, then ask a question about them. */
#include <ollama_rag.h>
#include <stdio.h>

static int print(const char *text, void *data) {
    (void)data;
    fputs(text, stdout);
    return 0;
}

int main(void) {
    rag_t *rag;
    rag_answer_t *answer;
    rag_ask_options_t options = {.on_token = print};

    if (rag_open("notes.db", NULL, &rag) == RAG_OK && rag_add_dir(rag, "notes", NULL) == RAG_OK &&
        rag_ask(rag, "What did I plan for the holidays?", &options, &answer) == RAG_OK) {
        for (size_t i = 0; i < answer->sources_count; i++)
            printf("\n[%zu] %s, line %zu", i + 1, answer->sources[i].path, answer->sources[i].line_start);
        putchar('\n');
        rag_answer_free(answer);
    }
    int failed = rag_last_error(rag)->code != RAG_OK;
    if (failed) fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
    rag_close(rag);
    return failed;
}
