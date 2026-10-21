/* Unit tests of the vector helpers: normalization, similarity, top k. */
#include "internal.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>

static bool near(double a, double b) { return fabs(a - b) < 1e-6; }

static void test_normalize(void) {
    float v[3] = {3, 4, 0};
    CHECK(rag_vec_normalize(v, 3));
    CHECK(near(v[0], 0.6) && near(v[1], 0.8) && v[2] == 0);
    CHECK(near(rag_vec_dot(v, v, 3), 1));

    /* already normalized vectors do not change */
    float u[2] = {1, 0};
    CHECK(rag_vec_normalize(u, 2));
    CHECK(u[0] == 1 && u[1] == 0);

    /* a zero vector stays zero */
    float z[4] = {0, 0, 0, 0};
    CHECK(!rag_vec_normalize(z, 4));
    CHECK(z[0] == 0 && z[3] == 0);

    /* large values do not overflow */
    float big[2] = {3e38f, 3e38f};
    CHECK(rag_vec_normalize(big, 2));
    CHECK(near(big[0], sqrt(0.5)) && near(big[1], sqrt(0.5)));

    float tiny[2] = {1e-30f, 0};
    CHECK(rag_vec_normalize(tiny, 2));
    CHECK(near(tiny[0], 1));
}

static void test_cosine(void) {
    float a[3] = {1, 2, 3}, b[3] = {2, 4, 6}, c[3] = {-1, -2, -3}, d[3] = {3, 0, -1};
    rag_vec_normalize(a, 3);
    rag_vec_normalize(b, 3);
    rag_vec_normalize(c, 3);
    rag_vec_normalize(d, 3);
    CHECK(near(rag_vec_dot(a, b, 3), 1));  /* same direction */
    CHECK(near(rag_vec_dot(a, c, 3), -1)); /* opposite */
    CHECK(near(rag_vec_dot(a, d, 3), 0));  /* orthogonal */
    float e[2] = {1, 1}, f[2] = {1, 0};
    rag_vec_normalize(e, 2);
    CHECK(near(rag_vec_dot(e, f, 2), sqrt(0.5))); /* 45 degrees */
}

static int compare_desc(const void *a, const void *b) {
    const topk_item_t *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static uint64_t rng_state = 42;
static uint32_t rng(void) {
    rng_state = rng_state * 6364136223846793005u + 1442695040888963407u;
    return (uint32_t)(rng_state >> 33);
}

static void test_topk(void) {
    /* against sorting everything */
    for (int round = 0; round < 200; round++) {
        size_t n = rng() % 300, k = rng() % 20;
        topk_item_t *all = malloc((n ? n : 1) * sizeof(*all));
        topk_t t;
        CHECK(rag_topk_init(&t, k));
        for (size_t i = 0; i < n; i++) {
            /* few distinct scores, to have ties */
            all[i] = (topk_item_t){(double)(rng() % 50) / 25.0 - 1.0, (int64_t)(rng() % 1000)};
            rag_topk_push(&t, all[i].score, all[i].id);
        }
        qsort(all, n, sizeof(*all), compare_desc);
        rag_topk_sort(&t);
        size_t expected = n < k ? n : k;
        CHECK_SIZE(t.count, expected);
        for (size_t i = 0; i < t.count && i < expected; i++) {
            CHECK(t.items[i].score == all[i].score);
            CHECK(t.items[i].id == all[i].id);
        }
        rag_topk_free(&t);
        free(all);
    }

    topk_t t;
    CHECK(rag_topk_init(&t, 2));
    rag_topk_push(&t, 0.5, 3);
    rag_topk_push(&t, 0.9, 1);
    rag_topk_push(&t, 0.1, 2);
    rag_topk_push(&t, 0.5, 0); /* ties go to the smallest id */
    rag_topk_sort(&t);
    CHECK_SIZE(t.count, 2);
    CHECK(t.items[0].id == 1 && t.items[1].id == 0);
    rag_topk_free(&t);
}

static void test_blob(void) {
    float v[4] = {1.5f, -2.25f, 0.0f, 3.4e38f};
    unsigned char blob[16];
    rag_vec_to_blob(v, 4, blob);
    static const unsigned char expected[8] = {0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0}; /* little-endian */
    CHECK(memcmp(blob, expected, 8) == 0);
    float back[4];
    rag_vec_from_blob(blob, 4, back);
    CHECK(memcmp(v, back, sizeof(v)) == 0);
}

int main(void) {
    RUN(test_normalize);
    RUN(test_cosine);
    RUN(test_topk);
    RUN(test_blob);
    return test_report();
}
