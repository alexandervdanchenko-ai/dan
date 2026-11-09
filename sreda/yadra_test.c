/* Test of sreda/yadra.c: every variant of the kernels gives the same bits as
   plain C, the pool covers a range exactly once, the kernels compute what
   they should. Built and run by test_yadra.py; prints «ok» and the variants. */

#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include "yadra.c"
#include "yadra_etalon.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("ошибка, строка %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double rnd(void) { return (double)(next() >> 11) / 9007199254740992.0 * 2.0 - 1.0; }   /* [-1, 1) */
static uint64_t rng2 = 2463534242ull;   /* for inputs added later, so that rnd gives the inputs of the recorded hashes */
static double rnd2(void) {
    rng2 ^= rng2 << 13; rng2 ^= rng2 >> 7; rng2 ^= rng2 << 17;
    return (double)(rng2 >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static uint64_t rng3 = 3141592653589793ull;   /* for the formats Q2_K, Q3_K, IQ4_XS: the inputs of the recorded hashes stay */
static double rnd3(void) {
    rng3 ^= rng3 << 13; rng3 ^= rng3 >> 7; rng3 ^= rng3 << 17;
    return (double)(rng3 >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static int same32(float a, float b) { return memcmp(&a, &b, 4) == 0; }
static int same64(double a, double b) { return memcmp(&a, &b, 8) == 0; }

static void test_f16(void) {
    for (uint32_t h = 0; h < 65536; h++) {
        if ((h & 0x7c00) == 0x7c00 && (h & 0x3ff)) continue;   /* NaN */
        CHECK(dan_f32_to_f16(dan_f16_to_f32((uint16_t)h)) == h, "половинная точность %u", h);
    }
    /* rounding: halfway cases go to even */
    CHECK(dan_f32_to_f16(1.0f + 1.0f / 2048) == 0x3c00, "1 + 2^-11");
    CHECK(dan_f32_to_f16(1.0f + 3.0f / 2048) == 0x3c02, "1 + 3 * 2^-11");
    CHECK(dan_f32_to_f16(65520.0f) == 0x7c00, "65520");
    CHECK(dan_f32_to_f16(65519.0f) == 0x7bff, "65519");
    CHECK(dan_f32_to_f16(5.9604645e-8f) == 1, "2^-24");
    CHECK(dan_f32_to_f16(2.9802322e-8f) == 0, "2^-25 к чётному");
    for (int i = 0; i < 200000; i++) {   /* nearest: the error is at most half a unit */
        float f = (float)(rnd() * 70000.0 * pow(2.0, -(double)(next() % 30)));
        float g = dan_f16_to_f32(dan_f32_to_f16(f));
        if (fabsf(f) >= 65520.0f) { CHECK(isinf(g), "переполнение %g", f); continue; }
        uint16_t h = dan_f32_to_f16(f);
        double dist = fabs((double)g - f);
        if ((h & 0x7fff) != 0x7bff) CHECK(dist <= fabs((double)dan_f16_to_f32((uint16_t)(h + 1)) - f), "округление %g", f);
        if (h & 0x7fff) CHECK(dist <= fabs((double)dan_f16_to_f32((uint16_t)(h - 1)) - f), "округление %g", f);
#if defined(__FLT16_MAX__) && (defined(__x86_64__) || defined(__aarch64__))
        _Float16 hw = (_Float16)f;
        uint16_t hb;
        memcpy(&hb, &hw, 2);
        CHECK(hb == h, "как у процессора %g: %x и %x", f, h, hb);
#endif
    }
}

static const dan_vk_table *variants[8];
static const char *labels[8];   /* the names asked for (several tables are named «avx512») */
static int nvariants;

static void find_variants(void) {
    static const char *const names[] = {"c", "avx2", "avx512vnni", "avx512z", "avx512", "neon", "dotprod", "i8mm"};
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++) {
        const dan_vk_table *t;
        if (!dan_vk_supported(names[i], &t)) continue;
        int seen = 0;
        for (int v = 0; v < nvariants; v++) seen |= variants[v] == t;
        if (!seen) { labels[nvariants] = t->many && strcmp(names[i], "avx512") == 0 ? "amx" : names[i]; variants[nvariants++] = t; }
    }
}

static void test_dot(void) {
    static const int64_t sizes[] = {0, 1, 2, 7, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 1000, 4097};
    float *a = malloc(4097 * sizeof(float)), *b = malloc(4097 * sizeof(float));
    double *c = malloc(4097 * sizeof(double)), *d = malloc(4097 * sizeof(double));
    for (int i = 0; i < 4097; i++) { a[i] = (float)rnd(); b[i] = (float)rnd(); c[i] = rnd(); d[i] = rnd() * 1e3; }
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int64_t n = sizes[s];
        float r32 = dan_vk_dot_f32_c(a, b, n);
        double r64 = dan_vk_dot_f64_c(c, d, n);
        double exact = 0;
        for (int64_t i = 0; i < n; i++) exact += (double)a[i] * b[i];
        CHECK(fabs(r32 - exact) <= 1e-4 * (1 + n), "точность скалярного произведения, n = %d", (int)n);
        for (int v = 0; v < nvariants; v++) {
            CHECK(same32(variants[v]->dot_f32(a, b, n), r32), "%s dot_f32 n = %d", variants[v]->name, (int)n);
            CHECK(same64(variants[v]->dot_f64(c, d, n), r64), "%s dot_f64 n = %d", variants[v]->name, (int)n);
        }
    }
    free(a); free(b); free(c); free(d);
}

static void test_mm(void) {
    static const int64_t dims[][3] = {{1, 1, 1}, {4, 16, 3}, {5, 17, 9}, {8, 32, 64}, {13, 70, 33}, {33, 129, 50}, {3, 200, 1}, {64, 64, 64}};
    for (size_t s = 0; s < sizeof dims / sizeof *dims; s++) {
        int64_t m = dims[s][0], n = dims[s][1], k = dims[s][2];
        float *A = malloc(m * k * 4), *B = malloc(k * n * 4), *R = malloc(m * n * 4), *C = malloc(m * n * 4);
        double *A2 = malloc(m * k * 8), *B2 = malloc(k * n * 8), *R2 = malloc(m * n * 8), *C2 = malloc(m * n * 8);
        for (int64_t i = 0; i < m * k; i++) { A[i] = (float)rnd(); A2[i] = rnd(); }
        for (int64_t i = 0; i < k * n; i++) { B[i] = (float)rnd(); B2[i] = rnd(); }
        dan_vk_mm_f32_c(A, B, R, m, n, k, 0, n);
        dan_vk_mm_f64_c(A2, B2, R2, m, n, k, 0, n);
        for (int64_t i = 0; i < m; i++) for (int64_t j = 0; j < n; j++) {
            double e = 0;
            for (int64_t p = 0; p < k; p++) e += (double)A[i * k + p] * B[p * n + j];
            CHECK(fabs(R[i * n + j] - e) <= 1e-4 * (1 + k), "точность произведения матриц");
        }
        for (int v = 0; v < nvariants; v++) {
            memset(C, 0xff, m * n * 4);
            memset(C2, 0xff, m * n * 8);
            variants[v]->mm_f32(A, B, C, m, n, k, 0, n);
            variants[v]->mm_f64(A2, B2, C2, m, n, k, 0, n);
            CHECK(memcmp(C, R, m * n * 4) == 0, "%s mm_f32 %dx%dx%d", variants[v]->name, (int)m, (int)n, (int)k);
            CHECK(memcmp(C2, R2, m * n * 8) == 0, "%s mm_f64 %dx%dx%d", variants[v]->name, (int)m, (int)n, (int)k);
            /* the parallel product (strips of 64 columns) */
            dan_vk_cur = variants[v];
            memset(C, 0, m * n * 4);
            dan_vk_matmat_f32(A, B, C, m, n, k);
            CHECK(memcmp(C, R, m * n * 4) == 0, "%s matmat_f32", variants[v]->name);
            dan_vk_matmat_f64(A2, B2, C2, m, n, k);
            CHECK(memcmp(C2, R2, m * n * 8) == 0, "%s matmat_f64", variants[v]->name);
        }
        free(A); free(B); free(R); free(C); free(A2); free(B2); free(R2); free(C2);
    }
}

/* перевод вектора в 8 бит простым кодом C (как был до команд SSE2): ядра на x86 должны давать те же байты */
static void ref_q8_quantize(const float *x, uint8_t *out, int64_t n) {
    for (int64_t b = 0; b < n / 32; b++) {
        const float *v = x + b * 32;
        uint8_t *o = out + b * 34;
        float amax = 0.0f;
        for (int i = 0; i < 32; i++) { float a = fabsf(v[i]); if (a > amax) amax = a; }
        float d = amax / 127.0f, id = d != 0.0f ? 1.0f / d : 0.0f;
        if (!isfinite(id) || !isfinite(d)) { d = 0.0f; id = 0.0f; }
        uint16_t h = dan_f32_to_f16(d);
        memcpy(o, &h, 2);
        for (int i = 0; i < 32; i++) {
            float q = v[i] * id;
            q = q != q ? 0.0f : q < -127.0f ? -127.0f : q > 127.0f ? 127.0f : q;
            o[2 + i] = (uint8_t)(int8_t)lrintf(q);
        }
    }
}
static void ref_q8k_quantize(const float *x, dan_q8k *y, int64_t n) {
    for (int64_t b = 0; b < n / 256; b++, x += 256) {
        dan_q8k *o = y + b;
        float max = 0.0f, amax = 0.0f;
        for (int j = 0; j < 256; j++) { float ax = fabsf(x[j]); if (ax > amax) { amax = ax; max = x[j]; } }
        float iscale = amax != 0.0f ? -127.0f / max : 0.0f;
        if (!isfinite(iscale) || iscale == 0.0f) { memset(o, 0, sizeof *o); continue; }
        for (int j = 0; j < 256; j++) {
            float v = iscale * x[j];
            long r = lrintf(v);
            o->qs[j] = (int8_t)(r < 127 ? r : 127);
        }
        for (int j = 0; j < 16; j++) {
            int s = 0;
            for (int i = 0; i < 16; i++) s += o->qs[16 * j + i];
            o->bsums[j] = (int16_t)s;
        }
        o->d = 1.0f / iscale;
    }
}

/* перевод в 8 бит на случайных числах разных порядков, с равными по модулю числами разных знаков,
   крошечными, NaN и бесконечностями (Q8_0) - побайтно как у простого кода */
static void test_quantize_ref(void) {
    uint64_t saved = rng;   /* свои случайные числа: остальные пробы и хеши - как прежде */
    static float x[1024];
    static uint8_t a[32 * 34], b[32 * 34];
    static dan_q8k ka[4], kb[4];
    for (int t = 0; t < 20000; t++) {
        int mode = t % 6;
        for (int i = 0; i < 1024; i++) {
            double g = rnd();
            x[i] = mode == 0 ? (float)g : mode == 1 ? (float)(g * 1e-38) : mode == 2 ? (float)(g * 1e6)
                 : mode == 3 ? (float)((int)(g * 20)) / 4.0f : mode == 4 ? 0.0f : (float)(g * (i % 37 == 0 ? 100 : 1));
        }
        if (t % 3 == 0) { int k = (int)(next() % 1023); x[k] = -x[k + 1]; }
        dan_vk_q8k_quantize(x, ka, 1024);
        ref_q8k_quantize(x, kb, 1024);
        CHECK(memcmp(ka, kb, sizeof ka) == 0, "Q8_K как простой код, проба %d", t);
        if (t % 5 == 0) x[next() % 1024] = NAN;
        if (t % 7 == 0) x[next() % 1024] = -INFINITY;
        dan_vk_q8_quantize(x, a, 1024);
        ref_q8_quantize(x, b, 1024);
        CHECK(memcmp(a, b, sizeof a) == 0, "Q8_0 как простой код, проба %d", t);
    }
    rng = saved;
}

static void test_q8(void) {
    int8_t x[32], y[32];
    for (int t = 0; t < 2000; t++) {
        for (int i = 0; i < 32; i++) {
            x[i] = (int8_t)(t < 3 ? -128 : (int)(next() % 256) - 128);
            y[i] = (int8_t)(t < 2 ? -128 : (int)(next() % 256) - 128);
        }
        int32_t r = dan_vk_q8_block_c(x, y);
        for (int v = 0; v < nvariants; v++) CHECK(variants[v]->q8_block(x, y) == r, "%s q8_block", variants[v]->name);
    }
    /* quantizing: tiny, huge and not finite numbers do not break the block */
    {
        float v[64];
        uint8_t q[68];
        for (int i = 0; i < 32; i++) v[i] = 1e-41f * (i - 16);   /* subnormal: 1/d is infinite */
        v[32] = NAN; v[33] = INFINITY;
        for (int i = 34; i < 64; i++) v[i] = (float)i;
        dan_vk_q8_quantize(v, q, 64);
        for (int i = 0; i < 32; i++) CHECK(q[2 + i] == 0, "крошечные числа");
        float back[64];
        dan_vk_q8_dequantize(q, back, 64);
        for (int i = 0; i < 64; i++) CHECK(!isnan(back[i]), "без NaN после квантования");
    }
    /* dot products of rows of blocks: every variant, any number of blocks */
    {
        static const int64_t nb[] = {0, 1, 7, 8, 9, 16, 17, 63, 64, 65};
        uint8_t *qa = malloc(65 * 34), *qb = malloc(65 * 34);
        float va[65 * 32], vb[65 * 32];
        for (int i = 0; i < 65 * 32; i++) { va[i] = (float)rnd() * 7; vb[i] = (float)rnd(); }
        dan_vk_q8_quantize(va, qa, 65 * 32);
        dan_vk_q8_quantize(vb, qb, 65 * 32);
        for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
            float r = variants[0]->q8_dot(qa, qb, nb[s]);
            double e = 0;
            float da[65 * 32], db[65 * 32];
            dan_vk_q8_dequantize(qa, da, nb[s] * 32);
            dan_vk_q8_dequantize(qb, db, nb[s] * 32);
            for (int64_t i = 0; i < nb[s] * 32; i++) e += (double)da[i] * db[i];
            CHECK(fabs(r - e) <= 1e-4 * (1 + fabs(e)), "q8_dot %d блоков: %g и %g", (int)nb[s], r, e);
            for (int v = 0; v < nvariants; v++)
                CHECK(same32(variants[v]->q8_dot(qa, qb, nb[s]), r), "%s q8_dot %d блоков", variants[v]->name, (int)nb[s]);
        }
        free(qa); free(qb);
    }
    /* matrix 300 x 256 by vector: by blocks equals the float product of the
       dequantized numbers within rounding */
    int64_t rows = 300, cols = 256;
    float *w = malloc(rows * cols * 4), *xv = malloc(cols * 4), *wd = malloc(rows * cols * 4), *xd = malloc(cols * 4);
    float *y1 = malloc(rows * 4), *y2 = malloc(rows * 4);
    uint8_t *wq = malloc(rows * cols / 32 * 34), *xq = malloc(cols / 32 * 34);
    for (int64_t i = 0; i < rows * cols; i++) w[i] = (float)rnd();
    for (int64_t i = 0; i < cols; i++) xv[i] = (float)rnd() * 3;
    dan_vk_q8_quantize(w, wq, rows * cols);
    dan_vk_q8_quantize(xv, xq, cols);
    dan_vk_q8_dequantize(wq, wd, rows * cols);
    dan_vk_q8_dequantize(xq, xd, cols);
    for (int64_t i = 0; i < rows * cols; i++) CHECK(fabsf(wd[i] - w[i]) <= 1.0f / 254 + 1.0f / 2048, "квантование %g", w[i]);
    dan_vk_cur = &dan_vk_plain;
    dan_vk_matvec_q8(wq, xq, y1, rows, cols);
    for (int64_t i = 0; i < rows; i++) {
        double e = 0;
        for (int64_t j = 0; j < cols; j++) e += (double)wd[i * cols + j] * xd[j];
        CHECK(fabs(y1[i] - e) <= 1e-3 * (1 + fabs(e)), "matvec_q8 строка %d: %g и %g", (int)i, y1[i], e);
    }
    for (int v = 0; v < nvariants; v++) {
        dan_vk_cur = variants[v];
        dan_vk_matvec_q8(wq, xq, y2, rows, cols);
        CHECK(memcmp(y1, y2, rows * 4) == 0, "%s matvec_q8", variants[v]->name);
    }
    free(w); free(xv); free(wd); free(xd); free(y1); free(y2); free(wq); free(xq);
}

/* ---- weights of llama.cpp ---- */

static const char *const kv_names[DAN_NFMT] = {"Q4_0", "Q4_K", "Q5_K", "Q6_K", "Q5_0", "Q4_1", "Q5_1", "Q8_0", "Q2_K", "Q3_K",
                                               "IQ4_XS"};

/* the offsets of d and of dmin (or m; -1 - none) in a block */
static int kv_doff(int f) { return f == DAN_FMT_Q6K ? 208 : f == DAN_FMT_Q2K ? 80 : f == DAN_FMT_Q3K ? 108 : 0; }
static int kv_moff(int f) {
    return f == DAN_FMT_Q4K || f == DAN_FMT_Q5K || f == DAN_FMT_Q41 || f == DAN_FMT_Q51 ? 2 : f == DAN_FMT_Q2K ? 82 : -1;
}
/* the numbers of the inputs: the formats after Q6_K take them from rnd2, Q2_K and later from rnd3 */
static double kv_rnd(int f, int first) { return f >= DAN_FMT_Q2K ? rnd3() : f < 4 && first ? rnd() : rnd2(); }

static void kv_dequant(int fmt, const uint8_t *in, float *out, int64_t blocks) {
    dan_vk_fmt_dequantize(fmt, in, out, blocks);
}

/* random blocks of the format (Q8_0 - random bytes and a half scale) */
static void kv_blocks(int fmt, uint8_t *w, int64_t blocks, uint64_t seed) {
    if (fmt < ETALON_KVANT_FORMATS) { etalon_kvant_blocks(fmt, w, blocks, seed); return; }
    if (fmt >= DAN_FMT_Q2K) { etalon_kvant2_blocks(fmt - DAN_FMT_Q2K, w, blocks, seed); return; }
    etalon_rng = seed;
    for (int64_t i = 0; i < blocks * DAN_Q8_BYTES; i++) {
        etalon_rng ^= etalon_rng << 13; etalon_rng ^= etalon_rng >> 7; etalon_rng ^= etalon_rng << 17;
        w[i] = (uint8_t)(etalon_rng >> 32);
    }
    for (int64_t i = 0; i < blocks; i++) { uint16_t h = etalon_half(); memcpy(w + i * DAN_Q8_BYTES, &h, 2); }
}

/* the vector in 8 bits for the format */
static void *kv_quantize_x(int fmt, const float *x, int64_t n) {
    uint8_t *q = malloc((size_t)(n / dan_vk_fmt_n[fmt] * dan_vk_fmt_xbytes[fmt]) + 1);
    dan_vk_fmt_quantize_x(fmt, x, q, n);
    return q;
}
static void kv_dequantize_x(int fmt, const void *q, double *x, int64_t n) {
    if (dan_vk_fmt_n[fmt] == 32) {
        int xb = dan_vk_fmt_xbytes[fmt], off = xb - 32;
        for (int64_t b = 0; b < n / 32; b++) {
            const uint8_t *o = (const uint8_t *)q + b * xb;
            int s = 0;
            for (int i = 0; i < 32; i++) { x[b * 32 + i] = (double)dan_vk_half(o) * (int8_t)o[off + i]; s += (int8_t)o[off + i]; }
            if (off == 4) { int16_t t; memcpy(&t, o + 2, 2); CHECK(t == s, "сумма блока вектора %d и %d", t, s); }
        }
        return;
    }
    const dan_q8k *k = (const dan_q8k *)q;
    for (int64_t i = 0; i < n; i++) x[i] = (double)k[i / 256].d * k[i / 256].qs[i % 256];
}

static uint64_t kv_hash = 14695981039346656037ull;   /* of the results of the matrices: the same for any number of threads */
static uint64_t kv_hash2 = 14695981039346656037ull;
static uint64_t kv_hash3 = 14695981039346656037ull;   /* Q2_K, Q3_K, IQ4_XS */

static void kv_dequant2(int fmt, const uint8_t *in, float *out, int64_t blocks) {
    dan_vk_fmt_dequantize(DAN_FMT_Q2K + fmt, in, out, blocks);
}

static void test_q8k(void) {
    float x[512];
    dan_q8k q[2];
    for (int i = 0; i < 512; i++) x[i] = (float)rnd() * (i < 256 ? 1e3f : 1e-3f);
    x[17] = -2e3f;   /* the greatest by modulus: becomes -127 (as in ggml, the sign goes to the scale) */
    dan_vk_q8k_quantize(x, q, 512);
    for (int b = 0; b < 2; b++) {
        for (int j = 0; j < 16; j++) {
            int s = 0;
            for (int i = 0; i < 16; i++) s += q[b].qs[16 * j + i];
            CHECK(q[b].bsums[j] == s, "Q8_K: сумма части %d", j);
        }
        for (int i = 0; i < 256; i++)
            CHECK(fabsf(q[b].d * q[b].qs[i] - x[256 * b + i]) <= fabsf(q[b].d) * 0.5001f, "Q8_K: число %d", i);
    }
    CHECK(q[0].qs[17] == -127 && q[0].d > 0, "Q8_K: наибольшее %d", q[0].qs[17]);
    for (int i = 0; i < 256; i++) x[i] = i % 3 ? 1e-41f : 0.0f;   /* the scale does not fit a float */
    for (int i = 256; i < 512; i++) x[i] = 0.0f;
    dan_vk_q8k_quantize(x, q, 512);
    for (int b = 0; b < 2; b++) {
        CHECK(q[b].d == 0.0f, "Q8_K: нулевой блок");
        for (int i = 0; i < 256; i++) CHECK(q[b].qs[i] == 0, "Q8_K: нулевой блок");
    }
}

/* the matrix by several vectors of every variant: the bits of the matrix by each vector */
static void kv_matmul(int f, const uint8_t *w, const float *x, int64_t rows, int64_t cols, int64_t nv, const float *expect) {
    uint8_t *xq = kv_quantize_x(f, x, cols * nv);
    float *y = malloc((size_t)(rows * nv) * 4 + 4);
    static const int64_t tiles[] = {1, 5000, 1 << 30};
    for (int v = 0; v < nvariants; v++)
        for (int ti = 0; ti < 3; ti++) {
            char buf[32];
            snprintf(buf, sizeof buf, "%lld", (long long)tiles[ti]);
#ifdef _WIN32
            _putenv_s("DAN_YADRA_PLITKA", buf);
#else
            setenv("DAN_YADRA_PLITKA", buf, 1);
#endif
            dan_vk_cur = variants[v];
            memset(y, 0xff, (size_t)(rows * nv) * 4);
            dan_vk_matmul_fmt(f, w, xq, y, rows, cols, nv);
            CHECK(memcmp(y, expect, (size_t)(rows * nv) * 4) == 0, "%s matmul %s, векторов %d, плитка %lld", variants[v]->name,
                  kv_names[f], (int)nv, (long long)tiles[ti]);
        }
#ifdef _WIN32
    _putenv_s("DAN_YADRA_PLITKA", "");
#else
    unsetenv("DAN_YADRA_PLITKA");
#endif
    free(xq);
    free(y);
}

static void test_kvanty(void) {
    rng = 88172645463325252ull;   /* the same inputs whatever the tests before took */
    /* the numbers of the blocks: those of ggml (hashes in sreda/yadra_etalon.h) */
    uint64_t h[ETALON_KVANT_FORMATS];
    etalon_kvant_hashes(kv_dequant, h);
    for (int f = 0; f < ETALON_KVANT_FORMATS; f++)
        CHECK(h[f] == etalon_kvanty_ggml[f], "%s: числа блоков не те, что даёт ggml (%016llx)", kv_names[f], (unsigned long long)h[f]);
    uint64_t h2[ETALON_KVANT2_FORMATS];
    etalon_kvant2_hashes(kv_dequant2, h2);
    for (int f = 0; f < ETALON_KVANT2_FORMATS; f++)
        CHECK(h2[f] == etalon_kvanty2_ggml[f], "%s: числа блоков не те, что даёт ggml (%016llx)", kv_names[DAN_FMT_Q2K + f],
              (unsigned long long)h2[f]);
    test_q8k();
    enum { MAXB = 33 };
    static const int64_t nb[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 14, 16, 17, 33};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f];
        uint8_t *w = malloc((size_t)(MAXB * bytes));
        float *x = malloc((size_t)(4 * MAXB * n) * 4), *wd = malloc((size_t)(MAXB * n) * 4);
        double *xd = malloc((size_t)(MAXB * n) * 8);
        for (int pass = 0; pass < 3; pass++) {
            kv_blocks(f, w, MAXB, 555 + (uint64_t)(10 * f + pass));
            /* the formats after Q6_K and the other 3 vectors take numbers not from rnd: the inputs of kv_hash stay */
            for (int64_t i = 0; i < 4 * MAXB * n; i++)
                x[i] = (float)(kv_rnd(f, i < MAXB * n) * (pass == 1 ? 1e4 : 1.0));
            if (pass == 2) for (int64_t i = 0; i < n; i++) x[i] = 0.0f;   /* a zero block of the vector */
            /* 4 vectors one after another, as the matrix by vectors has them; the first - for the row kernel */
            uint8_t *xq = kv_quantize_x(f, x, 4 * MAXB * n);
            kv_dequant(f, w, wd, MAXB);
            kv_dequantize_x(f, xq, xd, MAXB * n);
            for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
                float r = dan_vk_fmt_dot(&dan_vk_plain, f, w, xq, nb[s]);
                double e = 0, mag = 0;
                for (int64_t i = 0; i < nb[s] * n; i++) { e += (double)wd[i] * xd[i]; mag += fabs((double)wd[i] * xd[i]); }
                CHECK(fabs(r - e) <= 1e-5 * mag + 1e-30, "%s, %d блоков: %.9g, а через деквантование %.9g", kv_names[f],
                      (int)nb[s], r, e);
                /* the vectors MAXB blocks apart, nb[s] blocks of each */
                int64_t xs = MAXB * dan_vk_fmt_xbytes[f];
                float r4[4];
                for (int k = 0; k < 4; k++) r4[k] = dan_vk_fmt_dot(&dan_vk_plain, f, w, xq + k * xs, nb[s]);
                for (int v = 0; v < nvariants; v++) {
                    CHECK(same32(dan_vk_fmt_dot(variants[v], f, w, xq, nb[s]), r), "%s %s, %d блоков", variants[v]->name,
                          kv_names[f], (int)nb[s]);
                    if (variants[v]->dot4[f]) {
                        float o[4];
                        variants[v]->dot4[f](w, xq, xs, nb[s], o);
                        for (int k = 0; k < 4; k++)
                            CHECK(same32(o[k], r4[k]), "%s %s на 4 вектора, вектор %d, %d блоков: %.9g и %.9g", variants[v]->name,
                                  kv_names[f], k, (int)nb[s], o[k], r4[k]);
                    }
                }
            }
            free(xq);
        }
        free(w); free(x); free(wd); free(xd);
    }
    /* matrices on the pool: every variant the bits of C; the results go into
       kv_hash, which test_yadra.py compares for DAN_POTOKI = 1..8 */
    int64_t rows = 600, cols = 2304;
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f], blocks = rows * cols / n;
        int64_t NV = 11;   /* vectors of the matrix by vectors: 4 + 4 + 3 */
        uint8_t *w = malloc((size_t)(blocks * bytes));
        float *x = malloc((size_t)(cols * NV) * 4), *y1 = malloc((size_t)rows * 4), *y2 = malloc((size_t)rows * 4);
        float *ys = malloc((size_t)(rows * NV) * 4);
        float *wd = malloc((size_t)(rows * cols) * 4);
        double *xd = malloc((size_t)cols * 8);
        kv_blocks(f, w, blocks, 777 + (uint64_t)f);
        /* scales of a usual size, so that the rows are compared with a tolerance */
        for (int64_t b = 0; b < blocks; b++) {
            double r1 = kv_rnd(f, 1), r2 = kv_rnd(f, 1);
            uint16_t hd = dan_f32_to_f16((float)(0.001 + 0.01 * fabs(r1))), hm = dan_f32_to_f16((float)(0.01 * fabs(r2)));
            memcpy(w + b * bytes + kv_doff(f), &hd, 2);
            if (kv_moff(f) >= 0) memcpy(w + b * bytes + kv_moff(f), &hm, 2);
        }
        for (int64_t i = 0; i < cols * NV; i++) x[i] = (float)kv_rnd(f, i < cols);
        void *xq = kv_quantize_x(f, x, cols);
        kv_dequant(f, w, wd, blocks);
        kv_dequantize_x(f, xq, xd, cols);
        dan_vk_cur = &dan_vk_plain;
        dan_vk_matvec_fmt(f, w, xq, y1, rows, cols);
        for (int64_t i = 0; i < rows; i++) {
            double e = 0, mag = 0, e0 = 0;
            for (int64_t j = 0; j < cols; j++) {
                e += (double)wd[i * cols + j] * xd[j];
                mag += fabs((double)wd[i * cols + j] * xd[j]);
                e0 += (double)wd[i * cols + j] * x[j];
            }
            CHECK(fabs(y1[i] - e) <= 1e-5 * mag, "%s матрица, строка %d: %g и %g", kv_names[f], (int)i, y1[i], e);
            /* and the vector before quantization: the error of 8 bits */
            CHECK(fabs(y1[i] - e0) <= 0.02 * mag, "%s матрица, строка %d: %g и %g без квантования", kv_names[f], (int)i, y1[i], e0);
        }
        for (int v = 0; v < nvariants; v++) {
            dan_vk_cur = variants[v];
            dan_vk_matvec_fmt(f, w, xq, y2, rows, cols);
            CHECK(memcmp(y1, y2, (size_t)rows * 4) == 0, "%s matvec %s", variants[v]->name, kv_names[f]);
        }
        /* the matrix by NV vectors: for each vector the bits of the matrix by it */
        for (int64_t k = 0; k < NV; k++) {
            void *xk = kv_quantize_x(f, x + k * cols, cols);
            dan_vk_cur = &dan_vk_plain;
            dan_vk_matvec_fmt(f, w, xk, ys + k * rows, rows, cols);
            free(xk);
        }
        kv_matmul(f, w, x, rows, cols, NV, ys);
        kv_matmul(f, w, x, rows, cols, 1, ys);
        if (f < 4) kv_hash = etalon_fnv(kv_hash, y1, (size_t)rows * 4);
        else if (f < DAN_FMT_Q2K) kv_hash2 = etalon_fnv(kv_hash2, ys, (size_t)(rows * NV) * 4);
        else kv_hash3 = etalon_fnv(kv_hash3, ys, (size_t)(rows * NV) * 4);
        free(xq); free(w); free(x); free(y1); free(y2); free(ys); free(wd); free(xd);
    }
    /* recorded on x86-64 and checked on ARM (qemu): the same on every processor */
    CHECK(kv_hash == 0x0ac57fdfafdce8d2ull, "матрицы весов llama.cpp: биты не те, что были записаны (%016llx)",
          (unsigned long long)kv_hash);
    CHECK(kv_hash2 == 0x5058fce2fff5086aull, "матрицы весов Q5_0, Q4_1, Q5_1, Q8_0: биты не те, что были записаны (%016llx)",
          (unsigned long long)kv_hash2);
    CHECK(kv_hash3 == 0xd9118f424316faf7ull, "матрицы весов Q2_K, Q3_K, IQ4_XS: биты не те, что были записаны (%016llx)",
          (unsigned long long)kv_hash3);
}


