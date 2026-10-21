/*
 * Integration tests of the public API against tests/mock_server.py, which
 * starts a fake Ollama server and runs this program with OLLAMA_HOST set.
 */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#endif

#include "ollama_rag.h"
#include "test.h"
#include "test_files.h"

#include <curl/curl.h>
#include <stdint.h>

static char *tmp;  /* temporary directory */
static char *docs; /* the documents */

/* ---- talking to the mock server ---- */

static size_t collect(char *ptr, size_t size, size_t nmemb, void *userdata) {
    char **out = userdata;
    size_t old = *out ? strlen(*out) : 0, n = size * nmemb;
    char *s = realloc(*out, old + n + 1);
    if (!s) return 0;
    memcpy(s + old, ptr, n);
    s[old + n] = '\0';
    *out = s;
    return n;
}

/* GET or POST (if body is not NULL) to the mock server; returns the response body. */
static char *mock(const char *path, const char *body) {
    char url[512];
    const char *host = getenv("OLLAMA_HOST");
    snprintf(url, sizeof(url), "http://%s%s", host ? host : "127.0.0.1:11434", path);
    char *text = NULL;
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &text);
    if (body) curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    curl_easy_cleanup(h);
    return text ? text : calloc(1, 1);
}

/* A counter of /_stats. */
static long mock_stat(const char *name) {
    char *json = mock("/_stats", NULL);
    char key[64];
    snprintf(key, sizeof(key), "\"%s\": ", name);
    const char *p = strstr(json, key);
    long value = p ? strtol(p + strlen(key), NULL, 10) : -1;
    free(json);
    return value;
}

static void mock_reset(void) { free(mock("/_reset", "{}")); }

static void mock_config(const char *json) { free(mock("/_config", json)); }

/* True if the last request received by the server contains `text`. */
static bool last_request_has(const char *text) {
    char *json = mock("/_last", NULL);
    bool found = strstr(json, text) != NULL;
    if (!found) fprintf(stderr, "  last request: %s\n  does not contain: %s\n", json, text);
    free(json);
    return found;
}

/* ---- helpers ---- */

typedef struct events {
    int indexed, unchanged, removed, failed;
    size_t chunks;
    char failure[1024];
    char removed_path[4096];
    size_t last_done, total;
    int stop_at; /* stop at the n-th event */
    int count;
} events_t;

static int on_progress(const rag_progress_t *p, void *data) {
    events_t *ev = data;
    switch (p->event) {
    case RAG_FILE_INDEXED:
        ev->indexed++;
        ev->chunks += p->chunks;
        break;
    case RAG_FILE_UNCHANGED: ev->unchanged++; break;
    case RAG_FILE_REMOVED:
        ev->removed++;
        snprintf(ev->removed_path, sizeof(ev->removed_path), "%s", p->path);
        break;
    case RAG_FILE_FAILED:
        ev->failed++;
        CHECK(p->message != NULL);
        snprintf(ev->failure, sizeof(ev->failure), "%s", p->message ? p->message : "");
        break;
    }
    CHECK(p->path && rag_os_root_len(p->path) > 0); /* absolute */
    CHECK(p->done <= p->total);
    ev->last_done = p->done;
    ev->total = p->total;
    return ++ev->count == ev->stop_at;
}

static rag_t *open_index(const char *name, rag_options_t options) {
    if (!options.embed_model) options.embed_model = "mock-embed";
    rag_t *rag = NULL;
    rag_status_t st = rag_open(path_of(tmp, name), &options, &rag);
    if (st != RAG_OK) fprintf(stderr, "  rag_open: %s\n", rag_last_error(rag)->message);
    CHECK(st == RAG_OK);
    return rag;
}

static void write_binary(const char *path) {
    FILE *f = rag_os_fopen(path, "wb", NULL);
    if (!f) {
        perror(path);
        exit(2);
    }
    fwrite("abc\0def", 1, 7, f);
    fclose(f);
}

static void make_documents(void) {
    write_file(path_of(docs, "fruit.md"), "# Fruit\n"
                                          "\n"
                                          "Bananas are yellow fruits rich in potassium. Monkeys love bananas.\n"
                                          "\n"
                                          "Apples can be red or green. An apple a day keeps the doctor away.\n");
    write_file(path_of(docs, "space.md"), "# Space\n"
                                          "\n"
                                          "The Moon orbits the Earth every 27 days.\n"
                                          "\n"
                                          "Mars is the red planet, with two small moons.\n");
    write_file(path_of(docs, "code.txt"), "The function main returns zero when the program succeeds.\n");
    write_file(path_of(docs, "sub/cooking.md"), "Pasta needs salted boiling water. Cook spaghetti for nine minutes.\n");
    write_file(path_of(docs, "empty.md"), "");
    write_file(path_of(docs, ".hidden.md"), "secret bananas");
    write_file(path_of(docs, "notes.org"), "Org mode notes about tomatoes and basil.");
    write_binary(path_of(docs, "binary.txt"));
}

static void print_results(const rag_results_t *r) {
    for (size_t i = 0; i < r->count; i++)
        fprintf(stderr, "  [%zu] %.3f %s:%zu-%zu %s\n", i, r->items[i].score, r->items[i].path, r->items[i].line_start,
                r->items[i].line_end, r->items[i].text);
}

