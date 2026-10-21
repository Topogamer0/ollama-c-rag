/* Unit tests of the chunker and of the UTF-8 helpers. */
#include "internal.h"
#include "test.h"

#include <stdlib.h>

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static chunk_list_t split(const char *text, size_t size, size_t overlap) {
    chunk_list_t list = {0};
    CHECK(rag_chunk_split(text, strlen(text), size, overlap, &list));
    return list;
}

/* Copy of a chunk's text, for comparisons. */
static char *chunk_text(const char *text, const chunk_span_t *c) {
    return rag_strndup(text + c->start, c->end - c->start);
}

static char *repeat(const char *s, size_t times) {
    size_t n = strlen(s);
    char *out = malloc(n * times + 1);
    for (size_t i = 0; i < times; i++) memcpy(out + i * n, s, n);
    out[n * times] = '\0';
    return out;
}

/* Checks the properties every split must have. Returns false if one does not hold. */
static bool check_invariants(const char *t, size_t len, size_t size, size_t overlap, const chunk_list_t *l) {
    int failures = test_failures;
    bool *boundary = calloc(len + 1, 1);
    bool *covered = calloc(len + 1, 1);
    for (size_t i = 0; i < len; i += rag_utf8_char_len(t, len, i)) boundary[i] = true;
    boundary[len] = true;

    for (size_t k = 0; k < l->count; k++) {
        const chunk_span_t *c = &l->items[k];
        CHECK(c->start < c->end && c->end <= len);
        if (!(c->start < c->end && c->end <= len)) break;
        CHECK(!is_space(t[c->start]) && !is_space(t[c->end - 1]));
        CHECK(boundary[c->start] && boundary[c->end]);
        CHECK(rag_utf8_count(t, c->start, c->end) <= size);

        size_t lines_before_start = 0, lines_before_end = 0;
        for (size_t i = 0; i < c->start; i++) lines_before_start += t[i] == '\n';
        for (size_t i = 0; i < c->end - 1; i++) lines_before_end += t[i] == '\n';
        CHECK_SIZE(c->line_start, lines_before_start + 1);
        CHECK_SIZE(c->line_end, lines_before_end + 1);

        for (size_t i = c->start; i < c->end; i++) covered[i] = true;
        if (k > 0) {
            const chunk_span_t *p = &l->items[k - 1];
            CHECK(c->start > p->start);
            CHECK(c->end > p->end);
            if (c->start < p->end) CHECK(rag_utf8_count(t, c->start, p->end) <= overlap);
        }
    }
    size_t first = len >= 3 && memcmp(t, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
    for (size_t i = first; i < len; i++)
        if (!is_space(t[i]) && !covered[i]) {
            CHECK(covered[i]); /* every non-space byte belongs to a chunk */
            break;
        }
    free(boundary);
    free(covered);
    return failures == test_failures;
}

static void test_empty(void) {
    chunk_list_t l = split("", 800, 100);
    CHECK_SIZE(l.count, 0);
    rag_chunk_list_free(&l);

    l = split(" \n\n\t \r\n ", 800, 100);
    CHECK_SIZE(l.count, 0);
    rag_chunk_list_free(&l);

    l = split("\xEF\xBB\xBF", 800, 100);
    CHECK_SIZE(l.count, 0);
    rag_chunk_list_free(&l);
}

static void test_short_text(void) {
    const char *text = "Hello world.";
    chunk_list_t l = split(text, 800, 100);
    CHECK_SIZE(l.count, 1);
    if (l.count == 1) {
        CHECK_SIZE(l.items[0].start, 0);
        CHECK_SIZE(l.items[0].end, 12);
        CHECK_SIZE(l.items[0].line_start, 1);
        CHECK_SIZE(l.items[0].line_end, 1);
    }
    rag_chunk_list_free(&l);

    /* surrounding whitespace is not part of the chunk */
    text = "  \n\nHello\nworld\n\n  ";
    l = split(text, 800, 100);
    CHECK_SIZE(l.count, 1);
    if (l.count == 1) {
        char *s = chunk_text(text, &l.items[0]);
        CHECK_STR(s, "Hello\nworld");
        free(s);
        CHECK_SIZE(l.items[0].line_start, 3);
        CHECK_SIZE(l.items[0].line_end, 4);
    }
    rag_chunk_list_free(&l);

    /* exactly `size` characters fit in one chunk */
    l = split("abcdefghij", 10, 3);
    CHECK_SIZE(l.count, 1);
    rag_chunk_list_free(&l);
}

static void test_byte_order_mark(void) {
    const char *text = "\xEF\xBB\xBFHello";
    chunk_list_t l = split(text, 800, 100);
    CHECK_SIZE(l.count, 1);
    if (l.count == 1) {
        CHECK_SIZE(l.items[0].start, 3);
        CHECK_SIZE(l.items[0].end, 8);
    }
    rag_chunk_list_free(&l);
}

static void test_paragraphs(void) {
    char *p1 = repeat("Lorem ipsum dolor sit amet. ", 18);
    p1[strlen(p1) - 1] = '\0';
    char *p2 = repeat("Sed do eiusmod tempor incididunt. ", 15);
    char *text = rag_format("%s\n\n%s", p1, p2);
    size_t len = strlen(text);

    chunk_list_t l = split(text, 800, 100);
    CHECK(l.count >= 2);
    if (l.count >= 2) {
        /* the first chunk is exactly the first paragraph */
        CHECK_SIZE(l.items[0].start, 0);
        CHECK_SIZE(l.items[0].end, strlen(p1));
        /* the second one starts with a word of the end of the first paragraph */
        CHECK(l.items[1].start < strlen(p1));
        CHECK(l.items[1].start + 100 >= strlen(p1));
        CHECK(text[l.items[1].start - 1] == ' ');
    }
    check_invariants(text, len, 800, 100, &l);
    rag_chunk_list_free(&l);

    /* CRLF line endings */
    const char *crlf = "first paragraph text.\r\n\r\nsecond paragraph text.";
    l = split(crlf, 30, 0);
    CHECK_SIZE(l.count, 2);
    if (l.count == 2) {
        char *a = chunk_text(crlf, &l.items[0]), *b = chunk_text(crlf, &l.items[1]);
        CHECK_STR(a, "first paragraph text.");
        CHECK_STR(b, "second paragraph text.");
        CHECK_SIZE(l.items[1].line_start, 3);
        free(a);
        free(b);
    }
    rag_chunk_list_free(&l);

    /* a Markdown heading starts a new chunk */
    const char *md = "Intro text here and more.\n# Title\nBody text follows here.";
    l = split(md, 40, 0);
    CHECK(l.count >= 2);
    if (l.count >= 2) {
        char *a = chunk_text(md, &l.items[0]), *b = chunk_text(md, &l.items[1]);
        CHECK_STR(a, "Intro text here and more.");
        CHECK(strncmp(b, "# Title", 7) == 0);
        free(a);
        free(b);
    }
    rag_chunk_list_free(&l);

    free(p1);
    free(p2);
    free(text);
}

static void test_markdown(void) {
    /* a heading starts a new chunk... */
    const char *text = "alpha beta gamma delta\n# Title\nepsilon zeta eta theta iota";
    chunk_list_t l = split(text, 40, 0);
    CHECK(l.count >= 1);
    if (l.count >= 1) CHECK_SIZE(l.items[0].end, 22);
    rag_chunk_list_free(&l);

    /* ...but a comment in a code block or a #tag are not headings */
    text = "```\nalpha beta gamma delta\n# Title\nepsilon zeta eta theta iota\n```";
    l = split(text, 40, 0);
    CHECK(l.count >= 1);
    if (l.count >= 1) CHECK_SIZE(l.items[0].end, 34);
    rag_chunk_list_free(&l);

    text = "alpha beta gamma delta\n#tag\nepsilon zeta eta theta iota";
    l = split(text, 40, 0);
    CHECK(l.count >= 1);
    if (l.count >= 1) CHECK_SIZE(l.items[0].end, 27);
    rag_chunk_list_free(&l);

    /* after the end of the block, headings count again */
    text = "~~~~\n# comment\n~~~~\nalpha beta gamma\n## Next\nepsilon zeta eta theta iota";
    l = split(text, 50, 0);
    CHECK(l.count >= 1);
    if (l.count >= 1) CHECK_SIZE(l.items[0].end, 36);
    rag_chunk_list_free(&l);

    /* an unterminated block runs to the end of the text */
    text = "```\nalpha beta gamma delta\n# Title\nepsilon zeta eta theta iota";
    l = split(text, 40, 0);
    CHECK(l.count >= 1);
    if (l.count >= 1) CHECK_SIZE(l.items[0].end, 34);
    rag_chunk_list_free(&l);
}

static void test_sentences(void) {
    char *text = repeat("The quick brown fox jumps over the lazy dog again. ", 40);
    size_t len = strlen(text);
    chunk_list_t l = split(text, 200, 0);
    CHECK(l.count > 5);
    for (size_t k = 0; k + 1 < l.count; k++) CHECK(text[l.items[k].end - 1] == '.');
    check_invariants(text, len, 200, 0, &l);
    rag_chunk_list_free(&l);
    free(text);

    /* a sentence end is preferred to a line break inside the sentence */
    text = repeat("Hard wrapped prose goes\non and on. Then more\nprose follows here. ", 20);
    len = strlen(text);
    l = split(text, 150, 0);
    for (size_t k = 0; k + 1 < l.count; k++) CHECK(text[l.items[k].end - 1] == '.');
    check_invariants(text, len, 150, 0, &l);
    rag_chunk_list_free(&l);
    free(text);

    /* "3.14" or "file.txt" are not the end of a sentence */
    text = repeat("pi is 3.14159 and the file is notes.txt ok ", 20);
    len = strlen(text);
    l = split(text, 60, 0);
    for (size_t k = 0; k + 1 < l.count; k++) CHECK(text[l.items[k].end] == ' ');
    check_invariants(text, len, 60, 0, &l);
    rag_chunk_list_free(&l);
    free(text);

    /* ideographic full stops */
    text = repeat("日本語の文です。", 50);
    len = strlen(text);
    l = split(text, 30, 0);
    CHECK(l.count > 5);
    for (size_t k = 0; k + 1 < l.count; k++) CHECK(memcmp(text + l.items[k].end - 3, "。", 3) == 0);
    check_invariants(text, len, 30, 0, &l);
    rag_chunk_list_free(&l);
    free(text);
}

static void test_lines_and_words(void) {
    char *text = repeat("item one two three four five\n", 30);
    size_t len = strlen(text);
    chunk_list_t l = split(text, 100, 0);
    CHECK(l.count > 5);
    for (size_t k = 0; k + 1 < l.count; k++) CHECK(text[l.items[k].end] == '\n');
    check_invariants(text, len, 100, 0, &l);
    rag_chunk_list_free(&l);
    free(text);

    text = repeat("word ", 500);
    len = strlen(text);
    l = split(text, 50, 10);
    CHECK(l.count > 10);
    for (size_t k = 0; k < l.count; k++) {
        CHECK(strncmp(text + l.items[k].start, "word", 4) == 0);
        CHECK(text[l.items[k].end] == ' ');
    }
    check_invariants(text, len, 50, 10, &l);
    rag_chunk_list_free(&l);
    free(text);
}

static void test_hard_cut(void) {
    char *text = repeat("x", 2000);
    chunk_list_t l = split(text, 800, 100);
    CHECK_SIZE(l.count, 3);
    if (l.count == 3) {
        CHECK_SIZE(l.items[0].start, 0);
        CHECK_SIZE(l.items[0].end, 800);
        CHECK_SIZE(l.items[1].start, 700);
        CHECK_SIZE(l.items[1].end, 1500);
        CHECK_SIZE(l.items[2].start, 1400);
        CHECK_SIZE(l.items[2].end, 2000);
    }
    check_invariants(text, 2000, 800, 100, &l);
    rag_chunk_list_free(&l);
    free(text);
}

static void test_utf8_characters(void) {
    /* sizes are counted in characters, and multi-byte characters are never cut */
    char *text = repeat("è", 1000);
    chunk_list_t l = split(text, 100, 0);
    CHECK_SIZE(l.count, 10);
    for (size_t k = 0; k < l.count; k++) {
        CHECK_SIZE(l.items[k].start, k * 200);
        CHECK_SIZE(l.items[k].end, (k + 1) * 200);
    }
    rag_chunk_list_free(&l);
    free(text);

    text = repeat("€😀", 200);
    size_t len = strlen(text);
    l = split(text, 33, 7);
    check_invariants(text, len, 33, 7, &l);
    rag_chunk_list_free(&l);
    free(text);

    /* invalid bytes count as one character each */
    const char *bad = "\xff\xfe abc \xc3 def \xe2\x82";
    l = split(bad, 4, 1);
    check_invariants(bad, strlen(bad), 4, 1, &l);
    rag_chunk_list_free(&l);
}

static void test_overlap(void) {
    buf_t b = {0};
    for (int i = 0; i < 500; i++) rag_buf_printf(&b, "w%04d ", i);
    const char *text = rag_buf_cstr(&b);
    size_t len = b.len;

    chunk_list_t l = split(text, 800, 100);
    CHECK(l.count >= 4);
    for (size_t k = 1; k < l.count; k++) {
        const chunk_span_t *p = &l.items[k - 1], *c = &l.items[k];
        CHECK(c->start < p->end);
        size_t shared = p->end - c->start;
        CHECK(shared >= 90 && shared <= 100);
        CHECK(text[c->start] == 'w' && text[c->start - 1] == ' ');
    }
    check_invariants(text, len, 800, 100, &l);
    rag_chunk_list_free(&l);

    /* the overlap starts at the beginning of a line when the first half of it has one */
    char *lines = repeat("line number with a few words\n", 40);
    chunk_list_t ll = split(lines, 200, 80);
    CHECK(ll.count > 3);
    for (size_t k = 1; k < ll.count; k++) {
        CHECK(lines[ll.items[k].start - 1] == '\n');
        CHECK(ll.items[k].start < ll.items[k - 1].end);
    }
    check_invariants(lines, strlen(lines), 200, 80, &ll);
    rag_chunk_list_free(&ll);
    free(lines);

    /* no overlap */
    l = split(text, 800, 0);
    for (size_t k = 1; k < l.count; k++) CHECK(l.items[k].start > l.items[k - 1].end);
    check_invariants(text, len, 800, 0, &l);
    rag_chunk_list_free(&l);

    /* out of range parameters are clamped */
    l = split(text, 0, 0); /* one character per chunk */
    CHECK_SIZE(l.count, 2500);
    rag_chunk_list_free(&l);
    l = split("abc def ghi", 5, 50);
    check_invariants("abc def ghi", 11, 5, 4, &l);
    rag_chunk_list_free(&l);
    rag_buf_free(&b);
}

static void test_long_whitespace_runs(void) {
    /* the overlap must not make a chunk that ends where the previous one ended */
    buf_t b = {0};
    rag_buf_puts(&b, "alpha beta gamma");
    for (int i = 0; i < 300; i++) rag_buf_putc(&b, '\n');
    rag_buf_puts(&b, "delta epsilon");
    chunk_list_t l = split(rag_buf_cstr(&b), 20, 15);
    CHECK_SIZE(l.count, 2);
    check_invariants(b.data, b.len, 20, 15, &l);
    rag_chunk_list_free(&l);
    rag_buf_free(&b);
}

/* xorshift64*, so that the random tests are the same everywhere */
static uint64_t rng_state = 0x9E3779B97F4A7C15u;
static uint32_t rng(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Du) >> 32);
}

