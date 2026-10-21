/* ragc: index your documents and ask questions about them, with Ollama. */
#include <ollama_rag.h>

#include "compat.h"

#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define DEFAULT_DB "ragc.db"

#define STRING(x) #x
#define NUMBER(x) STRING(x)

static const char USAGE[] =
    "usage: ragc [options] <command> [arguments]\n"
    "\n"
    "Ask questions about your own documents, with Ollama, locally.\n"
    "\n"
    "commands:\n"
    "  index <path>...     index files and directories (.txt and .md files); only\n"
    "                      changes are processed, deleted files are removed\n"
    "  search <question>   show the passages most similar to the question\n"
    "  ask <question>      answer the question from the documents, citing them\n"
    "  stats               show what the database contains\n"
    "  remove <path>...    remove files, or directories, from the index\n"
    "\n"
    "options:\n"
    "  -d, --db PATH           database file (default: $RAGC_DB, or " DEFAULT_DB ")\n"
    "  -e, --embed-model NAME  embedding model (default: $RAGC_EMBED_MODEL, the\n"
    "                          database's, or " RAG_DEFAULT_EMBED_MODEL ")\n"
    "  -m, --chat-model NAME   chat model (default: $RAGC_CHAT_MODEL, or " RAG_DEFAULT_CHAT_MODEL ")\n"
    "  -k, --top-k N           passages to use (default: 5)\n"
    "  -s, --min-score X       ignore passages with a similarity below X (-1 to 1)\n"
    "  -H, --host URL          Ollama server (default: $OLLAMA_HOST, or\n"
    "                          http://127.0.0.1:11434)\n"
    "      --chunk-size N      characters per chunk (default: the database's, or 800)\n"
    "      --chunk-overlap N   characters shared by consecutive chunks (default: the\n"
    "                          database's, or 100, at most a quarter of a chunk)\n"
    "      --batch-size N      chunks per embedding request (default: 32, at most 65536)\n"
    "      --dimensions N      size of the embeddings, for models that can shorten them\n"
    "      --system TEXT       system prompt of the chat model\n"
    "      --timeout SECONDS   network timeout (default: none)\n"
    "  -q, --quiet             print only the results: no progress, no sources\n"
    "  -h, --help              show this help\n"
    "  -V, --version           show the version\n"
    "\n"
    "Options go before the question of search and ask, which is the rest of the line:\n"
    "  ragc -k 3 ask what did I plan for the holidays?\n";

typedef struct cli {
    const char *db;
    rag_options_t options;
    size_t top_k;
    double min_score;
    const char *system;
    bool quiet;
    char *cwd;    /* paths below it are shown relative to it */
    char *env[3]; /* RAGC_DB, RAGC_EMBED_MODEL, RAGC_CHAT_MODEL */
    const char *command;
    char **args;
    int nargs;
} cli_t;

enum {
    OPT_DB,
    OPT_EMBED_MODEL,
    OPT_CHAT_MODEL,
    OPT_TOP_K,
    OPT_MIN_SCORE,
    OPT_HOST,
    OPT_CHUNK_SIZE,
    OPT_CHUNK_OVERLAP,
    OPT_BATCH_SIZE,
    OPT_DIMENSIONS,
    OPT_SYSTEM,
    OPT_TIMEOUT,
    OPT_QUIET,
    OPT_HELP,
    OPT_VERSION
};

static const struct {
    char short_name;
    const char *long_name;
    bool has_value;
    int id;
} OPTIONS[] = {
    {'d', "db", true, OPT_DB},
    {'e', "embed-model", true, OPT_EMBED_MODEL},
    {'m', "chat-model", true, OPT_CHAT_MODEL},
    {'k', "top-k", true, OPT_TOP_K},
    {'s', "min-score", true, OPT_MIN_SCORE},
    {'H', "host", true, OPT_HOST},
    {0, "chunk-size", true, OPT_CHUNK_SIZE},
    {0, "chunk-overlap", true, OPT_CHUNK_OVERLAP},
    {0, "batch-size", true, OPT_BATCH_SIZE},
    {0, "dimensions", true, OPT_DIMENSIONS},
    {0, "system", true, OPT_SYSTEM},
    {0, "timeout", true, OPT_TIMEOUT},
    {'q', "quiet", false, OPT_QUIET},
    {'h', "help", false, OPT_HELP},
    {'V', "version", false, OPT_VERSION},
};