/* Searches and checks that the best result comes from `file` and contains `text`. */
static void check_best(rag_t *rag, const char *query, const char *file, const char *text) {
    rag_results_t *r = NULL;
    rag_search_options_t o = {.top_k = 3};
    CHECK(rag_search(rag, query, &o, &r) == RAG_OK);
    CHECK(r && r->count > 0);
    if (r && r->count > 0) {
        bool ok = strcmp(r->items[0].path, path_of(docs, file)) == 0 && strstr(r->items[0].text, text) != NULL;
        CHECK(ok);
        if (!ok) {
            fprintf(stderr, "  query \"%s\": expected %s with \"%s\"\n", query, file, text);
            print_results(r);
        }
    }
    rag_results_free(r);
}

/* ---- tests ---- */

static void test_index_dir(void) {
    mock_reset();
    events_t ev = {0};
    rag_t *rag = open_index(
        "main.db",
        (rag_options_t){
            .chunk_size = 80, .chunk_overlap = 10, .batch_size = 3, .progress = on_progress, .progress_data = &ev});
    rag_index_stats_t stats;
    rag_status_t st = rag_add_dir(rag, docs, &stats);
    if (st != RAG_OK) fprintf(stderr, "  %s\n", rag_last_error(rag)->message);
    CHECK(st == RAG_OK);
    CHECK(rag_last_error(rag)->code == RAG_OK);
    /* fruit.md, space.md, code.txt, sub/cooking.md, empty.md; binary.txt fails */
    CHECK_SIZE(stats.files_indexed, 5);
    CHECK_SIZE(stats.files_failed, 1);
    CHECK_SIZE(stats.files_unchanged, 0);
    CHECK_SIZE(stats.files_removed, 0);
    CHECK(stats.chunks_added > 5);
    CHECK(ev.indexed == 5 && ev.failed == 1);
    CHECK_SIZE(ev.chunks, stats.chunks_added);
    CHECK(strstr(ev.failure, "binary") != NULL);
    CHECK_SIZE(ev.total, 6);

    /* batches of 3 chunks, across files */
    CHECK(mock_stat("max_batch") == 3);
    CHECK(mock_stat("embed_inputs") == (long)stats.chunks_added);
    CHECK(mock_stat("embed_requests") == (long)(stats.chunks_added + 2) / 3);

    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 5);
    CHECK_SIZE(info.chunks, stats.chunks_added);
    CHECK_SIZE(info.dimensions, 256);
    CHECK_STR(info.embed_model, "mock-embed");
    CHECK_SIZE(info.chunk_size, 80);
    CHECK(info.last_indexed > 0);
    rag_close(rag);
}

static void test_search(void) {
    rag_t *rag = open_index("main.db", (rag_options_t){0});
    check_best(rag, "yellow bananas potassium", "fruit.md", "Bananas");
    check_best(rag, "Mars red planet moons", "space.md", "Mars");
    check_best(rag, "spaghetti pasta water", "sub/cooking.md", "spaghetti");
    CHECK(last_request_has("[\\\"spaghetti pasta water\\\"]"));

    rag_results_t *r = NULL;
    rag_search_options_t o = {.top_k = 3};
    CHECK(rag_search(rag, "yellow bananas potassium", &o, &r) == RAG_OK);
    CHECK_SIZE(r->count, 3);
    for (size_t i = 1; i < r->count; i++) CHECK(r->items[i - 1].score >= r->items[i].score);
    const rag_result_t *best = &r->items[0];
    CHECK(best->score > 0.3 && best->score <= 1.0001);
    /* the heading and the first paragraph */
    CHECK_SIZE(best->chunk, 0);
    CHECK_SIZE(best->line_start, 1);
    CHECK_SIZE(best->line_end, 3);
    CHECK_SIZE(best->start, 0);
    CHECK_SIZE(best->end, 75);
    rag_results_free(r);

    /* top_k and min_score */
    o.top_k = 1;
    CHECK(rag_search(rag, "bananas", &o, &r) == RAG_OK);
    CHECK_SIZE(r->count, 1);
    rag_results_free(r);
    o = (rag_search_options_t){.top_k = 100};
    CHECK(rag_search(rag, "bananas", &o, &r) == RAG_OK);
    size_t all = r->count;
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(all, info.chunks);
    rag_results_free(r);
    o.min_score = 0.2;
    CHECK(rag_search(rag, "bananas", &o, &r) == RAG_OK);
    CHECK(r->count >= 1 && r->count < all);
    for (size_t i = 0; i < r->count; i++) CHECK(r->items[i].score >= 0.2);
    rag_results_free(r);
    o.min_score = 0.9999;
    CHECK(rag_search(rag, "bananas", &o, &r) == RAG_OK);
    CHECK_SIZE(r->count, 0);
    rag_results_free(r);

    CHECK(rag_search(rag, "", NULL, &r) == RAG_ERROR_INVALID);
    CHECK(r == NULL);
    CHECK(rag_search(rag, "x", NULL, NULL) == RAG_ERROR_INVALID);
    rag_results_free(NULL);
    rag_close(rag);
}

