/*
 * Text chunking.
 *
 * A chunk holds at most `size` characters. When the rest of the text does not
 * fit, the end of the chunk is chosen among the positions of the second half
 * of the window, preferring, in this order: a paragraph break (a blank line or
 * a Markdown heading), a line that ends a sentence, the end of a sentence, a
 * line break, a space between words. Only when there is none of them is the
 * text cut at the end of the window. A chunk never ends right after a heading.
 * The next chunk starts `overlap` characters before the end of the previous
 * one, moved forward to the first start of a paragraph found in the first half
 * of the overlap, else of a line after a sentence, of a sentence, of a line, of
 * a word, in this order. Lines starting with "#" inside fenced code blocks are
 * not headings.
 */
#include "internal.h"

#include <stdlib.h>

enum { BREAK_PARAGRAPH, BREAK_SENTENCE_LINE, BREAK_SENTENCE, BREAK_LINE, BREAK_WORD, BREAK_LEVELS };

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* Whitespace inside a line. */
static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

static size_t skip_space(const char *t, size_t len, size_t i) {
    while (i < len && is_space(t[i])) i++;
    return i;
}

static bool is_closing(char c) { return c == '"' || c == '\'' || c == ')' || c == ']' || c == '*' || c == '_'; }

/* True if t[j] starts a CJK closing bracket: 」』】〕〗〙〛 or a fullwidth ")". */
static bool is_cjk_closing(const char *t, size_t len, size_t j) {
    if (j + 3 > len) return false;
    const unsigned char *u = (const unsigned char *)t + j;
    return (u[0] == 0xE3 && u[1] == 0x80 &&
            (u[2] == 0x8D || u[2] == 0x8F || u[2] == 0x91 || u[2] == 0x95 || u[2] == 0x97 || u[2] == 0x99 ||
             u[2] == 0x9B)) ||
           (u[0] == 0xEF && u[1] == 0xBC && u[2] == 0x89);
}

/* True if t[j - 3..j) is an ideographic full stop or a fullwidth "!" or "?", maybe followed by brackets. */
static bool ends_cjk_sentence(const char *t, size_t from, size_t j) {
    while (j >= from + 3 && is_cjk_closing(t, j, j - 3)) j -= 3;
    if (j < from + 3) return false;
    const unsigned char *u = (const unsigned char *)t + j - 3;
    return (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x82) ||
           (u[0] == 0xEF && u[1] == 0xBC && (u[2] == 0x81 || u[2] == 0x9F));
}

/*
 * True if a sentence ends right before t[j]: ".", "!" or "?", maybe followed
 * by quotes or brackets; but not "12." starting a line (a numbered list) nor
 * "e.g." or "i.e.".
 */
static bool ends_sentence(const char *t, size_t from, size_t j) {
    while (j > from && is_closing(t[j - 1])) j--;
    if (j <= from) return false;
    if (t[j - 1] == '!' || t[j - 1] == '?') return true;
    if (t[j - 1] != '.') return ends_cjk_sentence(t, from, j);

    size_t w = j - 1; /* start of the word that ends with the dot */
    while (w > from && !is_space(t[w - 1])) w--;
    bool digits = w < j - 1, abbreviation = false;
    for (size_t k = w; k < j - 1; k++) {
        if (t[k] < '0' || t[k] > '9') digits = false;
        if (t[k] == '.' && t[k + 1] != '.') abbreviation = true;
    }
    size_t b = w;
    while (b > from && is_blank(t[b - 1])) b--;
    if (digits && (b == 0 || t[b - 1] == '\n')) return false;
    return !abbreviation;
}

/* The text being split, with its fenced code blocks (``` or ~~~). */
typedef struct doc {
    const char *t;
    size_t len;
    size_t *fences; /* pairs of offsets: [start, end) of each block */
    size_t fences_count;
    size_t fences_cap;
} doc_t;

/* Number of times `c` is repeated at t[i]. */
static size_t run_of(const doc_t *d, size_t i, char c) {
    size_t n = 0;
    while (i + n < d->len && d->t[i + n] == c) n++;
    return n;
}

static bool add_fence(doc_t *d, size_t start, size_t end) {
    if (d->fences_count + 2 > d->fences_cap) {
        size_t cap = d->fences_cap ? d->fences_cap * 2 : 16;
        size_t *fences = realloc(d->fences, cap * sizeof(*fences));
        if (!fences) return false;
        d->fences = fences;
        d->fences_cap = cap;
    }
    d->fences[d->fences_count++] = start;
    d->fences[d->fences_count++] = end;
    return true;
}

