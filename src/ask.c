/* Answers: a prompt made of the chunks found, sent to the chat model. */
#include "internal.h"

#include "compat.h"

#include <ollama.h>

#include <stdlib.h>
#include <string.h>

static const char DEFAULT_SYSTEM_PROMPT[] =
    "You answer questions about the user's documents. Use only the information in the numbered passages "
    "given with the question. If they do not contain the answer, say that you could not find it in the "
    "documents. When you use a passage, cite its number in square brackets, like [1]. Answer in the "
    "language of the question.";

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (rag_os_is_sep(*p)) name = p + 1;
    return name;
}

/* The user message: the passages, then the question. */
static char *build_prompt(const char *question, const rag_results_t *found) {
    buf_t b = {0};
    rag_buf_puts(&b, "Passages from the documents:\n\n");
    if (found->count == 0) rag_buf_puts(&b, "(no relevant passage was found)\n\n");
    for (size_t i = 0; i < found->count; i++) {
        const rag_result_t *r = &found->items[i];
        if (r->line_start == r->line_end)
            rag_buf_printf(&b, "[%zu] %s, line %zu\n", i + 1, base_name(r->path), r->line_start);
        else
            rag_buf_printf(&b, "[%zu] %s, lines %zu-%zu\n", i + 1, base_name(r->path), r->line_start, r->line_end);
        rag_buf_puts(&b, r->text);
        rag_buf_puts(&b, "\n\n");
    }
    rag_buf_puts(&b, "Question: ");
    rag_buf_puts(&b, question);
    char *prompt = rag_buf_take(&b);
    if (!prompt) return NULL;
    /* the question comes from the user: make sure Ollama gets valid UTF-8 */
    char *clean = rag_utf8_sanitize(prompt, strlen(prompt), NULL);
    free(prompt);
    return clean;
}

void rag_answer_free(rag_answer_t *answer) {
    if (!answer) return;
    for (size_t i = 0; i < answer->sources_count; i++) {
        free((char *)answer->sources[i].path);
        free((char *)answer->sources[i].text);
    }
    free((rag_result_t *)answer->sources);
    free((char *)answer->text);
    free((char *)answer->model);
    free(answer);
}

/* Streams the answer of the chat model into `out`. */
static rag_status_t chat(rag_t *rag, const char *model, const char *system, const char *prompt,
                         const rag_ask_options_t *o, buf_t *out) {
    ollama_client_t *client = rag_client(rag);
    if (!client) return rag->err.pub.code;
    char *clean_system = rag_utf8_sanitize(system, strlen(system), NULL);
    if (!clean_system) return rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    ollama_message_t messages[] = {
        {.role = "system", .content = clean_system},
        {.role = "user", .content = prompt},
    };
    /* an empty system prompt is not sent */
    bool has_system = *clean_system != '\0';
    ollama_chat_request_t req = {
        .model = model, .messages = has_system ? messages : messages + 1, .messages_count = has_system ? 2 : 1};
    ollama_stream_t *stream = NULL;
    ollama_status_t ost = ollama_chat_stream(client, &req, &stream);
    free(clean_system);
    if (ost != OLLAMA_OK) return rag_ollama_error(rag, ost, "chat model", model);

    rag_status_t st = RAG_OK;
    const ollama_chat_response_t *part;
    while ((part = ollama_stream_next_chat(stream)) != NULL) {
        const char *text = part->message.content;
        if (!text || !*text) continue;
        if (!rag_buf_puts(out, text)) {
            st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
            break;
        }
        if (o->on_token && o->on_token(text, o->user_data) != 0) {
            st = rag_err_set(&rag->err, RAG_ERROR_ABORTED, "stopped by the token callback");
            break;
        }
    }
    const ollama_error_t *e = ollama_stream_error(stream);
    if (st == RAG_OK && e->code != OLLAMA_OK) {
        st = e->code == OLLAMA_ERROR_MEMORY
                 ? rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory")
                 : rag_err_set(&rag->err, RAG_ERROR_OLLAMA, "chat model '%s': %s", model, e->message);
    }
    ollama_stream_free(stream);
    return st;
}

rag_status_t rag_ask(rag_t *rag, const char *question, const rag_ask_options_t *options, rag_answer_t **answer) {
    static const rag_ask_options_t defaults = {0};
    if (answer) *answer = NULL;
    rag_status_t st = rag_begin_call(rag);
    if (st != RAG_OK) return st;
    if (!answer) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "no answer");
    if (!question || !*question) return rag_err_set(&rag->err, RAG_ERROR_INVALID, "empty question");
    const rag_ask_options_t *o = options ? options : &defaults;
    const char *model = o->chat_model && *o->chat_model ? o->chat_model : rag->chat_model;

    bool vectors = false;
    st = rag_db_has_vectors(rag, &vectors);
    if (st != RAG_OK) return st;
    if (!vectors)
        return rag_err_set(&rag->err, RAG_ERROR_EMPTY, "the index '%s' contains no documents: index some first",
                           rag->path);

    rag_search_options_t search = {.top_k = o->top_k, .min_score = o->min_score};
    rag_results_t *found = NULL;
    st = rag_find(rag, question, &search, &found);
    if (st != RAG_OK) return st;

    rag_answer_t *a = calloc(1, sizeof(*a));
    char *prompt = build_prompt(question, found);
    char *model_copy = rag_strdup(model);
    buf_t text = {0};
    if (!a || !prompt || !model_copy) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    if (st == RAG_OK)
        st = chat(rag, model, o->system_prompt ? o->system_prompt : DEFAULT_SYSTEM_PROMPT, prompt, o, &text);
    free(prompt);
    char *answer_text = st == RAG_OK ? rag_buf_take(&text) : NULL;
    if (st == RAG_OK && !answer_text) st = rag_err_set(&rag->err, RAG_ERROR_MEMORY, "out of memory");
    if (st != RAG_OK) {
        rag_buf_free(&text);
        free(model_copy);
        free(a);
        rag_results_free(found);
        return st;
    }

    /* the answer takes the results over */
    a->text = answer_text;
    a->model = model_copy;
    a->sources = found->items;
    a->sources_count = found->count;
    free(found);
    *answer = a;
    return RAG_OK;
}