static void test_incremental(void) {
    mock_reset();
    events_t ev = {0};
    rag_t *rag = open_index("main.db", (rag_options_t){.progress = on_progress, .progress_data = &ev});
    rag_index_stats_t stats;

    /* nothing changed: nothing is embedded */
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 5);
    CHECK_SIZE(stats.files_indexed, 0);
    CHECK_SIZE(stats.files_failed, 1);
    CHECK(ev.unchanged == 5);
    CHECK(mock_stat("embed_requests") == 0);

    /* a modified file */
    write_file(path_of(docs, "space.md"), "# Space\n\nJupiter is the largest planet of the solar system.\n");
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK_SIZE(stats.files_unchanged, 4);
    CHECK(mock_stat("embed_requests") == 1);
    check_best(rag, "Jupiter largest planet", "space.md", "Jupiter");
    rag_results_t *r = NULL;
    rag_search_options_t all = {.top_k = 100};
    CHECK(rag_search(rag, "Moon orbits Earth", &all, &r) == RAG_OK);
    for (size_t i = 0; i < r->count; i++) CHECK(strstr(r->items[i].text, "27 days") == NULL);
    rag_results_free(r);

    /* a deleted file */
    char *cooking = dup_string(path_of(docs, "sub/cooking.md"));
    remove_file(cooking);
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 1);
    CHECK(stats.chunks_removed >= 1);
    CHECK(ev.removed == 1);
    CHECK_STR(ev.removed_path, cooking);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 4);
    CHECK(rag_search(rag, "spaghetti pasta water", &all, &r) == RAG_OK);
    for (size_t i = 0; i < r->count; i++) CHECK(strstr(r->items[i].text, "spaghetti") == NULL);
    rag_results_free(r);
    free(cooking);

    /* single files */
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 1);
    CHECK(rag_add_file(rag, path_of(docs, "nothing.md"), &stats) == RAG_ERROR_NOT_FOUND);
    CHECK(strstr(rag_last_error(rag)->message, "nothing.md") != NULL);
    CHECK(rag_add_file(rag, docs, &stats) == RAG_ERROR_INVALID);
    CHECK(rag_add_file(rag, path_of(docs, "binary.txt"), &stats) == RAG_ERROR_IO);
    CHECK_SIZE(stats.files_failed, 1);
    CHECK(strstr(rag_last_error(rag)->message, "binary") != NULL);

    /* any extension can be added explicitly, and stays even if the directory does not list it */
    CHECK(rag_add_file(rag, path_of(docs, "notes.org"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    check_best(rag, "tomatoes basil", "notes.org", "tomatoes");
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 0);
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 5);

    /* relative paths */
    char *cwd = current_dir();
    CHECK(change_dir(docs));
    CHECK(rag_add_file(rag, "./fruit.md", &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 1);
    CHECK(rag_add_dir(rag, ".", &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 0);
    CHECK(change_dir(cwd));
    free(cwd);

    CHECK(rag_add_dir(rag, path_of(docs, "fruit.md"), &stats) == RAG_ERROR_INVALID);
    CHECK(rag_add_dir(rag, path_of(tmp, "nowhere"), &stats) == RAG_ERROR_NOT_FOUND);
    CHECK(rag_add_dir(rag, NULL, &stats) == RAG_ERROR_INVALID);
    rag_close(rag);
}

static void test_settings_change(void) {
    rag_index_stats_t stats;
    /* the stored chunk size is reused */
    rag_t *rag = open_index("main.db", (rag_options_t){0});
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 0);
    rag_close(rag);

    /* a new chunk size indexes everything again */
    rag = open_index("main.db", (rag_options_t){.chunk_size = 120});
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 4); /* notes.org is not in the listing of the directory */
    CHECK_SIZE(stats.files_unchanged, 0);
    rag_close(rag);

    /* and becomes the new default */
    rag = open_index("main.db", (rag_options_t){0});
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.chunk_size, 120);
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 4);
    rag_close(rag);
}

static void test_model_mismatch(void) {
    rag_t *rag = NULL;
    rag_options_t other = {.embed_model = "other-embed"};
    CHECK(rag_open(path_of(tmp, "main.db"), &other, &rag) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(rag_last_error(rag)->message, "mock-embed") != NULL);
    CHECK(strstr(rag_last_error(rag)->message, "other-embed") != NULL);
    rag_close(rag);

    /* the model now returns embeddings of another size */
    mock_config("{\"dimensions\": 16}");
    rag = open_index("main.db", (rag_options_t){0});
    write_file(path_of(docs, "new.md"), "A new file.");
    rag_index_stats_t stats;
    CHECK(rag_add_file(rag, path_of(docs, "new.md"), &stats) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(rag_last_error(rag)->message, "256") != NULL);
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "bananas", NULL, &r) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(r == NULL);
    rag_close(rag);
    mock_reset();
    remove_file(path_of(docs, "new.md"));
}

