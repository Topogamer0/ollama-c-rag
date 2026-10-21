/* Unit tests of the database: schema, settings, embedding model checks, removal. */
#define _XOPEN_SOURCE 700

#include "internal.h"
#include "sqlite3.h"
#include "test.h"
#include "test_files.h"

static char *tmp;

static rag_t *open_ok(const char *path, const rag_options_t *options) {
    rag_t *rag = NULL;
    rag_status_t st = rag_open(path, options, &rag);
    if (st != RAG_OK) fprintf(stderr, "  rag_open: %s\n", rag_last_error(rag)->message);
    CHECK(st == RAG_OK);
    return rag;
}

/* Opens a database expecting an error, and returns its message (static copy). */
static const char *open_fails(const char *path, const rag_options_t *options, rag_status_t expected) {
    static char message[1024];
    rag_t *rag = NULL;
    rag_status_t st = rag_open(path, options, &rag);
    CHECK(st == expected);
    CHECK(rag != NULL);
    snprintf(message, sizeof(message), "%s", rag_last_error(rag)->message);
    CHECK(rag_last_error(rag)->code == expected);
    /* a failed handle refuses to work */
    rag_stats_t stats;
    CHECK(rag_stats(rag, &stats) == RAG_ERROR_INVALID);
    rag_close(rag);
    return message;
}

/* Adds a file with `chunks` chunks of `dims` dimensions, as the indexer would. */
static void add_fake_file(rag_t *rag, const char *path, size_t chunks, size_t dims) {
    CHECK(rag_db_begin(rag) == RAG_OK);
    db_file_t f = {0, (char *)path, "hash", "settings", 10, 20, 30};
    CHECK(rag_db_put_file(rag, &f) == RAG_OK);
    CHECK(f.id > 0);
    float v[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < chunks; i++) {
        chunk_span_t span = {i * 10, i * 10 + 5, i + 1, i + 1};
        CHECK(rag_db_add_chunk(rag, f.id, i, &span, "chunk", 5, v, dims) == RAG_OK);
    }
    CHECK(rag_db_set_meta_int(rag, "dimensions", (int64_t)dims) == RAG_OK);
    CHECK(rag_db_commit(rag) == RAG_OK);
    rag->stored_dimensions = dims;
}

static void test_create(void) {
    const char *path = path_of(tmp, "create.db");
    rag_t *rag = open_ok(path, NULL);
    rag_stats_t stats;
    CHECK(rag_stats(rag, &stats) == RAG_OK);
    CHECK_STR(stats.path, path);
    CHECK_STR(stats.embed_model, RAG_DEFAULT_EMBED_MODEL);
    CHECK_SIZE(stats.dimensions, 0);
    CHECK_SIZE(stats.files, 0);
    CHECK_SIZE(stats.chunks, 0);
    CHECK_SIZE(stats.chunk_size, 800);
    CHECK_SIZE(stats.chunk_overlap, 100);
    CHECK(stats.database_bytes > 0);
    CHECK(stats.last_indexed == 0);
    CHECK(rag_last_error(rag)->code == RAG_OK);
    CHECK_STR(rag_last_error(rag)->message, "");
    int64_t version = 0;
    bool found = false;
    CHECK(rag_db_get_meta_int(rag, "schema_version", &version, &found) == RAG_OK);
    CHECK(found && version == 1);
    rag_close(rag);
    CHECK(file_exists(path));

    /* in memory */
    rag = open_ok(":memory:", NULL);
    CHECK(rag_stats(rag, &stats) == RAG_OK);
    rag_close(rag);
}

static void test_must_exist(void) {
    const char *path = path_of(tmp, "missing.db");
    rag_options_t options = {.must_exist = true};
    const char *message = open_fails(path, &options, RAG_ERROR_NOT_FOUND);
    CHECK(strstr(message, "missing.db") != NULL);
    CHECK(!file_exists(path));
}

static void test_not_a_database(void) {
    const char *path = path_of(tmp, "garbage.db");
    write_file(path, "this is not a database, but it is long enough to look like one....................."
                     "................................................................................");
    const char *message = open_fails(path, NULL, RAG_ERROR_DATABASE);
    CHECK(strstr(message, "not a database") != NULL);

    /* a SQLite database of another program */
    path = path_of(tmp, "other.db");
    sqlite3 *db;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE notes (text TEXT)", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    message = open_fails(path, NULL, RAG_ERROR_DATABASE);
    CHECK(strstr(message, "not an ollama-rag database") != NULL);

    /* a database of a newer version */
    path = path_of(tmp, "newer.db");
    rag_t *rag = open_ok(path, NULL);
    CHECK(rag_db_set_meta_int(rag, "schema_version", 99) == RAG_OK);
    rag_close(rag);
    message = open_fails(path, NULL, RAG_ERROR_DATABASE);
    CHECK(strstr(message, "newer version") != NULL);
}