/* rows by vectors: the unpacking of every variant gives the bytes of plain C,
   the kernels of every variant the bits of the row kernel of C for every row
   and vector (blocks of the tail, the last group of blocks of 32 not full),
   the matrix by vectors - for numbers of rows and vectors not multiple of the
   kernel's */
static void test_rn(void) {
    enum { MAXB = 33, RMAX = 8, VMAX = 8 };
    static const int64_t nb[] = {1, 2, 7, 8, 9, 15, 16, 17, 33};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f];
        int64_t units = dan_vk_rn_units(f, MAXB), us = units * DAN_VK_RN_UNIT, xs = MAXB * dan_vk_fmt_xbytes[f];
        uint8_t *w = malloc((size_t)(RMAX * MAXB * bytes)), *u1 = malloc((size_t)(RMAX * us)), *u2 = malloc((size_t)(RMAX * us));
        uint8_t *xp = malloc((size_t)(VMAX * us));
        float *x = malloc((size_t)(VMAX * MAXB * n) * 4);
        kv_blocks(f, w, RMAX * MAXB, 991 + (uint64_t)f);
        for (int64_t i = 0; i < VMAX * MAXB * n; i++) x[i] = (float)(kv_rnd(f, 0) * (i % 7 == 3 ? 100.0 : 1.0));
        uint8_t *xq = kv_quantize_x(f, x, VMAX * MAXB * n);
        for (int v = 0; v < nvariants; v++) {
            const dan_vk_table *t = variants[v];
            if (!t->rn[f]) continue;
            int R = t->rn_rows, NV = t->rn_vecs;
            for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
                int64_t b = nb[s];
                for (int r = 0; r < R; r++) {
                    memset(u1 + r * us, 0x55, (size_t)us);
                    memset(u2 + r * us, 0x55, (size_t)us);
                    t->unpack(f, w + r * MAXB * bytes, b, u1 + r * us, t->rn_mode);
                    dan_vk_rn_unpack_c(f, w + r * MAXB * bytes, b, u2 + r * us, t->rn_mode);
                    CHECK(memcmp(u1 + r * us, u2 + r * us, (size_t)(dan_vk_rn_units(f, b) * DAN_VK_RN_UNIT)) == 0,
                          "%s: строки %s развёрнуты не так, как в C (%d блоков)", t->name, kv_names[f], (int)b);
                }
                for (int k = 0; k < NV; k++) dan_vk_rn_prep(f, xq + k * xs, b, xp + k * us, t->rn_mode);
                float out[64];
                t->rn[f](u1, us, xp, us, b, out);
                for (int r = 0; r < R; r++)
                    for (int k = 0; k < NV; k++) {
                        float e = dan_vk_fmt_dot(&dan_vk_plain, f, w + r * MAXB * bytes, xq + k * xs, b);
                        CHECK(same32(out[r * NV + k], e), "%s %s строк на векторы: строка %d, вектор %d, %d блоков: %.9g и %.9g",
                              t->name, kv_names[f], r, k, (int)b, out[r * NV + k], e);
                    }
            }
        }
        free(xq); free(w); free(u1); free(u2); free(xp); free(x);
    }
    /* the matrix: rows and vectors not multiple of the kernel's, a group of blocks of 32 not full */
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t n = dan_vk_fmt_n[f], rows = 23, cols = n == 32 ? 13 * 32 : 3 * 256, bytes = dan_vk_fmt_bytes[f];
        int64_t blocks = rows * cols / n;
        for (int64_t nv = 1; nv <= 9; nv += 4) {
            uint8_t *w = malloc((size_t)(blocks * bytes));
            float *x = malloc((size_t)(cols * nv) * 4), *ys = malloc((size_t)(rows * nv) * 4);
            kv_blocks(f, w, blocks, 4242 + (uint64_t)(f * 10 + nv));
            for (int64_t i = 0; i < cols * nv; i++) x[i] = (float)kv_rnd(f, 0);
            for (int64_t k = 0; k < nv; k++) {
                void *xk = kv_quantize_x(f, x + k * cols, cols);
                dan_vk_cur = &dan_vk_plain;
                dan_vk_matvec_fmt(f, w, xk, ys + k * rows, rows, cols);
                free(xk);
            }
            kv_matmul(f, w, x, rows, cols, nv, ys);
            free(w); free(x); free(ys);
        }
    }
}