static void test_prefixes(void) {
    rag_t *rag = open_index("nomic.db", (rag_options_t){.embed_model = "nomic-embed-text"});
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    CHECK(last_request_has("search_document: # Fruit"));
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "bananas", NULL, &r) == RAG_OK);
    CHECK(last_request_has("[\\\"search_query: bananas\\\"]"));
    rag_results_free(r);
    rag_close(rag);

    /* another document prefix means other embeddings: the files are indexed again */
    rag = open_index("nomic.db", (rag_options_t){.embed_model = "nomic-embed-text", .document_prefix = "doc: "});
    rag_index_stats_t stats;
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK(last_request_has("doc: # Fruit"));
    rag_close(rag);

    /* dimensions are passed to Ollama */
    rag = open_index("small.db", (rag_options_t){.dimensions = 64});
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    CHECK(last_request_has("\\\"dimensions\\\":64"));
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.dimensions, 64);
    rag_close(rag);
}

static void test_ollama_errors(void) {
    rag_index_stats_t stats;
    rag_t *rag = open_index("missing.db", (rag_options_t){.embed_model = "missing"});
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_ERROR_OLLAMA);
    CHECK(rag_last_error(rag)->status_code == 404);
    CHECK(strstr(rag_last_error(rag)->message, "not found") != NULL);
    CHECK(strstr(rag_last_error(rag)->message, "ollama pull missing") != NULL);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.chunks, 0);
    rag_close(rag);

    rag = open_index("count.db", (rag_options_t){.embed_model = "wrong-count"});
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), &stats) == RAG_ERROR_OLLAMA);
    CHECK(strstr(rag_last_error(rag)->message, "returned") != NULL);
    rag_close(rag);

    rag = open_index("unreachable.db", (rag_options_t){.host = "http://127.0.0.1:1", .timeout_ms = 5000});
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), &stats) == RAG_ERROR_OLLAMA);
    CHECK(strstr(rag_last_error(rag)->message, "connect") != NULL);
    rag_close(rag);
}

static void test_interrupted(void) {
    /* a big file, embedded in several requests, the second of which fails */
    string_t big = {0};
    for (int i = 0; i < 200; i++)
        string_append(&big, i == 150 ? "Zanzibar is an island with spice farms and white beaches.\n"
                                     : "Line of filler text about nothing in particular, again.\n");
    write_file(path_of(docs, "big.md"), big.data);
    free(big.data);

    mock_reset();
    mock_config("{\"fail_after\": 1}");
    rag_t *rag = open_index("big.db", (rag_options_t){.chunk_size = 200, .batch_size = 4});
    rag_index_stats_t stats;
    CHECK(rag_add_file(rag, path_of(docs, "big.md"), &stats) == RAG_ERROR_OLLAMA);
    CHECK(rag_last_error(rag)->status_code == 500);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.chunks, 4); /* the first batch was stored */

    /* the file is not up to date: it is indexed again */
    mock_reset();
    CHECK(rag_add_file(rag, path_of(docs, "big.md"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.chunks, stats.chunks_added);
    CHECK(stats.chunks_added > 40);
    CHECK(mock_stat("max_batch") == 4);
    CHECK(rag_add_file(rag, path_of(docs, "big.md"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 1);

    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "Zanzibar island spice", NULL, &r) == RAG_OK);
    CHECK(r->count > 0);
    if (r->count > 0) {
        CHECK(strstr(r->items[0].text, "Zanzibar") != NULL);
        CHECK(r->items[0].line_start <= 151 && r->items[0].line_end >= 151);
    }
    rag_results_free(r);
    rag_close(rag);
    remove_file(path_of(docs, "big.md"));
}

static void test_abort(void) {
    events_t ev = {.stop_at = 2}; /* binary.txt fails, then code.txt is indexed */
    rag_t *rag =
        open_index("abort.db", (rag_options_t){.batch_size = 1, .progress = on_progress, .progress_data = &ev});
    rag_index_stats_t stats;
    CHECK(rag_add_dir(rag, docs, &stats) == RAG_ERROR_ABORTED);
    CHECK(ev.count == 2);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 1); /* the file reported before stopping is stored */
    rag_close(rag);
}

typedef struct tokens {
    string_t text;
    int count;
    int stop_at;
} tokens_t;

static int on_token(const char *text, void *data) {
    tokens_t *t = data;
    CHECK(text && *text);
    string_append(&t->text, text);
    return ++t->count == t->stop_at;
}

