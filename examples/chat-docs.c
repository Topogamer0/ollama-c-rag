/*
 * Chat with your documents: asks the questions typed on the standard input,
 * one per line, and streams the answers with their sources.
 *
 *     chat-docs [database] [chat model]
 *
 * Index the documents first, e.g. with the index-notes example.
 */
#include <ollama_rag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print(const char *text, void *data) {
    (void)data;
    fputs(text, stdout);
    fflush(stdout);
    return 0;
}

/* Reads a line of any length; NULL at the end of the input. */
static char *read_line(void) {
    size_t len = 0, cap = 256;
    char *line = malloc(cap);
    int c = 0;
    while (line && (c = getchar()) != EOF && c != '\n') {
        if (len + 1 == cap) {
            char *bigger = realloc(line, cap *= 2);
            if (!bigger) free(line);
            line = bigger;
            if (!line) break;
        }
        line[len++] = (char)c;
    }
    if (!line || (c == EOF && len == 0)) {
        free(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

int main(int argc, char **argv) {
    rag_options_t options = {
        .chat_model = argc > 2 ? argv[2] : NULL, /* NULL: gemma3 */
        .must_exist = true,
    };
    rag_t *rag;
    if (rag_open(argc > 1 ? argv[1] : "notes.db", &options, &rag) != RAG_OK) {
        fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
        rag_close(rag);
        return 1;
    }

    rag_ask_options_t ask = {.top_k = 5, .on_token = print};
    printf("Ask a question about your documents (an empty line or Ctrl-D to quit).\n");
    for (;;) {
        printf("\n> ");
        fflush(stdout);
        char *question = read_line();
        if (!question || !*question) {
            free(question);
            break;
        }
        rag_answer_t *answer;
        if (rag_ask(rag, question, &ask, &answer) == RAG_OK) {
            printf("\n");
            for (size_t i = 0; i < answer->sources_count; i++) {
                const rag_result_t *s = &answer->sources[i];
                if (s->line_start == s->line_end)
                    printf("  [%zu] %s, line %zu (%.2f)\n", i + 1, s->path, s->line_start, s->score);
                else
                    printf("  [%zu] %s, lines %zu-%zu (%.2f)\n", i + 1, s->path, s->line_start, s->line_end, s->score);
            }
            rag_answer_free(answer);
        } else {
            fprintf(stderr, "\nerror: %s\n", rag_last_error(rag)->message);
        }
        free(question);
    }
    rag_close(rag);
    return 0;
}