/* ---- output ---- */

/* True if `path` is below the directory `dir` (of length n > 1, not the root "/"). */
static bool is_below(const char *path, const char *dir, size_t n) {
#ifdef _WIN32
    /* names do not depend on case */
    for (size_t i = 0; i < n; i++) {
        char a = path[i] >= 'A' && path[i] <= 'Z' ? (char)(path[i] - 'A' + 'a') : path[i];
        char b = dir[i] >= 'A' && dir[i] <= 'Z' ? (char)(dir[i] - 'A' + 'a') : dir[i];
        if (a != b) return false;
    }
#else
    if (strncmp(path, dir, n) != 0) return false;
#endif
    return rag_os_is_sep(path[n]);
}

/* `path` relative to the current directory when it is below it. */
static const char *shown(const cli_t *c, const char *path) {
    size_t n = c->cwd ? strlen(c->cwd) : 0;
    if (n > 1 && is_below(path, c->cwd, n)) return path + n + 1;
    return path;
}

/* Prints a message of the library, with the paths below the current directory made relative. */
static void print_message(FILE *f, const cli_t *c, const char *message) {
    size_t n = c->cwd ? strlen(c->cwd) : 0;
    const char *start = message; /* what is not printed yet */
    for (const char *p = message; n > 1 && *p; p++) {
        /* the library quotes the paths of its messages: only the start of a path is made relative */
        if ((p == message || p[-1] == '\'') && is_below(p, c->cwd, n)) {
            fwrite(start, 1, (size_t)(p - start), f);
            p += n; /* the separator */
            start = p + 1;
        }
    }
    fputs(start, f);
}

static void print_error(const cli_t *c, const rag_t *rag) {
    fputs("ragc: ", stderr);
    print_message(stderr, c, rag_last_error(rag)->message);
    fputc('\n', stderr);
}

static void print_lines(const rag_result_t *r) {
    if (r->line_start == r->line_end)
        printf("%zu", r->line_start);
    else
        printf("%zu-%zu", r->line_start, r->line_end);
}

/*
 * Prints a text with every line indented, a line at a time (the console of
 * Windows shows a character cut between two writes as garbage). "\r\n" ends
 * a line too: stdout writes the line ends of the system.
 */
static void print_indented(const char *text, const char *indent) {
    for (const char *line = text;;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 0 && line[len - 1] == '\r') len--;
        if (len > 0) { /* no spaces on empty lines */
            fputs(indent, stdout);
            fwrite(line, 1, len, stdout);
        }
        putchar('\n');
        if (!end) break;
        line = end + 1;
    }
}