/* раскладка строк по 16 (поток 26А): раскладка каждого варианта - байты
   образца на C, ядро каждого варианта и формата - биты ядра строки C для
   каждой строки и вектора (блоки хвоста, ровно 8, 8 и хвост; числа вектора
   до 100 раз больше обычных и нулевой блок), матрица - при числе строк и
   векторов, не кратном ядру, и строки длиннее 8 блоков */
static void test_tl(void) {
    enum { MAXB = 33, ROWS = 16 * DAN_VK_TL_R, NV = DAN_VK_TL_V };
    static const int64_t nb[] = {1, 2, 7, 8, 9, 15, 16, 17, 33};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f], tu = dan_vk_tl_size(f), xu = dan_vk_tl_xu(f);
        int64_t xb = MAXB * dan_vk_fmt_xbytes[f];
        uint8_t *w = malloc((size_t)(ROWS * MAXB * bytes)), *t1 = malloc((size_t)(MAXB * DAN_VK_TL_R * tu));
        uint8_t *t2 = malloc((size_t)tu), *xp = malloc((size_t)(NV * MAXB * xu));
        float *x = malloc((size_t)(NV * MAXB * n) * 4);
        kv_blocks(f, w, ROWS * MAXB, 2626 + (uint64_t)f);
        for (int64_t i = 0; i < NV * MAXB * n; i++) x[i] = (float)(rnd2() * (i % 7 == 3 ? 100.0 : 1.0));
        for (int64_t i = 0; i < n; i++) x[MAXB * n + 2 * n + i] = 0.0f;   /* нулевой блок */
        uint8_t *xq = kv_quantize_x(f, x, NV * MAXB * n);
        for (int v = 0; v < nvariants; v++) {
            const dan_vk_table *t = variants[v];
            if (!t->tl[f]) continue;
            for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
                int64_t b = nb[s];
                for (int64_t bl = 0; bl < b; bl++)
                    for (int h = 0; h < DAN_VK_TL_R; h++) {
                        const uint8_t *p[16];
                        for (int i = 0; i < 16; i++) p[i] = w + ((16 * h + i) * MAXB + bl) * bytes;
                        uint8_t *o = t1 + (bl * DAN_VK_TL_R + h) * tu;
                        memset(o, 0x55, (size_t)tu);
                        memset(t2, 0xAA, (size_t)tu);
                        t->tl_unpack(f, p, o);
                        dan_vk_tl_unpack_c(f, p, t2);
                        CHECK(memcmp(o, t2, (size_t)tu) == 0, "%s: строки %s разложены не так, как в C (блок %d)", labels[v], kv_names[f], (int)bl);
                    }
                for (int k = 0; k < NV; k++) dan_vk_tl_prep(f, xq + k * xb, b, xp + k * b * xu);
                float out[ROWS * NV];
                t->tl[f](t1, xp, b * xu, b, out);
                for (int r = 0; r < ROWS; r++)
                    for (int k = 0; k < NV; k++) {
                        float e = dan_vk_fmt_dot(&dan_vk_plain, f, w + r * MAXB * bytes, xq + k * xb, b);
                        CHECK(same32(out[k * ROWS + r], e), "%s %s раскладка по 16: строка %d, вектор %d, %d блоков: %.9g и %.9g",
                              labels[v], kv_names[f], r, k, (int)b, out[k * ROWS + r], e);
                    }
            }
        }
        free(xq); free(w); free(t1); free(t2); free(xp); free(x);
    }
    static const int64_t rows_[] = {1, 15, 33, 70}, blocks_[] = {1, 9, 17}, nv_[] = {4, 5, 11, 13};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t n = dan_vk_fmt_n[f], bytes = dan_vk_fmt_bytes[f];
        for (int ri = 0; ri < 4; ri++)
            for (int bi = 0; bi < 3; bi++) {
                int64_t rows = rows_[ri], cols = blocks_[bi] * n, nv = nv_[(ri + bi) % 4], blocks = rows * cols / n;
                uint8_t *w = malloc((size_t)(blocks * bytes));
                float *x = malloc((size_t)(cols * nv) * 4), *ys = malloc((size_t)(rows * nv) * 4), *y = malloc((size_t)(rows * nv) * 4);
                kv_blocks(f, w, blocks, 2727 + (uint64_t)(f * 100 + ri * 10 + bi));
                for (int64_t i = 0; i < cols * nv; i++) x[i] = (float)rnd2();
                uint8_t *xq = kv_quantize_x(f, x, cols * nv);
                int64_t xs = cols / n * dan_vk_fmt_xbytes[f];
                for (int64_t k = 0; k < nv; k++)
                    for (int64_t i = 0; i < rows; i++)
                        ys[k * rows + i] = dan_vk_fmt_dot(&dan_vk_plain, f, w + i * (cols / n) * bytes, xq + k * xs, cols / n);
                for (int v = 0; v < nvariants; v++) {
                    if (!variants[v]->tl[f]) continue;
                    dan_vk_cur = variants[v];
                    memset(y, 0xff, (size_t)(rows * nv) * 4);
                    CHECK(dan_vk_matmul_tl(f, w, xq, y, rows, cols, nv), "%s %s: раскладка по 16 не взялась", labels[v], kv_names[f]);
                    for (int64_t k = 0; k < nv * rows; k++)
                        if (!same32(y[k], ys[k])) {
                            CHECK(0, "%s %s раскладка по 16, матрица: %d строк, %d блоков, %d векторов: строка %d, вектор %d: %.9g и %.9g",
                                  labels[v], kv_names[f], (int)rows, (int)(cols / n), (int)nv, (int)(k % rows), (int)(k / rows), y[k], ys[k]);
                            break;
                        }
                }
                free(w); free(x); free(ys); free(y); free(xq);
            }
    }
}