static void test_ask(void) {
    mock_reset();
    rag_t *rag = open_index("main.db", (rag_options_t){.chat_model = "mock-chat"});
    tokens_t tokens = {{0}, 0, 0};
    rag_ask_options_t o = {.top_k = 2, .on_token = on_token, .user_data = &tokens};
    rag_answer_t *answer = NULL;
    rag_status_t st = rag_ask(rag, "Why are bananas yellow?", &o, &answer);
    if (st != RAG_OK) fprintf(stderr, "  %s\n", rag_last_error(rag)->message);
    CHECK(st == RAG_OK);
    CHECK(answer != NULL);
    if (answer) {
        CHECK_STR(answer->text, "Found 2 sources [1]. You asked: Why are bananas yellow?");
        CHECK_STR(tokens.text.data, answer->text); /* the streamed pieces make the answer */
        CHECK(tokens.count > 1);
        CHECK_STR(answer->model, "mock-chat");
        CHECK_SIZE(answer->sources_count, 2);
        CHECK_STR(answer->sources[0].path, path_of(docs, "fruit.md"));
        CHECK(answer->sources[0].score >= answer->sources[1].score);
    }
    rag_answer_free(answer);
    free(tokens.text.data);
    CHECK(mock_stat("chat_requests") == 1);
    CHECK(last_request_has("\\\"model\\\":\\\"mock-chat\\\""));
    CHECK(last_request_has("Question: Why are bananas yellow?"));
    CHECK(last_request_has("[1] fruit.md, lines 1-3\\\\n# Fruit")); /* JSON in JSON */

    /* without streaming, with another model and no system prompt */
    rag_ask_options_t plain = {.chat_model = "other-chat", .system_prompt = ""};
    CHECK(rag_ask(rag, "bananas", &plain, &answer) == RAG_OK);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(answer->sources_count, info.chunks < 5 ? info.chunks : 5);
    CHECK_STR(answer->model, "other-chat");
    rag_answer_free(answer);
    CHECK(last_request_has("\\\"model\\\":\\\"other-chat\\\""));
    char *last = mock("/_last", NULL);
    CHECK(strstr(last, "\\\"role\\\":\\\"system") == NULL);
    free(last);

    /* every chunk can be filtered out: the model is told so */
    rag_ask_options_t strict = {.min_score = 0.9999};
    CHECK(rag_ask(rag, "bananas", &strict, &answer) == RAG_OK);
    CHECK_SIZE(answer->sources_count, 0);
    CHECK(strstr(answer->text, "Found 0 sources") != NULL);
    CHECK(last_request_has("no relevant passage"));
    rag_answer_free(answer);

    /* the token callback can stop the answer */
    tokens = (tokens_t){{0}, 0, 2};
    CHECK(rag_ask(rag, "bananas", &o, &answer) == RAG_ERROR_ABORTED);
    CHECK(answer == NULL);
    CHECK(tokens.count == 2);
    free(tokens.text.data);

    /* errors of the chat model */
    rag_ask_options_t missing = {.chat_model = "missing"};
    CHECK(rag_ask(rag, "bananas", &missing, &answer) == RAG_ERROR_OLLAMA);
    CHECK(rag_last_error(rag)->status_code == 404);
    CHECK(strstr(rag_last_error(rag)->message, "chat model 'missing'") != NULL);
    rag_ask_options_t broken = {.chat_model = "midstream-error"};
    CHECK(rag_ask(rag, "bananas", &broken, &answer) == RAG_ERROR_OLLAMA);
    CHECK(strstr(rag_last_error(rag)->message, "something went wrong") != NULL);
    CHECK(answer == NULL);

    CHECK(rag_ask(rag, "", NULL, &answer) == RAG_ERROR_INVALID);
    CHECK(rag_ask(rag, "x", NULL, NULL) == RAG_ERROR_INVALID);
    rag_answer_free(NULL);
    rag_close(rag);

    /* nothing to answer from */
    rag = open_index("empty-ask.db", (rag_options_t){0});
    CHECK(rag_ask(rag, "bananas", NULL, &answer) == RAG_ERROR_EMPTY);
    CHECK(strstr(rag_last_error(rag)->message, "no documents") != NULL);
    rag_close(rag);
}

/* ---- several handles on the same database ---- */

typedef struct race {
    rag_t *other;
    const char *file;
    rag_index_stats_t other_stats;
    rag_status_t other_status;
    int calls;
} race_t;

/* While the first handle is between two batches, the other one indexes the same file. */
static int index_meanwhile(const rag_progress_t *p, void *data) {
    race_t *r = data;
    if (p->event == RAG_FILE_INDEXED && r->calls++ == 0)
        r->other_status = rag_add_file(r->other, r->file, &r->other_stats);
    return 0;
}

static void test_same_file_twice(void) {
    char *dir = dup_string(path_of(tmp, "race"));
    write_file(path_of(dir, "a.txt"), "Alpha.");
    string_t text = {0};
    for (int i = 0; i < 12; i++) string_append(&text, "A sentence of the long file. ");
    write_file(path_of(dir, "f.txt"), text.data);
    free(text.data);

    race_t r = {0};
    r.file = path_of(dir, "f.txt");
    r.other = open_index("race.db", (rag_options_t){.chunk_size = 40});
    rag_t *rag = open_index(
        "race.db",
        (rag_options_t){.chunk_size = 40, .batch_size = 2, .progress = index_meanwhile, .progress_data = &r});
    rag_index_stats_t stats;
    CHECK(rag_add_dir(rag, dir, &stats) == RAG_OK);
    CHECK(r.other_status == RAG_OK);
    CHECK_SIZE(r.other_stats.files_indexed, 1);
    size_t f_chunks = r.other_stats.chunks_added;
    CHECK(f_chunks > 4);
    /* the first handle leaves the file to the other one, that started writing it last */
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK_SIZE(stats.files_unchanged, 1);

    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 2);
    CHECK_SIZE(info.chunks, 1 + f_chunks); /* no chunk twice */
    CHECK(rag_add_dir(r.other, dir, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 2);
    rag_close(rag);
    rag_close(r.other);
    free(dir);
}