static void format_size(char *out, size_t size, uint64_t bytes) {
    if (bytes < 1024)
        snprintf(out, size, "%llu bytes", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, size, "%.1f KB", (double)bytes / 1024);
    else if (bytes < 1024ull * 1024 * 1024)
        snprintf(out, size, "%.1f MB", (double)bytes / (1024 * 1024));
    else
        snprintf(out, size, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
}

static int on_progress(const rag_progress_t *p, void *data) {
    const cli_t *c = data;
    if (c->quiet) return 0;
    switch (p->event) {
    case RAG_FILE_INDEXED:
        fprintf(stderr, "indexed  %s (%zu chunk%s)\n", shown(c, p->path), p->chunks, p->chunks == 1 ? "" : "s");
        break;
    case RAG_FILE_REMOVED: fprintf(stderr, "removed  %s\n", shown(c, p->path)); break;
    case RAG_FILE_FAILED:
        fprintf(stderr, "skipped  %s: ", shown(c, p->path));
        print_message(stderr, c, p->message);
        fputc('\n', stderr);
        break;
    case RAG_FILE_UNCHANGED: break;
    }
    return 0;
}

/* Prints the answer while it comes; stops it if the output cannot be written. */
static int on_token(const char *text, void *data) {
    (void)data;
    return fputs(text, stdout) == EOF || fflush(stdout) == EOF;
}

/* ---- arguments ---- */

static int usage_error(const char *fmt, const char *arg) {
    fputs("ragc: ", stderr);
    fprintf(stderr, fmt, arg);
    fputs("\nTry 'ragc --help' for more information.\n", stderr);
    return 2;
}

static bool parse_size(const char *s, size_t *out) {
    if (!s || !*s || *s == '-' || *s == '+') return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (*end || errno || v > (size_t)-1 / 2) return false;
    *out = (size_t)v;
    return true;
}

/* Returns -1 to go on, else the exit code. */
static int apply_option(cli_t *c, int id, const char *value, const char *name) {
    size_t n = 0;
    switch (id) {
    case OPT_DB:
        if (!*value) return usage_error("option '%s' needs a value", name);
        c->db = value;
        break;
    case OPT_EMBED_MODEL: c->options.embed_model = value; break;
    case OPT_CHAT_MODEL: c->options.chat_model = value; break;
    case OPT_HOST: c->options.host = value; break;
    case OPT_SYSTEM: c->system = value; break;
    case OPT_TOP_K:
        if (!parse_size(value, &c->top_k) || c->top_k == 0) return usage_error("invalid number for %s", name);
        break;
    case OPT_MIN_SCORE: {
        char *end;
        c->min_score = strtod(value, &end);
        if (end == value || *end || !(c->min_score >= -1 && c->min_score <= 1)) /* NaN too */
            return usage_error("%s must be a number between -1 and 1", name);
        /* for the library 0 means no minimum: keep the threshold 0 that was asked */
        if (c->min_score == 0) c->min_score = -DBL_MIN;
        break;
    }
    case OPT_CHUNK_SIZE:
        if (!parse_size(value, &c->options.chunk_size) || c->options.chunk_size == 0)
            return usage_error("invalid number for %s", name);
        break;
    case OPT_CHUNK_OVERLAP:
        if (!parse_size(value, &n)) return usage_error("invalid number for %s", name);
        c->options.chunk_overlap = n ? n : RAG_NO_OVERLAP;
        break;
    case OPT_BATCH_SIZE:
        if (!parse_size(value, &c->options.batch_size) || c->options.batch_size == 0 ||
            c->options.batch_size > RAG_MAX_BATCH_SIZE)
            return usage_error("%s must be a number from 1 to " NUMBER(RAG_MAX_BATCH_SIZE), name);
        break;
    case OPT_DIMENSIONS:
        if (!parse_size(value, &n) || n == 0 || n > 1000000) return usage_error("invalid number for %s", name);
        c->options.dimensions = (int)n;
        break;
    case OPT_TIMEOUT:
        if (!parse_size(value, &n) || n > 1000000) return usage_error("invalid number for %s", name);
        c->options.timeout_ms = (long)n * 1000;
        break;
    case OPT_QUIET: c->quiet = true; break;
    case OPT_HELP: fputs(USAGE, stdout); return 0;
    case OPT_VERSION: printf("ragc %s\n", rag_version()); return 0;
    }
    return -1;
}

/* Parses the command line; returns -1 to go on, else the exit code. */
static int parse_args(cli_t *c, int argc, char **argv) {
    bool options_done = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (options_done || arg[0] != '-' || arg[1] == '\0') {
            c->args[c->nargs++] = argv[i];
            /* the question is the rest of the line, even words like "-40" */
            if (c->nargs == 1 && (strcmp(arg, "ask") == 0 || strcmp(arg, "search") == 0)) {
                options_done = true;
                if (i + 1 < argc && strcmp(argv[i + 1], "--") == 0) i++;
            }
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            options_done = true;
            continue;
        }

        size_t k = sizeof(OPTIONS) / sizeof(OPTIONS[0]);
        const char *value = NULL;
        if (arg[1] == '-') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            for (k = 0; k < sizeof(OPTIONS) / sizeof(OPTIONS[0]); k++)
                if (strlen(OPTIONS[k].long_name) == len && strncmp(OPTIONS[k].long_name, name, len) == 0) break;
            if (eq) value = eq + 1;
        } else {
            for (k = 0; k < sizeof(OPTIONS) / sizeof(OPTIONS[0]); k++)
                if (OPTIONS[k].short_name == arg[1]) break;
            if (arg[2]) value = arg + 2;
        }
        if (k == sizeof(OPTIONS) / sizeof(OPTIONS[0]) || (value && !OPTIONS[k].has_value))
            return usage_error("unknown option '%s'", arg);
        if (OPTIONS[k].has_value && !value) {
            if (i + 1 >= argc) return usage_error("option '%s' needs a value", arg);
            value = argv[++i];
        }
        int code = apply_option(c, OPTIONS[k].id, value, arg);
        if (code >= 0) return code;
    }
    if (c->nargs == 0) {
        fputs(USAGE, stderr);
        return 2;
    }
    c->command = c->args[0];
    c->args++;
    c->nargs--;
    return -1;
}