static void test_random_texts(void) {
    static const char *pieces[] = {
        "word", "another", "città", "über", "日本語", "€uro",        "😀",   "x",    "supercalifragilistic",
        " ",    " ",       " ",     "  ",   "\t",     ". ",          "! ",   "? ",   ".\" ",
        "\n",   "\n",      "\n\n",  "\r\n", "\n\n\n", "# Heading\n", "3.14", "\xff", "\xe2\x82",
        "。",   "",
    };
    static const size_t sizes[] = {1, 2, 3, 5, 8, 13, 40, 100, 800};
    size_t npieces = sizeof(pieces) / sizeof(pieces[0]);

    for (int round = 0; round < 400; round++) {
        buf_t b = {0};
        if (round % 7 == 0) rag_buf_puts(&b, "\xEF\xBB\xBF");
        size_t count = rng() % 400;
        for (size_t i = 0; i < count; i++) rag_buf_puts(&b, pieces[rng() % npieces]);
        const char *text = rag_buf_cstr(&b);

        size_t size = sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))];
        size_t overlaps[] = {0, 1, size / 4, size / 2, size - 1};
        size_t overlap = overlaps[rng() % 5];
        if (overlap >= size) overlap = size - 1;

        chunk_list_t l = {0};
        CHECK(rag_chunk_split(text, b.len, size, overlap, &l));
        if (!check_invariants(text, b.len, size, overlap, &l)) {
            fprintf(stderr, "  round %d: size %zu, overlap %zu, %zu bytes\n", round, size, overlap, b.len);
            rag_chunk_list_free(&l);
            rag_buf_free(&b);
            break;
        }
        rag_chunk_list_free(&l);
        rag_buf_free(&b);
    }
}