static void test_models_of_two_handles(void) {
    /* both are opened on an empty index, each with its own model */
    rag_t *x = open_index("two-models.db", (rag_options_t){.embed_model = "model-x"});
    rag_t *y = open_index("two-models.db", (rag_options_t){.embed_model = "model-y"});
    rag_index_stats_t stats;
    CHECK(rag_add_file(y, path_of(docs, "fruit.md"), &stats) == RAG_OK);
    CHECK(rag_add_file(x, path_of(docs, "code.txt"), &stats) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(rag_last_error(x)->message, "model-y") != NULL);
    rag_close(x);
    rag_close(y);
    rag_t *rag = open_index("two-models.db", (rag_options_t){.embed_model = "model-y"});
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 1);
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "bananas", NULL, &r) == RAG_OK);
    rag_results_free(r);
    rag_close(rag);

    /* the same with two sizes of embeddings */
    rag_t *big = open_index("two-sizes.db", (rag_options_t){0});
    rag_t *narrow = open_index("two-sizes.db", (rag_options_t){.dimensions = 64});
    CHECK(rag_add_file(big, path_of(docs, "fruit.md"), &stats) == RAG_OK);
    CHECK(rag_add_file(narrow, path_of(docs, "code.txt"), &stats) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(rag_last_error(narrow)->message, "256") != NULL);
    /* the handle opened before sees what the other stored */
    CHECK(rag_stats(narrow, &info) == RAG_OK);
    CHECK_SIZE(info.dimensions, 256);
    CHECK(rag_search(big, "bananas", NULL, &r) == RAG_OK);
    CHECK(r && r->count == 1);
    rag_results_free(r);
    rag_close(big);
    rag_close(narrow);
}

static void test_emptied_index(void) {
    rag_t *rag = open_index("emptied.db", (rag_options_t){0});
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    CHECK(rag_remove_file(rag, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.dimensions, 0);
    rag_close(rag);

    /* nothing to search: Ollama is not even asked */
    rag = open_index("emptied.db", (rag_options_t){.host = "http://127.0.0.1:1"});
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "bananas", NULL, &r) == RAG_OK);
    CHECK(r && r->count == 0);
    rag_results_free(r);
    rag_close(rag);

    /* and the size of the embeddings can change */
    rag = open_index("emptied.db", (rag_options_t){.dimensions = 64});
    rag_status_t st = rag_add_file(rag, path_of(docs, "fruit.md"), NULL);
    if (st != RAG_OK) fprintf(stderr, "  %s\n", rag_last_error(rag)->message);
    CHECK(st == RAG_OK);
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.dimensions, 64);
    rag_close(rag);
}

typedef struct order {
    size_t done[16];
    int count;
} order_t;

static int record_order(const rag_progress_t *p, void *data) {
    order_t *o = data;
    if (o->count < 16) o->done[o->count++] = p->done;
    return 0;
}

static void test_progress_order(void) {
    char *dir = dup_string(path_of(tmp, "order"));
    write_file(path_of(dir, "b.md"), "Bravo.");
    write_file(path_of(dir, "c.md"), "Charlie.");
    rag_t *rag = open_index("order.db", (rag_options_t){0});
    CHECK(rag_add_dir(rag, dir, NULL) == RAG_OK);
    rag_close(rag);

    /* a.md waits for its embedding while b.md and c.md are found unchanged */
    write_file(path_of(dir, "a.md"), "Alpha.");
    order_t o = {{0}, 0};
    rag = open_index("order.db", (rag_options_t){.progress = record_order, .progress_data = &o});
    CHECK(rag_add_dir(rag, dir, NULL) == RAG_OK);
    CHECK(o.count == 3);
    for (int i = 1; i < o.count; i++) CHECK(o.done[i] >= o.done[i - 1]);
    rag_close(rag);
    free(dir);
}

static int stop_now(const rag_progress_t *p, void *data) {
    (void)p;
    (void)data;
    return 1;
}

static void test_stop_in_batch(void) {
    /* the two files are stored by the same batch: both are counted, though only one is reported */
    char *dir = dup_string(path_of(tmp, "stop"));
    write_file(path_of(dir, "a.md"), "Alpha.");
    write_file(path_of(dir, "b.md"), "Bravo.");
    rag_t *rag = open_index("stop.db", (rag_options_t){.progress = stop_now});
    rag_index_stats_t stats;
    CHECK(rag_add_dir(rag, dir, &stats) == RAG_ERROR_ABORTED);
    CHECK_SIZE(stats.files_indexed, 2);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 2);
    rag_close(rag);
    free(dir);
}

static void test_large_values(void) {
    rag_t *rag = open_index("main.db", (rag_options_t){0});
    rag_results_t *r = NULL;
    rag_search_options_t all = {.top_k = SIZE_MAX};
    CHECK(rag_search(rag, "bananas", &all, &r) == RAG_OK);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK(r && r->count == info.chunks);
    rag_results_free(r);
    rag_close(rag);

    rag = NULL;
    rag_options_t huge = {.embed_model = "mock-embed", .batch_size = (size_t)1 << 60};
    CHECK(rag_open(path_of(tmp, "main.db"), &huge, &rag) == RAG_ERROR_INVALID);
    CHECK(strstr(rag_last_error(rag)->message, "batch size") != NULL);
    rag_close(rag);
    rag_close(open_index("main.db", (rag_options_t){.batch_size = RAG_MAX_BATCH_SIZE}));
}