static void test_embedding_model(void) {
    const char *path = path_of(tmp, "model.db");
    rag_options_t nomic = {.embed_model = "nomic-embed-text"};
    rag_options_t gemma = {.embed_model = "embeddinggemma"};

    /* the model can change as long as nothing has been embedded */
    rag_t *rag = open_ok(path, &gemma);
    CHECK_STR(rag->embed_model, "embeddinggemma");
    rag_close(rag);
    rag = open_ok(path, &nomic);
    CHECK_STR(rag->embed_model, "nomic-embed-text");
    rag_close(rag);

    /* without a model, the database's one is used */
    rag = open_ok(path, NULL);
    CHECK_STR(rag->embed_model, "nomic-embed-text");
    CHECK_STR(rag->query_prefix, "search_query: ");
    CHECK_STR(rag->document_prefix, "search_document: ");
    add_fake_file(rag, path_of(tmp, "doc.md"), 3, 4);
    rag_close(rag);

    /* then it cannot */
    const char *message = open_fails(path, &gemma, RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(message, "nomic-embed-text") != NULL);
    CHECK(strstr(message, "embeddinggemma") != NULL);
    rag_options_t tagged = {.embed_model = "nomic-embed-text:v1.5"};
    open_fails(path, &tagged, RAG_ERROR_MODEL_MISMATCH);

    /* the same model with another spelling */
    rag_options_t latest = {.embed_model = "nomic-embed-text:latest"};
    rag = open_ok(path, &latest);
    rag_stats_t stats;
    CHECK(rag_stats(rag, &stats) == RAG_OK);
    CHECK_SIZE(stats.files, 1);
    CHECK_SIZE(stats.chunks, 3);
    CHECK_SIZE(stats.dimensions, 4);
    CHECK(stats.text_bytes == 15);
    CHECK(stats.last_indexed == 30);
    rag_close(rag);
    rag_options_t upper = {.embed_model = "NOMIC-EMBED-TEXT"};
    rag_close(open_ok(path, &upper));

    /* the size of the embeddings cannot change either */
    rag_options_t eight = {.dimensions = 8};
    message = open_fails(path, &eight, RAG_ERROR_MODEL_MISMATCH);
    CHECK(strstr(message, "4 dimensions") != NULL);
    rag_options_t four = {.dimensions = 4};
    rag_close(open_ok(path, &four));
    rag_options_t negative = {.dimensions = -1};
    open_fails(path, &negative, RAG_ERROR_INVALID);
}

static void test_same_model(void) {
    CHECK(rag_same_model("a", "a"));
    CHECK(rag_same_model("a", "a:latest"));
    CHECK(rag_same_model("A:Latest", "a"));
    CHECK(rag_same_model("ns/a:latest", "ns/a"));
    CHECK(rag_same_model("host:5000/ns/a", "host:5000/ns/a:latest"));
    CHECK(!rag_same_model("a:v1", "a"));
    CHECK(!rag_same_model("a", "ab"));
    CHECK(!rag_same_model("ns/a", "a"));
}