/* матрица на вектор: каждый вариант - биты ядра строки C при любом числе блоков строки (одни
   блоки хвоста, ровно 8, 8 и хвост) */
static void test_matvec_bloki(void) {
    static const int64_t nb[] = {1, 2, 3, 5, 6, 7, 8, 9, 14, 16, 17, 35};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f];
        for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
            int64_t blocks = nb[s], rows = 8 * 9 + 5, cols = blocks * n;
            uint8_t *w = malloc((size_t)(rows * blocks * bytes));
            float *x = malloc((size_t)cols * 4), *ys = malloc((size_t)rows * 4), *y = malloc((size_t)rows * 4);
            kv_blocks(f, w, rows * blocks, 9090 + (uint64_t)(f * 100 + (int64_t)s));
            for (int64_t i = 0; i < cols; i++) x[i] = (float)(rnd2() * (i % 7 == 2 ? 30.0 : 1.0));
            uint8_t *xq = kv_quantize_x(f, x, cols);
            for (int64_t i = 0; i < rows; i++) ys[i] = dan_vk_fmt_dot(&dan_vk_plain, f, w + i * blocks * bytes, xq, blocks);
            for (int v = 0; v < nvariants; v++) {
                dan_vk_cur = variants[v];
                memset(y, 0xff, (size_t)rows * 4);
                if (f == DAN_FMT_Q80) dan_vk_matvec_q8(w, xq, y, rows, cols);
                else dan_vk_matvec_fmt(f, w, xq, y, rows, cols);
                for (int64_t i = 0; i < rows; i++)
                    if (!same32(y[i], ys[i])) {
                        CHECK(0, "%s %s матрица на вектор, %d блоков: строка %d: %.9g и %.9g", labels[v], kv_names[f], (int)blocks,
                              (int)i, y[i], ys[i]);
                        break;
                    }
            }
            free(w); free(x); free(ys); free(y); free(xq);
        }
    }
}

