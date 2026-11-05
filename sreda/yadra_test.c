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

/* measurements: GFLOP/s by variant; «замер» as the first argument */
static void bench(void) {
    enum { DN = 4096, MV = 4096, MM = 512 };
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
        t0 = now();
        dan_vk_matmat_f32(A, B, C, MM, MM, MM);
        double t_mm = now() - t0;
        printf("%-8s скалярное %.1f ГФлопс; матрица на вектор %.1f ГФлопс (%.1f ГБ/с); q8 %.1f ГФлопс (%.1f ГБ/с); "
               "матрица на матрицу %.1f ГФлопс\n", variants[v]->name, 2.0 * DN * reps / t_dot * 1e-9,
               2.0 * MV * MV / t_mv * 1e-9, 4.0 * MV * MV / t_mv * 1e-9, 2.0 * MV * MV / t_q8 * 1e-9,
               34.0 / 32 * MV * MV / t_q8 * 1e-9, 2.0 * MM * MM * MM / t_mm * 1e-9);
    }
}

int main(int argc, char **argv) {
    find_variants();
    if (argc > 1 && strcmp(argv[1], "замер") == 0) { bench(); return 0; }
    test_f16();
    test_dot();
    test_mm();
    test_q8();
    test_matvec();
    test_pool();
    test_norms();
    printf("%s варианты:", failures ? "ОШИБКИ" : "ok");
    for (int v = 0; v < nvariants; v++) printf(" %s", variants[v]->name);
    dan_vk_cur = NULL;
    printf("; выбран: %s; потоков: %d\n", dan_vk_name(), dan_pool_threads());
    return failures ? 1 : 0;
}
