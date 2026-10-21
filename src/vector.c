/* Embedding vectors: storage format, similarity, selection of the best results. */
#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void rag_vec_to_blob(const float *v, size_t n, unsigned char *out) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], sizeof(bits));
        out[4 * i] = (unsigned char)bits;
        out[4 * i + 1] = (unsigned char)(bits >> 8);
        out[4 * i + 2] = (unsigned char)(bits >> 16);
        out[4 * i + 3] = (unsigned char)(bits >> 24);
    }
}

void rag_vec_from_blob(const unsigned char *blob, size_t n, float *out) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits = (uint32_t)blob[4 * i] | (uint32_t)blob[4 * i + 1] << 8 | (uint32_t)blob[4 * i + 2] << 16 |
                        (uint32_t)blob[4 * i + 3] << 24;
        memcpy(&out[i], &bits, sizeof(bits));
    }
}

/* ---- similarity ---- */

bool rag_vec_normalize(float *v, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)v[i] * v[i];
    if (!(sum > 0)) return false;
    double scale = 1.0 / sqrt(sum);
    for (size_t i = 0; i < n; i++) v[i] = (float)(v[i] * scale);
    return true;
}

double rag_vec_dot(const float *a, const float *b, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)a[i] * b[i];
    return sum;
}

/* ---- top k ---- */

/* True if a is a worse result than b. */
static bool worse(const topk_item_t *a, const topk_item_t *b) {
    return a->score < b->score || (a->score == b->score && a->id > b->id);
}

static void swap(topk_item_t *a, topk_item_t *b) {
    topk_item_t t = *a;
    *a = *b;
    *b = t;
}

bool rag_topk_init(topk_t *t, size_t k) {
    *t = (topk_t){.k = k};
    return true;
}

void rag_topk_push(topk_t *t, double score, int64_t id) {
    topk_item_t item = {score, id};
    if (t->k == 0 || t->failed) return;
    if (t->count < t->k) {
        /* the array grows with the results, so that a huge k costs nothing */
        if (t->count == t->cap) {
            size_t cap = t->cap ? t->cap * 2 : 16;
            if (cap > t->k || cap < t->cap) cap = t->k;
            topk_item_t *items = cap <= SIZE_MAX / sizeof(*items) ? realloc(t->items, cap * sizeof(*items)) : NULL;
            if (!items) {
                t->failed = true;
                return;
            }
            t->items = items;
            t->cap = cap;
        }
        /* sift up */
        size_t i = t->count++;
        t->items[i] = item;
        while (i > 0 && worse(&t->items[i], &t->items[(i - 1) / 2])) {
            swap(&t->items[i], &t->items[(i - 1) / 2]);
            i = (i - 1) / 2;
        }
        return;
    }
    if (!worse(&t->items[0], &item)) return;
    /* replace the worst and sift down */
    t->items[0] = item;
    for (size_t i = 0;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->count && worse(&t->items[l], &t->items[m])) m = l;
        if (r < t->count && worse(&t->items[r], &t->items[m])) m = r;
        if (m == i) break;
        swap(&t->items[i], &t->items[m]);
        i = m;
    }
}

static int compare_best_first(const void *a, const void *b) {
    const topk_item_t *x = a, *y = b;
    return worse(y, x) ? -1 : worse(x, y) ? 1 : 0;
}

void rag_topk_sort(topk_t *t) {
    if (t->count) qsort(t->items, t->count, sizeof(*t->items), compare_best_first);
}

void rag_topk_free(topk_t *t) {
    free(t->items);
    *t = (topk_t){0};
}