/* a matrix by many vectors (AMX and the rest): every variant the bits of
   the row kernel of C for every row and vector - rows and vectors not
   multiple of 16, blocks not multiple of 8, tiles of vectors of 2 groups
   and of all, numbers of a vector from -128 to 127 and large scales */
static void test_mnogo(void) {
    static const int64_t rows_[] = {1, 16, 17, 40}, blocks_[] = {1, 8, 9, 17}, nv_[] = {16, 17, 33, 48};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t n = dan_vk_fmt_n[f], bytes = dan_vk_fmt_bytes[f];
        for (int ri = 0; ri < 4; ri++)
            for (int bi = 0; bi < 4; bi++) {
                int64_t rows = rows_[ri], cols = blocks_[bi] * n, nv = nv_[(ri + bi) % 4], blocks = rows * cols / n;
                uint8_t *w = malloc((size_t)(blocks * bytes));
                float *x = malloc((size_t)(cols * nv) * 4), *ys = malloc((size_t)(rows * nv) * 4);
                float *y = malloc((size_t)(rows * nv) * 4);
                kv_blocks(f, w, blocks, 6060 + (uint64_t)(f * 100 + ri * 10 + bi));
                for (int64_t i = 0; i < cols * nv; i++) x[i] = (float)(rnd2() * (i % 5 == 1 ? 1000.0 : 1.0));
                uint8_t *xq = kv_quantize_x(f, x, cols * nv);
                int64_t xs = cols / n * dan_vk_fmt_xbytes[f];
                for (int64_t k = 0; k < nv; k++)
                    for (int64_t i = 0; i < rows; i++)
                        ys[k * rows + i] = dan_vk_fmt_dot(&dan_vk_plain, f, w + i * (cols / n) * bytes, xq + k * xs, cols / n);
                for (int v = 0; v < nvariants; v++)
                    for (int ti = 0; ti < 2; ti++) {
#ifdef _WIN32
                        _putenv_s("DAN_YADRA_PLITKA", ti ? "" : "1");
#else
                        if (ti) unsetenv("DAN_YADRA_PLITKA");
                        else setenv("DAN_YADRA_PLITKA", "1", 1);
#endif
                        dan_vk_cur = variants[v];
                        memset(y, 0xff, (size_t)(rows * nv) * 4);
                        dan_vk_matmul_fmt(f, w, xq, y, rows, cols, nv);
                        for (int64_t k = 0; k < nv * rows; k++)
                            if (!same32(y[k], ys[k])) {
                                CHECK(0, "%s %s на много векторов: %d строк, %d блоков, %d векторов, плитка %s: строка %d, вектор %d: %.9g и %.9g",
                                      labels[v], kv_names[f], (int)rows, (int)(cols / n), (int)nv, ti ? "обычная" : "малая",
                                      (int)(k % rows), (int)(k / rows), y[k], ys[k]);
                                break;
                            }
                    }
#ifdef _WIN32
                _putenv_s("DAN_YADRA_PLITKA", "");
#else
                unsetenv("DAN_YADRA_PLITKA");
#endif
                free(w); free(x); free(ys); free(y); free(xq);
            }
    }
}

/* small batches: the kernel of a row by 1..8 vectors of every variant - the
   bits of the row kernel of C (blocks of the tail, a zero block); the
   matrix by 2..9 vectors - for every row and vector */
static void test_malye(void) {
    enum { MAXB = 17, VMAX = 9 };
    static const int64_t nb[] = {1, 7, 8, 9, 16, 17};
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t bytes = dan_vk_fmt_bytes[f], n = dan_vk_fmt_n[f];
        int64_t us = dan_vk_rn_units(f, MAXB) * DAN_VK_RN_UNIT, xs = MAXB * dan_vk_fmt_xbytes[f];
        uint8_t *w = malloc((size_t)(MAXB * bytes)), *xp = malloc((size_t)(VMAX * us));
        float *x = malloc((size_t)(VMAX * MAXB * n) * 4);
        kv_blocks(f, w, MAXB, 7070 + (uint64_t)f);
        for (int64_t i = 0; i < VMAX * MAXB * n; i++) x[i] = (float)(rnd2() * (i % 3 == 0 ? 50.0 : 1.0));
        for (int64_t i = 0; i < n; i++) x[2 * MAXB * n + 3 * n + i] = 0.0f;   /* a zero block */
        uint8_t *xq = kv_quantize_x(f, x, VMAX * MAXB * n);
        for (int v = 0; v < nvariants; v++) {
            const dan_vk_table *t = variants[v];
            if (!t->sm || !t->sm(f, 1, NULL, 0, NULL, 0, NULL)) continue;
            for (size_t s2 = 0; s2 < sizeof nb / sizeof *nb; s2++) {
                int64_t b = nb[s2];
                for (int k = 0; k < VMAX; k++) dan_vk_rn_prep(f, xq + k * xs, b, xp + k * us, 0);
                for (int nv = 1; nv <= DAN_VK_SM_MAX; nv++) {
                    if (!t->sm(f, nv, NULL, 0, NULL, 0, NULL)) continue;
                    float out[DAN_VK_SM_MAX];
                    t->sm(f, nv, w, b, xp + (nv & 1) * us, us, out);
                    for (int k = 0; k < nv; k++) {
                        float e = dan_vk_fmt_dot(&dan_vk_plain, f, w, xq + (k + (nv & 1)) * xs, b);
                        CHECK(same32(out[k], e), "%s %s на %d векторов, вектор %d, %d блоков: %.9g и %.9g", labels[v], kv_names[f], nv, k,
                              (int)b, out[k], e);
                    }
                }
            }
        }
        free(w); free(xp); free(x); free(xq);
    }
    /* the batches up to 16 by the kernels of small batches (groups), whatever the processor */
#ifdef _WIN32
    _putenv_s("DAN_YADRA_MALYE", "16");
#else
    setenv("DAN_YADRA_MALYE", "16", 1);
#endif
    for (int f = 0; f < DAN_NFMT; f++) {
        int64_t n = dan_vk_fmt_n[f], rows = 37, cols = n == 32 ? 11 * 32 : 9 * 256, bytes = dan_vk_fmt_bytes[f];
        int64_t blocks = rows * cols / n;
        for (int64_t nv = 2; nv <= 16; nv += nv < 9 ? 1 : 7) {
            uint8_t *w = malloc((size_t)(blocks * bytes));
            float *x = malloc((size_t)(cols * nv) * 4), *ys = malloc((size_t)(rows * nv) * 4);
            kv_blocks(f, w, blocks, 8080 + (uint64_t)(f * 10 + nv));
            for (int64_t i = 0; i < cols * nv; i++) x[i] = (float)rnd2();
            for (int64_t k = 0; k < nv; k++) {
                void *xk = kv_quantize_x(f, x + k * cols, cols);
                dan_vk_cur = &dan_vk_plain;
                dan_vk_matvec_fmt(f, w, xk, ys + k * rows, rows, cols);
                free(xk);
            }
            kv_matmul(f, w, x, rows, cols, nv, ys);
            free(w); free(x); free(ys);
        }
    }
#ifdef _WIN32
    _putenv_s("DAN_YADRA_MALYE", "");
#else
    unsetenv("DAN_YADRA_MALYE");
#endif
}

static void test_matvec(void) {
    int64_t rows = 517, cols = 300;
    float *w = malloc(rows * cols * 4), *x = malloc(cols * 4), *y1 = malloc(rows * 4), *y2 = malloc(rows * 4);
    for (int64_t i = 0; i < rows * cols; i++) w[i] = (float)rnd();
    for (int64_t i = 0; i < cols; i++) x[i] = (float)rnd();
    for (int64_t i = 0; i < rows; i++) y1[i] = dan_vk_dot_f32_c(w + i * cols, x, cols);
    for (int v = 0; v < nvariants; v++) {
        dan_vk_cur = variants[v];
        dan_vk_matvec_f32(w, x, y2, rows, cols);
        CHECK(memcmp(y1, y2, rows * 4) == 0, "%s matvec_f32", variants[v]->name);
    }
    free(w); free(x); free(y1); free(y2);
}

static void test_f16_kernels(void) {
    static const int64_t sizes[] = {0, 1, 63, 64, 65, 1023, 1024, 1025, 3000, 5000};
    enum { N = 5000 };
    uint16_t *a = malloc(N * 2), *b = malloc(N * 2);
    float *fa = malloc(N * 4), *fb = malloc(N * 4), *x = malloc(N * 4);
    for (int i = 0; i < N; i++) {
        a[i] = dan_f32_to_f16((float)rnd() * 4); b[i] = dan_f32_to_f16((float)rnd());
        fa[i] = dan_f16_to_f32(a[i]); fb[i] = dan_f16_to_f32(b[i]); x[i] = (float)rnd();
    }
    for (int v = 0; v < nvariants; v++) {
        dan_vk_cur = variants[v];
        float h[N];
        variants[v]->h2f(a, h, N);
        CHECK(memcmp(h, fa, sizeof h) == 0, "%s h2f", variants[v]->name);
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            int64_t n = sizes[s];
            CHECK(same32(dan_vk_dot_f16(a, b, n), dan_vk_dot_f32_c(fa, fb, n)), "%s dot_f16 n = %d", variants[v]->name, (int)n);
            CHECK(same32(dan_vk_dot_h(a, NULL, x, n), dan_vk_dot_f32_c(fa, x, n)), "%s dot_f16_f32 n = %d", variants[v]->name, (int)n);
        }
        /* weights 40 x 125 in half precision by a float vector */
        float y[40];
        dan_vk_matvec_f16(a, x, y, 40, 125);
        for (int i = 0; i < 40; i++) CHECK(same32(y[i], dan_vk_dot_f32_c(fa + i * 125, x, 125)), "%s matvec_f16", variants[v]->name);
    }
    uint16_t back[N];
    dan_vk_f2h(fa, back, N);
    CHECK(memcmp(back, a, sizeof back) == 0, "f2h");
    free(a); free(b); free(fa); free(fb); free(x);
}

