/* Comparison of the formats of llama.cpp weights in sreda/yadra.c with ggml:
   built by test_yadra.py together with ggml/src/ggml-quants.c of llama.cpp
   b12190 (variable DAN_GGML - the folder of llama.cpp sources) and run.
   The numbers of dequantize_row_* of ggml and of the kernels of Dan must be
   the same bits on random blocks, the vector in 8 bits (Q8_K) the same bytes
   as quantize_row_q8_K_ref. With the argument «печать» (or «pechat») prints
   the hashes of ggml for sreda/yadra_etalon.h. */

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

/* ggml (the types of blocks are those of ggml-common.h, here - bytes) */
void dequantize_row_q4_0(const void *x, float *y, int64_t k);
void dequantize_row_q4_K(const void *x, float *y, int64_t k);
void dequantize_row_q5_K(const void *x, float *y, int64_t k);
void dequantize_row_q6_K(const void *x, float *y, int64_t k);
void dequantize_row_q5_0(const void *x, float *y, int64_t k);
void dequantize_row_q4_1(const void *x, float *y, int64_t k);
void dequantize_row_q5_1(const void *x, float *y, int64_t k);
void quantize_row_q8_K_ref(const float *x, void *y, int64_t k);

/* what ggml-quants.c asks from the rest of ggml */
void ggml_abort(const char *file, int line, const char *fmt, ...) {
    (void)fmt;
    fprintf(stderr, "ggml_abort: %s:%d\n", file, line);
    abort();
}
size_t ggml_row_size(int type, int64_t ne) { (void)type; (void)ne; abort(); }
size_t ggml_type_size(int type) { (void)type; abort(); }
const char *ggml_type_name(int type) { (void)type; return "?"; }

static void deq_ggml(int fmt, const uint8_t *in, float *out, int64_t blocks) {
    int64_t k = blocks * etalon_kvant_n[fmt];
    switch (fmt) {
    case 0: dequantize_row_q4_0(in, out, k); break;
    case 1: dequantize_row_q4_K(in, out, k); break;
    case 2: dequantize_row_q5_K(in, out, k); break;
    case 3: dequantize_row_q6_K(in, out, k); break;
    case 4: dequantize_row_q5_0(in, out, k); break;
    case 5: dequantize_row_q4_1(in, out, k); break;
    default: dequantize_row_q5_1(in, out, k); break;
    }
}
static void deq_dan(int fmt, const uint8_t *in, float *out, int64_t blocks) {
    switch (fmt) {
    case 0: dan_vk_q40_dequantize(in, out, blocks); break;
    case 1: dan_vk_q4k_dequantize(in, out, blocks); break;
    case 2: dan_vk_q5k_dequantize(in, out, blocks); break;
    case 3: dan_vk_q6k_dequantize(in, out, blocks); break;
    case 4: dan_vk_q50_dequantize(in, out, blocks); break;
    case 5: dan_vk_q41_dequantize(in, out, blocks); break;
    default: dan_vk_q51_dequantize(in, out, blocks); break;
    }
}

int main(int argc, char **argv) {
    static const char *const names[ETALON_KVANT_FORMATS] = {"Q4_0", "Q4_K", "Q5_K", "Q6_K", "Q5_0", "Q4_1", "Q5_1"};
    int failures = 0;
    uint64_t h[ETALON_KVANT_FORMATS];
    etalon_kvant_hashes(deq_ggml, h);
    if (argc > 1 && (strcmp(argv[1], "печать") == 0 || strcmp(argv[1], "pechat") == 0)) {
        printf("static const uint64_t etalon_kvanty_ggml[ETALON_KVANT_FORMATS] = {");
        for (int f = 0; f < ETALON_KVANT_FORMATS; f++)
            printf("%s0x%016llxull", f ? (f % 4 ? ", " : ",\n    ") : "", (unsigned long long)h[f]);
        printf("};\n");
        return 0;
    }
    for (int f = 0; f < ETALON_KVANT_FORMATS; f++)
        if (h[f] != etalon_kvanty_ggml[f]) {
            failures++;
            printf("ошибка: хеш %s у ggml %016llx, а записан %016llx\n", names[f], (unsigned long long)h[f],
                   (unsigned long long)etalon_kvanty_ggml[f]);
        }
    /* many random blocks: the same bits */
    enum { B = 512 };
    static uint8_t in[B * 210];
    static float a[B * 256], b[B * 256];
    int64_t checked = 0;
    for (int f = 0; f < ETALON_KVANT_FORMATS; f++)
        for (uint64_t seed = 1; seed <= 200; seed++) {
            etalon_kvant_blocks(f, in, B, seed * 7919 + (uint64_t)f);
            deq_ggml(f, in, a, B);
            deq_dan(f, in, b, B);
            int64_t n = (int64_t)B * etalon_kvant_n[f];
            checked += B;
            if (memcmp(a, b, (size_t)n * 4) != 0) {
                failures++;
                for (int64_t i = 0; i < n; i++)
                    if (memcmp(&a[i], &b[i], 4) != 0) {
                        printf("ошибка: %s, семя %llu, число %lld: ggml %.9g, Дан %.9g\n", names[f], (unsigned long long)seed,
                               (long long)i, (double)a[i], (double)b[i]);
                        break;
                    }
            }
        }
    /* the vector in Q8_K: numbers of every size (not below 4e-37, where Dan
       makes the block zero and ggml gets an infinite scale) */
    static float x[B * 256];
    static dan_q8k qa[B], qb[B];
    for (int t = 0; t < 20; t++) {
        etalon_rng = 99 + (uint64_t)t;
        for (int i = 0; i < B * 256; i++) {
            double v = etalon_rnd() * pow(10.0, (double)(i / 256 % 40) - 20.0);
            if (i % 101 == 3) v = 0.0;
            x[i] = (float)v;
        }
        if (t == 0) memset(x, 0, 256 * sizeof(float));   /* a zero block */
        memset(qa, 0, sizeof qa);   /* ggml leaves the sums of a zero block as they were */
        quantize_row_q8_K_ref(x, qa, (int64_t)B * 256);
        dan_vk_q8k_quantize(x, qb, (int64_t)B * 256);
        for (int i = 0; i < B; i++)
            if (memcmp(&qa[i], &qb[i], sizeof qa[i]) != 0) {
                failures++;
                printf("ошибка: Q8_K, проход %d, блок %d\n", t, i);
                break;
            }
    }
    printf("%s ggml: сверено блоков %lld\n", failures ? "ОШИБКИ" : "ok", (long long)checked);
    return failures ? 1 : 0;
}