/*
 * Finds the fenced code blocks: a fence is a line starting with 3 or more ` or
 * ~ (a line like ```code``` is inline code). `start` skips a byte order mark.
 */
static bool find_fences(doc_t *d, size_t start) {
    char open = 0;
    size_t open_len = 0, open_start = 0;
    for (size_t line = start; line < d->len;) {
        size_t eol = line;
        while (eol < d->len && d->t[eol] != '\n') eol++;
        size_t i = line;
        while (i < eol && i - line < 3 && d->t[i] == ' ') i++;
        char c = i < eol ? d->t[i] : 0;
        size_t n = c == '`' || c == '~' ? run_of(d, i, c) : 0;
        bool inline_code = false;
        for (size_t k = i + n; c == '`' && k < eol; k++)
            if (d->t[k] == '`') inline_code = true;
        if (n >= 3 && !inline_code) {
            if (!open) {
                open = c;
                open_len = n;
                open_start = line;
            } else if (c == open && n >= open_len) {
                if (!add_fence(d, open_start, eol)) return false;
                open = 0;
            }
        }
        line = eol + 1;
    }
    return !open || add_fence(d, open_start, d->len);
}

static bool in_fence(const doc_t *d, size_t pos) {
    size_t lo = 0, hi = d->fences_count / 2;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pos < d->fences[2 * mid])
            hi = mid;
        else if (pos >= d->fences[2 * mid + 1])
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

/* True if the line starting at t[i] is a Markdown heading: "# Title" ... "###### Title". */
static bool is_heading(const doc_t *d, size_t i) {
    size_t n = run_of(d, i, '#');
    if (n < 1 || n > 6) return false;
    return (i + n >= d->len || is_space(d->t[i + n])) && !in_fence(d, i);
}

/* True if the line ending at t[j] (a newline, or the start of a line) follows a blank line. */
static bool after_blank_line(const char *t, size_t from, size_t j) {
    while (j > from && is_blank(t[j - 1])) j--;
    return j > from && t[j - 1] == '\n';
}

/* True if the last line before t[j] that is not blank is a heading. */
static bool after_heading(const doc_t *d, size_t from, size_t j) {
    const char *t = d->t;
    while (j > from && is_space(t[j - 1])) j--;
    size_t start = j;
    while (start > from && t[start - 1] != '\n') start--;
    return (start == 0 || t[start - 1] == '\n') && start < j && is_heading(d, start);
}

/* Kind of cut possible right before t[j], or -1. The chunk being built starts at `from`. */
static int classify(const doc_t *d, size_t from, size_t j) {
    const char *t = d->t;
    char c = t[j];
    if (c == '\n') {
        if (after_heading(d, from, j)) return -1; /* a heading goes with what follows it */
        size_t k = j + 1;
        while (k < d->len && is_blank(t[k])) k++;
        if (k >= d->len || t[k] == '\n') return BREAK_PARAGRAPH; /* a blank line follows */
        if (is_heading(d, j + 1)) return BREAK_PARAGRAPH;
        if (after_blank_line(t, from, j)) return BREAK_PARAGRAPH;
        k = j;
        while (k > from && is_blank(t[k - 1])) k--;
        return ends_sentence(t, from, k) ? BREAK_SENTENCE_LINE : BREAK_LINE;
    }
    if (is_blank(c)) return ends_sentence(t, from, j) ? BREAK_SENTENCE : BREAK_WORD;
    if (((unsigned char)c & 0xC0) != 0x80 && !is_cjk_closing(t, d->len, j) && ends_cjk_sentence(t, from, j))
        return BREAK_SENTENCE;
    return -1;
}

/* Best cut in t[lo..limit] for a chunk starting at `from`, or `limit` if there is none. */
static size_t find_break(const doc_t *d, size_t from, size_t lo, size_t limit) {
    size_t best[BREAK_LEVELS];
    bool found[BREAK_LEVELS] = {false};
    for (size_t j = limit + 1; j-- > lo;) {
        int kind = classify(d, from, j);
        if (kind < 0 || found[kind]) continue;
        found[kind] = true;
        best[kind] = j;
        if (kind == BREAK_PARAGRAPH) break;
    }
    for (int kind = 0; kind < BREAK_LEVELS; kind++)
        if (found[kind]) return best[kind];
    return limit;
}

/*
 * Start of the chunk after t[from..e): `overlap` characters before e (at most
 * all but one of the chunk), moved to the best start found in the first half of
 * the overlap, ranked like the cuts. Returns e if the chunk is too short.
 */