/* «приближённо»: close to the exact result */
static void test_fast(void) {
    enum { N = 4099 };
    float *a = malloc(N * 4), *b = malloc(N * 4);
    double *c = malloc(N * 8), *d = malloc(N * 8);
    for (int i = 0; i < N; i++) { a[i] = (float)rnd(); b[i] = (float)rnd(); c[i] = rnd(); d[i] = rnd(); }
    static const int64_t sizes[] = {0, 1, 5, 31, 64, 100, 1000, 4099};
    for (int v = 0; v < nvariants; v++) {
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            int64_t n = sizes[s];
            double e32 = 0, e64 = 0;
            for (int64_t i = 0; i < n; i++) { e32 += (double)a[i] * b[i]; e64 += c[i] * d[i]; }
            CHECK(fabs(variants[v]->dot_f32_fast(a, b, n) - e32) <= 1e-4 * (1 + n), "%s dot_f32_fast", variants[v]->name);
            CHECK(fabs(variants[v]->dot_f64_fast(c, d, n) - e64) <= 1e-12 * (1 + n), "%s dot_f64_fast", variants[v]->name);
        }
        int64_t m = 37, n = 75, k = 41;
        float A[37 * 41], B[41 * 75], C1[37 * 75], C2[37 * 75];
        double A2[37 * 41], B2[41 * 75], D1[37 * 75], D2[37 * 75];
        for (int i = 0; i < m * k; i++) { A[i] = (float)rnd(); A2[i] = rnd(); }
        for (int i = 0; i < k * n; i++) { B[i] = (float)rnd(); B2[i] = rnd(); }
        dan_vk_mm_f32_c(A, B, C1, m, n, k, 0, n);
        dan_vk_mm_f64_c(A2, B2, D1, m, n, k, 0, n);
        dan_vk_cur = variants[v];
        dan_vk_matmat_f32_fast(A, B, C2, m, n, k);
        dan_vk_matmat_f64_fast(A2, B2, D2, m, n, k);
        for (int i = 0; i < m * n; i++) {
            CHECK(fabsf(C1[i] - C2[i]) <= 1e-5f * k, "%s matmat_f32_fast", variants[v]->name);
            CHECK(fabs(D1[i] - D2[i]) <= 1e-13 * k, "%s matmat_f64_fast", variants[v]->name);
        }
        float y1[37], y2[37];
        for (int i = 0; i < m; i++) y1[i] = dan_vk_dot_f32_c(A + i * k, B, k);
        dan_vk_matvec_f32_fast(A, B, y2, m, k);
        for (int i = 0; i < m; i++) CHECK(fabsf(y1[i] - y2[i]) <= 1e-5f * k, "%s matvec_f32_fast", variants[v]->name);
    }
    free(a); free(b); free(c); free(d);
}

static int64_t *marks;
static void mark(void *ctx, int64_t b, int64_t e) {
    (void)ctx;
    for (int64_t i = b; i < e; i++) marks[i]++;
}
static void count4(void *ctx, int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) ((int *)ctx)[i]++;
}
static void nested(void *ctx, int64_t b, int64_t e) {
    (void)ctx;
    for (int64_t i = b; i < e; i++) {   /* runs whole on this thread */
        int c[4] = {0, 0, 0, 0};
        dan_pool_run(4, 1, count4, c);
        marks[i] = c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1;
    }
}

static int saw_inside;
static void check_inside(void *ctx, int64_t b, int64_t e) {
    (void)ctx; (void)b; (void)e;
    if (dan_pool_inside) saw_inside++;
}
static void few_parts(void *ctx, int64_t b, int64_t e) {
    (void)ctx;
    for (int64_t i = b; i < e; i++) dan_pool_run(1000, 1, check_inside, NULL);   /* inside: one whole call */
}

/* rows of floats by several vectors (матвеки): the bits of dot_f32 of each
   row and vector in every variant (and with DAN_YADRA_STROKI=0); a matrix of
   a few rows by a matrix (матмат of the weights of a group of heads by the
   values): the bits of plain C */
static uint64_t rng4 = 1618033988749894ull;
static double rnd4(void) {
    rng4 ^= rng4 << 13; rng4 ^= rng4 >> 7; rng4 ^= rng4 << 17;
    return (double)(rng4 >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static void test_stroki(void) {
    static const int64_t colss[] = {1, 7, 63, 64, 65, 128, 130, 200};
    static const int64_t nvs[] = {1, 2, 3, 4, 5, 7, 8, 9, 16, 17};
    static const int64_t rowss[] = {1, 5, 33};
    for (size_t ci = 0; ci < sizeof colss / sizeof *colss; ci++)
        for (size_t ri = 0; ri < sizeof rowss / sizeof *rowss; ri++)
            for (size_t ni = 0; ni < sizeof nvs / sizeof *nvs; ni++) {
                int64_t cols = colss[ci], rows = rowss[ri], nv = nvs[ni];
                float *w = malloc((size_t)(rows * cols) * 4), *x = malloc((size_t)(nv * cols) * 4);
                float *y = malloc((size_t)(rows * nv) * 4), *e = malloc((size_t)(rows * nv) * 4);
                for (int64_t i = 0; i < rows * cols; i++) w[i] = (float)(rnd4() * (i % 5 == 0 ? 1e3 : 1.0));
                for (int64_t i = 0; i < nv * cols; i++) x[i] = (float)rnd4();
                /* a row of zeros by a vector of negative numbers: the sum of -0 products */
                for (int64_t i = 0; i < cols; i++) { w[i] = 0.0f; x[i] = -1.0f - (float)i; }
                for (int64_t v = 0; v < nv; v++)
                    for (int64_t i = 0; i < rows; i++) e[v * rows + i] = dan_vk_dot_f32_c(w + i * cols, x + v * cols, cols);
                for (int v = 0; v < nvariants; v++)
                    for (int off = 0; off < 2; off++) {
                        dan_vk_stroki = !off;   /* as DAN_YADRA_STROKI=0 */
                        dan_vk_cur = variants[v];
                        memset(y, 0xff, (size_t)(rows * nv) * 4);
                        dan_vk_rows_f32(w, x, y, rows, cols, nv);
                        CHECK(memcmp(y, e, (size_t)(rows * nv) * 4) == 0, "%s матвеки %lldx%lld на %lld векторов%s", labels[v],
                              (long long)rows, (long long)cols, (long long)nv, off ? " (DAN_YADRA_STROKI=0)" : "");
                    }
                free(w); free(x); free(y); free(e);
            }
    dan_vk_stroki = 1;
    static const int64_t ns[] = {1, 15, 16, 17, 64, 100, 128, 130};
    static const int64_t ks[] = {1, 2, 37, 300};
    for (int64_t m = 1; m <= DAN_VK_FEW + 1; m++)
        for (size_t ni = 0; ni < sizeof ns / sizeof *ns; ni++)
            for (size_t ki = 0; ki < sizeof ks / sizeof *ks; ki++) {
                int64_t n = ns[ni], k = ks[ki];
                float *A = malloc((size_t)(m * k) * 4), *B = malloc((size_t)(k * n) * 4);
                float *C = malloc((size_t)(m * n) * 4), *R = malloc((size_t)(m * n) * 4);
                for (int64_t i = 0; i < m * k; i++) A[i] = (float)fabs(rnd4());
                for (int64_t i = 0; i < k * n; i++) B[i] = (float)(rnd4() * (i % 7 == 0 ? 100.0 : 1.0));
                A[0] = 0.0f;
                for (int64_t j = 0; j < n; j++) B[j] = -1.0f;   /* 0 * -1 = -0 first: the sum starts from +0 */
                dan_vk_mm_f32_c(A, B, R, m, n, k, 0, n);
                for (int v = 0; v < nvariants; v++) {
                    dan_vk_cur = variants[v];
                    memset(C, 0xff, (size_t)(m * n) * 4);
                    dan_vk_matmat_f32(A, B, C, m, n, k);
                    CHECK(memcmp(C, R, (size_t)(m * n) * 4) == 0, "%s матмат %lld строк x %lld x %lld", labels[v], (long long)m,
                          (long long)n, (long long)k);
                }
                free(A); free(B); free(C); free(R);
            }
}

static void test_pool(void) {
    static const int64_t sizes[] = {1, 2, 3, 17, 1000, 100003};
    marks = calloc(100003, sizeof(int64_t));
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int64_t n = sizes[s];
        memset(marks, 0, n * sizeof(int64_t));
        dan_pool_run(n, 1, mark, NULL);
        for (int64_t i = 0; i < n; i++) CHECK(marks[i] == 1, "пул: n = %d, элемент %d - %d раз", (int)n, (int)i, (int)marks[i]);
    }
    memset(marks, 0, 1000 * sizeof(int64_t));
    dan_pool_run(1000, 1, nested, NULL);
    for (int i = 0; i < 1000; i++) CHECK(marks[i] == 1, "пул внутри пула");
    for (int r = 0; r < 2000; r++) dan_pool_run(64, 1, mark, NULL);   /* many short jobs */
    free(marks);
    CHECK(dan_pool_threads() >= 1, "число потоков");
    /* a job with fewer parts than threads runs whole, and so does a job inside it */
    int t = dan_pool_threads();
    if (t >= 3) {
        saw_inside = 0;
        dan_pool_run(2 * 10, 10, few_parts, NULL);   /* 2 parts of 10 < t threads */
        CHECK(saw_inside == 20, "работа внутри короткой работы: %d из 20 целиком", saw_inside);
    }
}

/* sums, normalisations and softmax: the bits recorded before the variants of
   sums (sreda/yadra_etalon.h), in every variant */
static void test_sums(void) {
    static const uint64_t etalon[4] = {0xe339639362246289ull, 0xf0b1b42632833252ull, 0x27e200fb1756f484ull,
                                       0x176b0614a28ca690ull};
    static const int64_t sizes[] = {0, 1, 31, 63, 64, 65, 200, 1000, 4097};
    float a[4097];
    double c[4097];
    for (int i = 0; i < 4097; i++) { a[i] = (float)rnd() * 3; c[i] = rnd(); }
    for (int v = 0; v < nvariants; v++) {
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            int64_t n = sizes[s];
            CHECK(same32(variants[v]->sum_f32(a, n), dan_vk_sum_f32_c(a, n)), "%s sum_f32 n = %d", variants[v]->name, (int)n);
            CHECK(same64(variants[v]->sum_f64(c, n), dan_vk_sum_f64_c(c, n)), "%s sum_f64 n = %d", variants[v]->name, (int)n);
        }
        dan_vk_cur = variants[v];
        uint64_t h[4];
        etalon_hashes(h);
        CHECK(h[0] == etalon[0], "%s суммы: биты не те, что были записаны", variants[v]->name);
        CHECK(h[1] == etalon[1], "%s послойная нормировка: биты не те, что были записаны", variants[v]->name);
        CHECK(h[2] == etalon[2], "%s нормировка по среднеквадратичному: биты не те, что были записаны", variants[v]->name);
#ifdef __GLIBC__
        CHECK(h[3] == etalon[3], "%s софтмакс: биты не те, что были записаны (exp из glibc)", variants[v]->name);
#endif
    }
}