static void test_stale_handle(void) {
    /* opened on an empty index, which another handle then fills with another model */
    rag_t *old = open_index("stale.db", (rag_options_t){.embed_model = "model-a"});
    rag_t *other = open_index("stale.db", (rag_options_t){.embed_model = "model-b"});
    CHECK(rag_add_file(other, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    rag_results_t *r = NULL;
    CHECK(rag_search(old, "bananas", NULL, &r) == RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(rag_last_error(old)->message, "model-b") != NULL);
    CHECK(rag_add_file(old, path_of(docs, "code.txt"), NULL) == RAG_ERROR_MODEL_MISMATCH);
    rag_stats_t info;
    CHECK(rag_stats(old, &info) == RAG_OK);
    CHECK_STR(info.embed_model, "model-b");
    rag_close(old);
    rag_close(other);

    /* another handle chose the size of the embeddings: this one follows */
    old = open_index("stale-size.db", (rag_options_t){0});
    other = open_index("stale-size.db", (rag_options_t){.dimensions = 64});
    CHECK(rag_add_file(other, path_of(docs, "fruit.md"), NULL) == RAG_OK);
    rag_status_t st = rag_search(old, "bananas", NULL, &r);
    if (st != RAG_OK) fprintf(stderr, "  %s\n", rag_last_error(old)->message);
    CHECK(st == RAG_OK);
    CHECK(r && r->count == 1);
    rag_results_free(r);
    CHECK(rag_add_file(old, path_of(docs, "code.txt"), NULL) == RAG_OK);
    rag_close(old);
    rag_close(other);
}

typedef struct reuse {
    rag_t *other;
    const char *victim;
    const char *outsider;
    int calls;
} reuse_t;

/* Between two groups of removals, the other handle frees the highest id and indexes another file. */
static int reuse_id(const rag_progress_t *p, void *data) {
    reuse_t *r = data;
    if (p->event == RAG_FILE_REMOVED && r->calls++ == 0) {
        CHECK(rag_remove_file(r->other, r->victim, NULL) == RAG_OK);
        CHECK(rag_add_file(r->other, r->outsider, NULL) == RAG_OK);
    }
    return 0;
}

static void test_removal_with_reused_ids(void) {
    char *dir = dup_string(path_of(tmp, "many"));
    char name[32];
    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof(name), "f%03d.txt", i);
        write_file(path_of(dir, name), "A small note.");
    }
    char *outsider = dup_string(path_of(tmp, "outsider.txt"));
    write_file(outsider, "Not in the directory.");
    rag_t *rag = open_index("many.db", (rag_options_t){0});
    CHECK(rag_add_dir(rag, dir, NULL) == RAG_OK);
    rag_close(rag);

    reuse_t r = {open_index("many.db", (rag_options_t){0}), NULL, outsider, 0};
    r.victim = path_of(dir, "f299.txt");
    rag = open_index("many.db", (rag_options_t){.progress = reuse_id, .progress_data = &r});
    rag_index_stats_t stats;
    CHECK(rag_remove_file(rag, dir, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 299);
    /* the file that took the id of f299.txt is still there */
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 1);
    rag_results_t *found = NULL;
    CHECK(rag_search(rag, "directory", NULL, &found) == RAG_OK);
    CHECK(found && found->count == 1 && strcmp(found->items[0].path, outsider) == 0);
    rag_results_free(found);
    rag_close(rag);
    rag_close(r.other);
    free(outsider);
    free(dir);
}

typedef struct vanish {
    rag_t *other;
    const char *file;
    int calls;
    int failed;
} vanish_t;

/* While the first handle is between two batches, the other one removes the file being written. */
static int remove_meanwhile(const rag_progress_t *p, void *data) {
    vanish_t *v = data;
    if (p->event == RAG_FILE_INDEXED && v->calls++ == 0) CHECK(rag_remove_file(v->other, v->file, NULL) == RAG_OK);
    if (p->event == RAG_FILE_FAILED) v->failed++;
    return 0;
}