static size_t overlap_start(const doc_t *d, size_t from, size_t e, size_t overlap) {
    const char *t = d->t;
    size_t n = rag_utf8_count(t, from, e);
    if (n <= 1) return e;
    if (overlap >= n) overlap = n - 1;
    size_t o = rag_utf8_advance(t, d->len, from, n - overlap);
    size_t half = rag_utf8_advance(t, d->len, o, overlap / 2);

    size_t best[BREAK_LEVELS];
    bool found[BREAK_LEVELS] = {false};
    for (size_t k = o; k <= half && k < e; k++) {
        if (is_space(t[k])) continue;
        size_t q = k;
        while (q > from && is_blank(t[q - 1])) q--;
        int kind;
        if (t[q - 1] == '\n') {
            size_t p = q - 1;
            while (p > from && is_blank(t[p - 1])) p--;
            kind = after_blank_line(t, from, q - 1) || is_heading(d, k) ? BREAK_PARAGRAPH
                   : ends_sentence(t, from, p)                          ? BREAK_SENTENCE_LINE
                                                                        : BREAK_LINE;
        } else if (q < k) {
            kind = ends_sentence(t, from, q) ? BREAK_SENTENCE : BREAK_WORD;
        } else if (((unsigned char)t[k] & 0xC0) != 0x80 && !is_cjk_closing(t, d->len, k) &&
                   ends_cjk_sentence(t, from, k)) {
            kind = BREAK_SENTENCE;
        } else {
            continue;
        }
        if (!found[kind]) {
            found[kind] = true;
            best[kind] = k;
        }
        if (kind == BREAK_PARAGRAPH) break;
    }
    for (int kind = 0; kind < BREAK_LEVELS; kind++)
        if (found[kind]) return best[kind];

    if (!is_space(t[o - 1]) && !is_space(t[o])) {
        /* inside a word: start at the next one, if the overlap contains one */
        size_t k = o;
        while (k < e && !is_space(t[k])) k++;
        if (k < e) o = k;
    }
    return o;
}

static bool push(chunk_list_t *list, chunk_span_t span) {
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        chunk_span_t *items = realloc(list->items, cap * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->cap = cap;
    }
    list->items[list->count++] = span;
    return true;
}

/* Line numbers of increasing offsets. */
typedef struct line_cursor {
    size_t pos;
    size_t line;
} line_cursor_t;

static size_t line_of(const char *t, line_cursor_t *c, size_t pos) {
    for (; c->pos < pos; c->pos++)
        if (t[c->pos] == '\n') c->line++;
    return c->line;
}

bool rag_chunk_split(const char *text, size_t len, size_t size, size_t overlap, chunk_list_t *out) {
    if (size == 0) size = 1;
    if (overlap >= size) overlap = size - 1;

    size_t pos = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        pos = 3; /* byte order mark */
    doc_t d = {text, len, NULL, 0, 0};
    if (!find_fences(&d, pos)) {
        free(d.fences);
        return false;
    }

    line_cursor_t starts = {0, 1}, ends = {0, 1};
    size_t min_end = 0; /* a cut must leave the chunk longer than the previous one */

    bool ok = true;
    for (;;) {
        pos = skip_space(text, len, pos);
        if (pos >= len) break;

        size_t limit = rag_utf8_advance(text, len, pos, size);
        size_t end = len;
        if (limit < len) {
            size_t lo = rag_utf8_advance(text, len, pos, size / 2 ? size / 2 : 1);
            if (lo < min_end) lo = min_end;
            end = lo <= limit ? find_break(&d, pos, lo, limit) : limit;
        }
        size_t e = end;
        while (e > pos && is_space(text[e - 1])) e--;

        chunk_span_t span = {pos, e, line_of(text, &starts, pos), line_of(text, &ends, e - 1)};
        if (!push(out, span)) {
            ok = false;
            break;
        }

        size_t rest = skip_space(text, len, end);
        if (rest >= len) break;
        min_end = rest + rag_utf8_char_len(text, len, rest);

        /* the next chunk must be able to reach past this one */
        size_t next = overlap ? overlap_start(&d, pos, e, overlap) : e;
        pos = next < e && rag_utf8_count(text, next, min_end) <= size ? next : rest;
    }
    free(d.fences);
    return ok;
}

void rag_chunk_list_free(chunk_list_t *list) {
    free(list->items);
    *list = (chunk_list_t){0};
}