static void test_settings(void) {
    const char *path = path_of(tmp, "settings.db");
    rag_t *rag = open_ok(path, NULL);
    CHECK_SIZE(rag->chunk_size, 800);
    CHECK_SIZE(rag->chunk_overlap, 100);
    CHECK_SIZE(rag->batch_size, RAG_DEFAULT_BATCH_SIZE);
    CHECK_STR(rag->chat_model, RAG_DEFAULT_CHAT_MODEL);
    CHECK_STR(rag->query_prefix, "task: search result | query: ");
    CHECK_STR(rag->document_prefix, "title: none | text: ");
    CHECK(rag->host == NULL);
    /* the indexer saves the settings it used */
    CHECK(rag_db_set_meta_int(rag, "chunk_size", 300) == RAG_OK);
    CHECK(rag_db_set_meta_int(rag, "chunk_overlap", 30) == RAG_OK);
    CHECK(rag_db_set_meta(rag, "query_prefix", "Q: ") == RAG_OK);
    rag_close(rag);

    /* stored settings are the defaults... */
    rag = open_ok(path, NULL);
    CHECK_SIZE(rag->chunk_size, 300);
    CHECK_SIZE(rag->chunk_overlap, 30);
    CHECK_STR(rag->query_prefix, "Q: ");
    rag_close(rag);

    /* ...that options override */
    rag_options_t o = {.chunk_size = 500,
                       .chunk_overlap = 50,
                       .query_prefix = "",
                       .chat_model = "llama3.2",
                       .host = "http://example.com:1234",
                       .batch_size = 7};
    rag = open_ok(path, &o);
    CHECK_SIZE(rag->chunk_size, 500);
    CHECK_SIZE(rag->chunk_overlap, 50);
    CHECK_STR(rag->query_prefix, "");
    CHECK_STR(rag->chat_model, "llama3.2");
    CHECK_STR(rag->host, "http://example.com:1234");
    CHECK_SIZE(rag->batch_size, 7);
    rag_close(rag);

    rag_options_t none = {.chunk_overlap = RAG_NO_OVERLAP};
    rag = open_ok(path, &none);
    CHECK_SIZE(rag->chunk_overlap, 0);
    rag_close(rag);

    /* a stored overlap too large for a smaller chunk size falls back to a quarter of it */
    rag_options_t small = {.chunk_size = 20};
    rag = open_ok(path, &small);
    CHECK_SIZE(rag->chunk_overlap, 5);
    rag_close(rag);

    rag_options_t bad = {.chunk_size = 100, .chunk_overlap = 100};
    const char *message = open_fails(path, &bad, RAG_ERROR_INVALID);
    CHECK(strstr(message, "overlap") != NULL);

    /* the default overlap is at most a quarter of the chunk size */
    rag = open_ok(":memory:", &(rag_options_t){.chunk_size = 200});
    CHECK_SIZE(rag->chunk_overlap, 50);
    rag_close(rag);

    /* unknown models have no prefixes */
    rag = open_ok(":memory:", &(rag_options_t){.embed_model = "my-model"});
    CHECK_STR(rag->query_prefix, "");
    CHECK_STR(rag->document_prefix, "");
    rag_close(rag);
    rag = open_ok(":memory:", &(rag_options_t){.embed_model = "library/embeddinggemma:300m"});
    CHECK_STR(rag->query_prefix, "task: search result | query: ");
    rag_close(rag);
}

typedef struct removed {
    int count;
    int stop_at;
    char last[4096];
} removed_t;

static int on_progress(const rag_progress_t *p, void *data) {
    removed_t *r = data;
    CHECK(p->event == RAG_FILE_REMOVED);
    r->count++;
    snprintf(r->last, sizeof(r->last), "%s", p->path);
    return r->count == r->stop_at;
}