static void test_file_removed_while_indexed(void) {
    char *dir = dup_string(path_of(tmp, "vanish"));
    write_file(path_of(dir, "a.txt"), "Alpha.");
    string_t text = {0};
    for (int i = 0; i < 12; i++) string_append(&text, "A sentence of the long file. ");
    write_file(path_of(dir, "f.txt"), text.data);
    free(text.data);

    vanish_t v = {open_index("vanish.db", (rag_options_t){.chunk_size = 40}), NULL, 0, 0};
    v.file = path_of(dir, "f.txt");
    rag_t *rag = open_index(
        "vanish.db",
        (rag_options_t){.chunk_size = 40, .batch_size = 2, .progress = remove_meanwhile, .progress_data = &v});
    rag_index_stats_t stats;
    CHECK(rag_add_dir(rag, dir, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK_SIZE(stats.files_failed, 1);
    CHECK(v.failed == 1);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 1);
    CHECK_SIZE(info.chunks, 1);
    rag_close(rag);
    rag_close(v.other);
    free(dir);
}

static void test_open_fails_without_changes(void) {
    rag_t *rag = open_index("unchanged.db", (rag_options_t){.embed_model = "model-a"});
    rag_close(rag);
    rag = NULL;
    rag_options_t bad = {.embed_model = "model-b", .batch_size = RAG_MAX_BATCH_SIZE + 1};
    CHECK(rag_open(path_of(tmp, "unchanged.db"), &bad, &rag) == RAG_ERROR_INVALID);
    rag_close(rag);
    rag_options_t bad_overlap = {.embed_model = "model-b", .chunk_size = 100, .chunk_overlap = 100};
    CHECK(rag_open(path_of(tmp, "unchanged.db"), &bad_overlap, &rag) == RAG_ERROR_INVALID);
    rag_close(rag);
    /* without a model option, the stored one is used: still model-a */
    CHECK(rag_open(path_of(tmp, "unchanged.db"), NULL, &rag) == RAG_OK);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_STR(info.embed_model, "model-a");
    rag_close(rag);

    CHECK(rag_open(path_of(tmp, "never.db"), &bad, &rag) == RAG_ERROR_INVALID);
    rag_close(rag);
    CHECK(!file_exists(path_of(tmp, "never.db")));
}

/*
 * A file renamed in another case is still there under its old name where
 * names do not depend on case (Windows): it is removed all the same.
 */
static void test_renamed_in_case(void) {
    char *dir = dup_string(path_of(tmp, "case"));
    write_file(path_of(dir, "Fruit.md"), "Bananas are yellow.");
    write_file(path_of(dir, "Other.md"), "Apples are red.");
    rag_t *rag = open_index("case.db", (rag_options_t){0});
    rag_index_stats_t stats;
    CHECK(rag_add_dir(rag, dir, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 2);

    /* like a rename, which Wine does not do when only the case changes */
    CHECK(remove_file(path_of(dir, "Fruit.md")));
    write_file(path_of(dir, "fruit.md"), "Bananas are yellow.");
    CHECK(rag_add_dir(rag, dir, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK_SIZE(stats.files_unchanged, 1);
    CHECK_SIZE(stats.files_removed, 1);
    rag_stats_t info;
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 2);
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "bananas yellow", NULL, &r) == RAG_OK);
    CHECK(r && r->count == 2);
    if (r && r->count > 0) CHECK_STR(r->items[0].path, path_of(dir, "fruit.md"));
    rag_results_free(r);

    /* indexing the file again finds it up to date */
    CHECK(rag_add_file(rag, path_of(dir, "fruit.md"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_unchanged, 1);

#ifdef _WIN32
    /* a directory renamed in another case, indexed by its new name */
    char *notes = dup_string(path_of(dir, "Notes"));
    write_file(path_of(notes, "a.md"), "Cherries are red.");
    CHECK(rag_add_dir(rag, notes, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    remove_tree(notes);
    write_file(path_of(dir, "notes/a.md"), "Cherries are red.");
    CHECK(rag_add_dir(rag, path_of(dir, "notes"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_indexed, 1);
    CHECK_SIZE(stats.files_removed, 1);
    CHECK(rag_stats(rag, &info) == RAG_OK);
    CHECK_SIZE(info.files, 3);
    /* and a deleted file removed by another spelling */
    remove_file(path_of(dir, "notes/a.md"));
    CHECK(rag_remove_file(rag, path_of(dir, "NOTES/A.MD"), &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 1);
    free(notes);
#endif
    rag_close(rag);
    free(dir);
}

/* A host that Ollama-C rejects, like the Python client, is an invalid argument. */
static void test_invalid_host(void) {
    rag_t *rag = open_index("invalid-host.db", (rag_options_t){.host = "http://[::1"});
    rag_index_stats_t stats;
    CHECK(rag_add_file(rag, path_of(docs, "fruit.md"), &stats) == RAG_ERROR_INVALID);
    CHECK(strstr(rag_last_error(rag)->message, "invalid Ollama host 'http://[::1'") != NULL);
    rag_close(rag);
}

static void test_empty_index(void) {
    rag_t *rag = open_index("empty.db", (rag_options_t){0});
    rag_results_t *r = NULL;
    CHECK(rag_search(rag, "anything", NULL, &r) == RAG_OK);
    CHECK(r && r->count == 0);
    rag_results_free(r);
    rag_close(rag);
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    tmp = make_temp_dir();
    docs = dup_string(path_of(tmp, "docs"));
    make_documents();

    RUN(test_index_dir);
    RUN(test_search);
    RUN(test_incremental);
    RUN(test_settings_change);
    RUN(test_model_mismatch);
    RUN(test_prefixes);
    RUN(test_ollama_errors);
    RUN(test_interrupted);
    RUN(test_abort);
    RUN(test_ask);
    RUN(test_same_file_twice);
    RUN(test_models_of_two_handles);
    RUN(test_emptied_index);
    RUN(test_progress_order);
    RUN(test_stop_in_batch);
    RUN(test_large_values);
    RUN(test_stale_handle);
    RUN(test_removal_with_reused_ids);
    RUN(test_file_removed_while_indexed);
    RUN(test_open_fails_without_changes);
    RUN(test_renamed_in_case);
    RUN(test_invalid_host);
    RUN(test_empty_index);

    remove_tree(tmp);
    free(docs);
    free(tmp);
    curl_global_cleanup();
    return test_report();
}