static double dan_vk_dot_f64_fast_check(void) {
    double a[1] = {1}, b[1] = {1}, c[1];
    dan_vk_matvec_f64_fast(a, b, c, 1, 1);
    return c[0];
}

static void test_norms(void) {
    enum { N = 777 };
    float x[N], y[N], w[N], b[N];
    double xs[N], ys[N];
    for (int i = 0; i < N; i++) { x[i] = (float)(rnd() * 5); w[i] = (float)rnd(); b[i] = (float)rnd(); xs[i] = rnd() * 5; }
    memcpy(y, x, sizeof x);
    dan_vk_softmax_f32(y, N);
    double s = 0;
    for (int i = 0; i < N; i++) { s += y[i]; CHECK(y[i] > 0, "софтмакс положителен"); }
    CHECK(fabs(s - 1) < 1e-5, "софтмакс: сумма %g", s);
    CHECK(y[0] / y[1] > 0 && fabs(log(y[0] / y[1]) - (x[0] - x[1])) < 1e-4, "софтмакс: отношение");
    memcpy(ys, xs, sizeof xs);
    dan_vk_softmax_f64(ys, N);
    s = 0;
    for (int i = 0; i < N; i++) s += ys[i];
    CHECK(fabs(s - 1) < 1e-12, "софтмакс f64");
    float big[3] = {1000.0f, 1000.0f, -1000.0f};
    dan_vk_softmax_f32(big, 3);
    CHECK(big[0] == 0.5f && big[2] == 0.0f, "софтмакс без переполнения");
    dan_vk_rmsnorm_f32(y, x, w, N, 1e-6f);
    double ms = 0;
    for (int i = 0; i < N; i++) ms += (double)x[i] * x[i];
    ms = 1 / sqrt(ms / N + 1e-6);
    for (int i = 0; i < N; i++) CHECK(fabs(y[i] - x[i] * ms * w[i]) < 1e-5, "rmsnorm");
    dan_vk_rmsnorm_f64(ys, xs, NULL, N, 1e-9);
    ms = 0;
    for (int i = 0; i < N; i++) ms += ys[i] * ys[i];
    CHECK(fabs(ms / N - 1) < 1e-9, "rmsnorm f64");
    dan_vk_layernorm_f32(y, x, NULL, NULL, N, 0.0f);
    double mean = 0, var = 0;
    for (int i = 0; i < N; i++) mean += y[i];
    mean /= N;
    for (int i = 0; i < N; i++) var += (y[i] - mean) * (y[i] - mean);
    CHECK(fabs(mean) < 1e-5 && fabs(var / N - 1) < 1e-4, "layernorm: %g %g", mean, var / N);
    dan_vk_layernorm_f32(y, x, w, b, N, 1e-5f);
    dan_vk_layernorm_f64(ys, xs, NULL, NULL, N, 0.0);
    mean = 0;
    for (int i = 0; i < N; i++) mean += ys[i];
    CHECK(fabs(mean) < 1e-9, "layernorm f64");
    {   /* double kernels used by the language */
        double w2[6] = {1, 2, 3, 4, 5, 6}, x2[3] = {1, 0, -1}, y2[2], z2[3] = {1, 1, 1};
        dan_vk_matvec_f64(w2, x2, y2, 2, 3);
        CHECK(y2[0] == -2 && y2[1] == -2, "matvec_f64");
        dan_vk_axpy_f64(z2, 2.0, x2, 3);
        CHECK(z2[0] == 3 && z2[1] == 1 && z2[2] == -1, "axpy_f64");
        float t2[3] = {1, 2, 3};
        uint16_t h2[3];
        dan_vk_f2h(t2, h2, 3);
        CHECK(dan_f16_to_f32(h2[2]) == 3.0f, "f2h");
        double r = dan_vk_dot_f64_fast_check();
        CHECK(r == 1.0, "dot_f64_fast %g", r);
    }
    float s1 = dan_vk_sum_f32(x, N);
    double e = 0;
    for (int i = 0; i < N; i++) e += x[i];
    CHECK(fabs(s1 - e) < 1e-3, "сумма");
    for (int i = 0; i < N; i++) y[i] = 1.0f;
    dan_vk_axpy_f32(y, 2.0f, x, N);
    for (int i = 0; i < N; i++) CHECK(y[i] == 1.0f + 2.0f * x[i], "axpy");
}

#include <time.h>
static double now(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

static volatile float sink;

/* reading of weights from memory: a matrix with rows of 4096 numbers that
   fills DAN_ZAMER_MB megabytes (1024 by default: more than the caches) by a
   vector, for every format; GB/s of weights (GB - 10^9 bytes), the best of
   three passes */
static void bench_weights(void) {
    const char *e = getenv("DAN_ZAMER_MB");
    long mb = e ? strtol(e, NULL, 10) : 0;
    if (mb <= 0) mb = 1024;
    size_t total = (size_t)mb << 20;
    enum { COLS = 4096, NF = 6 };
    static const char *const fmts[NF] = {"q4_0", "q4k", "q5k", "q6k", "q8_0", "f32"};
    static const int row_bytes[NF] = {COLS / 32 * 18, COLS / 256 * 144, COLS / 256 * 176, COLS / 256 * 210, COLS / 32 * 34, COLS * 4};
    uint8_t *buf = malloc(total);
    float *y = malloc((total / (COLS / 32 * 18) + 1) * 4), x[COLS];
    if (!buf || !y) { printf("нет памяти для замера весов\n"); free(buf); free(y); return; }
    uint64_t r = 12345;
    for (size_t i = 0; i + 8 <= total; i += 8) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; memcpy(buf + i, &r, 8); }
    for (int i = 0; i < COLS; i++) x[i] = (float)rnd();
    uint8_t *x8 = malloc(COLS / 32 * 34);
    dan_q8k *xk = malloc(COLS / 256 * sizeof(dan_q8k));
    dan_vk_q8_quantize(x, x8, COLS);
    dan_vk_q8k_quantize(x, xk, COLS);
    double gbs[8][NF];
    uint16_t hd = dan_f32_to_f16(0.01f);
    for (int f = 0; f < NF; f++) {
        int64_t rows = (int64_t)(total / (size_t)row_bytes[f]);
        /* scales of a usual size (random halves could be subnormal: slow on some processors) */
        if (f < 5) {
            int bb = f == 0 ? 18 : f == 1 ? 144 : f == 2 ? 176 : f == 3 ? 210 : 34;
            int64_t blocks = rows * row_bytes[f] / bb;
            for (int64_t b = 0; b < blocks; b++) {
                memcpy(buf + b * bb + (f == 3 ? 208 : 0), &hd, 2);
                if (f == 1 || f == 2) memcpy(buf + b * bb + 2, &hd, 2);
            }
        } else {
            for (size_t i = 0; i < total / 4; i++) ((float *)buf)[i] = (float)(i % 1000) * 0.001f - 0.5f;
        }
        for (int v = 0; v < nvariants; v++) {
            dan_vk_cur = variants[v];
            double best = 1e30;
            for (int pass = 0; pass < 3; pass++) {
                double t0 = now();
                if (f < 4) dan_vk_matvec_fmt(f, buf, f == 0 ? (const void *)x8 : (const void *)xk, y, rows, COLS);
                else if (f == 4) dan_vk_matvec_q8(buf, x8, y, rows, COLS);
                else dan_vk_matvec_f32((const float *)buf, x, y, rows, COLS);
                double t = now() - t0;
                if (t < best) best = t;
            }
            gbs[v][f] = (double)rows * row_bytes[f] / best * 1e-9;
        }
    }
    for (int v = 0; v < nvariants; v++) {
        printf("%-10s чтение весов (%ld МБ), ГБ/с:", labels[v], mb);
        for (int f = 0; f < NF; f++) printf(" %s %.1f;", fmts[f], gbs[v][f]);
        printf("\n");
    }
    free(buf); free(y); free(x8); free(xk);
}


/* the matrix of weights by vectors (the batch of a prompt): 4096 x 4096 by
   256 vectors, billions of multiplications per second, the kernels of rows
   by vectors and (DAN_YADRA_RN=0) of a row by 4 vectors; «замер_пакет» or
   «zamer_paket» as the first argument; DAN_ZAMER_VARIANT - только этот вариант */