static void test_utf8(void) {
    const char *s = "aè€😀";
    CHECK_SIZE(rag_utf8_count(s, 0, strlen(s)), 4);
    CHECK_SIZE(rag_utf8_advance(s, strlen(s), 0, 2), 3);
    CHECK_SIZE(rag_utf8_advance(s, strlen(s), 0, 100), strlen(s));

    struct {
        const char *in, *out;
    } cases[] = {
        {"plain ascii", "plain ascii"},
        {"città 😀", "città 😀"},
        {"a\xff"
         "b",
         "a\xEF\xBF\xBD"
         "b"},
        {"\xe2\x82", "\xEF\xBF\xBD\xEF\xBF\xBD"},                                 /* truncated */
        {"\xc0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD"},                                 /* overlong */
        {"\xed\xa0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},                 /* surrogate */
        {"\xf4\x90\x80\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"}, /* > U+10FFFF */
        {"\xf4\x8f\xbf\xbf", "\xf4\x8f\xbf\xbf"},                                 /* U+10FFFF */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = 0;
        char *out = rag_utf8_sanitize(cases[i].in, strlen(cases[i].in), &n);
        CHECK_STR(out, cases[i].out);
        CHECK_SIZE(n, strlen(cases[i].out));
        free(out);
    }
}

int main(void) {
    RUN(test_empty);
    RUN(test_short_text);
    RUN(test_byte_order_mark);
    RUN(test_paragraphs);
    RUN(test_markdown);
    RUN(test_sentences);
    RUN(test_lines_and_words);
    RUN(test_hard_cut);
    RUN(test_utf8_characters);
    RUN(test_overlap);
    RUN(test_long_whitespace_runs);
    RUN(test_random_texts);
    RUN(test_utf8);
    return test_report();
}