static void test_remove(void) {
    const char *path = path_of(tmp, "remove.db");
    const char *docs = path_of(tmp, "docs");
    char *a = rag_strdup(path_of(docs, "a.md")), *b = rag_strdup(path_of(docs, "sub/b.md"));
    char *c = rag_strdup(path_of(tmp, "docsx/c.md")), *d = rag_strdup(path_of(tmp, "docs.md"));
    removed_t removed = {0, 0, ""};
    rag_options_t o = {.progress = on_progress, .progress_data = &removed};
    rag_t *rag = open_ok(path, &o);
    add_fake_file(rag, a, 2, 4);
    add_fake_file(rag, b, 3, 4);
    add_fake_file(rag, c, 1, 4);
    add_fake_file(rag, d, 1, 4);

    /* a callback can stop the removal: nothing is removed */
    removed.stop_at = 1;
    rag_index_stats_t stats;
    CHECK(rag_remove_file(rag, docs, &stats) == RAG_ERROR_ABORTED);
    CHECK_SIZE(stats.files_removed, 0);
    db_counts_t counts;
    CHECK(rag_db_counts(rag, &counts) == RAG_OK);
    CHECK_SIZE(counts.files, 4);
    CHECK_SIZE(counts.vectors, 7);

    /* a directory removes everything below it, and only that */
    removed.count = 0;
    removed.stop_at = 0;
    CHECK(rag_remove_file(rag, docs, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 2);
    CHECK_SIZE(stats.chunks_removed, 5);
    CHECK(removed.count == 2);
    CHECK_STR(removed.last, b);
    CHECK(rag_db_counts(rag, &counts) == RAG_OK);
    CHECK_SIZE(counts.files, 2);
    CHECK_SIZE(counts.chunks, 2);
    CHECK_SIZE(counts.vectors, 2);

    /* nothing left there */
    CHECK(rag_remove_file(rag, docs, &stats) == RAG_ERROR_NOT_FOUND);
    CHECK(strstr(rag_last_error(rag)->message, "not in the index") != NULL);
    CHECK_SIZE(stats.files_removed, 0);

    /* a file that does not exist anymore, by a relative path */
    char *cwd = getcwd(NULL, 0);
    CHECK(chdir(tmp) == 0);
    CHECK(rag_remove_file(rag, "docsx/../docsx/c.md", NULL) == RAG_OK);
    CHECK(chdir(cwd) == 0);
    free(cwd);
    CHECK(rag_remove_file(rag, d, &stats) == RAG_OK);
    CHECK_SIZE(stats.files_removed, 1);
    CHECK(rag_db_counts(rag, &counts) == RAG_OK);
    CHECK_SIZE(counts.files, 0);
    CHECK_SIZE(counts.vectors, 0);
    CHECK(rag_last_error(rag)->code == RAG_OK);

    CHECK(rag_remove_file(rag, NULL, NULL) == RAG_ERROR_INVALID);
    CHECK(rag_remove_file(rag, "", NULL) == RAG_ERROR_INVALID);
    rag_close(rag);
    free(a);
    free(b);
    free(c);
    free(d);
}

static void test_replace_file(void) {
    rag_t *rag = open_ok(":memory:", NULL);
    const char *path = "/notes/a.md";
    add_fake_file(rag, path, 3, 4);
    db_file_t first = {0};
    bool found = false;
    CHECK(rag_db_find_file(rag, path, &first, &found) == RAG_OK);
    CHECK(found);

    /* putting the file again replaces its chunks and keeps its id */
    add_fake_file(rag, path, 1, 4);
    db_file_t second = {0};
    CHECK(rag_db_find_file(rag, path, &second, &found) == RAG_OK);
    CHECK(found && second.id == first.id);
    CHECK_STR(second.hash, "hash");
    CHECK(second.size == 10 && second.mtime == 20 && second.indexed_at == 30);
    db_counts_t counts;
    CHECK(rag_db_counts(rag, &counts) == RAG_OK);
    CHECK_SIZE(counts.files, 1);
    CHECK_SIZE(counts.chunks, 1);
    CHECK_SIZE(counts.vectors, 1);
    rag_db_file_release(&first);
    rag_db_file_release(&second);

    CHECK(rag_db_find_file(rag, "/notes/b.md", &second, &found) == RAG_OK);
    CHECK(!found);

    /* "/notes" covers "/notes/a.md" but not "/notes2/x.md"; "/" covers everything */
    add_fake_file(rag, "/notes2/x.md", 1, 4);
    db_file_list_t list = {0};
    CHECK(rag_db_files_under(rag, "/notes", &list) == RAG_OK);
    CHECK_SIZE(list.count, 1);
    rag_db_file_list_free(&list);
    CHECK(rag_db_files_under(rag, "/", &list) == RAG_OK);
    CHECK_SIZE(list.count, 2);
    rag_db_file_list_free(&list);
    CHECK(rag_db_files_under(rag, "/notes/a.md", &list) == RAG_OK);
    CHECK_SIZE(list.count, 1);
    rag_db_file_list_free(&list);
    rag_close(rag);
}

static void test_errors(void) {
    CHECK(rag_last_error(NULL)->code == RAG_ERROR_MEMORY);
    CHECK(rag_open("x.db", NULL, NULL) == RAG_ERROR_INVALID);
    rag_t *rag = NULL;
    CHECK(rag_open(NULL, NULL, &rag) == RAG_ERROR_INVALID);
    CHECK(rag != NULL);
    CHECK(strstr(rag_last_error(rag)->message, "path") != NULL);
    rag_close(rag);
    rag_close(NULL);
    CHECK(rag_stats(NULL, NULL) == RAG_ERROR_INVALID);
    CHECK_STR(rag_status_string(RAG_ERROR_MODEL_MISMATCH), "embedding model mismatch");
    CHECK_STR(rag_version(), RAG_VERSION);
}

int main(void) {
    tmp = make_temp_dir();
    RUN(test_create);
    RUN(test_must_exist);
    RUN(test_not_a_database);
    RUN(test_embedding_model);
    RUN(test_same_model);
    RUN(test_settings);
    RUN(test_remove);
    RUN(test_replace_file);
    RUN(test_errors);
    remove_tree(tmp);
    free(tmp);
    return test_report();
}