/* The arguments joined with spaces, so that the question does not need quotes. */
static char *join_args(const cli_t *c) {
    size_t len = 1;
    for (int i = 0; i < c->nargs; i++) len += strlen(c->args[i]) + 1;
    char *s = malloc(len);
    if (!s) return NULL;
    s[0] = '\0';
    for (int i = 0; i < c->nargs; i++) {
        if (i) strcat(s, " ");
        strcat(s, c->args[i]);
    }
    return s;
}

/* ---- commands ---- */

static int cmd_index(cli_t *c, rag_t *rag) {
    rag_index_stats_t total = {0};
    int code = 0;
    for (int i = 0; i < c->nargs; i++) {
        os_stat_t st;
        bool dir = rag_os_stat(c->args[i], true, &st, NULL) && st.type == RAG_OS_DIR;
        rag_index_stats_t s;
        rag_status_t rc = dir ? rag_add_dir(rag, c->args[i], &s) : rag_add_file(rag, c->args[i], &s);
        total.files_indexed += s.files_indexed;
        total.files_unchanged += s.files_unchanged;
        total.files_removed += s.files_removed;
        total.files_failed += s.files_failed;
        total.chunks_added += s.chunks_added;
        if (rc != RAG_OK) {
            print_error(c, rag);
            /* a path that cannot be read only concerns itself: go on with the others */
            if (rc != RAG_ERROR_IO && rc != RAG_ERROR_NOT_FOUND && rc != RAG_ERROR_INVALID) return 1;
            code = 1;
        }
    }
    printf("%zu indexed, %zu unchanged, %zu removed, %zu skipped (%zu chunks added)\n", total.files_indexed,
           total.files_unchanged, total.files_removed, total.files_failed, total.chunks_added);
    return code;
}

static int cmd_search(cli_t *c, rag_t *rag) {
    char *query = join_args(c);
    rag_search_options_t o = {.top_k = c->top_k, .min_score = c->min_score};
    rag_results_t *results = NULL;
    rag_status_t rc = query ? rag_search(rag, query, &o, &results) : RAG_ERROR_MEMORY;
    free(query);
    if (rc != RAG_OK) {
        print_error(c, rag);
        return 1;
    }
    if (results->count == 0) fprintf(stderr, "ragc: nothing found\n");
    for (size_t i = 0; i < results->count; i++) {
        const rag_result_t *r = &results->items[i];
        printf("%s%zu. %s:", i ? "\n" : "", i + 1, shown(c, r->path));
        print_lines(r);
        printf("  (score %.3f)\n", r->score);
        print_indented(r->text, "   ");
    }
    rag_results_free(results);
    return 0;
}

static int cmd_ask(cli_t *c, rag_t *rag) {
    char *question = join_args(c);
    rag_ask_options_t o = {
        .top_k = c->top_k, .min_score = c->min_score, .system_prompt = c->system, .on_token = on_token};
    rag_answer_t *answer = NULL;
    rag_status_t rc = question ? rag_ask(rag, question, &o, &answer) : RAG_ERROR_MEMORY;
    free(question);
    if (rc != RAG_OK) {
        fflush(stdout);
        print_error(c, rag);
        if (rc == RAG_ERROR_EMPTY) fprintf(stderr, "Index some documents first: ragc index <directory>\n");
        return 1;
    }
    size_t len = strlen(answer->text);
    if (len == 0 || answer->text[len - 1] != '\n') putchar('\n');
    if (!c->quiet && answer->sources_count) {
        printf("\nSources:\n");
        for (size_t i = 0; i < answer->sources_count; i++) {
            const rag_result_t *r = &answer->sources[i];
            printf("  [%zu] %s:", i + 1, shown(c, r->path));
            print_lines(r);
            printf(" (score %.2f)\n", r->score);
        }
    }
    rag_answer_free(answer);
    return 0;
}

