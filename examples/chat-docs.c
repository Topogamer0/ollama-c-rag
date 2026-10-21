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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * The console of Windows types UTF-16, and shows UTF-8 only when asked: the
 * questions, the answers and the paths are UTF-8, like the documents.
 */
static char *read_console_line(HANDLE in) {
    size_t len = 0, cap = 256;
    wchar_t *line = malloc(cap * sizeof(*line));
    DWORD n = 0;
    while (line && ReadConsoleW(in, line + len, (DWORD)(cap - len - 1), &n, NULL) && n > 0) {
        len += n;
        if (line[len - 1] == L'\n') break;
        if (len + 1 == cap) {
            wchar_t *bigger = realloc(line, (cap *= 2) * sizeof(*line));
            if (!bigger) free(line);
            line = bigger;
        }
    }
    if (!line || len == 0 || line[0] == 0x1A) { /* Ctrl-Z */
        free(line);
        return NULL;
    }
    while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) len--;
    line[len] = L'\0';
    int size = WideCharToMultiByte(CP_UTF8, 0, line, -1, NULL, 0, NULL, NULL);
    char *utf8 = size > 0 ? malloc((size_t)size) : NULL;
    if (utf8) WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, size, NULL, NULL);
    free(line);
    return utf8;
}
#endif

static int print(const char *text, void *data) {
    (void)data;
    fputs(text, stdout);
    fflush(stdout);
    return 0;
}

/* Reads a line of any length; NULL at the end of the input. */
static char *read_line(void) {
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(in, &mode)) return read_console_line(in);
#endif
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
#ifdef _WIN32
    UINT page = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#endif
    rag_options_t options = {
        .chat_model = argc > 2 ? argv[2] : NULL, /* NULL: gemma3 */
        .must_exist = true,
    };
    rag_t *rag;
    if (rag_open(argc > 1 ? argv[1] : "notes.db", &options, &rag) != RAG_OK) {
        fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
        rag_close(rag);
#ifdef _WIN32
        SetConsoleOutputCP(page);
#endif
        return 1;
    }

    rag_ask_options_t ask = {.top_k = 5, .on_token = print};
    printf("Ask a question about your documents (an empty line to quit).\n");
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
#ifdef _WIN32
    SetConsoleOutputCP(page);
#endif
    return 0;
}