static void bench_batch(void) {
    enum { ROWS = 4096, COLS = 4096, NV = 256 };
    static const int fmts[] = {DAN_FMT_Q40, DAN_FMT_Q4K, DAN_FMT_Q5K, DAN_FMT_Q6K, DAN_FMT_Q50, DAN_FMT_Q80,
                               DAN_FMT_Q2K, DAN_FMT_Q3K, DAN_FMT_IQ4XS};
    float *x = malloc((size_t)COLS * NV * 4), *y = malloc((size_t)ROWS * NV * 4);
    for (int64_t i = 0; i < (int64_t)COLS * NV; i++) x[i] = (float)rnd();
    printf("потоков: %d\n", dan_pool_threads());
    for (size_t fi = 0; fi < sizeof fmts / sizeof *fmts; fi++) {
        int f = fmts[fi];
        int64_t bytes = dan_vk_fmt_bytes[f], blocks = (int64_t)ROWS * COLS / dan_vk_fmt_n[f];
        uint8_t *w = malloc((size_t)(blocks * bytes));
        kv_blocks(f, w, blocks, 31 + (uint64_t)f);
        uint16_t hd = dan_f32_to_f16(0.01f);
        for (int64_t b = 0; b < blocks; b++) {
            memcpy(w + b * bytes + kv_doff(f), &hd, 2);
            if (f == DAN_FMT_Q4K || f == DAN_FMT_Q5K || f == DAN_FMT_Q2K) memcpy(w + b * bytes + kv_moff(f), &hd, 2);
        }
        uint8_t *xq = kv_quantize_x(f, x, (int64_t)COLS * NV);
        for (int v = 0; v < nvariants; v++) {
            if (getenv("DAN_ZAMER_VARIANT") && strcmp(getenv("DAN_ZAMER_VARIANT"), labels[v]) != 0) continue;
            dan_vk_cur = variants[v];
            /* 0 - строка на 4 вектора, 1 - строки на векторы, 2 - раскладка по 16 (если есть) */
            double best[3] = {1e30, 1e30, 1e30};
            int kinds = variants[v]->tl[f] ? 3 : 2;
            for (int pass = 0; pass < 3 * kinds; pass++) {
                int kind = pass % kinds;
#ifdef _WIN32
                _putenv_s("DAN_YADRA_RN", kind ? "1" : "0");
                _putenv_s("DAN_YADRA_RASKLADKA", kind == 2 ? "1" : "0");
#else
                setenv("DAN_YADRA_RN", kind ? "1" : "0", 1);
                setenv("DAN_YADRA_RASKLADKA", kind == 2 ? "1" : "0", 1);
#endif
                double t0 = now();
                dan_vk_matmul_fmt(f, w, xq, y, ROWS, COLS, NV);
                double t = now() - t0;
                if (t < best[kind]) best[kind] = t;
            }
            double ops = (double)ROWS * COLS * NV * 1e-9;
            printf("%-10s пакет %s: строки на векторы %.1f, строка на 4 вектора %.1f", labels[v], kv_names[f], ops / best[1], ops / best[0]);
            if (kinds == 3) printf(", раскладка по 16 %.1f", ops / best[2]);
            printf(" млрд умножений/с\n");
        }
        free(w); free(xq);
    }
#ifdef _WIN32
    _putenv_s("DAN_YADRA_RN", "");
    _putenv_s("DAN_YADRA_RASKLADKA", "");
#else
    unsetenv("DAN_YADRA_RN");
    unsetenv("DAN_YADRA_RASKLADKA");
#endif
    free(x); free(y);
}

/* small batches: a matrix of DAN_ZAMER_MB megabytes (256 by default: more
   than the caches) with rows of 4096 numbers by 1..9 vectors - the time
   divided by the time of the matrix by one vector (matvec), the best of 5;
   «zamer_malye» as the first argument */
static void bench_small(void) {
    const char *e = getenv("DAN_ZAMER_MB");
    long mb = e ? strtol(e, NULL, 10) : 0;
    if (mb <= 0) mb = 256;
    enum { COLS = 4096, NV = 9 };
    static const int fmts[] = {DAN_FMT_Q4K, DAN_FMT_Q6K, DAN_FMT_Q80};
    float *x = malloc((size_t)COLS * NV * 4);
    for (int64_t i = 0; i < (int64_t)COLS * NV; i++) x[i] = (float)rnd();
    printf("потоков: %d\n", dan_pool_threads());
    for (size_t fi = 0; fi < sizeof fmts / sizeof *fmts; fi++) {
        int f = fmts[fi];
        int64_t bytes = dan_vk_fmt_bytes[f], rb = COLS / dan_vk_fmt_n[f] * bytes, rows = ((int64_t)mb << 20) / rb;
        int64_t blocks = rows * COLS / dan_vk_fmt_n[f];
        uint8_t *w = malloc((size_t)(blocks * bytes));
        float *y = malloc((size_t)(rows * NV) * 4);
        kv_blocks(f, w, blocks, 131 + (uint64_t)f);
        uint16_t hd = dan_f32_to_f16(0.01f);
        for (int64_t b = 0; b < blocks; b++) {
            memcpy(w + b * bytes + (f == DAN_FMT_Q6K ? 208 : 0), &hd, 2);
            if (f == DAN_FMT_Q4K || f == DAN_FMT_Q5K) memcpy(w + b * bytes + 2, &hd, 2);
        }
        uint8_t *xq = kv_quantize_x(f, x, (int64_t)COLS * NV);
        for (int v = 0; v < nvariants; v++) {
            if (strcmp(labels[v], "c") == 0) continue;
            dan_vk_cur = variants[v];
            double t1 = 0;
            printf("%-10s малые пакеты %s (%ld МБ), проходов по одному вектору:", labels[v], kv_names[f], mb);
            for (int nv = 1; nv <= NV; nv++) {
                double best = 1e30;
                for (int pass = 0; pass < 5; pass++) {
                    double t0 = now();
                    if (nv == 1) dan_vk_matvec_fmt(f, w, xq, y, rows, COLS);
                    else dan_vk_matmul_fmt(f, w, xq, y, rows, COLS, nv);
                    double t = now() - t0;
                    if (t < best) best = t;
                }
                if (nv == 1) { t1 = best; printf(" (%.1f ГБ/с)", (double)blocks * bytes / best * 1e-9); }
                else printf(" %.2f", best / t1);
            }
            printf("\n");
        }
        free(w); free(y); free(xq);
    }
    free(x);
}

/* measurements: GFLOP/s by variant; «замер» or «zamer» (Windows) as the first argument */
static void bench(void) {
    enum { DN = 4096, MV = 4096, MM = 1024 };
    float *a = malloc(DN * 4), *b = malloc(DN * 4);
    float *w = malloc((size_t)MV * MV * 4), *x = malloc(MV * 4), *y = malloc(MV * 4);
    float *A = malloc(MM * MM * 4), *B = malloc(MM * MM * 4), *C = malloc(MM * MM * 4);
    uint8_t *wq = malloc((size_t)MV * MV / 32 * 34), *xq = malloc(MV / 32 * 34);
    for (int i = 0; i < DN; i++) { a[i] = (float)rnd(); b[i] = (float)rnd(); }
    for (size_t i = 0; i < (size_t)MV * MV; i++) w[i] = (float)rnd();
    for (int i = 0; i < MV; i++) x[i] = (float)rnd();
    for (int i = 0; i < MM * MM; i++) { A[i] = (float)rnd(); B[i] = (float)rnd(); }
    dan_vk_q8_quantize(w, wq, (int64_t)MV * MV);
    dan_vk_q8_quantize(x, xq, MV);
    printf("потоков: %d\n", dan_pool_threads());
    for (int v = 0; v < nvariants; v++) {
        dan_vk_cur = variants[v];
        double t0 = now();
        int reps = 20000;
        for (int r = 0; r < reps; r++) sink = dan_vk_dot_f32(a, b, DN);
        double t_dot = now() - t0;
        t0 = now();
        for (int r = 0; r < 5; r++) dan_vk_matvec_f32(w, x, y, MV, MV);
        double t_mv = (now() - t0) / 5;
        t0 = now();
        for (int r = 0; r < 5; r++) dan_vk_matvec_q8(wq, xq, y, MV, MV);
        double t_q8 = (now() - t0) / 5;
        double t_mm = 1e30;
        for (int r = 0; r < 3; r++) {   /* the best of three */
            t0 = now();
            dan_vk_matmat_f32(A, B, C, MM, MM, MM);
            if (now() - t0 < t_mm) t_mm = now() - t0;
        }
        double t_mf = 1e30;
        for (int r = 0; r < 3; r++) {
            t0 = now();
            dan_vk_matmat_f32_fast(A, B, C, MM, MM, MM);
            if (now() - t0 < t_mf) t_mf = now() - t0;
        }
        printf("%-10s приближённо: матрица на матрицу %.1f ГФлопс\n", labels[v], 2.0 * MM * MM * MM / t_mf * 1e-9);
        printf("%-10s скалярное %.1f ГФлопс; матрица на вектор %.1f ГФлопс (%.1f ГБ/с); q8 %.1f ГФлопс (%.1f ГБ/с); "
               "матрица на матрицу %.1f ГФлопс\n", labels[v], 2.0 * DN * reps / t_dot * 1e-9,
               2.0 * MV * MV / t_mv * 1e-9, 4.0 * MV * MV / t_mv * 1e-9, 2.0 * MV * MV / t_q8 * 1e-9,
               34.0 / 32 * MV * MV / t_q8 * 1e-9, 2.0 * MM * MM * MM / t_mm * 1e-9);
    }
    free(a); free(b); free(w); free(x); free(y); free(A); free(B); free(C); free(wq); free(xq);
    bench_weights();
}

/* The bits recorded in the tests are those without flushing subnormal numbers
   to zero: the environment of a Dan program does not turn FTZ/DAZ (x86) or
   FZ (ARM) on, and the test checks that nothing else did. */
static void check_subnormals(void) {
#ifdef DAN_VK_X86
    unsigned csr = _mm_getcsr();
    CHECK(!(csr & 0x8000) && !(csr & 0x40), "включены FTZ/DAZ (MXCSR %x): денормалы обнулялись бы", csr);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    CHECK(!(fpcr & (1u << 24)), "включён FZ (FPCR %llx): денормалы обнулялись бы", (unsigned long long)fpcr);
#endif
    volatile float tiny = 1e-41f;
    CHECK(tiny * 2.0f != 0.0f, "денормалы обнуляются");
}

int main(int argc, char **argv) {
    find_variants();
    check_subnormals();
    if (argc > 1 && (strcmp(argv[1], "замер") == 0 || strcmp(argv[1], "zamer") == 0)) { bench(); return 0; }
    if (argc > 1 && (strcmp(argv[1], "замер_пакет") == 0 || strcmp(argv[1], "zamer_paket") == 0)) { bench_batch(); return 0; }
    if (argc > 1 && (strcmp(argv[1], "замер_малые") == 0 || strcmp(argv[1], "zamer_malye") == 0)) { bench_small(); return 0; }
    test_f16();
    test_dot();
    test_mm();
    test_q8();
    test_quantize_ref();
    test_matvec();
    test_f16_kernels();
    test_fast();
    test_pool();
    test_norms();
    test_sums();
    test_kvanty();
    test_rn();
    test_tl();
    test_mnogo();
    test_malye();
    test_stroki();
    test_matvec_bloki();
    printf("%s варианты:", failures ? "ОШИБКИ" : "ok");
    for (int v = 0; v < nvariants; v++) printf(" %s", labels[v]);
    dan_vk_cur = NULL;
    printf("; выбран: %s; потоков: %d; хеш весов llama.cpp: %016llx %016llx %016llx\n", dan_vk_name(), dan_pool_threads(),
           (unsigned long long)kv_hash, (unsigned long long)kv_hash2, (unsigned long long)kv_hash3);
    return failures ? 1 : 0;
}
