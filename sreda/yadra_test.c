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
static int nvariants;

static void find_variants(void) {
    static const char *const names[] = {"c", "avx2", "avx512", "neon", "dotprod"};
    for (int i = 0; i < 5; i++) {
        const dan_vk_table *t;
        if (dan_vk_supported(names[i], &t)) variants[nvariants++] = t;
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

static const char *const kv_names[4] = {"Q4_0", "Q4_K", "Q5_K", "Q6_K"};

static void kv_dequant(int fmt, const uint8_t *in, float *out, int64_t blocks) {
    switch (fmt) {
    case 0: dan_vk_q40_dequantize(in, out, blocks); break;
    case 1: dan_vk_q4k_dequantize(in, out, blocks); break;
    case 2: dan_vk_q5k_dequantize(in, out, blocks); break;
    default: dan_vk_q6k_dequantize(in, out, blocks); break;
    }
}

/* the vector in 8 bits for the format: bytes of Q8_0 or blocks of Q8_K */
static void *kv_quantize_x(int fmt, const float *x, int64_t n) {
    if (fmt == 0) {
        uint8_t *q = malloc((size_t)(n / 32 * 34) + 1);
        dan_vk_q8_quantize(x, q, n);
        return q;
    }
    dan_q8k *q = malloc((size_t)(n / 256) * sizeof(dan_q8k) + 1);
    dan_vk_q8k_quantize(x, q, n);
    return q;
}
static void kv_dequantize_x(int fmt, const void *q, double *x, int64_t n) {
    if (fmt == 0) {
        for (int64_t b = 0; b < n / 32; b++) {
            const uint8_t *o = (const uint8_t *)q + b * 34;
            for (int i = 0; i < 32; i++) x[b * 32 + i] = (double)dan_vk_half(o) * (int8_t)o[2 + i];
        }
        return;
    }
    const dan_q8k *k = (const dan_q8k *)q;
    for (int64_t i = 0; i < n; i++) x[i] = (double)k[i / 256].d * k[i / 256].qs[i % 256];
}

static uint64_t kv_hash = 14695981039346656037ull;   /* of the results of the matrices: the same for any number of threads */

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

static void test_kvanty(void) {
    rng = 88172645463325252ull;   /* the same inputs whatever the tests before took */
    /* the numbers of the blocks: those of ggml (hashes in sreda/yadra_etalon.h) */
    uint64_t h[4];
    etalon_kvant_hashes(kv_dequant, h);
    for (int f = 0; f < 4; f++)
        CHECK(h[f] == etalon_kvanty_ggml[f], "%s: числа блоков не те, что даёт ggml (%016llx)", kv_names[f], (unsigned long long)h[f]);
    test_q8k();
    enum { MAXB = 33 };
    static const int64_t nb[] = {0, 1, 7, 8, 9, 16, 17, 33};
    for (int f = 0; f < 4; f++) {
        int64_t bytes = etalon_kvant_bytes[f], n = etalon_kvant_n[f];
        uint8_t *w = malloc((size_t)(MAXB * bytes));
        float *x = malloc((size_t)(MAXB * n) * 4), *wd = malloc((size_t)(MAXB * n) * 4);
        double *xd = malloc((size_t)(MAXB * n) * 8);
        for (int pass = 0; pass < 3; pass++) {
            etalon_kvant_blocks(f, w, MAXB, 555 + (uint64_t)(10 * f + pass));
            for (int64_t i = 0; i < MAXB * n; i++) x[i] = (float)(rnd() * (pass == 1 ? 1e4 : 1.0));
            if (pass == 2) for (int64_t i = 0; i < n; i++) x[i] = 0.0f;   /* a zero block of the vector */
            void *xq = kv_quantize_x(f, x, MAXB * n);
            kv_dequant(f, w, wd, MAXB);
            kv_dequantize_x(f, xq, xd, MAXB * n);
            for (size_t s = 0; s < sizeof nb / sizeof *nb; s++) {
                float r = dan_vk_fmt_dot(&dan_vk_plain, f, w, xq, nb[s]);
                double e = 0, mag = 0;
                for (int64_t i = 0; i < nb[s] * n; i++) { e += (double)wd[i] * xd[i]; mag += fabs((double)wd[i] * xd[i]); }
                CHECK(fabs(r - e) <= 1e-5 * mag + 1e-30, "%s, %d блоков: %.9g, а через деквантование %.9g", kv_names[f],
                      (int)nb[s], r, e);
                for (int v = 0; v < nvariants; v++)
                    CHECK(same32(dan_vk_fmt_dot(variants[v], f, w, xq, nb[s]), r), "%s %s, %d блоков", variants[v]->name,
                          kv_names[f], (int)nb[s]);
            }
            free(xq);
        }
        free(w); free(x); free(wd); free(xd);
    }
    /* matrices on the pool: every variant the bits of C; the results go into
       kv_hash, which test_yadra.py compares for DAN_POTOKI = 1..8 */
    int64_t rows = 600, cols = 2304;
    for (int f = 0; f < 4; f++) {
        int64_t bytes = etalon_kvant_bytes[f], n = etalon_kvant_n[f], blocks = rows * cols / n;
        uint8_t *w = malloc((size_t)(blocks * bytes));
        float *x = malloc((size_t)cols * 4), *y1 = malloc((size_t)rows * 4), *y2 = malloc((size_t)rows * 4);
        float *wd = malloc((size_t)(rows * cols) * 4);
        double *xd = malloc((size_t)cols * 8);
        etalon_kvant_blocks(f, w, blocks, 777 + (uint64_t)f);
        /* scales of a usual size, so that the rows are compared with a tolerance */
        for (int64_t b = 0; b < blocks; b++) {
            uint16_t hd = dan_f32_to_f16((float)(0.001 + 0.01 * fabs(rnd()))), hm = dan_f32_to_f16((float)(0.01 * fabs(rnd())));
            memcpy(w + b * bytes + (f == 3 ? 208 : 0), &hd, 2);
            if (f == 1 || f == 2) memcpy(w + b * bytes + 2, &hm, 2);
        }
        for (int64_t i = 0; i < cols; i++) x[i] = (float)rnd();
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
        kv_hash = etalon_fnv(kv_hash, y1, (size_t)rows * 4);
        free(xq); free(w); free(x); free(y1); free(y2); free(wd); free(xd);
    }
    /* recorded on x86-64 and checked on ARM (qemu): the same on every processor */
    CHECK(kv_hash == 0x0ac57fdfafdce8d2ull, "матрицы весов llama.cpp: биты не те, что были записаны (%016llx)",
          (unsigned long long)kv_hash);
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
        printf("%-8s чтение весов (%ld МБ), ГБ/с:", variants[v]->name, mb);
        for (int f = 0; f < NF; f++) printf(" %s %.1f;", fmts[f], gbs[v][f]);
        printf("\n");
    }
    free(buf); free(y); free(x8); free(xk);
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
        printf("%-8s приближённо: матрица на матрицу %.1f ГФлопс\n", variants[v]->name, 2.0 * MM * MM * MM / t_mf * 1e-9);
        printf("%-8s скалярное %.1f ГФлопс; матрица на вектор %.1f ГФлопс (%.1f ГБ/с); q8 %.1f ГФлопс (%.1f ГБ/с); "
               "матрица на матрицу %.1f ГФлопс\n", variants[v]->name, 2.0 * DN * reps / t_dot * 1e-9,
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
    test_f16();
    test_dot();
    test_mm();
    test_q8();
    test_matvec();
    test_f16_kernels();
    test_fast();
    test_pool();
    test_norms();
    test_sums();
    test_kvanty();
    printf("%s варианты:", failures ? "ОШИБКИ" : "ok");
    for (int v = 0; v < nvariants; v++) printf(" %s", variants[v]->name);
    dan_vk_cur = NULL;
    printf("; выбран: %s; потоков: %d; хеш весов llama.cpp: %016llx\n", dan_vk_name(), dan_pool_threads(),
           (unsigned long long)kv_hash);
    return failures ? 1 : 0;
}