static int cmd_stats(cli_t *c, rag_t *rag) {
    rag_stats_t s;
    if (rag_stats(rag, &s) != RAG_OK) {
        print_error(c, rag);
        return 1;
    }
    char db_size[32], text_size[32], when[64] = "never";
    format_size(db_size, sizeof(db_size), s.database_bytes);
    format_size(text_size, sizeof(text_size), s.text_bytes);
    time_t t = (time_t)s.last_indexed;
    const struct tm *tm = s.last_indexed ? localtime(&t) : NULL;
    if (tm)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    else if (s.last_indexed)
        snprintf(when, sizeof(when), "%lld (not a valid time)", (long long)s.last_indexed);
    printf("database         %s (%s)\n", s.path, db_size);
    if (s.dimensions)
        printf("embedding model  %s (%zu dimensions)\n", s.embed_model, s.dimensions);
    else
        printf("embedding model  %s\n", s.embed_model);
    printf("files            %zu\n", s.files);
    printf("chunks           %zu (%s of text)\n", s.chunks, text_size);
    printf("chunking         %zu characters, %zu shared\n", s.chunk_size, s.chunk_overlap);
    printf("last indexed     %s\n", when);
    return 0;
}

static int cmd_remove(cli_t *c, rag_t *rag) {
    int code = 0;
    size_t files = 0, chunks = 0;
    for (int i = 0; i < c->nargs; i++) {
        rag_index_stats_t s;
        if (rag_remove_file(rag, c->args[i], &s) != RAG_OK) {
            print_error(c, rag);
            code = 1;
        }
        files += s.files_removed;
        chunks += s.chunks_removed;
    }
    printf("%zu file%s removed (%zu chunks)\n", files, files == 1 ? "" : "s", chunks);
    return code;
}

/* The current directory, with its links resolved like the paths of the library. */
static char *current_directory(void) {
    char *cwd = rag_os_getcwd(NULL);
    char *real = cwd ? rag_os_realpath(cwd, NULL) : NULL;
    if (!real) return cwd;
    free(cwd);
    return real;
}

static int run_cli(cli_t c, int argc, char **argv) {
    c.args = calloc((size_t)argc + 1, sizeof(*c.args));
    if (!c.args) return 1;
    char **args = c.args;
    int code = parse_args(&c, argc, argv);
    if (code >= 0) {
        free(args);
        return code;
    }

    static const struct {
        const char *name;
        int (*run)(cli_t *, rag_t *);
        bool creates;        /* the database is created if it does not exist */
        bool takes_args;     /* stats takes none, the others need some */
        const char *missing; /* the message when they are missing */
    } COMMANDS[] = {
        {"index", cmd_index, true, true, "which files or directories?"},
        {"search", cmd_search, false, true, "what are you looking for?"},
        {"ask", cmd_ask, false, true, "what is your question?"},
        {"stats", cmd_stats, false, false, NULL},
        {"remove", cmd_remove, false, true, "which files or directories?"},
    };
    size_t k = 0;
    while (k < sizeof(COMMANDS) / sizeof(COMMANDS[0]) && strcmp(COMMANDS[k].name, c.command) != 0) k++;
    if (k == sizeof(COMMANDS) / sizeof(COMMANDS[0])) {
        code = usage_error("unknown command '%s'", c.command);
        free(args);
        return code;
    }
    if (COMMANDS[k].takes_args) {
        /* a question of empty words is no question, and an empty path is no path */
        bool blank = true, empty = false;
        for (int i = 0; i < c.nargs; i++) {
            if (c.args[i][0])
                blank = false;
            else
                empty = true;
        }
        bool paths = COMMANDS[k].run == cmd_index || COMMANDS[k].run == cmd_remove;
        if (blank || (paths && empty)) c.nargs = 0;
    }
    if (c.options.chunk_size && c.options.chunk_overlap != RAG_NO_OVERLAP &&
        c.options.chunk_overlap >= c.options.chunk_size) {
        free(args);
        return usage_error("%s", "--chunk-overlap must be smaller than --chunk-size");
    }
    if (COMMANDS[k].takes_args != (c.nargs > 0)) {
        fprintf(stderr, "ragc: %s: %s\n", c.command,
                COMMANDS[k].takes_args ? COMMANDS[k].missing : "no arguments expected");
        fputs("Try 'ragc --help' for more information.\n", stderr);
        free(args);
        return 2;
    }

    c.cwd = current_directory();
    c.options.must_exist = !COMMANDS[k].creates;
    c.options.progress = on_progress;
    c.options.progress_data = &c;
    rag_t *rag = NULL;
    rag_status_t rc = rag_open(c.db, &c.options, &rag);
    if (rc != RAG_OK) {
        print_error(&c, rag);
        if (rc == RAG_ERROR_NOT_FOUND) fprintf(stderr, "Index some documents first: ragc index <directory>\n");
        if (rc == RAG_ERROR_MODEL_MISMATCH) fprintf(stderr, "Choose another database with --db.\n");
        code = 1;
    } else {
        code = COMMANDS[k].run(&c, rag);
    }
    rag_close(rag);
    free(c.cwd);
    free(args);
    return code;
}

/* Runs ragc, with the defaults from the environment. */
static int run(int argc, char **argv) {
    cli_t c = {0};
    c.env[0] = rag_os_getenv("RAGC_DB");
    c.env[1] = rag_os_getenv("RAGC_EMBED_MODEL");
    c.env[2] = rag_os_getenv("RAGC_CHAT_MODEL");
    c.db = c.env[0] && *c.env[0] ? c.env[0] : DEFAULT_DB;
    c.options.embed_model = c.env[1];
    c.options.chat_model = c.env[2];
    int code = run_cli(c, argc, argv);
    for (size_t i = 0; i < sizeof(c.env) / sizeof(c.env[0]); i++) free(c.env[i]);
    return code;
}

/* The exit code, once the output is written: a lost answer is an error, e.g. when the disk is full. */
static int finish(int code) {
    if (fflush(stdout) == EOF || ferror(stdout)) {
        fprintf(stderr, "ragc: cannot write the output: %s\n", strerror(errno ? errno : EIO));
        if (code == 0) code = 1;
    }
    return code;
}

#ifdef _WIN32
static UINT console_page; /* the code page of the console, to restore */

/* Ctrl+C and the like: the console gets its code page back before the process ends. */
static BOOL WINAPI on_console_event(DWORD event) {
    (void)event;
    SetConsoleOutputCP(console_page);
    return FALSE; /* the default handler ends the process */
}

/* The arguments in UTF-16, so that any of them can be passed (main() would get them in the ANSI code page). */
int wmain(int argc, wchar_t **wargv);
int wmain(int argc, wchar_t **wargv) {
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    bool ok = argv != NULL;
    for (int i = 0; ok && i < argc; i++) ok = (argv[i] = rag_os_from_wide(wargv[i])) != NULL;
    int code = 1;
    if (ok) {
        /* the documents are in UTF-8, and so is the output: show it as such in the console */
        console_page = GetConsoleOutputCP();
        bool switched = console_page != 0 && console_page != CP_UTF8 && SetConsoleCtrlHandler(on_console_event, TRUE) &&
                        SetConsoleOutputCP(CP_UTF8);
        code = finish(run(argc, argv));
        if (switched) SetConsoleOutputCP(console_page);
    } else {
        fputs("ragc: out of memory\n", stderr);
    }
    for (int i = 0; argv && i < argc; i++) free(argv[i]);
    free(argv);
    return code;
}
#else
int main(int argc, char **argv) { return finish(run(argc, argv)); }
#endif
