/* ==== Vector kernels and the thread pool of the Dan language. ====

   Kernels for large computations: dot product, sum, element-wise
   operations, matrix by vector, matrix by matrix, softmax, RMS and layer
   normalisation for float (32-bit) and double numbers, and matrix by vector
   for 8-bit weights in blocks of 32 with a scale (the Q8_0 format of GGUF).

   The variant is chosen once, when the program first uses a kernel, by what
   the processor can do: plain C (always present, the reference), AVX2,
   AVX-512 (F and BW) on x86-64, NEON and NEON with dot product instructions
   on 64-bit ARM. The environment variable DAN_YADRA=c|avx2|avx512|neon|dotprod
   asks for a variant (an unsupported one is ignored).

   Every variant gives exactly the same bits as plain C. The order of
   summation is fixed by this file, not by the processor: a dot product of
   n numbers is computed in L = 64 lanes for float (32 for double) - lane l
   adds the products of elements l, l + L, l + 2L, ... in turn - then the lanes
   are added pairwise (l and l + L/2, then l and l + L/4, ...), then the
   remaining n mod L products one by one. Products and sums are rounded
   separately (no fused multiply-add: the program is built with
   -ffp-contract=off). An element of a matrix product adds its k products in
   order of k starting from zero. The integer part of the 8-bit kernels is
   exact. Functions that use exp (softmax) give the same bits on one system;
   different systems may differ in the last bit of exp.

   The pool runs a range [0, n) in parts on all cores: the parts are fixed by
   n and the number of threads, so a kernel run in parallel gives the same
   bits as run on one core. DAN_POTOKI=N sets the number of threads (up to 256, even above the core count). */

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define DAN_POOL 1
#if defined(__x86_64__) || defined(_M_X64)
#define DAN_VK_X86 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DAN_VK_ARM 1
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif
#if !defined(_WIN32) && !defined(__wasi__)
#include <pthread.h>
#include <sched.h>
#endif
#include <stdatomic.h>

#define DAN_VK_L32 64   /* lanes of a float dot product */
#define DAN_VK_L64 32   /* lanes of a double dot product */
#define DAN_Q8_BLOCK 32 /* numbers in a block of 8-bit weights */
#define DAN_Q8_BYTES 34 /* bytes in a block: 16-bit float scale and 32 numbers */

/* ---- half precision numbers (the scale of an 8-bit block) ---- */

/* exact: the bits of a half number placed into a float and scaled by 2^112
   (subnormal halves become normal floats) */
static inline float dan_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, m = (uint32_t)(h & 0x7fff) << 13;
    float f;
    if ((h & 0x7c00) == 0x7c00) { m |= 0x7f800000u | sign; memcpy(&f, &m, 4); return f; }
    memcpy(&f, &m, 4);
    f = f * 0x1p112f;
    memcpy(&m, &f, 4);
    m |= sign;
    memcpy(&f, &m, 4);
    return f;
}

/* round to nearest, ties to even */
static uint16_t dan_f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, a = x & 0x7fffffffu;
    if (a >= 0x7f800000u) return (uint16_t)(sign | 0x7c00 | (a > 0x7f800000u ? 0x200 : 0));
    if (a >= 0x477ff000u) return (uint16_t)(sign | 0x7c00);   /* rounds to infinity */
    if (a < 0x38800000u) {   /* subnormal or zero in half precision */
        if (a < 0x33000000u) return (uint16_t)sign;
        /* value = man * 2^(e - 150), a half subnormal counts units of 2^-24 */
        uint32_t man = (a & 0x7fffff) | 0x800000, shift = 126 - (a >> 23);
        uint32_t r = man >> shift, rest = man & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (r & 1))) r++;
        return (uint16_t)(sign | r);
    }
    uint32_t r = ((a >> 13) - ((127 - 15) << 10)), rest = a & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (r & 1))) r++;
    return (uint16_t)(sign | r);
}

/* ---- plain C: the reference ---- */

static float dan_vk_dot_f32_c(const float *a, const float *b, int64_t n) {
    float acc[DAN_VK_L32] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L32 <= n; k += DAN_VK_L32)
        for (int l = 0; l < DAN_VK_L32; l++) { float p = a[k + l] * b[k + l]; acc[l] = acc[l] + p; }
    for (int s = DAN_VK_L32 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    float r = acc[0];
    for (; k < n; k++) { float p = a[k] * b[k]; r = r + p; }
    return r;
}

static double dan_vk_dot_f64_c(const double *a, const double *b, int64_t n) {
    double acc[DAN_VK_L64] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L64 <= n; k += DAN_VK_L64)
        for (int l = 0; l < DAN_VK_L64; l++) { double p = a[k + l] * b[k + l]; acc[l] = acc[l] + p; }
    for (int s = DAN_VK_L64 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    double r = acc[0];
    for (; k < n; k++) { double p = a[k] * b[k]; r = r + p; }
    return r;
}

/* columns [j0, j1) of C = A B; A is m x k, B is k x n, all by rows */
static void dan_vk_mm_f32_c(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k,
                            int64_t j0, int64_t j1) {
    for (int64_t i = 0; i < m; i++) {
        float *c = C + i * n;
        for (int64_t j = j0; j < j1; j++) c[j] = 0.0f;
        for (int64_t p = 0; p < k; p++) {
            float a = A[i * k + p];
            const float *b = B + p * n;
            for (int64_t j = j0; j < j1; j++) { float t = a * b[j]; c[j] = c[j] + t; }
        }
    }
}

static void dan_vk_mm_f64_c(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k,
                            int64_t j0, int64_t j1) {
    for (int64_t i = 0; i < m; i++) {
        double *c = C + i * n;
        for (int64_t j = j0; j < j1; j++) c[j] = 0.0;
        for (int64_t p = 0; p < k; p++) {
            double a = A[i * k + p];
            const double *b = B + p * n;
            for (int64_t j = j0; j < j1; j++) { double t = a * b[j]; c[j] = c[j] + t; }
        }
    }
}

static int32_t dan_vk_q8_block_c(const int8_t *a, const int8_t *b) {
    int32_t s = 0;
    for (int i = 0; i < DAN_Q8_BLOCK; i++) s += (int32_t)a[i] * b[i];
    return s;
}

/* ---- the table of variants ---- */

/* vectors of the kernels of a row of weights by several vectors */
#ifndef DAN_VK_NV
#define DAN_VK_NV 4
#endif
/* formats of weights of llama.cpp (sreda/yadra.c below) */
enum { DAN_FMT_Q40, DAN_FMT_Q4K, DAN_FMT_Q5K, DAN_FMT_Q6K, DAN_FMT_Q50, DAN_FMT_Q41, DAN_FMT_Q51, DAN_FMT_Q80, DAN_NFMT };

typedef struct {
    const char *name;
    float (*dot_f32)(const float *, const float *, int64_t);
    double (*dot_f64)(const double *, const double *, int64_t);
    /* sums in the lanes of the dot products */
    float (*sum_f32)(const float *, int64_t);
    double (*sum_f64)(const double *, int64_t);
    void (*mm_f32)(const float *, const float *, float *, int64_t, int64_t, int64_t, int64_t, int64_t);
    void (*mm_f64)(const double *, const double *, double *, int64_t, int64_t, int64_t, int64_t, int64_t);
    int32_t (*q8_block)(const int8_t *, const int8_t *);
    float (*q8_dot)(const uint8_t *, const uint8_t *, int64_t);
    /* lanes of a float dot product: n a multiple of 64, acc - the 64 lanes */
    void (*dot32_acc)(const float *, const float *, int64_t, float *);
    /* half precision numbers to float (exact) */
    void (*h2f)(const uint16_t *, float *, int64_t);
    /* «приближённо»: fused multiply-add, the order of sums is not fixed */
    float (*dot_f32_fast)(const float *, const float *, int64_t);
    double (*dot_f64_fast)(const double *, const double *, int64_t);
    void (*mm_f32_fast)(const float *, const float *, float *, int64_t, int64_t, int64_t, int64_t, int64_t);
    void (*mm_f64_fast)(const double *, const double *, double *, int64_t, int64_t, int64_t, int64_t, int64_t);
    /* rows of weights of llama.cpp by the vector in 8 bits (Q8_0 for Q4_0, Q8_K for the rest) */
    float (*q40_dot)(const uint8_t *, const void *, int64_t);
    float (*q4k_dot)(const uint8_t *, const void *, int64_t);
    float (*q5k_dot)(const uint8_t *, const void *, int64_t);
    float (*q6k_dot)(const uint8_t *, const void *, int64_t);
    /* Q5_0 (vector Q8_0), Q4_1 and Q5_1 (vector in blocks of 36 bytes) */
    float (*q50_dot)(const uint8_t *, const void *, int64_t);
    float (*q41_dot)(const uint8_t *, const void *, int64_t);
    float (*q51_dot)(const uint8_t *, const void *, int64_t);
    /* a row of weights of a format by 4 vectors in 8 bits at once (the
       vectors xs bytes apart), out - 4 numbers with the same bits as the row
       kernel gives for each vector; NULL - the row kernel for each vector */
    void (*dot4[DAN_NFMT])(const uint8_t *, const uint8_t *, int64_t, int64_t, float *);
} dan_vk_table;

static void dan_vk_dot32_acc_c(const float *a, const float *b, int64_t n, float *acc) {
    for (int64_t k = 0; k + DAN_VK_L32 <= n; k += DAN_VK_L32)
        for (int l = 0; l < DAN_VK_L32; l++) { float p = a[k + l] * b[k + l]; acc[l] = acc[l] + p; }
}
static void dan_vk_h2f_c(const uint16_t *h, float *f, int64_t n) {
    for (int64_t i = 0; i < n; i++) f[i] = dan_f16_to_f32(h[i]);
}

/* Sum over blocks of p = (d_a * d_b) * (integer dot product of the block):
   8 lanes - lane l adds p of blocks l, l + 8, l + 16, ... in turn - then the
   lanes pairwise (l and l + 4, l and l + 2, l and l + 1), then the remaining
   blocks one by one. The integer part is exact in every variant. */
#define DAN_Q8_LANES 8
static inline float dan_vk_q8_p(const uint8_t *pa, const uint8_t *pb, int32_t isum) {
    uint16_t ha, hb;
    memcpy(&ha, pa, 2);
    memcpy(&hb, pb, 2);
    float d = dan_f16_to_f32(ha) * dan_f16_to_f32(hb);
    return d * (float)isum;
}
#define DAN_VK_Q8_DOT(NAME, ATTR, BLOCK) \
ATTR static float NAME(const uint8_t *a, const uint8_t *b, int64_t blocks) { \
    float acc[DAN_Q8_LANES] = {0}; \
    int64_t i = 0; \
    for (; i + DAN_Q8_LANES <= blocks; i += DAN_Q8_LANES) \
        for (int l = 0; l < DAN_Q8_LANES; l++) { \
            const uint8_t *pa = a + (i + l) * DAN_Q8_BYTES, *pb = b + (i + l) * DAN_Q8_BYTES; \
            float p = dan_vk_q8_p(pa, pb, BLOCK((const int8_t *)pa + 2, (const int8_t *)pb + 2)); \
            acc[l] = acc[l] + p; \
        } \
    for (int s = DAN_Q8_LANES / 2; s >= 1; s /= 2) \
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s]; \
    float r = acc[0]; \
    for (; i < blocks; i++) { \
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES; \
        float p = dan_vk_q8_p(pa, pb, BLOCK((const int8_t *)pa + 2, (const int8_t *)pb + 2)); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_Q8_DOT(dan_vk_q8_dot_c, , dan_vk_q8_block_c)

/* ---- weights of llama.cpp (GGUF): Q4_0, Q4_K, Q5_K, Q6_K ----

   Bytes of a block exactly as in ggml (ggml/src/ggml-common.h of llama.cpp):
   Q4_0 - 32 numbers: half d, 16 bytes of 4-bit q (low nibbles - numbers 0..15,
          high - 16..31), value d * (q - 8);
   Q4_K - 256 numbers in 8 parts of 32: half d, half dmin, 12 bytes of 6-bit
          scales and mins, 128 bytes of 4-bit q; value d * sc * q - dmin * m;
   Q5_K - as Q4_K with 32 bytes of fifth bits before the 4-bit q;
   Q6_K - 256 numbers in 16 parts of 16: 128 bytes of low 4 bits, 64 bytes of
          high 2 bits, 16 signed scales, half d; value d * sc * (q - 32).
   The vector is turned into 8 bits by blocks: Q8_0 for Q4_0, Q8_K (a float
   scale and 256 numbers, as quantize_row_q8_K_ref of ggml) for the K formats.
   The integer part of a block is exact; the block gives
   p = (d_x * d) * isum - (d_x * dmin) * imin   (imin only for Q4_K and Q5_K),
   and the blocks of a row are added in the 8 lanes of the Q8_0 kernel. */

#define DAN_QK 256
#define DAN_Q40_BYTES 18
#define DAN_Q4K_BYTES 144
#define DAN_Q5K_BYTES 176
#define DAN_Q6K_BYTES 210

/* a block of the vector for the K formats; bsums - sums of 16 numbers */
typedef struct { float d; int8_t qs[DAN_QK]; int16_t bsums[DAN_QK / 16]; } dan_q8k;

static inline float dan_vk_half(const uint8_t *p) {
    uint16_t h;
    memcpy(&h, p, 2);
    return dan_f16_to_f32(h);
}

/* 6-bit scales and mins of 8 parts (get_scale_min_k4 of ggml) */
static inline void dan_vk_k4_scales(const uint8_t *q, uint8_t *sc, uint8_t *m) {
    for (int j = 0; j < 4; j++) { sc[j] = q[j] & 63; m[j] = q[j + 4] & 63; }
    for (int j = 4; j < 8; j++) {
        sc[j] = (uint8_t)((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        m[j] = (uint8_t)((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

/* the same as 16 bytes sc0..sc7, m0..m7, by masks of 32 bits as in llama.cpp (little-endian) */
static inline void dan_vk_k4_sm(const uint8_t *q, uint8_t *sm) {
    uint32_t u[4];
    memcpy(u, q, 12);
    u[3] = ((u[2] >> 4) & 0x0f0f0f0fu) | (((u[1] >> 6) & 0x03030303u) << 4);
    uint32_t m03 = u[1] & 0x3f3f3f3fu;
    u[1] = (u[2] & 0x0f0f0f0fu) | (((u[0] >> 6) & 0x03030303u) << 4);
    u[2] = m03;
    u[0] &= 0x3f3f3f3fu;
    memcpy(sm, u, 16);
}

/* the sum of the mins with the sums of the vector's parts of 32 */
static inline int32_t dan_vk_k4_mins(const uint8_t *m, const dan_q8k *y) {
    int32_t s = 0;
    for (int j = 0; j < 8; j++) s += m[j] * (y->bsums[2 * j] + y->bsums[2 * j + 1]);
    return s;
}

static inline float dan_vk_k4_p(const uint8_t *w, const dan_q8k *y, int32_t is, int32_t im) {
    float a = y->d * dan_vk_half(w), b = y->d * dan_vk_half(w + 2);
    float t = a * (float)is, u = b * (float)im;
    return t - u;
}
static inline float dan_vk_q6k_p(const uint8_t *w, const dan_q8k *y, int32_t is, int32_t im) {
    (void)im;
    float a = y->d * dan_vk_half(w + 208);
    return a * (float)is;
}

static int32_t dan_vk_q40_block_c(const uint8_t *w, const uint8_t *x) {
    const int8_t *y = (const int8_t *)x + 2;
    int32_t s = 0;
    for (int j = 0; j < 16; j++) s += ((w[2 + j] & 0xF) - 8) * y[j] + ((w[2 + j] >> 4) - 8) * y[j + 16];
    return s;
}

static void dan_vk_q4k_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    uint8_t sc[8], m[8];
    dan_vk_k4_scales(w + 4, sc, m);
    const uint8_t *q = w + 16;
    const int8_t *v = y->qs;
    int32_t s = 0;
    for (int p = 0; p < 4; p++, q += 32, v += 64) {
        int32_t lo = 0, hi = 0;
        for (int l = 0; l < 32; l++) { lo += (q[l] & 0xF) * v[l]; hi += (q[l] >> 4) * v[l + 32]; }
        s += sc[2 * p] * lo + sc[2 * p + 1] * hi;
    }
    *is = s;
    *im = dan_vk_k4_mins(m, y);
}

static void dan_vk_q5k_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    uint8_t sc[8], m[8];
    dan_vk_k4_scales(w + 4, sc, m);
    const uint8_t *qh = w + 16, *q = w + 48;
    const int8_t *v = y->qs;
    int32_t s = 0;
    for (int p = 0; p < 4; p++, q += 32, v += 64) {
        int32_t lo = 0, hi = 0;
        for (int l = 0; l < 32; l++) {
            lo += ((q[l] & 0xF) | (((qh[l] >> (2 * p)) & 1) << 4)) * v[l];
            hi += ((q[l] >> 4) | (((qh[l] >> (2 * p + 1)) & 1) << 4)) * v[l + 32];
        }
        s += sc[2 * p] * lo + sc[2 * p + 1] * hi;
    }
    *is = s;
    *im = dan_vk_k4_mins(m, y);
}

/* (q - 32) y = q y - 32 y: 32 times the sum of scale times the sum of the vector's part of 16 */
static inline int32_t dan_vk_q6k_bias(const int8_t *sc, const dan_q8k *y) {
    int32_t s = 0;
    for (int j = 0; j < 16; j++) s += sc[j] * y->bsums[j];
    return 32 * s;
}

static void dan_vk_q6k_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    const uint8_t *ql = w, *qh = w + 128;
    const int8_t *sc = (const int8_t *)w + 192, *v = y->qs;
    int32_t s = 0;
    for (int n = 0; n < 2; n++, ql += 64, qh += 32, v += 128, sc += 8) {
        int32_t part[8] = {0};
        for (int l = 0; l < 32; l++) {
            int h = l / 16;
            part[h + 0] += ((ql[l] & 0xF) | ((qh[l] & 3) << 4)) * v[l];
            part[h + 2] += ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) * v[l + 32];
            part[h + 4] += ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) * v[l + 64];
            part[h + 6] += ((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4)) * v[l + 96];
        }
        for (int j = 0; j < 8; j++) s += sc[j] * part[j];
    }
    *is = s - dan_vk_q6k_bias((const int8_t *)w + 192, y);
    *im = 0;
}

/* a row of Q4_0 blocks by Q8_0 blocks of the vector */
#define DAN_VK_Q40_DOT(NAME, ATTR, BLOCK) \
ATTR static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const uint8_t *x = (const uint8_t *)xv; \
    float acc[DAN_Q8_LANES] = {0}; \
    int64_t i = 0; \
    for (; i + DAN_Q8_LANES <= blocks; i += DAN_Q8_LANES) \
        for (int l = 0; l < DAN_Q8_LANES; l++) { \
            const uint8_t *pw = w + (i + l) * DAN_Q40_BYTES, *px = x + (i + l) * DAN_Q8_BYTES; \
            float p = dan_vk_q8_p(pw, px, BLOCK(pw, px)); \
            acc[l] = acc[l] + p; \
        } \
    for (int s = DAN_Q8_LANES / 2; s >= 1; s /= 2) \
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s]; \
    float r = acc[0]; \
    for (; i < blocks; i++) { \
        const uint8_t *pw = w + i * DAN_Q40_BYTES, *px = x + i * DAN_Q8_BYTES; \
        float p = dan_vk_q8_p(pw, px, BLOCK(pw, px)); \
        r = r + p; \
    } \
    return r; \
}

/* a row of K blocks (WB bytes) by Q8_K blocks of the vector */
#define DAN_VK_KDOT(NAME, ATTR, INTS, WB, PF) \
ATTR static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const dan_q8k *x = (const dan_q8k *)xv; \
    float acc[DAN_Q8_LANES] = {0}; \
    int64_t i = 0; \
    for (; i + DAN_Q8_LANES <= blocks; i += DAN_Q8_LANES) \
        for (int l = 0; l < DAN_Q8_LANES; l++) { \
            int32_t is_, im_; \
            INTS(w + (i + l) * WB, x + i + l, &is_, &im_); \
            float p = PF(w + (i + l) * WB, x + i + l, is_, im_); \
            acc[l] = acc[l] + p; \
        } \
    for (int s = DAN_Q8_LANES / 2; s >= 1; s /= 2) \
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s]; \
    float r = acc[0]; \
    for (; i < blocks; i++) { \
        int32_t is_, im_; \
        INTS(w + i * WB, x + i, &is_, &im_); \
        float p = PF(w + i * WB, x + i, is_, im_); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_Q40_DOT(dan_vk_q40_dot_c, , dan_vk_q40_block_c)
DAN_VK_KDOT(dan_vk_q4k_dot_c, , dan_vk_q4k_ints_c, DAN_Q4K_BYTES, dan_vk_k4_p)
DAN_VK_KDOT(dan_vk_q5k_dot_c, , dan_vk_q5k_ints_c, DAN_Q5K_BYTES, dan_vk_k4_p)
DAN_VK_KDOT(dan_vk_q6k_dot_c, , dan_vk_q6k_ints_c, DAN_Q6K_BYTES, dan_vk_q6k_p)

/* ---- Q5_0, Q4_1, Q5_1: blocks of 32 numbers as in ggml ----
   Q5_0 - half d, 4 bytes of fifth bits qh (bit j - number j), 16 bytes of
          4-bit q (low nibbles - numbers 0..15, high - 16..31); value d * (q - 16);
   Q4_1 - half d, half m, 16 bytes of 4-bit q; value d * q + m;
   Q5_1 - half d, half m, 4 bytes qh, 16 bytes of 4-bit q; value d * q + m.
   The vector: Q8_0 for Q5_0 (p as for Q4_0); for Q4_1 and Q5_1 a block of 36
   bytes - the half scale of the Q8_0 block, the sum of its 32 numbers (16
   bits) and the numbers, and the block gives
   p = (d_x * d) * isum + (d_x * m) * sum. */

#define DAN_Q50_BYTES 22
#define DAN_Q41_BYTES 20
#define DAN_Q51_BYTES 24
#define DAN_Q81_BYTES 36

static inline uint32_t dan_vk_u32(const uint8_t *p) {
    uint32_t u;
    memcpy(&u, p, 4);
    return u;
}

static int32_t dan_vk_q50_block_c(const uint8_t *w, const uint8_t *x) {
    const int8_t *y = (const int8_t *)x + 2;
    const uint8_t *q = w + 6;
    uint32_t qh = dan_vk_u32(w + 2);
    int32_t s = 0;
    for (int j = 0; j < 16; j++) {
        int32_t a = (int32_t)((q[j] & 0xF) | (((qh >> j) & 1) << 4)) - 16;
        int32_t b = (int32_t)((q[j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) - 16;
        s += a * y[j] + b * y[j + 16];
    }
    return s;
}

static int32_t dan_vk_q41_block_c(const uint8_t *w, const uint8_t *x) {
    const int8_t *y = (const int8_t *)x + 4;
    const uint8_t *q = w + 4;
    int32_t s = 0;
    for (int j = 0; j < 16; j++) s += (q[j] & 0xF) * y[j] + (q[j] >> 4) * y[j + 16];
    return s;
}

static int32_t dan_vk_q51_block_c(const uint8_t *w, const uint8_t *x) {
    const int8_t *y = (const int8_t *)x + 4;
    const uint8_t *q = w + 8;
    uint32_t qh = dan_vk_u32(w + 4);
    int32_t s = 0;
    for (int j = 0; j < 16; j++)
        s += (int32_t)((q[j] & 0xF) | (((qh >> j) & 1) << 4)) * y[j]
           + (int32_t)((q[j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) * y[j + 16];
    return s;
}

/* p of Q4_1 and Q5_1: (d_x * d) * isum + (d_x * m) * sum */
static inline float dan_vk_q81_p(const uint8_t *w, const uint8_t *x, int32_t isum) {
    int16_t sum;
    memcpy(&sum, x + 2, 2);
    float dx = dan_vk_half(x);
    float a = dx * dan_vk_half(w), b = dx * dan_vk_half(w + 2);
    float t = a * (float)isum, u = b * (float)sum;
    return t + u;
}

/* a row of blocks of 32 (WB bytes) by blocks of the vector (XB bytes) in the 8 lanes */
#define DAN_VK_B32_DOT(NAME, ATTR, BLOCK, WB, XB, PF) \
ATTR static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const uint8_t *x = (const uint8_t *)xv; \
    float acc[DAN_Q8_LANES] = {0}; \
    int64_t i = 0; \
    for (; i + DAN_Q8_LANES <= blocks; i += DAN_Q8_LANES) \
        for (int l = 0; l < DAN_Q8_LANES; l++) { \
            const uint8_t *pw = w + (i + l) * (WB), *px = x + (i + l) * (XB); \
            float p = PF(pw, px, BLOCK(pw, px)); \
            acc[l] = acc[l] + p; \
        } \
    for (int s = DAN_Q8_LANES / 2; s >= 1; s /= 2) \
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s]; \
    float r = acc[0]; \
    for (; i < blocks; i++) { \
        const uint8_t *pw = w + i * (WB), *px = x + i * (XB); \
        float p = PF(pw, px, BLOCK(pw, px)); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_B32_DOT(dan_vk_q50_dot_c, , dan_vk_q50_block_c, DAN_Q50_BYTES, DAN_Q8_BYTES, dan_vk_q8_p)
DAN_VK_B32_DOT(dan_vk_q41_dot_c, , dan_vk_q41_block_c, DAN_Q41_BYTES, DAN_Q81_BYTES, dan_vk_q81_p)
DAN_VK_B32_DOT(dan_vk_q51_dot_c, , dan_vk_q51_block_c, DAN_Q51_BYTES, DAN_Q81_BYTES, dan_vk_q81_p)

/* sums: the order of a dot product with the products replaced by the numbers */
static float dan_vk_sum_f32_c(const float *a, int64_t n) {
    float acc[DAN_VK_L32] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L32 <= n; k += DAN_VK_L32)
        for (int l = 0; l < DAN_VK_L32; l++) acc[l] = acc[l] + a[k + l];
    for (int s = DAN_VK_L32 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    float r = acc[0];
    for (; k < n; k++) r = r + a[k];
    return r;
}

static double dan_vk_sum_f64_c(const double *a, int64_t n) {
    double acc[DAN_VK_L64] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L64 <= n; k += DAN_VK_L64)
        for (int l = 0; l < DAN_VK_L64; l++) acc[l] = acc[l] + a[k + l];
    for (int s = DAN_VK_L64 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    double r = acc[0];
    for (; k < n; k++) r = r + a[k];
    return r;
}

static const dan_vk_table dan_vk_plain = {"c", dan_vk_dot_f32_c, dan_vk_dot_f64_c, dan_vk_sum_f32_c, dan_vk_sum_f64_c, dan_vk_mm_f32_c,
                                          dan_vk_mm_f64_c, dan_vk_q8_block_c, dan_vk_q8_dot_c, dan_vk_dot32_acc_c, dan_vk_h2f_c, dan_vk_dot_f32_c, dan_vk_dot_f64_c, dan_vk_mm_f32_c, dan_vk_mm_f64_c,
                                          dan_vk_q40_dot_c, dan_vk_q4k_dot_c, dan_vk_q5k_dot_c, dan_vk_q6k_dot_c,
                                          dan_vk_q50_dot_c, dan_vk_q41_dot_c, dan_vk_q51_dot_c, {0}};

/* Edges of a blocked matrix product: rows [i0, m) or columns [j0, j1) that do
   not fill a tile are computed by plain C (same order of summation). */
#define DAN_VK_MM_EDGE(T, A, B, C, n, k, i0, i1, j0, j1) do { \
    for (int64_t i_ = (i0); i_ < (i1); i_++) { \
        T *c_ = (C) + i_ * (n); \
        for (int64_t j_ = (j0); j_ < (j1); j_++) c_[j_] = 0; \
        for (int64_t p_ = 0; p_ < (k); p_++) { \
            T a_ = (A)[i_ * (k) + p_]; const T *b_ = (B) + p_ * (n); \
            for (int64_t j_ = (j0); j_ < (j1); j_++) { T t_ = a_ * b_[j_]; c_[j_] = c_[j_] + t_; } \
        } \
    } } while (0)


/* Blocked product: for every strip of NR columns the strip of B (k x NR) is
   copied into one piece of memory once, then tiles of MR rows are computed
   by the variant's kernel mk(A rows, k, packed strip, C, n). Every element
   adds its k products in order, so the tile sizes do not change the bits. */
#define DAN_VK_MM_DRIVE(NAME, T) \
typedef void (*NAME##_mk)(const T *, int64_t, const T *, T *, int64_t); \
static void NAME(const T *A, const T *B, T *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1, \
                 int MR, int NR, NAME##_mk mk) { \
    int64_t je = j0 + (j1 - j0) / NR * NR, ie = m / MR * MR; \
    T *buf = je > j0 && ie > 0 && k > 0 ? malloc((size_t)k * NR * sizeof(T)) : NULL; \
    if (!buf) { je = j0; ie = 0; } \
    for (int64_t j = j0; j < je; j += NR) { \
        for (int64_t p = 0; p < k; p++) memcpy(buf + p * NR, B + p * n + j, (size_t)NR * sizeof(T)); \
        for (int64_t i = 0; i < ie; i += MR) mk(A + i * k, k, buf, C + i * n + j, n); \
    } \
    free(buf); \
    DAN_VK_MM_EDGE(T, A, B, C, n, k, ie, m, j0, je); \
    DAN_VK_MM_EDGE(T, A, B, C, n, k, 0, m, je, j1); \
}
DAN_VK_MM_DRIVE(dan_vk_mm_drive_f32, float)
DAN_VK_MM_DRIVE(dan_vk_mm_drive_f64, double)

#ifdef DAN_VK_X86
#define DAN_VK_AVX2 __attribute__((target("avx2")))
#define DAN_VK_AVX512 __attribute__((target("avx512f,avx512bw")))
#define DAN_VK_AVX2FMA __attribute__((target("avx2,fma")))

/* ---- AVX2 ---- */

DAN_VK_AVX2 static float dan_vk_hsum8_f32(__m256 v8) {   /* lanes: l + 4, then l + 2, then l + 1 */
    __m128 v = _mm_add_ps(_mm256_castps256_ps128(v8), _mm256_extractf128_ps(v8, 1));
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    return _mm_cvtss_f32(v);
}

DAN_VK_AVX2 static float dan_vk_dot_f32_avx2(const float *a, const float *b, int64_t n) {
    __m256 s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_ps();
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 8; j++)
            s[j] = _mm256_add_ps(s[j], _mm256_mul_ps(_mm256_loadu_ps(a + k + 8 * j), _mm256_loadu_ps(b + k + 8 * j)));
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_ps(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = _mm256_add_ps(s[j], s[j + 2]);
    float r = dan_vk_hsum8_f32(_mm256_add_ps(s[0], s[1]));
    for (; k < n; k++) { float p = a[k] * b[k]; r = r + p; }
    return r;
}

DAN_VK_AVX2 static double dan_vk_hsum4_f64(__m256d v4) {   /* lanes: l + 2, then l + 1 */
    __m128d v = _mm_add_pd(_mm256_castpd256_pd128(v4), _mm256_extractf128_pd(v4, 1));
    v = _mm_add_sd(v, _mm_unpackhi_pd(v, v));
    return _mm_cvtsd_f64(v);
}

DAN_VK_AVX2 static double dan_vk_dot_f64_avx2(const double *a, const double *b, int64_t n) {
    __m256d s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_pd();
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 8; j++)
            s[j] = _mm256_add_pd(s[j], _mm256_mul_pd(_mm256_loadu_pd(a + k + 4 * j), _mm256_loadu_pd(b + k + 4 * j)));
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_pd(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = _mm256_add_pd(s[j], s[j + 2]);
    double r = dan_vk_hsum4_f64(_mm256_add_pd(s[0], s[1]));
    for (; k < n; k++) { double p = a[k] * b[k]; r = r + p; }
    return r;
}

DAN_VK_AVX2 static float dan_vk_sum_f32_avx2(const float *a, int64_t n) {
    __m256 s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_ps();
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 8; j++) s[j] = _mm256_add_ps(s[j], _mm256_loadu_ps(a + k + 8 * j));
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_ps(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = _mm256_add_ps(s[j], s[j + 2]);
    float r = dan_vk_hsum8_f32(_mm256_add_ps(s[0], s[1]));
    for (; k < n; k++) r = r + a[k];
    return r;
}
DAN_VK_AVX2 static double dan_vk_sum_f64_avx2(const double *a, int64_t n) {
    __m256d s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_pd();
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 8; j++) s[j] = _mm256_add_pd(s[j], _mm256_loadu_pd(a + k + 4 * j));
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_pd(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = _mm256_add_pd(s[j], s[j + 2]);
    double r = dan_vk_hsum4_f64(_mm256_add_pd(s[0], s[1]));
    for (; k < n; k++) r = r + a[k];
    return r;
}

/* tiles of 6 rows by 16 columns (float) or 6 by 8 (double) */
DAN_VK_AVX2 static void dan_vk_mm_f32_avx2_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    __m256 c[6][2];
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm256_setzero_ps();
    for (int64_t p = 0; p < k; p++) {
        __m256 b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm256_loadu_ps(Bp + p * 16 + 8 * q);
        for (int r = 0; r < 6; r++) {
            __m256 a = _mm256_set1_ps(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm256_add_ps(c[r][q], _mm256_mul_ps(a, b[q]));
        }
    }
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) _mm256_storeu_ps(C + r * n + 8 * q, c[r][q]);
}
DAN_VK_AVX2 static void dan_vk_mm_f32_avx2(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 6, 16, dan_vk_mm_f32_avx2_mk);
}

DAN_VK_AVX2FMA static void dan_vk_mm_f32_avx2_fast_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    __m256 c[6][2];
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm256_setzero_ps();
    for (int64_t p = 0; p < k; p++) {
        __m256 b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm256_loadu_ps(Bp + p * 16 + 8 * q);
        for (int r = 0; r < 6; r++) {
            __m256 a = _mm256_set1_ps(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm256_fmadd_ps(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) _mm256_storeu_ps(C + r * n + 8 * q, c[r][q]);
}
DAN_VK_AVX2FMA static void dan_vk_mm_f32_avx2_fast(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 6, 16, dan_vk_mm_f32_avx2_fast_mk);
}

DAN_VK_AVX2 static void dan_vk_mm_f64_avx2_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    __m256d c[6][2];
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm256_setzero_pd();
    for (int64_t p = 0; p < k; p++) {
        __m256d b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm256_loadu_pd(Bp + p * 8 + 4 * q);
        for (int r = 0; r < 6; r++) {
            __m256d a = _mm256_set1_pd(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm256_add_pd(c[r][q], _mm256_mul_pd(a, b[q]));
        }
    }
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) _mm256_storeu_pd(C + r * n + 4 * q, c[r][q]);
}
DAN_VK_AVX2 static void dan_vk_mm_f64_avx2(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 6, 8, dan_vk_mm_f64_avx2_mk);
}

DAN_VK_AVX2FMA static void dan_vk_mm_f64_avx2_fast_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    __m256d c[6][2];
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm256_setzero_pd();
    for (int64_t p = 0; p < k; p++) {
        __m256d b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm256_loadu_pd(Bp + p * 8 + 4 * q);
        for (int r = 0; r < 6; r++) {
            __m256d a = _mm256_set1_pd(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm256_fmadd_pd(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 6; r++) for (int q = 0; q < 2; q++) _mm256_storeu_pd(C + r * n + 4 * q, c[r][q]);
}
DAN_VK_AVX2FMA static void dan_vk_mm_f64_avx2_fast(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 6, 8, dan_vk_mm_f64_avx2_fast_mk);
}

DAN_VK_AVX2 static int32_t dan_vk_q8_block_avx2(const int8_t *a, const int8_t *b) {
    __m256i x = _mm256_loadu_si256((const __m256i *)a), y = _mm256_loadu_si256((const __m256i *)b);
    __m256i xl = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(x)), xh = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(x, 1));
    __m256i yl = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(y)), yh = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(y, 1));
    __m256i s = _mm256_add_epi32(_mm256_madd_epi16(xl, yl), _mm256_madd_epi16(xh, yh));
    __m128i v = _mm_add_epi32(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}


#define DAN_VK_AVX2F16 __attribute__((target("avx2,f16c")))
/* 32 products of a block as 8 partial sums */
DAN_VK_AVX2F16 static inline __m256i dan_vk_q8_part_avx2(const uint8_t *a, const uint8_t *b) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(a + 2)), y = _mm256_loadu_si256((const __m256i *)(b + 2));
    __m256i xl = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(x)), xh = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(x, 1));
    __m256i yl = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(y)), yh = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(y, 1));
    return _mm256_add_epi32(_mm256_madd_epi16(xl, yl), _mm256_madd_epi16(xh, yh));
}



/* 8 scales in half precision, one in every «stride» bytes */
/* (word by word into the register: through an array in memory gcc 15 of
   MinGW stores 8 halves and loads 16 bytes, and the store does not forward) */
DAN_VK_AVX2F16 static inline __m256 dan_vk_halves8_avx2(const uint8_t *p, int stride) {
    __m128i h = _mm_setzero_si128();
    uint16_t v;
#define DAN_VK_HALF_IN(l) memcpy(&v, p + (l) * stride, 2); h = _mm_insert_epi16(h, v, (l));
    DAN_VK_HALF_IN(0) DAN_VK_HALF_IN(1) DAN_VK_HALF_IN(2) DAN_VK_HALF_IN(3)
    DAN_VK_HALF_IN(4) DAN_VK_HALF_IN(5) DAN_VK_HALF_IN(6) DAN_VK_HALF_IN(7)
#undef DAN_VK_HALF_IN
    return _mm256_cvtph_ps(h);
}

/* 8 blocks at a time: the sums of 8 vectors of partial sums gathered by hadd */
DAN_VK_AVX2F16 static float dan_vk_q8_dot_avx2(const uint8_t *a, const uint8_t *b, int64_t blocks) {
    __m256 acc = _mm256_setzero_ps();
    int64_t i = 0;
    for (; i + 8 <= blocks; i += 8) {
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES;
        __m256i v[8];
        for (int l = 0; l < 8; l++) v[l] = dan_vk_q8_part_avx2(pa + l * DAN_Q8_BYTES, pb + l * DAN_Q8_BYTES);
        __m256i h0 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[0], v[1]), _mm256_hadd_epi32(v[2], v[3]));
        __m256i h1 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[4], v[5]), _mm256_hadd_epi32(v[6], v[7]));
        __m256i isum = _mm256_add_epi32(_mm256_permute2x128_si256(h0, h1, 0x20), _mm256_permute2x128_si256(h0, h1, 0x31));
        __m256 d = _mm256_mul_ps(dan_vk_halves8_avx2(pa, DAN_Q8_BYTES), dan_vk_halves8_avx2(pb, DAN_Q8_BYTES));
        acc = _mm256_add_ps(acc, _mm256_mul_ps(d, _mm256_cvtepi32_ps(isum)));
    }
    __m128 v4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    v4 = _mm_add_ps(v4, _mm_movehl_ps(v4, v4));
    v4 = _mm_add_ss(v4, _mm_shuffle_ps(v4, v4, 1));
    float r = _mm_cvtss_f32(v4);
    for (; i < blocks; i++) {
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES;
        float p = dan_vk_q8_p(pa, pb, dan_vk_q8_block_avx2((const int8_t *)pa + 2, (const int8_t *)pb + 2));
        r = r + p;
    }
    return r;
}

DAN_VK_AVX2F16 static void dan_vk_dot32_acc_avx2(const float *a, const float *b, int64_t n, float *acc) {
    __m256 s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_loadu_ps(acc + 8 * j);
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 8; j++)
            s[j] = _mm256_add_ps(s[j], _mm256_mul_ps(_mm256_loadu_ps(a + k + 8 * j), _mm256_loadu_ps(b + k + 8 * j)));
    for (int j = 0; j < 8; j++) _mm256_storeu_ps(acc + 8 * j, s[j]);
}
DAN_VK_AVX2F16 static void dan_vk_h2f_avx2(const uint16_t *h, float *f, int64_t n) {
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(f + i, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(h + i))));
    for (; i < n; i++) f[i] = dan_f16_to_f32(h[i]);
}


/* «приближённо»: 8 lanes with fused multiply-add */
DAN_VK_AVX2FMA static float dan_vk_dot_f32_avx2_fast(const float *a, const float *b, int64_t n) {
    __m256 s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_ps();
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 8; j++) s[j] = _mm256_fmadd_ps(_mm256_loadu_ps(a + k + 8 * j), _mm256_loadu_ps(b + k + 8 * j), s[j]);
    for (; k + 8 <= n; k += 8) s[0] = _mm256_fmadd_ps(_mm256_loadu_ps(a + k), _mm256_loadu_ps(b + k), s[0]);
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_ps(s[j], s[j + 4]);
    s[0] = _mm256_add_ps(_mm256_add_ps(s[0], s[2]), _mm256_add_ps(s[1], s[3]));
    __m128 v = _mm_add_ps(_mm256_castps256_ps128(s[0]), _mm256_extractf128_ps(s[0], 1));
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    float r = _mm_cvtss_f32(v);
    for (; k < n; k++) r = fmaf(a[k], b[k], r);
    return r;
}
DAN_VK_AVX2FMA static double dan_vk_dot_f64_avx2_fast(const double *a, const double *b, int64_t n) {
    __m256d s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_pd();
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 8; j++) s[j] = _mm256_fmadd_pd(_mm256_loadu_pd(a + k + 4 * j), _mm256_loadu_pd(b + k + 4 * j), s[j]);
    for (; k + 4 <= n; k += 4) s[0] = _mm256_fmadd_pd(_mm256_loadu_pd(a + k), _mm256_loadu_pd(b + k), s[0]);
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_pd(s[j], s[j + 4]);
    s[0] = _mm256_add_pd(_mm256_add_pd(s[0], s[2]), _mm256_add_pd(s[1], s[3]));
    __m128d v = _mm_add_pd(_mm256_castpd256_pd128(s[0]), _mm256_extractf128_pd(s[0], 1));
    v = _mm_add_sd(v, _mm_unpackhi_pd(v, v));
    double r = _mm_cvtsd_f64(v);
    for (; k < n; k++) r = fma(a[k], b[k], r);
    return r;
}

/* ---- AVX2: weights of llama.cpp ---- */

DAN_VK_AVX2 static inline int32_t dan_vk_hsum_i32_avx2(__m256i s) {
    __m128i v = _mm_add_epi32(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}


/* products of a Q4_0 block and a Q8_0 block as 8 partial sums */
DAN_VK_AVX2 static inline __m256i dan_vk_q40_part_avx2(const uint8_t *w, const uint8_t *x) {
    const __m128i m4 = _mm_set1_epi8(0xF);
    __m128i q = _mm_loadu_si128((const __m128i *)(w + 2));
    __m256i v = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_and_si128(q, m4)),
                                        _mm_and_si128(_mm_srli_epi16(q, 4), m4), 1);
    v = _mm256_sub_epi8(v, _mm256_set1_epi8(8));
    __m256i y = _mm256_loadu_si256((const __m256i *)(x + 2));
    /* |v| * (y with the sign of v) = v * y; pairs fit 16 bits (2 * 8 * 128) */
    __m256i p = _mm256_maddubs_epi16(_mm256_sign_epi8(v, v), _mm256_sign_epi8(y, v));
    return _mm256_madd_epi16(p, _mm256_set1_epi16(1));
}
DAN_VK_AVX2 static int32_t dan_vk_q40_block_avx2(const uint8_t *w, const uint8_t *x) {
    return dan_vk_hsum_i32_avx2(dan_vk_q40_part_avx2(w, x));
}

/* 8 blocks at a time: lane l of acc adds the blocks l, l + 8, ... as in C */
DAN_VK_AVX2F16 static float dan_vk_q40_dot_avx2(const uint8_t *w, const void *xv, int64_t blocks) {
    const uint8_t *x = (const uint8_t *)xv;
    __m256 acc = _mm256_setzero_ps();
    int64_t i = 0;
    for (; i + 8 <= blocks; i += 8) {
        const uint8_t *pw = w + i * DAN_Q40_BYTES, *px = x + i * DAN_Q8_BYTES;
        __m256i v[8];
        for (int l = 0; l < 8; l++) v[l] = dan_vk_q40_part_avx2(pw + l * DAN_Q40_BYTES, px + l * DAN_Q8_BYTES);
        __m256i h0 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[0], v[1]), _mm256_hadd_epi32(v[2], v[3]));
        __m256i h1 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[4], v[5]), _mm256_hadd_epi32(v[6], v[7]));
        __m256i isum = _mm256_add_epi32(_mm256_permute2x128_si256(h0, h1, 0x20), _mm256_permute2x128_si256(h0, h1, 0x31));
        __m256 d = _mm256_mul_ps(dan_vk_halves8_avx2(pw, DAN_Q40_BYTES), dan_vk_halves8_avx2(px, DAN_Q8_BYTES));
        acc = _mm256_add_ps(acc, _mm256_mul_ps(d, _mm256_cvtepi32_ps(isum)));
    }
    __m128 v4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    v4 = _mm_add_ps(v4, _mm_movehl_ps(v4, v4));
    v4 = _mm_add_ss(v4, _mm_shuffle_ps(v4, v4, 1));
    float r = _mm_cvtss_f32(v4);
    for (; i < blocks; i++) {
        const uint8_t *pw = w + i * DAN_Q40_BYTES, *px = x + i * DAN_Q8_BYTES;
        float p = dan_vk_q8_p(pw, px, dan_vk_q40_block_avx2(pw, px));
        r = r + p;
    }
    return r;
}

/* scales and mins of a Q4_K or Q5_K block as 16 numbers of 16 bits: sc0..sc7, m0..m7 */
DAN_VK_AVX2 static inline __m256i dan_vk_k4_sm_avx2(const uint8_t *q) {
    uint8_t sm[16];
    dan_vk_k4_sm(q, sm);
    return _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)sm));
}

/* number k (0..7) of the 8 numbers of 16 bits in both halves of b, in all 16 places */
#define DAN_VK_BC16(b, k) _mm256_shuffle_epi8((b), _mm256_set1_epi16((short)(((2 * (k) + 1) << 8) | (2 * (k)))))

/* q (unsigned, below 64) by 32 numbers of y: 8 sums of 32 bits, multiplied
   by the scales s (16 numbers of 16 bits: one for each pair of products) */
DAN_VK_AVX2 static inline __m256i dan_vk_mulsc_avx2(__m256i q, const int8_t *y, __m256i s) {
    return _mm256_madd_epi16(_mm256_maddubs_epi16(q, _mm256_loadu_si256((const __m256i *)y)), s);
}

/* the sum of the mins with the sums of the vector's parts of 32 */
DAN_VK_AVX2 static inline int32_t dan_vk_k4_mins_avx2(__m256i sm, const dan_q8k *y) {
    __m256i bs = _mm256_loadu_si256((const __m256i *)y->bsums);
    __m128i pairs = _mm_hadd_epi16(_mm256_castsi256_si128(bs), _mm256_extracti128_si256(bs, 1));   /* below 2 * 16 * 128 */
    __m128i v = _mm_madd_epi16(_mm256_extracti128_si256(sm, 1), pairs);
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}

/* the integer part of a block: 8 partial sums of isum; *extra - imin */
DAN_VK_AVX2 static inline __m256i dan_vk_q4k_parts_avx2(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    __m256i sm = dan_vk_k4_sm_avx2(w + 4), sc = _mm256_broadcastsi128_si256(_mm256_castsi256_si128(sm));
    const __m256i m4 = _mm256_set1_epi8(0xF);
    __m256i acc = _mm256_setzero_si256();
    for (int p = 0; p < 4; p++) {
        __m256i q = _mm256_loadu_si256((const __m256i *)(w + 16 + 32 * p));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(q, m4), y->qs + 64 * p, DAN_VK_BC16(sc, 2 * p)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(_mm256_srli_epi16(q, 4), m4), y->qs + 64 * p + 32,
                                                      DAN_VK_BC16(sc, 2 * p + 1)));
    }
    *extra = dan_vk_k4_mins_avx2(sm, y);
    return acc;
}

DAN_VK_AVX2 static inline __m256i dan_vk_q5k_parts_avx2(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    __m256i sm = dan_vk_k4_sm_avx2(w + 4), sc = _mm256_broadcastsi128_si256(_mm256_castsi256_si128(sm));
    const __m256i m4 = _mm256_set1_epi8(0xF), one = _mm256_set1_epi8(1);
    __m256i hb = _mm256_loadu_si256((const __m256i *)(w + 16)), acc = _mm256_setzero_si256();
    for (int p = 0; p < 4; p++) {
        __m256i q = _mm256_loadu_si256((const __m256i *)(w + 48 + 32 * p));
        /* bit 2p (then 2p + 1) of every byte of the fifth bits as 16; hb moves right by 2 each step */
        __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(hb, one), 4);
        __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hb, 1), one), 4);
        hb = _mm256_srli_epi16(hb, 2);
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_or_si256(_mm256_and_si256(q, m4), h0), y->qs + 64 * p,
                                                      DAN_VK_BC16(sc, 2 * p)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(q, 4), m4), h1),
                                                      y->qs + 64 * p + 32, DAN_VK_BC16(sc, 2 * p + 1)));
    }
    *extra = dan_vk_k4_mins_avx2(sm, y);
    return acc;
}

/* scales a (numbers 0..15 of 32) and b (16..31) out of 8 numbers of 16 bits in both halves of s */
#define DAN_VK_BC16X2(s, a, b) _mm256_shuffle_epi8((s), _mm256_setr_epi8( \
    2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, \
    2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, 2 * (a), 2 * (a) + 1, \
    2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1, \
    2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1, 2 * (b), 2 * (b) + 1))

/* Q6_K: the bias 32 * sum of sc * bsums is taken off here, *extra = 0 */
DAN_VK_AVX2 static inline __m256i dan_vk_q6k_parts_avx2(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    const __m256i m4 = _mm256_set1_epi8(0xF), m3 = _mm256_set1_epi8(3);
    __m256i s16 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i *)(w + 192)));
    __m256i bias = _mm256_madd_epi16(s16, _mm256_loadu_si256((const __m256i *)y->bsums));   /* below 16 * 128 * 2048 */
    __m256i acc = _mm256_sub_epi32(_mm256_setzero_si256(), _mm256_slli_epi32(bias, 5));
    for (int n = 0; n < 2; n++) {
        const uint8_t *ql = w + 64 * n, *qh = w + 128 + 32 * n;
        const int8_t *v = y->qs + 128 * n;
        __m256i s = n == 0 ? _mm256_broadcastsi128_si256(_mm256_castsi256_si128(s16))
                           : _mm256_broadcastsi128_si256(_mm256_extracti128_si256(s16, 1));
        __m256i l0 = _mm256_loadu_si256((const __m256i *)ql), l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
        __m256i h = _mm256_loadu_si256((const __m256i *)qh);
        __m256i q1 = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m3), 4));
        __m256i q2 = _mm256_or_si256(_mm256_and_si256(l1, m4),
                                     _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4));
        __m256i q3 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                                     _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4));
        __m256i q4 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                                     _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(q1, v, DAN_VK_BC16X2(s, 0, 1)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(q2, v + 32, DAN_VK_BC16X2(s, 2, 3)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(q3, v + 64, DAN_VK_BC16X2(s, 4, 5)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(q4, v + 96, DAN_VK_BC16X2(s, 6, 7)));
    }
    *extra = 0;
    return acc;
}

/* 8 blocks at a time: the integer sums of 8 blocks gathered by hadd into
   one vector, lane l - block l; then p of the 8 blocks as in C (the lanes of
   the Q8_0 kernel) */
#define DAN_VK_KDOT_AVX2(NAME, PARTS, WB, DOFF, MINS) \
DAN_VK_AVX2F16 static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const dan_q8k *x = (const dan_q8k *)xv; \
    __m256 acc = _mm256_setzero_ps(); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        const dan_q8k *px = x + i; \
        __m256i v[8]; \
        int32_t ex[8]; \
        for (int l = 0; l < 8; l++) v[l] = PARTS(pw + l * (WB), px + l, &ex[l]); \
        __m256i h0 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[0], v[1]), _mm256_hadd_epi32(v[2], v[3])); \
        __m256i h1 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[4], v[5]), _mm256_hadd_epi32(v[6], v[7])); \
        __m256i isum = _mm256_add_epi32(_mm256_permute2x128_si256(h0, h1, 0x20), _mm256_permute2x128_si256(h0, h1, 0x31)); \
        __m256 yd = _mm256_setr_ps(px[0].d, px[1].d, px[2].d, px[3].d, px[4].d, px[5].d, px[6].d, px[7].d); \
        __m256 t = _mm256_mul_ps(_mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + (DOFF), WB)), _mm256_cvtepi32_ps(isum)); \
        if (MINS) { \
            __m256 b = _mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + 2, WB)); \
            t = _mm256_sub_ps(t, _mm256_mul_ps(b, _mm256_cvtepi32_ps(_mm256_loadu_si256((const __m256i *)ex)))); \
        } \
        acc = _mm256_add_ps(acc, t); \
    } \
    __m128 v4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1)); \
    v4 = _mm_add_ps(v4, _mm_movehl_ps(v4, v4)); \
    v4 = _mm_add_ss(v4, _mm_shuffle_ps(v4, v4, 1)); \
    float r = _mm_cvtss_f32(v4); \
    for (; i < blocks; i++) { \
        int32_t ex; \
        int32_t is = dan_vk_hsum_i32_avx2(PARTS(w + i * (WB), x + i, &ex)); \
        float p = MINS ? dan_vk_k4_p(w + i * (WB), x + i, is, ex) : dan_vk_q6k_p(w + i * (WB), x + i, is, 0); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_KDOT_AVX2(dan_vk_q4k_dot_avx2, dan_vk_q4k_parts_avx2, DAN_Q4K_BYTES, 0, 1)
DAN_VK_KDOT_AVX2(dan_vk_q5k_dot_avx2, dan_vk_q5k_parts_avx2, DAN_Q5K_BYTES, 0, 1)
DAN_VK_KDOT_AVX2(dan_vk_q6k_dot_avx2, dan_vk_q6k_parts_avx2, DAN_Q6K_BYTES, 208, 0)

/* ---- AVX2: rows of weights by 4 vectors at once ----

   The weights of a block are taken apart once (numbers as bytes, scales)
   and multiplied by the blocks of 4 vectors: the work on the weights is
   shared by the vectors, the row stays in the first level cache. The
   integer sums are exact, the numbers of each vector are made by the same
   operations in the same order as in the row kernels, so the bits are the
   same. */

/* the sum of the 8 lanes of each of v[0..7] as one vector: lane l - v[l] */
DAN_VK_AVX2 static inline __m256i dan_vk_hadd8_avx2(const __m256i *v) {
    __m256i h0 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[0], v[1]), _mm256_hadd_epi32(v[2], v[3]));
    __m256i h1 = _mm256_hadd_epi32(_mm256_hadd_epi32(v[4], v[5]), _mm256_hadd_epi32(v[6], v[7]));
    return _mm256_add_epi32(_mm256_permute2x128_si256(h0, h1, 0x20), _mm256_permute2x128_si256(h0, h1, 0x31));
}
DAN_VK_AVX2 static inline float dan_vk_hsum8_lanes_avx2(__m256 acc) {   /* lanes: l + 4, then l + 2, then l + 1 */
    __m128 v4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    v4 = _mm_add_ps(v4, _mm_movehl_ps(v4, v4));
    v4 = _mm_add_ss(v4, _mm_shuffle_ps(v4, v4, 1));
    return _mm_cvtss_f32(v4);
}

/* 16 bytes of 4-bit numbers as 32 bytes: numbers 0..15 (low nibbles), then 16..31 */
DAN_VK_AVX2 static inline __m256i dan_vk_nib32_avx2(const uint8_t *q) {
    const __m128i m4 = _mm_set1_epi8(0xF);
    __m128i v = _mm_loadu_si128((const __m128i *)q);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_and_si128(v, m4)), _mm_and_si128(_mm_srli_epi16(v, 4), m4), 1);
}
/* 32 bits as 32 bytes: 16 where the bit is set, 0 where not */
DAN_VK_AVX2 static inline __m256i dan_vk_bits32_avx2(const uint8_t *p) {
    const __m256i shuf = _mm256_set_epi64x(0x0303030303030303ll, 0x0202020202020202ll, 0x0101010101010101ll, 0);
    __m256i b = _mm256_shuffle_epi8(_mm256_set1_epi32((int)dan_vk_u32(p)), shuf);
    b = _mm256_or_si256(b, _mm256_set1_epi64x(0x7fbfdfeff7fbfdfell));
    return _mm256_and_si256(_mm256_cmpeq_epi8(b, _mm256_set1_epi64x(-1)), _mm256_set1_epi8(16));
}
/* the 32 numbers of a block of weights as bytes (signed for Q4_0, Q5_0, Q8_0) */
DAN_VK_AVX2 static inline __m256i dan_vk_w_q40_avx2(const uint8_t *w) { return _mm256_sub_epi8(dan_vk_nib32_avx2(w + 2), _mm256_set1_epi8(8)); }
DAN_VK_AVX2 static inline __m256i dan_vk_w_q50_avx2(const uint8_t *w) {
    return _mm256_sub_epi8(_mm256_or_si256(dan_vk_nib32_avx2(w + 6), dan_vk_bits32_avx2(w + 2)), _mm256_set1_epi8(16));
}
DAN_VK_AVX2 static inline __m256i dan_vk_w_q80_avx2(const uint8_t *w) { return _mm256_loadu_si256((const __m256i *)(w + 2)); }
DAN_VK_AVX2 static inline __m256i dan_vk_w_q41_avx2(const uint8_t *w) { return dan_vk_nib32_avx2(w + 4); }
DAN_VK_AVX2 static inline __m256i dan_vk_w_q51_avx2(const uint8_t *w) {
    return _mm256_or_si256(dan_vk_nib32_avx2(w + 8), dan_vk_bits32_avx2(w + 4));
}

/* the products of 32 numbers of weights (v, its modulus a; unsigned: a = v) and of
   32 numbers of the vector as 8 partial sums: |v| * (y with the sign of v) = v * y,
   pairs fit 16 bits (the vector is within -127..127) */
DAN_VK_AVX2 static inline __m256i dan_vk_mul32_avx2(__m256i v, __m256i a, const uint8_t *y, int sgn) {
    __m256i yv = _mm256_loadu_si256((const __m256i *)y);
    if (sgn) yv = _mm256_sign_epi8(yv, v);
    return _mm256_madd_epi16(_mm256_maddubs_epi16(a, yv), _mm256_set1_epi16(1));
}

/* 8 sums of 16 bits, one in every «stride» bytes, as floats */
DAN_VK_AVX2 static inline __m256 dan_vk_sums8_avx2(const uint8_t *p, int stride) {
    __m128i h = _mm_setzero_si128();
    uint16_t v;
#define DAN_VK_SUM_IN(l) memcpy(&v, p + (l) * stride, 2); h = _mm_insert_epi16(h, v, (l));
    DAN_VK_SUM_IN(0) DAN_VK_SUM_IN(1) DAN_VK_SUM_IN(2) DAN_VK_SUM_IN(3)
    DAN_VK_SUM_IN(4) DAN_VK_SUM_IN(5) DAN_VK_SUM_IN(6) DAN_VK_SUM_IN(7)
#undef DAN_VK_SUM_IN
    return _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(h));
}

/* A row of blocks of 32 by NV vectors (blocks of XB bytes, the vectors xs bytes
   apart): WFN - the numbers of a block of weights, SGN - signed, MINS - Q4_1
   and Q5_1 (the half m after d and the sums of the vector's blocks), BLOCK and
   PF - the integer sum and p of a block for the remaining blocks. */
#define DAN_VK_B32N_AVX2(NAME, ATTR, MUL32, NV, WFN, SGN, WB, XB, XOFF, MINS, PF) \
ATTR static void NAME(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    __m256 acc[NV]; \
    for (int k = 0; k < (NV); k++) acc[k] = _mm256_setzero_ps(); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        __m256i v[NV][8]; \
        for (int l = 0; l < 8; l++) { \
            __m256i q = WFN(pw + l * (WB)), a = (SGN) ? _mm256_sign_epi8(q, q) : q; \
            for (int k = 0; k < (NV); k++) \
                v[k][l] = MUL32(q, a, x + k * xs + (i + l) * (XB) + (XOFF), (SGN)); \
        } \
        __m256 dw = dan_vk_halves8_avx2(pw, WB); \
        for (int k = 0; k < (NV); k++) { \
            const uint8_t *px = x + k * xs + i * (XB); \
            __m256 dx = dan_vk_halves8_avx2(px, XB); \
            __m256 t = _mm256_mul_ps(_mm256_mul_ps(dw, dx), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(v[k]))); \
            if (MINS) { \
                __m256 b = _mm256_mul_ps(dx, dan_vk_halves8_avx2(pw + 2, WB)); \
                t = _mm256_add_ps(t, _mm256_mul_ps(b, dan_vk_sums8_avx2(px + 2, XB))); \
            } \
            acc[k] = _mm256_add_ps(acc[k], t); \
        } \
    } \
    float r[NV]; \
    for (int k = 0; k < (NV); k++) r[k] = dan_vk_hsum8_lanes_avx2(acc[k]); \
    for (; i < blocks; i++) { \
        const uint8_t *pw = w + i * (WB); \
        __m256i q = WFN(pw), a = (SGN) ? _mm256_sign_epi8(q, q) : q; \
        for (int k = 0; k < (NV); k++) { \
            const uint8_t *px = x + k * xs + i * (XB); \
            float p = PF(pw, px, dan_vk_hsum_i32_avx2(MUL32(q, a, px + (XOFF), (SGN)))); \
            r[k] = r[k] + p; \
        } \
    } \
    for (int k = 0; k < (NV); k++) out[k] = r[k]; \
}
DAN_VK_B32N_AVX2(dan_vk_q50_dot1_avx2, DAN_VK_AVX2F16, dan_vk_mul32_avx2, 1, dan_vk_w_q50_avx2, 1, DAN_Q50_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p)
DAN_VK_B32N_AVX2(dan_vk_q41_dot1_avx2, DAN_VK_AVX2F16, dan_vk_mul32_avx2, 1, dan_vk_w_q41_avx2, 0, DAN_Q41_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p)
DAN_VK_B32N_AVX2(dan_vk_q51_dot1_avx2, DAN_VK_AVX2F16, dan_vk_mul32_avx2, 1, dan_vk_w_q51_avx2, 0, DAN_Q51_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p)
#define DAN_VK_DOT1_AVX2(NAME, ONE) \
DAN_VK_AVX2F16 static float NAME(const uint8_t *w, const void *x, int64_t blocks) { \
    float r; \
    ONE(w, (const uint8_t *)x, 0, blocks, &r); \
    return r; \
}
DAN_VK_DOT1_AVX2(dan_vk_q50_dot_avx2, dan_vk_q50_dot1_avx2)
DAN_VK_DOT1_AVX2(dan_vk_q41_dot_avx2, dan_vk_q41_dot1_avx2)
DAN_VK_DOT1_AVX2(dan_vk_q51_dot_avx2, dan_vk_q51_dot1_avx2)

/* the sums of the 4 lanes of each of m[0..7] as one vector: lane l - m[l] */
DAN_VK_AVX2 static inline __m256i dan_vk_hadd8x4_avx2(const __m128i *m) {
    __m128i a = _mm_hadd_epi32(_mm_hadd_epi32(m[0], m[1]), _mm_hadd_epi32(m[2], m[3]));
    __m128i b = _mm_hadd_epi32(_mm_hadd_epi32(m[4], m[5]), _mm_hadd_epi32(m[6], m[7]));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(a), b, 1);
}
DAN_VK_AVX2 static inline int32_t dan_vk_hsum4_i32_avx2(__m128i v) {
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}

/* a row of K blocks by 4 vectors (blocks of Q8_K, the vectors xs bytes apart):
   for each vector the operations of the row kernel */
#define DAN_VK_KDOT4_AVX2(NAME, ATTR, BLK, WB, DOFF, MINS) \
ATTR static void NAME(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    const dan_q8k *xv[DAN_VK_NV]; \
    __m256 acc[DAN_VK_NV]; \
    for (int k = 0; k < DAN_VK_NV; k++) { xv[k] = (const dan_q8k *)(const void *)(x + k * xs); acc[k] = _mm256_setzero_ps(); } \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        __m256i v[8][DAN_VK_NV]; \
        __m128i mn[8][DAN_VK_NV]; \
        for (int l = 0; l < 8; l++) { \
            const dan_q8k *y[DAN_VK_NV]; \
            for (int k = 0; k < DAN_VK_NV; k++) y[k] = xv[k] + i + l; \
            BLK(pw + l * (WB), y, v[l], mn[l]); \
        } \
        __m256 dw = dan_vk_halves8_avx2(pw + (DOFF), WB); \
        __m256 mw = (MINS) ? dan_vk_halves8_avx2(pw + 2, WB) : _mm256_setzero_ps(); \
        for (int k = 0; k < DAN_VK_NV; k++) { \
            const dan_q8k *px = xv[k] + i; \
            __m256i vk[8]; \
            for (int l = 0; l < 8; l++) vk[l] = v[l][k]; \
            __m256 yd = _mm256_setr_ps(px[0].d, px[1].d, px[2].d, px[3].d, px[4].d, px[5].d, px[6].d, px[7].d); \
            __m256 t = _mm256_mul_ps(_mm256_mul_ps(yd, dw), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(vk))); \
            if (MINS) { \
                __m128i mk[8]; \
                for (int l = 0; l < 8; l++) mk[l] = mn[l][k]; \
                __m256 bm = _mm256_mul_ps(yd, mw); \
                t = _mm256_sub_ps(t, _mm256_mul_ps(bm, _mm256_cvtepi32_ps(dan_vk_hadd8x4_avx2(mk)))); \
            } \
            acc[k] = _mm256_add_ps(acc[k], t); \
        } \
    } \
    float r[DAN_VK_NV]; \
    for (int k = 0; k < DAN_VK_NV; k++) r[k] = dan_vk_hsum8_lanes_avx2(acc[k]); \
    for (; i < blocks; i++) { \
        const dan_q8k *y[DAN_VK_NV]; \
        for (int k = 0; k < DAN_VK_NV; k++) y[k] = xv[k] + i; \
        __m256i a[DAN_VK_NV]; \
        __m128i m[DAN_VK_NV]; \
        BLK(w + i * (WB), y, a, m); \
        for (int k = 0; k < DAN_VK_NV; k++) { \
            int32_t is = dan_vk_hsum_i32_avx2(a[k]); \
            float p = (MINS) ? dan_vk_k4_p(w + i * (WB), y[k], is, dan_vk_hsum4_i32_avx2(m[k])) \
                             : dan_vk_q6k_p(w + i * (WB), y[k], is, 0); \
            r[k] = r[k] + p; \
        } \
    } \
    for (int k = 0; k < DAN_VK_NV; k++) out[k] = r[k]; \
}
/* Rows of weights by DAN_VK_NV vectors at once for a way to multiply and add
   (MACC: acc + (q by 32 numbers of the vector, pairs) by the scales s; MUL32:
   dan_vk_mul32_avx2 and the like): a K block is taken apart once (32 numbers
   at a time) and multiplied by the blocks of all the vectors; acc[k] - 8
   partial sums of isum of vector k, mins[k] - 4 partial sums of imin (Q4_K,
   Q5_K; for Q6_K the bias 32 * sum of sc * bsums is taken off isum). */
#define DAN_VK_KN_AVX2(SUF, ATTR, ATTRF, MACC, MUL32) \
ATTR static inline void dan_vk_k4_mins4_##SUF(__m256i sm, const dan_q8k *const *y, __m128i *mins) { \
    __m128i mm = _mm256_extracti128_si256(sm, 1); \
    for (int k = 0; k < DAN_VK_NV; k++) { \
        __m256i bs = _mm256_loadu_si256((const __m256i *)y[k]->bsums); \
        __m128i pairs = _mm_hadd_epi16(_mm256_castsi256_si128(bs), _mm256_extracti128_si256(bs, 1)); \
        mins[k] = _mm_madd_epi16(mm, pairs); \
    } \
} \
ATTR static inline void dan_vk_q4k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, __m256i *acc, __m128i *mins) { \
    __m256i sm = dan_vk_k4_sm_avx2(w + 4), sc = _mm256_broadcastsi128_si256(_mm256_castsi256_si128(sm)); \
    const __m256i m4 = _mm256_set1_epi8(0xF); \
    for (int k = 0; k < DAN_VK_NV; k++) acc[k] = _mm256_setzero_si256(); \
    for (int p = 0; p < 4; p++) { \
        __m256i q = _mm256_loadu_si256((const __m256i *)(w + 16 + 32 * p)); \
        __m256i lo = _mm256_and_si256(q, m4), hi = _mm256_and_si256(_mm256_srli_epi16(q, 4), m4); \
        __m256i s0 = DAN_VK_BC16(sc, 2 * p), s1 = DAN_VK_BC16(sc, 2 * p + 1); \
        for (int k = 0; k < DAN_VK_NV; k++) acc[k] = MACC(acc[k], lo, y[k]->qs + 64 * p, s0); \
        for (int k = 0; k < DAN_VK_NV; k++) acc[k] = MACC(acc[k], hi, y[k]->qs + 64 * p + 32, s1); \
    } \
    dan_vk_k4_mins4_##SUF(sm, y, mins); \
} \
ATTR static inline void dan_vk_q5k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, __m256i *acc, __m128i *mins) { \
    __m256i sm = dan_vk_k4_sm_avx2(w + 4), sc = _mm256_broadcastsi128_si256(_mm256_castsi256_si128(sm)); \
    const __m256i m4 = _mm256_set1_epi8(0xF), one = _mm256_set1_epi8(1); \
    __m256i hb = _mm256_loadu_si256((const __m256i *)(w + 16)); \
    for (int k = 0; k < DAN_VK_NV; k++) acc[k] = _mm256_setzero_si256(); \
    for (int p = 0; p < 4; p++) { \
        __m256i q = _mm256_loadu_si256((const __m256i *)(w + 48 + 32 * p)); \
        __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(hb, one), 4); \
        __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hb, 1), one), 4); \
        hb = _mm256_srli_epi16(hb, 2); \
        __m256i lo = _mm256_or_si256(_mm256_and_si256(q, m4), h0); \
        __m256i hi = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(q, 4), m4), h1); \
        __m256i s0 = DAN_VK_BC16(sc, 2 * p), s1 = DAN_VK_BC16(sc, 2 * p + 1); \
        for (int k = 0; k < DAN_VK_NV; k++) acc[k] = MACC(acc[k], lo, y[k]->qs + 64 * p, s0); \
        for (int k = 0; k < DAN_VK_NV; k++) acc[k] = MACC(acc[k], hi, y[k]->qs + 64 * p + 32, s1); \
    } \
    dan_vk_k4_mins4_##SUF(sm, y, mins); \
} \
ATTR static inline void dan_vk_q6k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, __m256i *acc, __m128i *mins) { \
    const __m256i m4 = _mm256_set1_epi8(0xF), m3 = _mm256_set1_epi8(3); \
    __m256i s16 = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i *)(w + 192))); \
    for (int k = 0; k < DAN_VK_NV; k++) { \
        __m256i bias = _mm256_madd_epi16(s16, _mm256_loadu_si256((const __m256i *)y[k]->bsums)); \
        acc[k] = _mm256_sub_epi32(_mm256_setzero_si256(), _mm256_slli_epi32(bias, 5)); \
        mins[k] = _mm_setzero_si128(); \
    } \
    for (int n = 0; n < 2; n++) { \
        const uint8_t *ql = w + 64 * n, *qh = w + 128 + 32 * n; \
        __m256i s = n == 0 ? _mm256_broadcastsi128_si256(_mm256_castsi256_si128(s16)) \
                           : _mm256_broadcastsi128_si256(_mm256_extracti128_si256(s16, 1)); \
        __m256i l0 = _mm256_loadu_si256((const __m256i *)ql), l1 = _mm256_loadu_si256((const __m256i *)(ql + 32)); \
        __m256i h = _mm256_loadu_si256((const __m256i *)qh); \
        __m256i q1 = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m3), 4)); \
        __m256i q2 = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4)); \
        __m256i q3 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4)); \
        __m256i q4 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4)); \
        __m256i s0 = DAN_VK_BC16X2(s, 0, 1), s1 = DAN_VK_BC16X2(s, 2, 3), s2 = DAN_VK_BC16X2(s, 4, 5), s3 = DAN_VK_BC16X2(s, 6, 7); \
        for (int k = 0; k < DAN_VK_NV; k++) { \
            const int8_t *v = y[k]->qs + 128 * n; \
            acc[k] = MACC(acc[k], q1, v, s0); \
            acc[k] = MACC(acc[k], q2, v + 32, s1); \
            acc[k] = MACC(acc[k], q3, v + 64, s2); \
            acc[k] = MACC(acc[k], q4, v + 96, s3); \
        } \
    } \
} \
DAN_VK_KDOT4_AVX2(dan_vk_q4k_dot4_##SUF, ATTRF, dan_vk_q4k_blk4_##SUF, DAN_Q4K_BYTES, 0, 1) \
DAN_VK_KDOT4_AVX2(dan_vk_q5k_dot4_##SUF, ATTRF, dan_vk_q5k_blk4_##SUF, DAN_Q5K_BYTES, 0, 1) \
DAN_VK_KDOT4_AVX2(dan_vk_q6k_dot4_##SUF, ATTRF, dan_vk_q6k_blk4_##SUF, DAN_Q6K_BYTES, 208, 0) \
DAN_VK_B32N_AVX2(dan_vk_q40_dot4_##SUF, ATTRF, MUL32, DAN_VK_NV, dan_vk_w_q40_avx2, 1, DAN_Q40_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_AVX2(dan_vk_q50_dot4_##SUF, ATTRF, MUL32, DAN_VK_NV, dan_vk_w_q50_avx2, 1, DAN_Q50_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_AVX2(dan_vk_q80_dot4_##SUF, ATTRF, MUL32, DAN_VK_NV, dan_vk_w_q80_avx2, 1, DAN_Q8_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_AVX2(dan_vk_q41_dot4_##SUF, ATTRF, MUL32, DAN_VK_NV, dan_vk_w_q41_avx2, 0, DAN_Q41_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p) \
DAN_VK_B32N_AVX2(dan_vk_q51_dot4_##SUF, ATTRF, MUL32, DAN_VK_NV, dan_vk_w_q51_avx2, 0, DAN_Q51_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p)

DAN_VK_AVX2 static inline __m256i dan_vk_macc_avx2(__m256i acc, __m256i q, const int8_t *y, __m256i s) {
    return _mm256_add_epi32(acc, dan_vk_mulsc_avx2(q, y, s));
}
DAN_VK_KN_AVX2(avx2, DAN_VK_AVX2, DAN_VK_AVX2F16, dan_vk_macc_avx2, dan_vk_mul32_avx2)

/* the same with the instructions of AVX-512 VNNI on 256 bits (the avx512
   variant, where the processor has them): the products of a pair are added
   with the scales by one instruction, the products of 4 bytes - by one */
#define DAN_VK_VNNI __attribute__((target("avx2,f16c,avx512f,avx512bw,avx512vl,avx512vnni")))
DAN_VK_VNNI static inline __m256i dan_vk_macc_vnni(__m256i acc, __m256i q, const int8_t *y, __m256i s) {
    return _mm256_dpwssd_epi32(acc, _mm256_maddubs_epi16(q, _mm256_loadu_si256((const __m256i *)y)), s);
}
DAN_VK_VNNI static inline __m256i dan_vk_mul32_vnni(__m256i v, __m256i a, const uint8_t *y, int sgn) {
    __m256i yv = _mm256_loadu_si256((const __m256i *)y);
    if (sgn) yv = _mm256_sign_epi8(yv, v);
    return _mm256_dpbusd_epi32(_mm256_setzero_si256(), a, yv);
}
DAN_VK_KN_AVX2(vnni, DAN_VK_VNNI, DAN_VK_VNNI, dan_vk_macc_vnni, dan_vk_mul32_vnni)
DAN_VK_B32N_AVX2(dan_vk_q50_dot1_vnni, DAN_VK_VNNI, dan_vk_mul32_vnni, 1, dan_vk_w_q50_avx2, 1, DAN_Q50_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p)
DAN_VK_B32N_AVX2(dan_vk_q41_dot1_vnni, DAN_VK_VNNI, dan_vk_mul32_vnni, 1, dan_vk_w_q41_avx2, 0, DAN_Q41_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p)
DAN_VK_B32N_AVX2(dan_vk_q51_dot1_vnni, DAN_VK_VNNI, dan_vk_mul32_vnni, 1, dan_vk_w_q51_avx2, 0, DAN_Q51_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p)
DAN_VK_VNNI static float dan_vk_q50_dot_vnni(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q50_dot1_vnni(w, (const uint8_t *)x, 0, blocks, &r); return r; }
DAN_VK_VNNI static float dan_vk_q41_dot_vnni(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q41_dot1_vnni(w, (const uint8_t *)x, 0, blocks, &r); return r; }
DAN_VK_VNNI static float dan_vk_q51_dot_vnni(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q51_dot1_vnni(w, (const uint8_t *)x, 0, blocks, &r); return r; }

/* AVX2 (and avx512 without VNNI) */
#define DAN_VK_AVX2_EXTRA dan_vk_q50_dot_avx2, dan_vk_q41_dot_avx2, dan_vk_q51_dot_avx2, \
    {dan_vk_q40_dot4_avx2, dan_vk_q4k_dot4_avx2, dan_vk_q5k_dot4_avx2, dan_vk_q6k_dot4_avx2, \
     dan_vk_q50_dot4_avx2, dan_vk_q41_dot4_avx2, dan_vk_q51_dot4_avx2, dan_vk_q80_dot4_avx2}

static const dan_vk_table dan_vk_avx2 = {"avx2",dan_vk_dot_f32_avx2, dan_vk_dot_f64_avx2, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2, dan_vk_mm_f32_avx2,
                                         dan_vk_mm_f64_avx2, dan_vk_q8_block_avx2, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx2, dan_vk_h2f_avx2, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx2_fast, dan_vk_mm_f64_avx2_fast,
                                         dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_AVX2_EXTRA};

/* ---- AVX-512 ---- */

DAN_VK_AVX512 static float dan_vk_dot_f32_avx512(const float *a, const float *b, int64_t n) {
    __m512 s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm512_setzero_ps();
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 4; j++)
            s[j] = _mm512_add_ps(s[j], _mm512_mul_ps(_mm512_loadu_ps(a + k + 16 * j), _mm512_loadu_ps(b + k + 16 * j)));
    for (int j = 0; j < 2; j++) s[j] = _mm512_add_ps(s[j], s[j + 2]);
    __m512 t = _mm512_add_ps(s[0], s[1]);
    __m256 lo = _mm512_castps512_ps256(t), hi = _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(t), 1));
    __m256 v8 = _mm256_add_ps(lo, hi);
    __m128 v = _mm_add_ps(_mm256_castps256_ps128(v8), _mm256_extractf128_ps(v8, 1));
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    float r = _mm_cvtss_f32(v);
    for (; k < n; k++) { float p = a[k] * b[k]; r = r + p; }
    return r;
}

DAN_VK_AVX512 static double dan_vk_dot_f64_avx512(const double *a, const double *b, int64_t n) {
    __m512d s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm512_setzero_pd();
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 4; j++)
            s[j] = _mm512_add_pd(s[j], _mm512_mul_pd(_mm512_loadu_pd(a + k + 8 * j), _mm512_loadu_pd(b + k + 8 * j)));
    for (int j = 0; j < 2; j++) s[j] = _mm512_add_pd(s[j], s[j + 2]);
    __m512d t = _mm512_add_pd(s[0], s[1]);
    __m256d v4 = _mm256_add_pd(_mm512_castpd512_pd256(t), _mm512_extractf64x4_pd(t, 1));
    __m128d v = _mm_add_pd(_mm256_castpd256_pd128(v4), _mm256_extractf128_pd(v4, 1));
    v = _mm_add_sd(v, _mm_unpackhi_pd(v, v));
    double r = _mm_cvtsd_f64(v);
    for (; k < n; k++) { double p = a[k] * b[k]; r = r + p; }
    return r;
}

/* tiles of 8 rows by 32 columns (float) or 8 by 16 (double) */
DAN_VK_AVX512 static void dan_vk_mm_f32_avx512_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    __m512 c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm512_setzero_ps();
    for (int64_t p = 0; p < k; p++) {
        __m512 b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm512_loadu_ps(Bp + p * 32 + 16 * q);
        for (int r = 0; r < 8; r++) {
            __m512 a = _mm512_set1_ps(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm512_add_ps(c[r][q], _mm512_mul_ps(a, b[q]));
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) _mm512_storeu_ps(C + r * n + 16 * q, c[r][q]);
}
DAN_VK_AVX512 static void dan_vk_mm_f32_avx512(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 8, 32, dan_vk_mm_f32_avx512_mk);
}

DAN_VK_AVX512 static void dan_vk_mm_f32_avx512_fast_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    __m512 c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm512_setzero_ps();
    for (int64_t p = 0; p < k; p++) {
        __m512 b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm512_loadu_ps(Bp + p * 32 + 16 * q);
        for (int r = 0; r < 8; r++) {
            __m512 a = _mm512_set1_ps(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm512_fmadd_ps(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) _mm512_storeu_ps(C + r * n + 16 * q, c[r][q]);
}
DAN_VK_AVX512 static void dan_vk_mm_f32_avx512_fast(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 8, 32, dan_vk_mm_f32_avx512_fast_mk);
}

DAN_VK_AVX512 static void dan_vk_mm_f64_avx512_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    __m512d c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm512_setzero_pd();
    for (int64_t p = 0; p < k; p++) {
        __m512d b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm512_loadu_pd(Bp + p * 16 + 8 * q);
        for (int r = 0; r < 8; r++) {
            __m512d a = _mm512_set1_pd(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm512_add_pd(c[r][q], _mm512_mul_pd(a, b[q]));
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) _mm512_storeu_pd(C + r * n + 8 * q, c[r][q]);
}
DAN_VK_AVX512 static void dan_vk_mm_f64_avx512(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 8, 16, dan_vk_mm_f64_avx512_mk);
}

DAN_VK_AVX512 static void dan_vk_mm_f64_avx512_fast_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    __m512d c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = _mm512_setzero_pd();
    for (int64_t p = 0; p < k; p++) {
        __m512d b[2];
        for (int q = 0; q < 2; q++) b[q] = _mm512_loadu_pd(Bp + p * 16 + 8 * q);
        for (int r = 0; r < 8; r++) {
            __m512d a = _mm512_set1_pd(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = _mm512_fmadd_pd(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) _mm512_storeu_pd(C + r * n + 8 * q, c[r][q]);
}
DAN_VK_AVX512 static void dan_vk_mm_f64_avx512_fast(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 8, 16, dan_vk_mm_f64_avx512_fast_mk);
}

DAN_VK_AVX512 static int32_t dan_vk_q8_block_avx512(const int8_t *a, const int8_t *b) {
    __m512i x = _mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i *)a));
    __m512i y = _mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i *)b));
    return _mm512_reduce_add_epi32(_mm512_madd_epi16(x, y));
}


DAN_VK_AVX512 static void dan_vk_dot32_acc_avx512(const float *a, const float *b, int64_t n, float *acc) {
    __m512 s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm512_loadu_ps(acc + 16 * j);
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 4; j++)
            s[j] = _mm512_add_ps(s[j], _mm512_mul_ps(_mm512_loadu_ps(a + k + 16 * j), _mm512_loadu_ps(b + k + 16 * j)));
    for (int j = 0; j < 4; j++) _mm512_storeu_ps(acc + 16 * j, s[j]);
}
DAN_VK_AVX512 static void dan_vk_h2f_avx512(const uint16_t *h, float *f, int64_t n) {
    int64_t i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(f + i, _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)(h + i))));
    for (; i < n; i++) f[i] = dan_f16_to_f32(h[i]);
}

static const dan_vk_table dan_vk_avx512 = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast,
                                           dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_AVX2_EXTRA};

/* avx512 with VNNI: the kernels of rows by several vectors use its instructions */
#define DAN_VK_VNNI_EXTRA dan_vk_q50_dot_vnni, dan_vk_q41_dot_vnni, dan_vk_q51_dot_vnni, \
    {dan_vk_q40_dot4_vnni, dan_vk_q4k_dot4_vnni, dan_vk_q5k_dot4_vnni, dan_vk_q6k_dot4_vnni, \
     dan_vk_q50_dot4_vnni, dan_vk_q41_dot4_vnni, dan_vk_q51_dot4_vnni, dan_vk_q80_dot4_vnni}
static const dan_vk_table dan_vk_avx512_vnni = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast,
                                           dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_VNNI_EXTRA};
#endif

#ifdef DAN_VK_ARM
/* ---- NEON ---- */
#define DAN_VK_FMA32(a, b, c) vfmaq_f32(c, a, b)
#define DAN_VK_FMA64(a, b, c) vfmaq_f64(c, a, b)

/* 4 of the 16 vectors of lanes: the products of 16 numbers from offset o.
   The empty asm ties the next loads to the end of this group: otherwise gcc
   and clang load all 32 vectors of an iteration at once and keep a part of
   the lanes on the stack (a store and a load per iteration), and the kernel
   was slower than plain C (on Neoverse-N2 21 GB/s against 26). The order of
   the sums does not change. */
#define DAN_VK_NEON_G4(s0, s1, s2, s3, o) do { \
    s0 = vaddq_f32(s0, vmulq_f32(vld1q_f32(pa + (o)), vld1q_f32(pb + (o)))); \
    s1 = vaddq_f32(s1, vmulq_f32(vld1q_f32(pa + (o) + 4), vld1q_f32(pb + (o) + 4))); \
    s2 = vaddq_f32(s2, vmulq_f32(vld1q_f32(pa + (o) + 8), vld1q_f32(pb + (o) + 8))); \
    s3 = vaddq_f32(s3, vmulq_f32(vld1q_f32(pa + (o) + 12), vld1q_f32(pb + (o) + 12))); \
    __asm__("" : "+r"(pa), "+r"(pb), "+w"(s0), "+w"(s1), "+w"(s2), "+w"(s3)); \
} while (0)

/* 64 lanes of a float dot product over n (a multiple of 64) numbers, from the lanes in s */
#define DAN_VK_NEON_DOT64(s) do { \
    float32x4_t s0 = s[0], s1 = s[1], s2 = s[2], s3 = s[3], s4 = s[4], s5 = s[5], s6 = s[6], s7 = s[7]; \
    float32x4_t s8 = s[8], s9 = s[9], s10 = s[10], s11 = s[11], s12 = s[12], s13 = s[13], s14 = s[14], s15 = s[15]; \
    const float *pa = a, *pb = b; \
    for (int64_t k_ = 0; k_ + 64 <= n; k_ += 64, pa += 64, pb += 64) { \
        /* the first factor (the weights) 1 KB ahead: on Neoverse-N2 from memory 16-17 GB/s on one \
           thread instead of 13-14, 65 instead of 56 on 4 */ \
        __builtin_prefetch(pa + 256); __builtin_prefetch(pa + 272); \
        __builtin_prefetch(pa + 288); __builtin_prefetch(pa + 304); \
        DAN_VK_NEON_G4(s0, s1, s2, s3, 0); \
        DAN_VK_NEON_G4(s4, s5, s6, s7, 16); \
        DAN_VK_NEON_G4(s8, s9, s10, s11, 32); \
        DAN_VK_NEON_G4(s12, s13, s14, s15, 48); \
    } \
    s[0] = s0; s[1] = s1; s[2] = s2; s[3] = s3; s[4] = s4; s[5] = s5; s[6] = s6; s[7] = s7; \
    s[8] = s8; s[9] = s9; s[10] = s10; s[11] = s11; s[12] = s12; s[13] = s13; s[14] = s14; s[15] = s15; \
} while (0)

static float dan_vk_dot_f32_neon(const float *a, const float *b, int64_t n) {
    float32x4_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f32(0.0f);
    DAN_VK_NEON_DOT64(s);
    int64_t k = n / 64 * 64;
    for (int j = 0; j < 8; j++) s[j] = vaddq_f32(s[j], s[j + 8]);
    for (int j = 0; j < 4; j++) s[j] = vaddq_f32(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = vaddq_f32(s[j], s[j + 2]);
    float32x4_t v = vaddq_f32(s[0], s[1]);
    float32x2_t h = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    float r = vget_lane_f32(h, 0) + vget_lane_f32(h, 1);
    for (; k < n; k++) { float p = a[k] * b[k]; r = r + p; }
    return r;
}

static double dan_vk_dot_f64_neon(const double *a, const double *b, int64_t n) {
    float64x2_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f64(0.0);
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 16; j++)
            s[j] = vaddq_f64(s[j], vmulq_f64(vld1q_f64(a + k + 2 * j), vld1q_f64(b + k + 2 * j)));
    for (int j = 0; j < 8; j++) s[j] = vaddq_f64(s[j], s[j + 8]);
    for (int j = 0; j < 4; j++) s[j] = vaddq_f64(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = vaddq_f64(s[j], s[j + 2]);
    float64x2_t v = vaddq_f64(s[0], s[1]);
    double r = vgetq_lane_f64(v, 0) + vgetq_lane_f64(v, 1);
    for (; k < n; k++) { double p = a[k] * b[k]; r = r + p; }
    return r;
}

static float dan_vk_sum_f32_neon(const float *a, int64_t n) {
    float32x4_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f32(0.0f);
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 16; j++) s[j] = vaddq_f32(s[j], vld1q_f32(a + k + 4 * j));
    for (int j = 0; j < 8; j++) s[j] = vaddq_f32(s[j], s[j + 8]);
    for (int j = 0; j < 4; j++) s[j] = vaddq_f32(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = vaddq_f32(s[j], s[j + 2]);
    float32x4_t v = vaddq_f32(s[0], s[1]);
    float32x2_t h = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    float r = vget_lane_f32(h, 0) + vget_lane_f32(h, 1);
    for (; k < n; k++) r = r + a[k];
    return r;
}
static double dan_vk_sum_f64_neon(const double *a, int64_t n) {
    float64x2_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f64(0.0);
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 16; j++) s[j] = vaddq_f64(s[j], vld1q_f64(a + k + 2 * j));
    for (int j = 0; j < 8; j++) s[j] = vaddq_f64(s[j], s[j + 8]);
    for (int j = 0; j < 4; j++) s[j] = vaddq_f64(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = vaddq_f64(s[j], s[j + 2]);
    float64x2_t v = vaddq_f64(s[0], s[1]);
    double r = vgetq_lane_f64(v, 0) + vgetq_lane_f64(v, 1);
    for (; k < n; k++) r = r + a[k];
    return r;
}

/* tiles of 8 rows by 8 columns (float) or 8 by 4 (double) */
static void dan_vk_mm_f32_neon_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    float32x4_t c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = vdupq_n_f32(0.0f);
    for (int64_t p = 0; p < k; p++) {
        float32x4_t b[2];
        for (int q = 0; q < 2; q++) b[q] = vld1q_f32(Bp + p * 8 + 4 * q);
        for (int r = 0; r < 8; r++) {
            float32x4_t a = vdupq_n_f32(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = vaddq_f32(c[r][q], vmulq_f32(a, b[q]));
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) vst1q_f32(C + r * n + 4 * q, c[r][q]);
}
static void dan_vk_mm_f32_neon(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 8, 8, dan_vk_mm_f32_neon_mk);
}

static void dan_vk_mm_f32_neon_fast_mk(const float *A, int64_t k, const float *Bp, float *C, int64_t n) {
    float32x4_t c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = vdupq_n_f32(0.0f);
    for (int64_t p = 0; p < k; p++) {
        float32x4_t b[2];
        for (int q = 0; q < 2; q++) b[q] = vld1q_f32(Bp + p * 8 + 4 * q);
        for (int r = 0; r < 8; r++) {
            float32x4_t a = vdupq_n_f32(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = DAN_VK_FMA32(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) vst1q_f32(C + r * n + 4 * q, c[r][q]);
}
static void dan_vk_mm_f32_neon_fast(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f32(A, B, C, m, n, k, j0, j1, 8, 8, dan_vk_mm_f32_neon_fast_mk);
}

static void dan_vk_mm_f64_neon_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    float64x2_t c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = vdupq_n_f64(0.0);
    for (int64_t p = 0; p < k; p++) {
        float64x2_t b[2];
        for (int q = 0; q < 2; q++) b[q] = vld1q_f64(Bp + p * 4 + 2 * q);
        for (int r = 0; r < 8; r++) {
            float64x2_t a = vdupq_n_f64(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = vaddq_f64(c[r][q], vmulq_f64(a, b[q]));
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) vst1q_f64(C + r * n + 2 * q, c[r][q]);
}
static void dan_vk_mm_f64_neon(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 8, 4, dan_vk_mm_f64_neon_mk);
}

static void dan_vk_mm_f64_neon_fast_mk(const double *A, int64_t k, const double *Bp, double *C, int64_t n) {
    float64x2_t c[8][2];
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) c[r][q] = vdupq_n_f64(0.0);
    for (int64_t p = 0; p < k; p++) {
        float64x2_t b[2];
        for (int q = 0; q < 2; q++) b[q] = vld1q_f64(Bp + p * 4 + 2 * q);
        for (int r = 0; r < 8; r++) {
            float64x2_t a = vdupq_n_f64(A[r * k + p]);
            for (int q = 0; q < 2; q++) c[r][q] = DAN_VK_FMA64(a, b[q], c[r][q]);
        }
    }
    for (int r = 0; r < 8; r++) for (int q = 0; q < 2; q++) vst1q_f64(C + r * n + 2 * q, c[r][q]);
}
static void dan_vk_mm_f64_neon_fast(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k, int64_t j0, int64_t j1) {
    dan_vk_mm_drive_f64(A, B, C, m, n, k, j0, j1, 8, 4, dan_vk_mm_f64_neon_fast_mk);
}

static int32_t dan_vk_q8_block_neon(const int8_t *a, const int8_t *b) {
    int32x4_t s = vdupq_n_s32(0);
    for (int h = 0; h < 2; h++) {
        int8x16_t x = vld1q_s8(a + 16 * h), y = vld1q_s8(b + 16 * h);
        s = vpadalq_s16(s, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
        s = vpadalq_s16(s, vmull_s8(vget_high_s8(x), vget_high_s8(y)));
    }
    return vaddvq_s32(s);
}

static inline int32x4_t dan_vk_q8_part_neon(const uint8_t *a, const uint8_t *b) {
    int32x4_t s = vdupq_n_s32(0);
    for (int h = 0; h < 2; h++) {
        int8x16_t x = vld1q_s8((const int8_t *)a + 2 + 16 * h), y = vld1q_s8((const int8_t *)b + 2 + 16 * h);
        s = vpadalq_s16(s, vmull_s8(vget_low_s8(x), vget_low_s8(y)));
        s = vpadalq_s16(s, vmull_s8(vget_high_s8(x), vget_high_s8(y)));
    }
    return s;
}

static inline float32x4_t dan_vk_q8_scales_neon(const uint8_t *p) {
    uint16_t h[4];
    for (int l = 0; l < 4; l++) memcpy(&h[l], p + l * DAN_Q8_BYTES, 2);
    return vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(h)));
}

/* 8 blocks at a time in two vectors of 4 lanes */
#define DAN_VK_Q8_DOT_NEON(NAME, ATTR, PART, BLOCK) \
ATTR static float NAME(const uint8_t *a, const uint8_t *b, int64_t blocks) { \
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES; \
        int32x4_t v[8]; \
        for (int l = 0; l < 8; l++) v[l] = PART(pa + l * DAN_Q8_BYTES, pb + l * DAN_Q8_BYTES); \
        int32x4_t s0 = vpaddq_s32(vpaddq_s32(v[0], v[1]), vpaddq_s32(v[2], v[3])); \
        int32x4_t s1 = vpaddq_s32(vpaddq_s32(v[4], v[5]), vpaddq_s32(v[6], v[7])); \
        float32x4_t d0 = vmulq_f32(dan_vk_q8_scales_neon(pa), dan_vk_q8_scales_neon(pb)); \
        float32x4_t d1 = vmulq_f32(dan_vk_q8_scales_neon(pa + 4 * DAN_Q8_BYTES), dan_vk_q8_scales_neon(pb + 4 * DAN_Q8_BYTES)); \
        acc0 = vaddq_f32(acc0, vmulq_f32(d0, vcvtq_f32_s32(s0))); \
        acc1 = vaddq_f32(acc1, vmulq_f32(d1, vcvtq_f32_s32(s1))); \
    } \
    float32x4_t v4 = vaddq_f32(acc0, acc1); \
    float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4)); \
    float r = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1); \
    for (; i < blocks; i++) { \
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES; \
        float p = dan_vk_q8_p(pa, pb, BLOCK((const int8_t *)pa + 2, (const int8_t *)pb + 2)); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_Q8_DOT_NEON(dan_vk_q8_dot_neon, , dan_vk_q8_part_neon, dan_vk_q8_block_neon)

static void dan_vk_dot32_acc_neon(const float *a, const float *b, int64_t n, float *acc) {
    float32x4_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vld1q_f32(acc + 4 * j);
    DAN_VK_NEON_DOT64(s);
    for (int j = 0; j < 16; j++) vst1q_f32(acc + 4 * j, s[j]);
}
static void dan_vk_h2f_neon(const uint16_t *h, float *f, int64_t n) {
    int64_t i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(f + i, vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(h + i))));
    for (; i < n; i++) f[i] = dan_f16_to_f32(h[i]);
}


/* «приближённо»: fused multiply-add */
static float dan_vk_dot_f32_neon_fast(const float *a, const float *b, int64_t n) {
    float32x4_t s[8];
    for (int j = 0; j < 8; j++) s[j] = vdupq_n_f32(0.0f);
    int64_t k = 0;
    for (; k + 32 <= n; k += 32)
        for (int j = 0; j < 8; j++) s[j] = vfmaq_f32(s[j], vld1q_f32(a + k + 4 * j), vld1q_f32(b + k + 4 * j));
    for (; k + 4 <= n; k += 4) s[0] = vfmaq_f32(s[0], vld1q_f32(a + k), vld1q_f32(b + k));
    for (int j = 1; j < 8; j++) s[0] = vaddq_f32(s[0], s[j]);
    float r = vaddvq_f32(s[0]);
    for (; k < n; k++) r = fmaf(a[k], b[k], r);
    return r;
}
static double dan_vk_dot_f64_neon_fast(const double *a, const double *b, int64_t n) {
    float64x2_t s[8];
    for (int j = 0; j < 8; j++) s[j] = vdupq_n_f64(0.0);
    int64_t k = 0;
    for (; k + 16 <= n; k += 16)
        for (int j = 0; j < 8; j++) s[j] = vfmaq_f64(s[j], vld1q_f64(a + k + 2 * j), vld1q_f64(b + k + 2 * j));
    for (; k + 2 <= n; k += 2) s[0] = vfmaq_f64(s[0], vld1q_f64(a + k), vld1q_f64(b + k));
    for (int j = 1; j < 8; j++) s[0] = vaddq_f64(s[0], s[j]);
    double r = vaddvq_f64(s[0]);
    for (; k < n; k++) r = fma(a[k], b[k], r);
    return r;
}

/* ---- NEON: weights of llama.cpp ---- */

static inline float32x4_t dan_vk_halves4_neon(const uint8_t *p, int stride) {
    uint16_t h[4];
    for (int l = 0; l < 4; l++) memcpy(&h[l], p + l * stride, 2);
    return vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(h)));
}

/* 16 products of a and b (each fits 16 bits) added to acc in 4 sums */
static inline int32x4_t dan_vk_dot16_neon(int32x4_t acc, int8x16_t a, int8x16_t b) {
    acc = vpadalq_s16(acc, vmull_s8(vget_low_s8(a), vget_low_s8(b)));
    return vpadalq_s16(acc, vmull_s8(vget_high_s8(a), vget_high_s8(b)));
}

/* the sums of the 4 lanes of 4 vectors as one vector */
static inline int32x4_t dan_vk_sum4_neon(const int32x4_t *d) {
    return vpaddq_s32(vpaddq_s32(d[0], d[1]), vpaddq_s32(d[2], d[3]));
}

/* Q4_K, Q5_K: d - 8 parts of 32 (sums in 4 lanes), sm - scales and mins
   (dan_vk_k4_sm); lanes of the result add up to isum, *imin - the sum of the
   mins with the sums of the vector's parts of 32 */
static inline int32x4_t dan_vk_k4_apply_neon(const int32x4_t *d, const uint8_t *sm, const dan_q8k *y, int32_t *imin) {
    uint8x16_t smv = vld1q_u8(sm);
    uint16x8_t s16 = vmovl_u8(vget_low_u8(smv));
    int32x4_t sc0 = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(s16)));
    int32x4_t sc1 = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(s16)));
    int16x8_t pairs = vpaddq_s16(vld1q_s16(y->bsums), vld1q_s16(y->bsums + 8));   /* below 2 * 16 * 128 */
    int16x8_t mm = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(smv)));
    int32x4_t ms = vmull_s16(vget_low_s16(mm), vget_low_s16(pairs));
    ms = vmlal_s16(ms, vget_high_s16(mm), vget_high_s16(pairs));
    *imin = vaddvq_s32(ms);
    return vmlaq_s32(vmulq_s32(dan_vk_sum4_neon(d), sc0), dan_vk_sum4_neon(d + 4), sc1);
}

/* Q6_K: d - 16 parts of 16, the scales sc; minus 32 * sum of sc * bsums */
static inline int32x4_t dan_vk_q6k_apply_neon(const int32x4_t *d, const int8_t *sc, const dan_q8k *y) {
    int8x16_t s = vld1q_s8(sc);
    int16x8_t lo = vmovl_s8(vget_low_s8(s)), hi = vmovl_s8(vget_high_s8(s));
    int16x8_t b0 = vld1q_s16(y->bsums), b1 = vld1q_s16(y->bsums + 8);
    int32x4_t bias = vmull_s16(vget_low_s16(lo), vget_low_s16(b0));
    bias = vmlal_s16(bias, vget_high_s16(lo), vget_high_s16(b0));
    bias = vmlal_s16(bias, vget_low_s16(hi), vget_low_s16(b1));
    bias = vmlal_s16(bias, vget_high_s16(hi), vget_high_s16(b1));
    int32x4_t acc = vnegq_s32(vshlq_n_s32(bias, 5));
    acc = vmlaq_s32(acc, dan_vk_sum4_neon(d), vmovl_s16(vget_low_s16(lo)));
    acc = vmlaq_s32(acc, dan_vk_sum4_neon(d + 4), vmovl_s16(vget_high_s16(lo)));
    acc = vmlaq_s32(acc, dan_vk_sum4_neon(d + 8), vmovl_s16(vget_low_s16(hi)));
    return vmlaq_s32(acc, dan_vk_sum4_neon(d + 12), vmovl_s16(vget_high_s16(hi)));
}

/* 8 blocks at a time: lane l of acc0 and acc1 - blocks l and l + 4 of every 8 */
#define DAN_VK_KDOT_NEON(NAME, ATTR, PARTS, WB, DOFF, MINS) \
ATTR static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const dan_q8k *x = (const dan_q8k *)xv; \
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        const dan_q8k *px = x + i; \
        int32x4_t v[8]; \
        int32_t ex[8]; \
        for (int l = 0; l < 8; l++) v[l] = PARTS(pw + l * (WB), px + l, &ex[l]); \
        int32x4_t s0 = vpaddq_s32(vpaddq_s32(v[0], v[1]), vpaddq_s32(v[2], v[3])); \
        int32x4_t s1 = vpaddq_s32(vpaddq_s32(v[4], v[5]), vpaddq_s32(v[6], v[7])); \
        float yd[8]; \
        for (int l = 0; l < 8; l++) yd[l] = px[l].d; \
        float32x4_t y0 = vld1q_f32(yd), y1 = vld1q_f32(yd + 4); \
        float32x4_t t0 = vmulq_f32(vmulq_f32(y0, dan_vk_halves4_neon(pw + (DOFF), WB)), vcvtq_f32_s32(s0)); \
        float32x4_t t1 = vmulq_f32(vmulq_f32(y1, dan_vk_halves4_neon(pw + 4 * (WB) + (DOFF), WB)), vcvtq_f32_s32(s1)); \
        if (MINS) { \
            float32x4_t b0 = vmulq_f32(y0, dan_vk_halves4_neon(pw + 2, WB)); \
            float32x4_t b1 = vmulq_f32(y1, dan_vk_halves4_neon(pw + 4 * (WB) + 2, WB)); \
            t0 = vsubq_f32(t0, vmulq_f32(b0, vcvtq_f32_s32(vld1q_s32(ex)))); \
            t1 = vsubq_f32(t1, vmulq_f32(b1, vcvtq_f32_s32(vld1q_s32(ex + 4)))); \
        } \
        acc0 = vaddq_f32(acc0, t0); \
        acc1 = vaddq_f32(acc1, t1); \
    } \
    float32x4_t v4 = vaddq_f32(acc0, acc1); \
    float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4)); \
    float r = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1); \
    for (; i < blocks; i++) { \
        int32_t ex; \
        int32_t is = vaddvq_s32(PARTS(w + i * (WB), x + i, &ex)); \
        float p = MINS ? dan_vk_k4_p(w + i * (WB), x + i, is, ex) : dan_vk_q6k_p(w + i * (WB), x + i, is, 0); \
        r = r + p; \
    } \
    return r; \
}

#define DAN_VK_S8(u) vreinterpretq_s8_u8(u)

/* the kernels of weights of llama.cpp for a way to multiply 16 numbers
   (DOT16): plain NEON or the dot product instructions */
#define DAN_VK_NEON_QUANTS(SUF, ATTR, DOT16) \
ATTR static inline int32x4_t dan_vk_q40_part_##SUF(const uint8_t *w, const uint8_t *x) { \
    uint8x16_t q = vld1q_u8(w + 2); \
    int8x16_t lo = vsubq_s8(DAN_VK_S8(vandq_u8(q, vdupq_n_u8(0xF))), vdupq_n_s8(8)); \
    int8x16_t hi = vsubq_s8(DAN_VK_S8(vshrq_n_u8(q, 4)), vdupq_n_s8(8)); \
    int32x4_t s = DOT16(vdupq_n_s32(0), lo, vld1q_s8((const int8_t *)x + 2)); \
    return DOT16(s, hi, vld1q_s8((const int8_t *)x + 18)); \
} \
ATTR static int32_t dan_vk_q40_block_##SUF(const uint8_t *w, const uint8_t *x) { \
    return vaddvq_s32(dan_vk_q40_part_##SUF(w, x)); \
} \
/* 8 blocks at a time in two vectors of 4 lanes */ \
ATTR static float dan_vk_q40_dot_##SUF(const uint8_t *w, const void *xv, int64_t blocks) { \
    const uint8_t *x = (const uint8_t *)xv; \
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * DAN_Q40_BYTES, *px = x + i * DAN_Q8_BYTES; \
        int32x4_t v[8]; \
        for (int l = 0; l < 8; l++) v[l] = dan_vk_q40_part_##SUF(pw + l * DAN_Q40_BYTES, px + l * DAN_Q8_BYTES); \
        int32x4_t s0 = vpaddq_s32(vpaddq_s32(v[0], v[1]), vpaddq_s32(v[2], v[3])); \
        int32x4_t s1 = vpaddq_s32(vpaddq_s32(v[4], v[5]), vpaddq_s32(v[6], v[7])); \
        float32x4_t d0 = vmulq_f32(dan_vk_halves4_neon(pw, DAN_Q40_BYTES), dan_vk_halves4_neon(px, DAN_Q8_BYTES)); \
        float32x4_t d1 = vmulq_f32(dan_vk_halves4_neon(pw + 4 * DAN_Q40_BYTES, DAN_Q40_BYTES), \
                                   dan_vk_halves4_neon(px + 4 * DAN_Q8_BYTES, DAN_Q8_BYTES)); \
        acc0 = vaddq_f32(acc0, vmulq_f32(d0, vcvtq_f32_s32(s0))); \
        acc1 = vaddq_f32(acc1, vmulq_f32(d1, vcvtq_f32_s32(s1))); \
    } \
    float32x4_t v4 = vaddq_f32(acc0, acc1); \
    float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4)); \
    float r = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1); \
    for (; i < blocks; i++) { \
        const uint8_t *pw = w + i * DAN_Q40_BYTES, *px = x + i * DAN_Q8_BYTES; \
        float p = dan_vk_q8_p(pw, px, dan_vk_q40_block_##SUF(pw, px)); \
        r = r + p; \
    } \
    return r; \
} \
/* the integer part of a block: the 4 lanes add up to isum; *extra - imin. \
   The sums of the parts are gathered by pairwise adds and multiplied by the \
   scales at once (a multiplication per part was slower on Neoverse-N2) */ \
ATTR static inline int32x4_t dan_vk_q4k_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    uint8_t sm[16]; \
    dan_vk_k4_sm(w + 4, sm); \
    const uint8x16_t m4 = vdupq_n_u8(0xF); \
    int32x4_t d[8]; \
    for (int p = 0; p < 4; p++) { \
        const uint8_t *q = w + 16 + 32 * p; \
        const int8_t *v = y->qs + 64 * p; \
        uint8x16_t q0 = vld1q_u8(q), q1 = vld1q_u8(q + 16); \
        d[2 * p] = DOT16(DOT16(vdupq_n_s32(0), DAN_VK_S8(vandq_u8(q0, m4)), vld1q_s8(v)), \
                         DAN_VK_S8(vandq_u8(q1, m4)), vld1q_s8(v + 16)); \
        d[2 * p + 1] = DOT16(DOT16(vdupq_n_s32(0), DAN_VK_S8(vshrq_n_u8(q0, 4)), vld1q_s8(v + 32)), \
                             DAN_VK_S8(vshrq_n_u8(q1, 4)), vld1q_s8(v + 48)); \
    } \
    return dan_vk_k4_apply_neon(d, sm, y, extra); \
} \
ATTR static inline int32x4_t dan_vk_q5k_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    uint8_t sm[16]; \
    dan_vk_k4_sm(w + 4, sm); \
    const uint8x16_t m4 = vdupq_n_u8(0xF), b16 = vdupq_n_u8(16); \
    uint8x16_t h0 = vld1q_u8(w + 16), h1 = vld1q_u8(w + 32); \
    int32x4_t d[8]; \
    for (int p = 0; p < 4; p++) { \
        const uint8_t *q = w + 48 + 32 * p; \
        const int8_t *v = y->qs + 64 * p; \
        uint8x16_t u1 = vdupq_n_u8((uint8_t)(1u << (2 * p))), u2 = vdupq_n_u8((uint8_t)(2u << (2 * p))); \
        uint8x16_t q0 = vld1q_u8(q), q1 = vld1q_u8(q + 16); \
        uint8x16_t a0 = vorrq_u8(vandq_u8(q0, m4), vandq_u8(vtstq_u8(h0, u1), b16)); \
        uint8x16_t a1 = vorrq_u8(vandq_u8(q1, m4), vandq_u8(vtstq_u8(h1, u1), b16)); \
        uint8x16_t c0 = vorrq_u8(vshrq_n_u8(q0, 4), vandq_u8(vtstq_u8(h0, u2), b16)); \
        uint8x16_t c1 = vorrq_u8(vshrq_n_u8(q1, 4), vandq_u8(vtstq_u8(h1, u2), b16)); \
        d[2 * p] = DOT16(DOT16(vdupq_n_s32(0), DAN_VK_S8(a0), vld1q_s8(v)), DAN_VK_S8(a1), vld1q_s8(v + 16)); \
        d[2 * p + 1] = DOT16(DOT16(vdupq_n_s32(0), DAN_VK_S8(c0), vld1q_s8(v + 32)), DAN_VK_S8(c1), vld1q_s8(v + 48)); \
    } \
    return dan_vk_k4_apply_neon(d, sm, y, extra); \
} \
/* Q6_K: 16 parts of 16; the bias 32 * sum of sc * bsums is taken off here, *extra = 0 */ \
ATTR static inline int32x4_t dan_vk_q6k_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    const uint8x16_t m4 = vdupq_n_u8(0xF), m3 = vdupq_n_u8(3); \
    const int8_t *sc = (const int8_t *)w + 192; \
    int32x4_t d[16]; \
    for (int n = 0; n < 2; n++) { \
        const uint8_t *ql = w + 64 * n, *qh = w + 128 + 32 * n; \
        const int8_t *v = y->qs + 128 * n; \
        int32x4_t *e = d + 8 * n; \
        for (int t = 0; t < 2; t++) {   /* numbers 16t..16t + 15 of each quarter: parts t, 2 + t, 4 + t, 6 + t */ \
            uint8x16_t l0 = vld1q_u8(ql + 16 * t), l1 = vld1q_u8(ql + 32 + 16 * t), h = vld1q_u8(qh + 16 * t); \
            uint8x16_t q1 = vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(h, m3), 4)); \
            uint8x16_t q2 = vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 2), m3), 4)); \
            uint8x16_t q3 = vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 4), m3), 4)); \
            uint8x16_t q4 = vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(h, 6), 4)); \
            e[t] = DOT16(vdupq_n_s32(0), DAN_VK_S8(q1), vld1q_s8(v + 16 * t)); \
            e[2 + t] = DOT16(vdupq_n_s32(0), DAN_VK_S8(q2), vld1q_s8(v + 32 + 16 * t)); \
            e[4 + t] = DOT16(vdupq_n_s32(0), DAN_VK_S8(q3), vld1q_s8(v + 64 + 16 * t)); \
            e[6 + t] = DOT16(vdupq_n_s32(0), DAN_VK_S8(q4), vld1q_s8(v + 96 + 16 * t)); \
        } \
    } \
    *extra = 0; \
    return dan_vk_q6k_apply_neon(d, sc, y); \
} \
DAN_VK_KDOT_NEON(dan_vk_q4k_dot_##SUF, ATTR, dan_vk_q4k_parts_##SUF, DAN_Q4K_BYTES, 0, 1) \
DAN_VK_KDOT_NEON(dan_vk_q5k_dot_##SUF, ATTR, dan_vk_q5k_parts_##SUF, DAN_Q5K_BYTES, 0, 1) \
DAN_VK_KDOT_NEON(dan_vk_q6k_dot_##SUF, ATTR, dan_vk_q6k_parts_##SUF, DAN_Q6K_BYTES, 208, 0)

DAN_VK_NEON_QUANTS(neon, , dan_vk_dot16_neon)

/* ---- NEON: rows of weights by several vectors at once (as the AVX2 kernels) ---- */

/* the numbers of a block of 32 as two vectors of 16 (numbers 0..15 and 16..31) */
typedef struct { int8x16_t lo, hi; } dan_vk_w32_neon;
static inline uint8x16_t dan_vk_bits16_neon(uint32_t bits) {   /* 16 where bit j is set */
    static const uint8_t mk[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t b = vcombine_u8(vdup_n_u8((uint8_t)(bits & 0xff)), vdup_n_u8((uint8_t)((bits >> 8) & 0xff)));
    return vandq_u8(vtstq_u8(b, vld1q_u8(mk)), vdupq_n_u8(16));
}
static inline dan_vk_w32_neon dan_vk_nib32_neon(const uint8_t *q, uint32_t qh, int sub) {
    uint8x16_t v = vld1q_u8(q);
    uint8x16_t lo = vandq_u8(v, vdupq_n_u8(0xF)), hi = vshrq_n_u8(v, 4);
    if (sub >= 0) {   /* fifth bits */
        lo = vorrq_u8(lo, dan_vk_bits16_neon(qh));
        hi = vorrq_u8(hi, dan_vk_bits16_neon(qh >> 16));
    }
    dan_vk_w32_neon r = {DAN_VK_S8(lo), DAN_VK_S8(hi)};
    if (sub > 0) { r.lo = vsubq_s8(r.lo, vdupq_n_s8((int8_t)sub)); r.hi = vsubq_s8(r.hi, vdupq_n_s8((int8_t)sub)); }
    return r;
}
static inline dan_vk_w32_neon dan_vk_w_q40_neon(const uint8_t *w) {
    dan_vk_w32_neon r = dan_vk_nib32_neon(w + 2, 0, -1);
    r.lo = vsubq_s8(r.lo, vdupq_n_s8(8));
    r.hi = vsubq_s8(r.hi, vdupq_n_s8(8));
    return r;
}
static inline dan_vk_w32_neon dan_vk_w_q50_neon(const uint8_t *w) { return dan_vk_nib32_neon(w + 6, dan_vk_u32(w + 2), 16); }
static inline dan_vk_w32_neon dan_vk_w_q80_neon(const uint8_t *w) {
    dan_vk_w32_neon r = {vld1q_s8((const int8_t *)w + 2), vld1q_s8((const int8_t *)w + 18)};
    return r;
}
static inline dan_vk_w32_neon dan_vk_w_q41_neon(const uint8_t *w) { return dan_vk_nib32_neon(w + 4, 0, -1); }
static inline dan_vk_w32_neon dan_vk_w_q51_neon(const uint8_t *w) { return dan_vk_nib32_neon(w + 8, dan_vk_u32(w + 4), 0); }

static inline float32x4_t dan_vk_sums4_neon(const uint8_t *p, int stride) {
    int32_t s[4];
    for (int l = 0; l < 4; l++) { int16_t v; memcpy(&v, p + l * stride, 2); s[l] = v; }
    return vcvtq_f32_s32(vld1q_s32(s));
}

/* a row of blocks of 32 by NV vectors: lanes as in the row kernel of Q4_0 */
#define DAN_VK_B32N_NEON(NAME, ATTR, MUL32, NV, WFN, WB, XB, XOFF, MINS, PF) \
ATTR static void NAME(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    float32x4_t acc0[NV], acc1[NV]; \
    for (int k = 0; k < (NV); k++) { acc0[k] = vdupq_n_f32(0.0f); acc1[k] = vdupq_n_f32(0.0f); } \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        int32x4_t v[NV][8]; \
        for (int l = 0; l < 8; l++) { \
            dan_vk_w32_neon q = WFN(pw + l * (WB)); \
            for (int k = 0; k < (NV); k++) v[k][l] = MUL32(q.lo, q.hi, x + k * xs + (i + l) * (XB) + (XOFF)); \
        } \
        float32x4_t dw0 = dan_vk_halves4_neon(pw, WB), dw1 = dan_vk_halves4_neon(pw + 4 * (WB), WB); \
        for (int k = 0; k < (NV); k++) { \
            const uint8_t *px = x + k * xs + i * (XB); \
            int32x4_t s0 = vpaddq_s32(vpaddq_s32(v[k][0], v[k][1]), vpaddq_s32(v[k][2], v[k][3])); \
            int32x4_t s1 = vpaddq_s32(vpaddq_s32(v[k][4], v[k][5]), vpaddq_s32(v[k][6], v[k][7])); \
            float32x4_t dx0 = dan_vk_halves4_neon(px, XB), dx1 = dan_vk_halves4_neon(px + 4 * (XB), XB); \
            float32x4_t t0 = vmulq_f32(vmulq_f32(dw0, dx0), vcvtq_f32_s32(s0)); \
            float32x4_t t1 = vmulq_f32(vmulq_f32(dw1, dx1), vcvtq_f32_s32(s1)); \
            if (MINS) { \
                float32x4_t b0 = vmulq_f32(dx0, dan_vk_halves4_neon(pw + 2, WB)); \
                float32x4_t b1 = vmulq_f32(dx1, dan_vk_halves4_neon(pw + 4 * (WB) + 2, WB)); \
                t0 = vaddq_f32(t0, vmulq_f32(b0, dan_vk_sums4_neon(px + 2, XB))); \
                t1 = vaddq_f32(t1, vmulq_f32(b1, dan_vk_sums4_neon(px + 4 * (XB) + 2, XB))); \
            } \
            acc0[k] = vaddq_f32(acc0[k], t0); \
            acc1[k] = vaddq_f32(acc1[k], t1); \
        } \
    } \
    float r[NV]; \
    for (int k = 0; k < (NV); k++) { \
        float32x4_t v4 = vaddq_f32(acc0[k], acc1[k]); \
        float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4)); \
        r[k] = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1); \
    } \
    for (; i < blocks; i++) { \
        const uint8_t *pw = w + i * (WB); \
        dan_vk_w32_neon q = WFN(pw); \
        for (int k = 0; k < (NV); k++) { \
            const uint8_t *px = x + k * xs + i * (XB); \
            float p = PF(pw, px, vaddvq_s32(MUL32(q.lo, q.hi, px + (XOFF)))); \
            r[k] = r[k] + p; \
        } \
    } \
    for (int k = 0; k < (NV); k++) out[k] = r[k]; \
}

/* a row of K blocks by DAN_VK_NV vectors: lanes as in the row kernel */
typedef void (*dan_vk_kblk_neon)(const uint8_t *, const dan_q8k *const *, int32x4_t *, int32_t *);
static inline __attribute__((always_inline)) void dan_vk_kdot4_neon(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks,
                                                                     float *out, dan_vk_kblk_neon blk, int WB, int DOFF, int MINS) {
    const dan_q8k *xv[DAN_VK_NV];
    float32x4_t acc0[DAN_VK_NV], acc1[DAN_VK_NV];
    for (int k = 0; k < DAN_VK_NV; k++) {
        xv[k] = (const dan_q8k *)(const void *)(x + k * xs);
        acc0[k] = vdupq_n_f32(0.0f);
        acc1[k] = vdupq_n_f32(0.0f);
    }
    int64_t i = 0;
    for (; i + 8 <= blocks; i += 8) {
        const uint8_t *pw = w + i * WB;
        int32x4_t v[8][DAN_VK_NV];
        int32_t ex[8][DAN_VK_NV];
        for (int l = 0; l < 8; l++) {
            const dan_q8k *y[DAN_VK_NV];
            for (int k = 0; k < DAN_VK_NV; k++) y[k] = xv[k] + i + l;
            blk(pw + l * WB, y, v[l], ex[l]);
        }
        float32x4_t dw0 = dan_vk_halves4_neon(pw + DOFF, WB), dw1 = dan_vk_halves4_neon(pw + 4 * WB + DOFF, WB);
        float32x4_t mw0 = vdupq_n_f32(0.0f), mw1 = vdupq_n_f32(0.0f);
        if (MINS) { mw0 = dan_vk_halves4_neon(pw + 2, WB); mw1 = dan_vk_halves4_neon(pw + 4 * WB + 2, WB); }
        for (int k = 0; k < DAN_VK_NV; k++) {
            const dan_q8k *px = xv[k] + i;
            int32x4_t s0 = vpaddq_s32(vpaddq_s32(v[0][k], v[1][k]), vpaddq_s32(v[2][k], v[3][k]));
            int32x4_t s1 = vpaddq_s32(vpaddq_s32(v[4][k], v[5][k]), vpaddq_s32(v[6][k], v[7][k]));
            float yd[8];
            for (int l = 0; l < 8; l++) yd[l] = px[l].d;
            float32x4_t y0 = vld1q_f32(yd), y1 = vld1q_f32(yd + 4);
            float32x4_t t0 = vmulq_f32(vmulq_f32(y0, dw0), vcvtq_f32_s32(s0));
            float32x4_t t1 = vmulq_f32(vmulq_f32(y1, dw1), vcvtq_f32_s32(s1));
            if (MINS) {
                int32_t e[8];
                for (int l = 0; l < 8; l++) e[l] = ex[l][k];
                t0 = vsubq_f32(t0, vmulq_f32(vmulq_f32(y0, mw0), vcvtq_f32_s32(vld1q_s32(e))));
                t1 = vsubq_f32(t1, vmulq_f32(vmulq_f32(y1, mw1), vcvtq_f32_s32(vld1q_s32(e + 4))));
            }
            acc0[k] = vaddq_f32(acc0[k], t0);
            acc1[k] = vaddq_f32(acc1[k], t1);
        }
    }
    float r[DAN_VK_NV];
    for (int k = 0; k < DAN_VK_NV; k++) {
        float32x4_t v4 = vaddq_f32(acc0[k], acc1[k]);
        float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4));
        r[k] = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1);
    }
    for (; i < blocks; i++) {
        const dan_q8k *y[DAN_VK_NV];
        for (int k = 0; k < DAN_VK_NV; k++) y[k] = xv[k] + i;
        int32x4_t a[DAN_VK_NV];
        int32_t m[DAN_VK_NV];
        blk(w + i * WB, y, a, m);
        for (int k = 0; k < DAN_VK_NV; k++) {
            int32_t is = vaddvq_s32(a[k]);
            float p = MINS ? dan_vk_k4_p(w + i * WB, y[k], is, m[k]) : dan_vk_q6k_p(w + i * WB, y[k], is, 0);
            r[k] = r[k] + p;
        }
    }
    for (int k = 0; k < DAN_VK_NV; k++) out[k] = r[k];
}

#define DAN_VK_NEON_N(SUF, ATTR, DOT16) \
ATTR static inline void dan_vk_q4k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, int32x4_t *out, int32_t *imin) { \
    uint8_t sm[16]; \
    dan_vk_k4_sm(w + 4, sm); \
    const uint8x16_t m4 = vdupq_n_u8(0xF); \
    int32x4_t d[DAN_VK_NV][8]; \
    for (int p = 0; p < 4; p++) { \
        uint8x16_t q0 = vld1q_u8(w + 16 + 32 * p), q1 = vld1q_u8(w + 32 + 32 * p); \
        int8x16_t a0 = DAN_VK_S8(vandq_u8(q0, m4)), a1 = DAN_VK_S8(vandq_u8(q1, m4)); \
        int8x16_t c0 = DAN_VK_S8(vshrq_n_u8(q0, 4)), c1 = DAN_VK_S8(vshrq_n_u8(q1, 4)); \
        for (int k = 0; k < DAN_VK_NV; k++) { \
            const int8_t *v = y[k]->qs + 64 * p; \
            d[k][2 * p] = DOT16(DOT16(vdupq_n_s32(0), a0, vld1q_s8(v)), a1, vld1q_s8(v + 16)); \
            d[k][2 * p + 1] = DOT16(DOT16(vdupq_n_s32(0), c0, vld1q_s8(v + 32)), c1, vld1q_s8(v + 48)); \
        } \
    } \
    for (int k = 0; k < DAN_VK_NV; k++) out[k] = dan_vk_k4_apply_neon(d[k], sm, y[k], &imin[k]); \
} \
ATTR static inline void dan_vk_q5k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, int32x4_t *out, int32_t *imin) { \
    uint8_t sm[16]; \
    dan_vk_k4_sm(w + 4, sm); \
    const uint8x16_t m4 = vdupq_n_u8(0xF), b16 = vdupq_n_u8(16); \
    uint8x16_t h0 = vld1q_u8(w + 16), h1 = vld1q_u8(w + 32); \
    int32x4_t d[DAN_VK_NV][8]; \
    for (int p = 0; p < 4; p++) { \
        uint8x16_t u1 = vdupq_n_u8((uint8_t)(1u << (2 * p))), u2 = vdupq_n_u8((uint8_t)(2u << (2 * p))); \
        uint8x16_t q0 = vld1q_u8(w + 48 + 32 * p), q1 = vld1q_u8(w + 64 + 32 * p); \
        int8x16_t a0 = DAN_VK_S8(vorrq_u8(vandq_u8(q0, m4), vandq_u8(vtstq_u8(h0, u1), b16))); \
        int8x16_t a1 = DAN_VK_S8(vorrq_u8(vandq_u8(q1, m4), vandq_u8(vtstq_u8(h1, u1), b16))); \
        int8x16_t c0 = DAN_VK_S8(vorrq_u8(vshrq_n_u8(q0, 4), vandq_u8(vtstq_u8(h0, u2), b16))); \
        int8x16_t c1 = DAN_VK_S8(vorrq_u8(vshrq_n_u8(q1, 4), vandq_u8(vtstq_u8(h1, u2), b16))); \
        for (int k = 0; k < DAN_VK_NV; k++) { \
            const int8_t *v = y[k]->qs + 64 * p; \
            d[k][2 * p] = DOT16(DOT16(vdupq_n_s32(0), a0, vld1q_s8(v)), a1, vld1q_s8(v + 16)); \
            d[k][2 * p + 1] = DOT16(DOT16(vdupq_n_s32(0), c0, vld1q_s8(v + 32)), c1, vld1q_s8(v + 48)); \
        } \
    } \
    for (int k = 0; k < DAN_VK_NV; k++) out[k] = dan_vk_k4_apply_neon(d[k], sm, y[k], &imin[k]); \
} \
ATTR static inline void dan_vk_q6k_blk4_##SUF(const uint8_t *w, const dan_q8k *const *y, int32x4_t *out, int32_t *imin) { \
    const uint8x16_t m4 = vdupq_n_u8(0xF), m3 = vdupq_n_u8(3); \
    int32x4_t d[DAN_VK_NV][16]; \
    for (int n = 0; n < 2; n++) { \
        const uint8_t *ql = w + 64 * n, *qh = w + 128 + 32 * n; \
        for (int t = 0; t < 2; t++) { \
            uint8x16_t l0 = vld1q_u8(ql + 16 * t), l1 = vld1q_u8(ql + 32 + 16 * t), h = vld1q_u8(qh + 16 * t); \
            int8x16_t q1 = DAN_VK_S8(vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(h, m3), 4))); \
            int8x16_t q2 = DAN_VK_S8(vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 2), m3), 4))); \
            int8x16_t q3 = DAN_VK_S8(vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 4), m3), 4))); \
            int8x16_t q4 = DAN_VK_S8(vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(h, 6), 4))); \
            for (int k = 0; k < DAN_VK_NV; k++) { \
                const int8_t *v = y[k]->qs + 128 * n; \
                int32x4_t *e = d[k] + 8 * n; \
                e[t] = DOT16(vdupq_n_s32(0), q1, vld1q_s8(v + 16 * t)); \
                e[2 + t] = DOT16(vdupq_n_s32(0), q2, vld1q_s8(v + 32 + 16 * t)); \
                e[4 + t] = DOT16(vdupq_n_s32(0), q3, vld1q_s8(v + 64 + 16 * t)); \
                e[6 + t] = DOT16(vdupq_n_s32(0), q4, vld1q_s8(v + 96 + 16 * t)); \
            } \
        } \
    } \
    for (int k = 0; k < DAN_VK_NV; k++) { out[k] = dan_vk_q6k_apply_neon(d[k], (const int8_t *)w + 192, y[k]); imin[k] = 0; } \
} \
ATTR static void dan_vk_q4k_dot4_##SUF(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    dan_vk_kdot4_neon(w, x, xs, blocks, out, dan_vk_q4k_blk4_##SUF, DAN_Q4K_BYTES, 0, 1); \
} \
ATTR static void dan_vk_q5k_dot4_##SUF(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    dan_vk_kdot4_neon(w, x, xs, blocks, out, dan_vk_q5k_blk4_##SUF, DAN_Q5K_BYTES, 0, 1); \
} \
ATTR static void dan_vk_q6k_dot4_##SUF(const uint8_t *w, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    dan_vk_kdot4_neon(w, x, xs, blocks, out, dan_vk_q6k_blk4_##SUF, DAN_Q6K_BYTES, 208, 0); \
} \
ATTR static inline int32x4_t dan_vk_mul32_##SUF(int8x16_t lo, int8x16_t hi, const uint8_t *y) { \
    return DOT16(DOT16(vdupq_n_s32(0), lo, vld1q_s8((const int8_t *)y)), hi, vld1q_s8((const int8_t *)y + 16)); \
} \
DAN_VK_B32N_NEON(dan_vk_q40_dot4_##SUF, ATTR, dan_vk_mul32_##SUF, DAN_VK_NV, dan_vk_w_q40_neon, DAN_Q40_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_NEON(dan_vk_q50_dot4_##SUF, ATTR, dan_vk_mul32_##SUF, DAN_VK_NV, dan_vk_w_q50_neon, DAN_Q50_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_NEON(dan_vk_q80_dot4_##SUF, ATTR, dan_vk_mul32_##SUF, DAN_VK_NV, dan_vk_w_q80_neon, DAN_Q8_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_NEON(dan_vk_q41_dot4_##SUF, ATTR, dan_vk_mul32_##SUF, DAN_VK_NV, dan_vk_w_q41_neon, DAN_Q41_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p) \
DAN_VK_B32N_NEON(dan_vk_q51_dot4_##SUF, ATTR, dan_vk_mul32_##SUF, DAN_VK_NV, dan_vk_w_q51_neon, DAN_Q51_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p) \
DAN_VK_B32N_NEON(dan_vk_q50_dot1_##SUF, ATTR, dan_vk_mul32_##SUF, 1, dan_vk_w_q50_neon, DAN_Q50_BYTES, DAN_Q8_BYTES, 2, 0, dan_vk_q8_p) \
DAN_VK_B32N_NEON(dan_vk_q41_dot1_##SUF, ATTR, dan_vk_mul32_##SUF, 1, dan_vk_w_q41_neon, DAN_Q41_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p) \
DAN_VK_B32N_NEON(dan_vk_q51_dot1_##SUF, ATTR, dan_vk_mul32_##SUF, 1, dan_vk_w_q51_neon, DAN_Q51_BYTES, DAN_Q81_BYTES, 4, 1, dan_vk_q81_p) \
ATTR static float dan_vk_q50_dot_##SUF(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q50_dot1_##SUF(w, (const uint8_t *)x, 0, blocks, &r); return r; } \
ATTR static float dan_vk_q41_dot_##SUF(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q41_dot1_##SUF(w, (const uint8_t *)x, 0, blocks, &r); return r; } \
ATTR static float dan_vk_q51_dot_##SUF(const uint8_t *w, const void *x, int64_t blocks) { float r; dan_vk_q51_dot1_##SUF(w, (const uint8_t *)x, 0, blocks, &r); return r; }
DAN_VK_NEON_N(neon, , dan_vk_dot16_neon)

static const dan_vk_table dan_vk_neon = {"neon",dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon, dan_vk_mm_f32_neon,
                                         dan_vk_mm_f64_neon, dan_vk_q8_block_neon, dan_vk_q8_dot_neon, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast,
                                         dan_vk_q40_dot_neon, dan_vk_q4k_dot_neon, dan_vk_q5k_dot_neon, dan_vk_q6k_dot_neon,
                                         dan_vk_q50_dot_neon, dan_vk_q41_dot_neon, dan_vk_q51_dot_neon,
                                         {dan_vk_q40_dot4_neon, dan_vk_q4k_dot4_neon, dan_vk_q5k_dot4_neon, dan_vk_q6k_dot4_neon,
                                          dan_vk_q50_dot4_neon, dan_vk_q41_dot4_neon, dan_vk_q51_dot4_neon, dan_vk_q80_dot4_neon}};

#if defined(__clang__)
#define DAN_VK_DOTPROD __attribute__((target("dotprod")))
#else
#define DAN_VK_DOTPROD __attribute__((target("+dotprod")))
#endif
DAN_VK_DOTPROD static int32_t dan_vk_q8_block_dotprod(const int8_t *a, const int8_t *b) {
    int32x4_t s = vdupq_n_s32(0);
    s = vdotq_s32(s, vld1q_s8(a), vld1q_s8(b));
    s = vdotq_s32(s, vld1q_s8(a + 16), vld1q_s8(b + 16));
    return vaddvq_s32(s);
}

DAN_VK_DOTPROD static inline int32x4_t dan_vk_q8_part_dotprod(const uint8_t *a, const uint8_t *b) {
    int32x4_t s = vdotq_s32(vdupq_n_s32(0), vld1q_s8((const int8_t *)a + 2), vld1q_s8((const int8_t *)b + 2));
    return vdotq_s32(s, vld1q_s8((const int8_t *)a + 18), vld1q_s8((const int8_t *)b + 18));
}
DAN_VK_Q8_DOT_NEON(dan_vk_q8_dot_dotprod, DAN_VK_DOTPROD, dan_vk_q8_part_dotprod, dan_vk_q8_block_dotprod)

DAN_VK_DOTPROD static inline int32x4_t dan_vk_dot16_dotprod(int32x4_t acc, int8x16_t a, int8x16_t b) {
    return vdotq_s32(acc, a, b);
}
DAN_VK_NEON_QUANTS(dotprod, DAN_VK_DOTPROD, dan_vk_dot16_dotprod)
DAN_VK_NEON_N(dotprod, DAN_VK_DOTPROD, dan_vk_dot16_dotprod)

static const dan_vk_table dan_vk_dotprod = {"dotprod", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon,
                                            dan_vk_mm_f32_neon, dan_vk_mm_f64_neon, dan_vk_q8_block_dotprod, dan_vk_q8_dot_dotprod, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast,
                                            dan_vk_q40_dot_dotprod, dan_vk_q4k_dot_dotprod, dan_vk_q5k_dot_dotprod, dan_vk_q6k_dot_dotprod,
                                         dan_vk_q50_dot_dotprod, dan_vk_q41_dot_dotprod, dan_vk_q51_dot_dotprod,
                                         {dan_vk_q40_dot4_dotprod, dan_vk_q4k_dot4_dotprod, dan_vk_q5k_dot4_dotprod, dan_vk_q6k_dot4_dotprod,
                                          dan_vk_q50_dot4_dotprod, dan_vk_q41_dot4_dotprod, dan_vk_q51_dot4_dotprod, dan_vk_q80_dot4_dotprod}};

static int dan_vk_has_dotprod(void) {
#if defined(__linux__)
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif
    return (getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0;
#elif defined(__APPLE__)
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_DotProd", &v, &len, NULL, 0) != 0) return 0;
    return v != 0;
#elif defined(_WIN32)
    return IsProcessorFeaturePresent(43) != 0;   /* PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE */
#else
    return 0;
#endif
}
#endif

/* ---- choice of the variant ---- */

static _Atomic(const dan_vk_table *) dan_vk_cur;

/* 1 if the processor can run the variant with this name */
static int dan_vk_supported(const char *name, const dan_vk_table **t) {
    if (strcmp(name, "c") == 0) { *t = &dan_vk_plain; return 1; }
#ifdef DAN_VK_X86
    __builtin_cpu_init();
    if (strcmp(name, "avx2") == 0 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("f16c")
        && __builtin_cpu_supports("fma")) { *t = &dan_vk_avx2; return 1; }
    if (strcmp(name, "avx512") == 0 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")
        && __builtin_cpu_supports("f16c") && __builtin_cpu_supports("fma")) {
        *t = __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vnni") ? &dan_vk_avx512_vnni : &dan_vk_avx512;
        return 1;
    }
#endif
#ifdef DAN_VK_ARM
    if (strcmp(name, "neon") == 0) { *t = &dan_vk_neon; return 1; }
    if (strcmp(name, "dotprod") == 0 && dan_vk_has_dotprod()) { *t = &dan_vk_dotprod; return 1; }
#endif
    return 0;
}

static const dan_vk_table *dan_vk(void) {
    const dan_vk_table *t = atomic_load_explicit(&dan_vk_cur, memory_order_relaxed);
    if (t) return t;
    static const char *const best[] = {"avx512", "avx2", "dotprod", "neon", "c"};
    const char *want = getenv("DAN_YADRA");
    if (!(want && dan_vk_supported(want, &t)))
        for (int i = 0; i < 5; i++) if (dan_vk_supported(best[i], &t)) break;
    /* two threads may choose at once: both find the same table */
    atomic_store_explicit(&dan_vk_cur, t, memory_order_relaxed);
    return t;
}

static const char *dan_vk_name(void) { return dan_vk()->name; }

/* ---- the thread pool ---- */

/* fn(ctx, begin, end) is called for parts of [0, n) */
typedef void (*dan_pool_fn)(void *ctx, int64_t begin, int64_t end);

#define DAN_POOL_MAX 256

#if defined(__wasi__)
static int dan_pool_threads(void) { return 1; }
static void dan_pool_run(int64_t n, int64_t min_part, dan_pool_fn fn, void *ctx) {
    (void)min_part;
    if (n > 0) fn(ctx, 0, n);
}
#else
#ifdef _WIN32
typedef CRITICAL_SECTION dan_pool_mx;
typedef CONDITION_VARIABLE dan_pool_cv;
static void dan_pool_mx_init(dan_pool_mx *m) { InitializeCriticalSection(m); }
static void dan_pool_lock(dan_pool_mx *m) { EnterCriticalSection(m); }
static int dan_pool_trylock(dan_pool_mx *m) { return TryEnterCriticalSection(m) != 0; }
static void dan_pool_unlock(dan_pool_mx *m) { LeaveCriticalSection(m); }
static void dan_pool_cv_init(dan_pool_cv *c) { InitializeConditionVariable(c); }
static void dan_pool_wait(dan_pool_cv *c, dan_pool_mx *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void dan_pool_wake_all(dan_pool_cv *c) { WakeAllConditionVariable(c); }
#else
typedef pthread_mutex_t dan_pool_mx;
typedef pthread_cond_t dan_pool_cv;
static void dan_pool_mx_init(dan_pool_mx *m) { pthread_mutex_init(m, NULL); }
static void dan_pool_lock(dan_pool_mx *m) { pthread_mutex_lock(m); }
static int dan_pool_trylock(dan_pool_mx *m) { return pthread_mutex_trylock(m) == 0; }
static void dan_pool_unlock(dan_pool_mx *m) { pthread_mutex_unlock(m); }
static void dan_pool_cv_init(dan_pool_cv *c) { pthread_cond_init(c, NULL); }
static void dan_pool_wait(dan_pool_cv *c, dan_pool_mx *m) { pthread_cond_wait(c, m); }
static void dan_pool_wake_all(dan_pool_cv *c) { pthread_cond_broadcast(c); }
#endif

static struct {
    _Atomic int state;  /* 0 - not started, 1 - working, -1 - one thread only; set after threads */
    int threads;        /* threads including the calling one */
    dan_pool_mx busy;   /* held by the thread that runs a job */
    dan_pool_mx mx;
    dan_pool_cv start, done;
    _Atomic uint64_t generation;  /* a new job: written under mx (release), read by spinning workers */
    _Atomic int left;   /* parts not finished */
    int sleeping;       /* workers waiting on «start» (under mx) */
    int64_t n;
    dan_pool_fn fn;
    void *ctx;
} dan_pool;

/* A worker that finished its part, and the thread that waits for the parts,
   first wait actively for a short while (the next job of a model layer usually
   comes in microseconds) and only then sleep on the condition variable: waking
   a sleeping thread costs tens of microseconds per job. The active wait is
   bounded (DAN_POOL_SPIN polls: pause instructions, then giving the processor
   to other threads), so idle threads go to sleep, and there is none when the
   pool has more threads than processors. It does not change what a job computes. */
#define DAN_POOL_SPIN 2048
#define DAN_POOL_SPIN_PAUSE 256
static int dan_pool_spin;   /* polls before sleeping: DAN_POOL_SPIN, or 0 if threads > processors */
static inline void dan_pool_relax(int i) {
    if (i >= DAN_POOL_SPIN_PAUSE && i % 16 == 15) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
        return;
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

static int dan_pool_cpus(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return c > 0 ? (int)c : 1;
#endif
}

/* set while the thread runs a part of a job: a job started inside runs whole
   (a critical section of Windows can be entered again by its owner, so the
   lock alone does not tell) */
static _Thread_local int dan_pool_inside;

static void dan_pool_part(int part) {
    int64_t n = dan_pool.n, t = dan_pool.threads;
    int64_t b = n / t * part + (part < n % t ? part : n % t);
    int64_t e = b + n / t + (part < n % t ? 1 : 0);
    if (b < e) {
        dan_pool_inside = 1;
        dan_pool.fn(dan_pool.ctx, b, e);
        dan_pool_inside = 0;
    }
}

#ifdef _WIN32
static DWORD WINAPI dan_pool_worker(void *arg)
#else
static void *dan_pool_worker(void *arg)
#endif
{
    int part = (int)(intptr_t)arg;
    uint64_t seen = 0;
    for (;;) {
        uint64_t g = atomic_load_explicit(&dan_pool.generation, memory_order_acquire);
        for (int i = 0; g == seen && i < dan_pool_spin; i++) {
            dan_pool_relax(i);
            g = atomic_load_explicit(&dan_pool.generation, memory_order_acquire);
        }
        if (g == seen) {
            dan_pool_lock(&dan_pool.mx);
            dan_pool.sleeping++;
            while ((g = atomic_load_explicit(&dan_pool.generation, memory_order_acquire)) == seen)
                dan_pool_wait(&dan_pool.start, &dan_pool.mx);
            dan_pool.sleeping--;
            dan_pool_unlock(&dan_pool.mx);
        }
        seen = g;
        dan_pool_part(part);
        if (atomic_fetch_sub_explicit(&dan_pool.left, 1, memory_order_acq_rel) == 1) {
            /* the last part: wake the waiting thread if it sleeps (under mx, so the wake is not lost) */
            dan_pool_lock(&dan_pool.mx);
            dan_pool_wake_all(&dan_pool.done);
            dan_pool_unlock(&dan_pool.mx);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* started by the first job; a race of two first jobs is excluded by «busy» */
static void dan_pool_start(void) {
    int t = dan_pool_cpus();
    const char *lim = getenv("DAN_POTOKI");
    if (lim && *lim) {
        long v = strtol(lim, NULL, 10);
        if (v >= 1) t = (int)v;       /* set explicitly: exactly N threads, even above the core count */
    }
    if (t > DAN_POOL_MAX) t = DAN_POOL_MAX;
    dan_pool_spin = t <= dan_pool_cpus() ? DAN_POOL_SPIN : 0;
    dan_pool_mx_init(&dan_pool.mx);
    dan_pool_cv_init(&dan_pool.start);
    dan_pool_cv_init(&dan_pool.done);
    int made = 1;
    for (int i = 1; i < t; i++) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, dan_pool_worker, (void *)(intptr_t)i, 0, NULL);
        if (!h) break;
        CloseHandle(h);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, dan_pool_worker, (void *)(intptr_t)i) != 0) break;
        pthread_detach(th);
#endif
        made++;
    }
    dan_pool.threads = made;
    atomic_store_explicit(&dan_pool.state, made > 1 ? 1 : -1, memory_order_release);
}

static _Atomic int dan_pool_ready;
#ifndef _WIN32
static void dan_pool_init_busy(void) { dan_pool_mx_init(&dan_pool.busy); atomic_store(&dan_pool_ready, 1); }
#endif

static void dan_pool_init_once(void) {
    if (atomic_load(&dan_pool_ready)) return;
#ifdef _WIN32
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) { dan_pool_mx_init(&dan_pool.busy); atomic_store(&dan_pool_ready, 1); }
    else while (!atomic_load(&dan_pool_ready)) Sleep(0);
#else
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, dan_pool_init_busy);
#endif
}

/* Runs fn on [0, n) in parts on all threads. A range shorter than 2 * min_part,
   a job started inside another job or while the pool is busy with another
   task's job runs whole on the calling thread. Parts depend only on n and the
   number of threads. */
static void dan_pool_run(int64_t n, int64_t min_part, dan_pool_fn fn, void *ctx) {
    if (n <= 0) return;
    if (min_part < 1) min_part = 1;
    if (dan_pool_inside || n < 2 * min_part) { fn(ctx, 0, n); return; }
    dan_pool_init_once();
    if (!dan_pool_trylock(&dan_pool.busy)) { fn(ctx, 0, n); return; }
    if (dan_pool.state == 0) dan_pool_start();
    if (dan_pool.state < 0) { dan_pool_unlock(&dan_pool.busy); fn(ctx, 0, n); return; }
    int t = dan_pool.threads;
    if (n / min_part < t) {
        /* fewer parts of min_part than threads: the whole range runs on this
           thread as a part of a job, so a job started inside runs whole too */
        dan_pool_unlock(&dan_pool.busy);
        dan_pool_inside = 1;
        fn(ctx, 0, n);
        dan_pool_inside = 0;
        return;
    }
    dan_pool.n = n;
    dan_pool.fn = fn;
    dan_pool.ctx = ctx;
    atomic_store_explicit(&dan_pool.left, t - 1, memory_order_relaxed);
    dan_pool_lock(&dan_pool.mx);
    /* release: n, fn, ctx and left are seen by a worker that sees the new generation */
    atomic_fetch_add_explicit(&dan_pool.generation, 1, memory_order_release);
    if (dan_pool.sleeping > 0) dan_pool_wake_all(&dan_pool.start);
    dan_pool_unlock(&dan_pool.mx);
    dan_pool_part(0);
    for (int i = 0; atomic_load_explicit(&dan_pool.left, memory_order_acquire) > 0 && i < dan_pool_spin; i++)
        dan_pool_relax(i);
    if (atomic_load_explicit(&dan_pool.left, memory_order_acquire) > 0) {
        dan_pool_lock(&dan_pool.mx);
        while (atomic_load_explicit(&dan_pool.left, memory_order_acquire) > 0) dan_pool_wait(&dan_pool.done, &dan_pool.mx);
        dan_pool_unlock(&dan_pool.mx);
    }
    dan_pool_unlock(&dan_pool.busy);
}

static int dan_pool_threads(void) {
    dan_pool_init_once();
    if (dan_pool_trylock(&dan_pool.busy)) {
        if (dan_pool.state == 0) dan_pool_start();
        dan_pool_unlock(&dan_pool.busy);
    }
    /* «threads» is written before «state» (release), so it is read after it */
    return atomic_load_explicit(&dan_pool.state, memory_order_acquire) > 0 ? dan_pool.threads : 1;
}
#endif

/* ---- kernels for the program ---- */

static float dan_vk_dot_f32(const float *a, const float *b, int64_t n) { return dan_vk()->dot_f32(a, b, n); }
static double dan_vk_dot_f64(const double *a, const double *b, int64_t n) { return dan_vk()->dot_f64(a, b, n); }

/* half precision: stored in 16 bits, computed in float. The numbers are
   turned into floats (exactly) by pieces and added in the lanes of a float
   dot product, so the bits are those of dan_vk_dot_f32 on the converted
   numbers. b16 or b32 is the second factor. */
#define DAN_VK_H_PIECE 1024
static float dan_vk_dot_h(const uint16_t *a, const uint16_t *b16, const float *b32, int64_t n) {
    const dan_vk_table *t = dan_vk();
    float acc[DAN_VK_L32] = {0}, fa[DAN_VK_H_PIECE], fb[DAN_VK_H_PIECE];
    int64_t k = 0;
    while (k + DAN_VK_L32 <= n) {
        int64_t c = (n - k) / DAN_VK_L32 * DAN_VK_L32;
        if (c > DAN_VK_H_PIECE) c = DAN_VK_H_PIECE;
        t->h2f(a + k, fa, c);
        if (b16) t->h2f(b16 + k, fb, c);
        t->dot32_acc(fa, b16 ? fb : b32 + k, c, acc);
        k += c;
    }
    for (int s = DAN_VK_L32 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    float r = acc[0];
    for (; k < n; k++) { float p = dan_f16_to_f32(a[k]) * (b16 ? dan_f16_to_f32(b16[k]) : b32[k]); r = r + p; }
    return r;
}
static float dan_vk_dot_f16(const uint16_t *a, const uint16_t *b, int64_t n) { return dan_vk_dot_h(a, b, NULL, n); }

/* numbers to half precision, rounded to nearest */
static void dan_vk_f2h(const float *f, uint16_t *h, int64_t n) {
    for (int64_t i = 0; i < n; i++) h[i] = dan_f32_to_f16(f[i]);
}

static float dan_vk_sum_f32(const float *a, int64_t n) { return dan_vk()->sum_f32(a, n); }
static double dan_vk_sum_f64(const double *a, int64_t n) { return dan_vk()->sum_f64(a, n); }

/* y = y + a x */
static void dan_vk_axpy_f32(float *y, float a, const float *x, int64_t n) {
    for (int64_t i = 0; i < n; i++) { float t = a * x[i]; y[i] = y[i] + t; }
}
static void dan_vk_axpy_f64(double *y, double a, const double *x, int64_t n) {
    for (int64_t i = 0; i < n; i++) { double t = a * x[i]; y[i] = y[i] + t; }
}

/* matrix by vector: y[i] = W[i] . x, W is rows x cols by rows */
typedef struct { const void *w, *x; void *y; int64_t cols; } dan_vk_mv_job;

static void dan_vk_mv_f32_part(void *c, int64_t b, int64_t e) {
    dan_vk_mv_job *j = (dan_vk_mv_job *)c;
    const dan_vk_table *t = dan_vk();
    for (int64_t i = b; i < e; i++)
        ((float *)j->y)[i] = t->dot_f32((const float *)j->w + i * j->cols, (const float *)j->x, j->cols);
}
static void dan_vk_mv_f64_part(void *c, int64_t b, int64_t e) {
    dan_vk_mv_job *j = (dan_vk_mv_job *)c;
    const dan_vk_table *t = dan_vk();
    for (int64_t i = b; i < e; i++)
        ((double *)j->y)[i] = t->dot_f64((const double *)j->w + i * j->cols, (const double *)j->x, j->cols);
}

/* about 64 thousand multiplications per part at least */
static int64_t dan_vk_min_rows(int64_t cols) { return cols >= 65536 ? 1 : 65536 / (cols > 0 ? cols : 1); }

static void dan_vk_matvec_f32(const float *w, const float *x, float *y, int64_t rows, int64_t cols) {
    dan_vk_mv_job j = {w, x, y, cols};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_mv_f32_part, &j);
}
static void dan_vk_matvec_f64(const double *w, const double *x, double *y, int64_t rows, int64_t cols) {
    dan_vk_mv_job j = {w, x, y, cols};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_mv_f64_part, &j);
}

static void dan_vk_mv_f16_part(void *c, int64_t b, int64_t e) {
    dan_vk_mv_job *j = (dan_vk_mv_job *)c;
    for (int64_t i = b; i < e; i++)
        ((float *)j->y)[i] = dan_vk_dot_h((const uint16_t *)j->w + i * j->cols, NULL, (const float *)j->x, j->cols);
}
/* weights in half precision, vector and result in float */
static void dan_vk_matvec_f16(const uint16_t *w, const float *x, float *y, int64_t rows, int64_t cols) {
    dan_vk_mv_job j = {w, x, y, cols};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_mv_f16_part, &j);
}

/* matrix by matrix: C (m x n) = A (m x k) B (k x n); threads take strips of
   64 columns (the last one may be narrower) */
typedef struct { const void *a, *b; void *c; int64_t m, n, k; } dan_vk_mm_job;

static void dan_vk_mm_f32_part(void *c, int64_t b, int64_t e) {
    dan_vk_mm_job *j = (dan_vk_mm_job *)c;
    int64_t j0 = b * 64, j1 = e * 64 < j->n ? e * 64 : j->n;
    dan_vk()->mm_f32((const float *)j->a, (const float *)j->b, (float *)j->c, j->m, j->n, j->k, j0, j1);
}
static void dan_vk_mm_f64_part(void *c, int64_t b, int64_t e) {
    dan_vk_mm_job *j = (dan_vk_mm_job *)c;
    int64_t j0 = b * 64, j1 = e * 64 < j->n ? e * 64 : j->n;
    dan_vk()->mm_f64((const double *)j->a, (const double *)j->b, (double *)j->c, j->m, j->n, j->k, j0, j1);
}

static int64_t dan_vk_min_strips(int64_t m, int64_t k) {
    int64_t work = m * k * 64;   /* multiplications in one strip */
    return work >= 1 << 20 ? 1 : (1 << 20) / (work > 0 ? work : 1);
}

static void dan_vk_matmat_f32(const float *a, const float *b, float *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f32_part, &j);
}
static void dan_vk_matmat_f64(const double *a, const double *b, double *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f64_part, &j);
}

/* «приближённо»: the same on all cores, but with fused multiply-add and an
   order of sums of the variant (results differ in the last digits between
   processors) */
static void dan_vk_mv_f32_fast_part(void *c, int64_t b, int64_t e) {
    dan_vk_mv_job *j = (dan_vk_mv_job *)c;
    const dan_vk_table *t = dan_vk();
    for (int64_t i = b; i < e; i++)
        ((float *)j->y)[i] = t->dot_f32_fast((const float *)j->w + i * j->cols, (const float *)j->x, j->cols);
}
static void dan_vk_mv_f64_fast_part(void *c, int64_t b, int64_t e) {
    dan_vk_mv_job *j = (dan_vk_mv_job *)c;
    const dan_vk_table *t = dan_vk();
    for (int64_t i = b; i < e; i++)
        ((double *)j->y)[i] = t->dot_f64_fast((const double *)j->w + i * j->cols, (const double *)j->x, j->cols);
}
static void dan_vk_matvec_f32_fast(const float *w, const float *x, float *y, int64_t rows, int64_t cols) {
    dan_vk_mv_job j = {w, x, y, cols};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_mv_f32_fast_part, &j);
}
static void dan_vk_matvec_f64_fast(const double *w, const double *x, double *y, int64_t rows, int64_t cols) {
    dan_vk_mv_job j = {w, x, y, cols};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_mv_f64_fast_part, &j);
}
static void dan_vk_mm_f32_fast_part(void *c, int64_t b, int64_t e) {
    dan_vk_mm_job *j = (dan_vk_mm_job *)c;
    int64_t j0 = b * 64, j1 = e * 64 < j->n ? e * 64 : j->n;
    dan_vk()->mm_f32_fast((const float *)j->a, (const float *)j->b, (float *)j->c, j->m, j->n, j->k, j0, j1);
}
static void dan_vk_mm_f64_fast_part(void *c, int64_t b, int64_t e) {
    dan_vk_mm_job *j = (dan_vk_mm_job *)c;
    int64_t j0 = b * 64, j1 = e * 64 < j->n ? e * 64 : j->n;
    dan_vk()->mm_f64_fast((const double *)j->a, (const double *)j->b, (double *)j->c, j->m, j->n, j->k, j0, j1);
}
static void dan_vk_matmat_f32_fast(const float *a, const float *b, float *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f32_fast_part, &j);
}
static void dan_vk_matmat_f64_fast(const double *a, const double *b, double *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f64_fast_part, &j);
}

/* softmax in place: x[i] = exp(x[i] - max) / sum */
static void dan_vk_softmax_f32(float *x, int64_t n) {
    if (n <= 0) return;
    float mx = x[0];
    for (int64_t i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int64_t i = 0; i < n; i++) x[i] = expf(x[i] - mx);
    float s = dan_vk_sum_f32(x, n);
    for (int64_t i = 0; i < n; i++) x[i] = x[i] / s;
}
static void dan_vk_softmax_f64(double *x, int64_t n) {
    if (n <= 0) return;
    double mx = x[0];
    for (int64_t i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int64_t i = 0; i < n; i++) x[i] = exp(x[i] - mx);
    double s = dan_vk_sum_f64(x, n);
    for (int64_t i = 0; i < n; i++) x[i] = x[i] / s;
}

/* RMS normalisation: y[i] = x[i] / sqrt(mean(x^2) + eps) * w[i] (w may be NULL) */
static void dan_vk_rmsnorm_f32(float *y, const float *x, const float *w, int64_t n, float eps) {
    if (n <= 0) return;
    float ms = dan_vk_dot_f32(x, x, n) / (float)n;
    float sc = 1.0f / sqrtf(ms + eps);
    for (int64_t i = 0; i < n; i++) { float t = x[i] * sc; y[i] = w ? t * w[i] : t; }
}
static void dan_vk_rmsnorm_f64(double *y, const double *x, const double *w, int64_t n, double eps) {
    if (n <= 0) return;
    double ms = dan_vk_dot_f64(x, x, n) / (double)n;
    double sc = 1.0 / sqrt(ms + eps);
    for (int64_t i = 0; i < n; i++) { double t = x[i] * sc; y[i] = w ? t * w[i] : t; }
}

/* layer normalisation: y = (x - mean) / sqrt(var + eps) * w + b (w, b may be NULL);
   y may be x */
static void dan_vk_layernorm_f32(float *y, const float *x, const float *w, const float *b, int64_t n, float eps) {
    if (n <= 0) return;
    float mean = dan_vk_sum_f32(x, n) / (float)n;
    /* y holds x - mean (y may be x); the sum of its squares is the dot product
       of y with itself: the same order as before, now in the variant's lanes */
    for (int64_t i = 0; i < n; i++) y[i] = x[i] - mean;
    float v = dan_vk_dot_f32(y, y, n);
    float sc = 1.0f / sqrtf(v / (float)n + eps);
    for (int64_t i = 0; i < n; i++) {
        float t = y[i] * sc;
        if (w) t = t * w[i];
        if (b) t = t + b[i];
        y[i] = t;
    }
}
static void dan_vk_layernorm_f64(double *y, const double *x, const double *w, const double *b, int64_t n,
                                 double eps) {
    if (n <= 0) return;
    double mean = dan_vk_sum_f64(x, n) / (double)n;
    /* y holds x - mean (y may be x); the sum of its squares is the dot product
       of y with itself: the same order as before, now in the variant's lanes */
    for (int64_t i = 0; i < n; i++) y[i] = x[i] - mean;
    double v = dan_vk_dot_f64(y, y, n);
    double sc = 1.0 / sqrt(v / (double)n + eps);
    for (int64_t i = 0; i < n; i++) {
        double t = y[i] * sc;
        if (w) t = t * w[i];
        if (b) t = t + b[i];
        y[i] = t;
    }
}

/* ---- 8-bit weights (Q8_0 of GGUF: per block a 16-bit float scale d and
   32 numbers q; value = d * q) ---- */

/* x (n numbers, n a multiple of 32) -> n / 32 blocks of 34 bytes */
static void dan_vk_q8_quantize(const float *x, uint8_t *out, int64_t n) {
    for (int64_t b = 0; b < n / DAN_Q8_BLOCK; b++) {
        const float *v = x + b * DAN_Q8_BLOCK;
        uint8_t *o = out + b * DAN_Q8_BYTES;
        float amax = 0.0f;
        for (int i = 0; i < DAN_Q8_BLOCK; i++) { float a = fabsf(v[i]); if (a > amax) amax = a; }
        float d = amax / 127.0f, id = d != 0.0f ? 1.0f / d : 0.0f;
        if (!isfinite(id) || !isfinite(d)) { d = 0.0f; id = 0.0f; }   /* scale below or above the range */
        uint16_t h = dan_f32_to_f16(d);
        memcpy(o, &h, 2);
        for (int i = 0; i < DAN_Q8_BLOCK; i++) {
            float q = v[i] * id;
            /* within -127..127 before rounding (lrintf outside the range of the
               result is undefined); NaN becomes 0 */
            q = q != q ? 0.0f : q < -127.0f ? -127.0f : q > 127.0f ? 127.0f : q;
            o[2 + i] = (uint8_t)(int8_t)lrintf(q);
        }
    }
}

/* back to numbers */
static void dan_vk_q8_dequantize(const uint8_t *in, float *x, int64_t n) {
    for (int64_t b = 0; b < n / DAN_Q8_BLOCK; b++) {
        const uint8_t *o = in + b * DAN_Q8_BYTES;
        uint16_t h;
        memcpy(&h, o, 2);
        float d = dan_f16_to_f32(h);
        for (int i = 0; i < DAN_Q8_BLOCK; i++) x[b * DAN_Q8_BLOCK + i] = d * (float)(int8_t)o[2 + i];
    }
}


/* y[i] = row i of w (rows x cols, cols a multiple of 32, in blocks) . xq (x in blocks) */
typedef struct { const uint8_t *w, *x; float *y; int64_t blocks; } dan_vk_q8mv_job;

static void dan_vk_q8mv_part(void *c, int64_t b, int64_t e) {
    dan_vk_q8mv_job *j = (dan_vk_q8mv_job *)c;
    const dan_vk_table *t = dan_vk();
    for (int64_t i = b; i < e; i++) j->y[i] = t->q8_dot(j->w + i * j->blocks * DAN_Q8_BYTES, j->x, j->blocks);
}

static void dan_vk_matvec_q8(const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols) {
    dan_vk_q8mv_job j = {w, xq, y, cols / DAN_Q8_BLOCK};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_q8mv_part, &j);
}

/* ---- weights of llama.cpp ---- */

/* the vector (n a multiple of 256, finite numbers) into Q8_K blocks, as
   quantize_row_q8_K_ref of ggml; a block whose scale does not fit a float
   (numbers below 4e-37) becomes zero */
static void dan_vk_q8k_quantize(const float *x, dan_q8k *y, int64_t n) {
    for (int64_t b = 0; b < n / DAN_QK; b++, x += DAN_QK) {
        dan_q8k *o = y + b;
        float max = 0.0f, amax = 0.0f;
        for (int j = 0; j < DAN_QK; j++) {
            float ax = fabsf(x[j]);
            if (ax > amax) { amax = ax; max = x[j]; }
        }
        float iscale = amax != 0.0f ? -127.0f / max : 0.0f;
        if (!isfinite(iscale) || iscale == 0.0f) {
            memset(o, 0, sizeof *o);
            continue;
        }
        for (int j = 0; j < DAN_QK; j++) {
            /* nearest integer, ties to even: |v| <= 127.5 */
            float v = iscale * x[j], t = v + 12582912.0f;
            int32_t i;
            memcpy(&i, &t, 4);
            i = (i & 0x007fffff) - 0x00400000;
            o->qs[j] = (int8_t)(i < 127 ? i : 127);
        }
        for (int j = 0; j < DAN_QK / 16; j++) {
            int s = 0;
            for (int i = 0; i < 16; i++) s += o->qs[16 * j + i];
            o->bsums[j] = (int16_t)s;
        }
        o->d = 1.0f / iscale;
    }
}

/* back to numbers: the same operations as dequantize_row_* of ggml, so the
   same bits (ggml built without fused multiply-add) */
static void dan_vk_q40_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q40_BYTES, y += 32) {
        float d = dan_vk_half(in);
        for (int j = 0; j < 16; j++) {
            int x0 = (in[2 + j] & 0x0F) - 8, x1 = (in[2 + j] >> 4) - 8;
            y[j] = (float)x0 * d;
            y[j + 16] = (float)x1 * d;
        }
    }
}

static void dan_vk_q4k_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q4K_BYTES) {
        float d = dan_vk_half(in), min = dan_vk_half(in + 2);
        uint8_t sc[8], m[8];
        dan_vk_k4_scales(in + 4, sc, m);
        const uint8_t *q = in + 16;
        for (int p = 0; p < 4; p++, q += 32) {
            float d1 = d * (float)sc[2 * p], m1 = min * (float)m[2 * p];
            float d2 = d * (float)sc[2 * p + 1], m2 = min * (float)m[2 * p + 1];
            for (int l = 0; l < 32; l++) { float t = d1 * (float)(q[l] & 0xF); *y++ = t - m1; }
            for (int l = 0; l < 32; l++) { float t = d2 * (float)(q[l] >> 4); *y++ = t - m2; }
        }
    }
}

static void dan_vk_q5k_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q5K_BYTES) {
        float d = dan_vk_half(in), min = dan_vk_half(in + 2);
        uint8_t sc[8], m[8];
        dan_vk_k4_scales(in + 4, sc, m);
        const uint8_t *qh = in + 16, *q = in + 48;
        for (int p = 0; p < 4; p++, q += 32) {
            float d1 = d * (float)sc[2 * p], m1 = min * (float)m[2 * p];
            float d2 = d * (float)sc[2 * p + 1], m2 = min * (float)m[2 * p + 1];
            uint8_t u1 = (uint8_t)(1u << (2 * p)), u2 = (uint8_t)(2u << (2 * p));
            for (int l = 0; l < 32; l++) { float t = d1 * (float)((q[l] & 0xF) + (qh[l] & u1 ? 16 : 0)); *y++ = t - m1; }
            for (int l = 0; l < 32; l++) { float t = d2 * (float)((q[l] >> 4) + (qh[l] & u2 ? 16 : 0)); *y++ = t - m2; }
        }
    }
}

static void dan_vk_q6k_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q6K_BYTES) {
        float d = dan_vk_half(in + 208);
        const uint8_t *ql = in, *qh = in + 128;
        const int8_t *sc = (const int8_t *)in + 192;
        for (int n = 0; n < 2; n++, y += 128, ql += 64, qh += 32, sc += 8)
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                int q1 = ((ql[l] & 0xF) | ((qh[l] & 3) << 4)) - 32;
                int q2 = ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = ((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4)) - 32;
                float t;
                t = d * (float)sc[is + 0]; y[l + 0] = t * (float)q1;
                t = d * (float)sc[is + 2]; y[l + 32] = t * (float)q2;
                t = d * (float)sc[is + 4]; y[l + 64] = t * (float)q3;
                t = d * (float)sc[is + 6]; y[l + 96] = t * (float)q4;
            }
    }
}

static void dan_vk_q50_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q50_BYTES, y += 32) {
        float d = dan_vk_half(in);
        uint32_t qh = dan_vk_u32(in + 2);
        for (int j = 0; j < 16; j++) {
            int32_t x0 = (int32_t)((in[6 + j] & 0x0F) | (((qh >> j) << 4) & 0x10)) - 16;
            int32_t x1 = (int32_t)((in[6 + j] >> 4) | ((qh >> (j + 12)) & 0x10)) - 16;
            y[j] = (float)x0 * d;
            y[j + 16] = (float)x1 * d;
        }
    }
}

static void dan_vk_q41_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q41_BYTES, y += 32) {
        float d = dan_vk_half(in), m = dan_vk_half(in + 2);
        for (int j = 0; j < 16; j++) {
            float t0 = (float)(in[4 + j] & 0x0F) * d, t1 = (float)(in[4 + j] >> 4) * d;
            y[j] = t0 + m;
            y[j + 16] = t1 + m;
        }
    }
}

static void dan_vk_q51_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q51_BYTES, y += 32) {
        float d = dan_vk_half(in), m = dan_vk_half(in + 2);
        uint32_t qh = dan_vk_u32(in + 4);
        for (int j = 0; j < 16; j++) {
            float t0 = (float)((in[8 + j] & 0x0F) | (((qh >> j) << 4) & 0x10)) * d;
            float t1 = (float)((in[8 + j] >> 4) | ((qh >> (j + 12)) & 0x10)) * d;
            y[j] = t0 + m;
            y[j + 16] = t1 + m;
        }
    }
}

/* the vector for Q4_1 and Q5_1: Q8_0 blocks with the sums of their numbers */
static void dan_vk_q81_quantize(const float *x, uint8_t *out, int64_t n) {
    for (int64_t b = 0; b < n / DAN_Q8_BLOCK; b++) {
        uint8_t q[DAN_Q8_BYTES], *o = out + b * DAN_Q81_BYTES;
        dan_vk_q8_quantize(x + b * DAN_Q8_BLOCK, q, DAN_Q8_BLOCK);
        int16_t s = 0;
        for (int i = 0; i < DAN_Q8_BLOCK; i++) s = (int16_t)(s + (int8_t)q[2 + i]);
        memcpy(o, q, 2);
        memcpy(o + 2, &s, 2);
        memcpy(o + 4, q + 2, DAN_Q8_BLOCK);
    }
}

/* the formats: bytes and numbers of a block of weights, bytes of a block of the vector in 8 bits */
static const int dan_vk_fmt_bytes[DAN_NFMT] = {DAN_Q40_BYTES, DAN_Q4K_BYTES, DAN_Q5K_BYTES, DAN_Q6K_BYTES,
                                               DAN_Q50_BYTES, DAN_Q41_BYTES, DAN_Q51_BYTES, DAN_Q8_BYTES};
static const int dan_vk_fmt_n[DAN_NFMT] = {32, DAN_QK, DAN_QK, DAN_QK, 32, 32, 32, 32};
static const int dan_vk_fmt_xbytes[DAN_NFMT] = {DAN_Q8_BYTES, (int)sizeof(dan_q8k), (int)sizeof(dan_q8k), (int)sizeof(dan_q8k),
                                                DAN_Q8_BYTES, DAN_Q81_BYTES, DAN_Q81_BYTES, DAN_Q8_BYTES};

/* the vector (n numbers, a multiple of the block) into 8 bits for the format */
static void dan_vk_fmt_quantize_x(int fmt, const float *x, uint8_t *out, int64_t n) {
    switch (fmt) {
    case DAN_FMT_Q4K: case DAN_FMT_Q5K: case DAN_FMT_Q6K: dan_vk_q8k_quantize(x, (dan_q8k *)(void *)out, n); break;
    case DAN_FMT_Q41: case DAN_FMT_Q51: dan_vk_q81_quantize(x, out, n); break;
    default: dan_vk_q8_quantize(x, out, n); break;
    }
}

static void dan_vk_fmt_dequantize(int fmt, const uint8_t *in, float *y, int64_t blocks) {
    switch (fmt) {
    case DAN_FMT_Q40: dan_vk_q40_dequantize(in, y, blocks); break;
    case DAN_FMT_Q4K: dan_vk_q4k_dequantize(in, y, blocks); break;
    case DAN_FMT_Q5K: dan_vk_q5k_dequantize(in, y, blocks); break;
    case DAN_FMT_Q6K: dan_vk_q6k_dequantize(in, y, blocks); break;
    case DAN_FMT_Q50: dan_vk_q50_dequantize(in, y, blocks); break;
    case DAN_FMT_Q41: dan_vk_q41_dequantize(in, y, blocks); break;
    case DAN_FMT_Q51: dan_vk_q51_dequantize(in, y, blocks); break;
    default: dan_vk_q8_dequantize(in, y, blocks * DAN_Q8_BLOCK); break;
    }
}

static float dan_vk_fmt_dot(const dan_vk_table *t, int fmt, const uint8_t *w, const void *x, int64_t blocks) {
    switch (fmt) {
    case DAN_FMT_Q40: return t->q40_dot(w, x, blocks);
    case DAN_FMT_Q4K: return t->q4k_dot(w, x, blocks);
    case DAN_FMT_Q5K: return t->q5k_dot(w, x, blocks);
    case DAN_FMT_Q6K: return t->q6k_dot(w, x, blocks);
    case DAN_FMT_Q50: return t->q50_dot(w, x, blocks);
    case DAN_FMT_Q41: return t->q41_dot(w, x, blocks);
    case DAN_FMT_Q51: return t->q51_dot(w, x, blocks);
    default: return t->q8_dot(w, (const uint8_t *)x, blocks);
    }
}

typedef struct { const uint8_t *w; const void *x; float *y; int64_t blocks; int fmt; } dan_vk_qmv_job;

static void dan_vk_qmv_part(void *c, int64_t b, int64_t e) {
    dan_vk_qmv_job *j = (dan_vk_qmv_job *)c;
    const dan_vk_table *t = dan_vk();
    int64_t row = j->blocks * dan_vk_fmt_bytes[j->fmt];
    for (int64_t i = b; i < e; i++) j->y[i] = dan_vk_fmt_dot(t, j->fmt, j->w + i * row, j->x, j->blocks);
}

/* y[i] = row i of w (rows x cols in blocks of the format) . x already in 8
   bits (dan_vk_fmt_quantize_x) */
static void dan_vk_matvec_fmt(int fmt, const uint8_t *w, const void *xq, float *y, int64_t rows, int64_t cols) {
    dan_vk_qmv_job j = {w, xq, y, cols / dan_vk_fmt_n[fmt], fmt};
    dan_pool_run(rows, dan_vk_min_rows(cols), dan_vk_qmv_part, &j);
}

/* ---- a matrix of weights by several vectors (the batch of a prompt) ----

   Every number is the one the matrix by each vector gives (the same row
   kernels or the kernels of 4 vectors with the same bits); only the order of
   work differs. The vectors are cut into tiles of about DAN_VK_TILE bytes in 8
   bits (they stay in the second level cache), each part of the rows on the
   pool goes through its rows once per tile, and each row (in the first level
   cache) is multiplied by all the vectors of the tile, 4 at a time. The
   environment variable DAN_YADRA_PLITKA sets the bytes of a tile (for
   measurements). */
#define DAN_VK_TILE (192 * 1024)

static int64_t dan_vk_tile_bytes(void) {
    const char *s = getenv("DAN_YADRA_PLITKA");
    if (s) {
        long long v = atoll(s);
        if (v > 0) return (int64_t)v;
    }
    return DAN_VK_TILE;
}

typedef struct { const uint8_t *w, *x; float *y; int64_t blocks, rows, nv, tile; int fmt; } dan_vk_qmm_job;

static void dan_vk_qmm_part(void *c, int64_t b, int64_t e) {
    dan_vk_qmm_job *j = (dan_vk_qmm_job *)c;
    const dan_vk_table *t = dan_vk();
    int fmt = j->fmt;
    int64_t row = j->blocks * dan_vk_fmt_bytes[fmt], xs = j->blocks * dan_vk_fmt_xbytes[fmt];
    void (*dot4)(const uint8_t *, const uint8_t *, int64_t, int64_t, float *) = t->dot4[fmt];
    for (int64_t v0 = 0; v0 < j->nv; v0 += j->tile) {
        int64_t v1 = v0 + j->tile < j->nv ? v0 + j->tile : j->nv;
        for (int64_t r = b; r < e; r++) {
            const uint8_t *wr = j->w + r * row;
            int64_t v = v0;
            if (dot4)
                for (; v + DAN_VK_NV <= v1; v += DAN_VK_NV) {
                    float o[DAN_VK_NV];
                    dot4(wr, j->x + v * xs, xs, j->blocks, o);
                    for (int k = 0; k < DAN_VK_NV; k++) j->y[(v + k) * j->rows + r] = o[k];
                }
            for (; v < v1; v++) j->y[v * j->rows + r] = dan_vk_fmt_dot(t, fmt, wr, j->x + v * xs, j->blocks);
        }
    }
}

/* y[v * rows + i] = row i of w (rows x cols in blocks of the format) . vector v
   of xq (nv vectors already in 8 bits, one after another) */
static void dan_vk_matmul_fmt(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    int64_t blocks = cols / dan_vk_fmt_n[fmt], xs = blocks * dan_vk_fmt_xbytes[fmt];
    int64_t tile = dan_vk_tile_bytes() / (xs > 0 ? xs : 1);
    tile = tile < DAN_VK_NV ? DAN_VK_NV : tile / DAN_VK_NV * DAN_VK_NV;
    dan_vk_qmm_job j = {w, xq, y, blocks, rows, nv, tile, fmt};
    int64_t work = cols * (nv > 0 ? nv : 1);
    dan_pool_run(rows, work >= 65536 ? 1 : 65536 / work, dan_vk_qmm_part, &j);
}

/* ---- functions of the program over dense arrays (sreda/massivy.c) ---- */
#ifdef DAN_ARR_ALIGN

static void dan_k_error(const char *what, int64_t a, int64_t b, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, what, (long long)a, (long long)b);
    dan_error(buf, line);
}

static double dan_k_dot_f32(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len != b->len) dan_k_error("скаляр: массивы разной длины (%lld и %lld)", a->len, b->len, line);
    return dan_vk_dot_f32((const float *)a->data, (const float *)b->data, a->len);
}
static double dan_k_dot_f64(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len != b->len) dan_k_error("скаляр: массивы разной длины (%lld и %lld)", a->len, b->len, line);
    return dan_vk_dot_f64((const double *)a->data, (const double *)b->data, a->len);
}

static int64_t dan_k_rows(const dan_arr *w, const dan_arr *x, int line) {
    if (x->len == 0 || w->len % x->len != 0)
        dan_k_error("матвек: длина матрицы (%lld) должна делиться на длину вектора (%lld)", w->len, x->len, line);
    return w->len / x->len;
}

static dan_arr *dan_k_matvec_f32(const dan_arr *w, const dan_arr *x, int line) {
    int64_t rows = dan_k_rows(w, x, line);
    dan_arr *y = dan_arr_new(DAN_AF32, rows, line);
    dan_vk_matvec_f32((const float *)w->data, (const float *)x->data, (float *)y->data, rows, x->len);
    return y;
}
static dan_arr *dan_k_matvec_f64(const dan_arr *w, const dan_arr *x, int line) {
    int64_t rows = dan_k_rows(w, x, line);
    dan_arr *y = dan_arr_new(DAN_AF64, rows, line);
    dan_vk_matvec_f64((const double *)w->data, (const double *)x->data, (double *)y->data, rows, x->len);
    return y;
}

/* a: m x k, b: k x n */
static void dan_k_mm_dims(const dan_arr *a, const dan_arr *b, int64_t k, int64_t *m, int64_t *n, int line) {
    if (k <= 0) dan_k_error("матмат: число столбцов первой матрицы должно быть положительным, а здесь %lld (длина первой матрицы %lld)", k, a->len, line);
    if (a->len % k != 0) dan_k_error("матмат: длина первой матрицы (%lld) не делится на %lld", a->len, k, line);
    if (b->len % k != 0) dan_k_error("матмат: длина второй матрицы (%lld) не делится на %lld", b->len, k, line);
    *m = a->len / k;
    *n = b->len / k;
    if (*m > 0 && *n > INT64_MAX / *m)
        dan_k_error("матмат: результат из %lld строк по %lld чисел слишком велик", *m, *n, line);
}

static dan_arr *dan_k_matmat_f32(const dan_arr *a, const dan_arr *b, int64_t k, int line) {
    int64_t m, n;
    dan_k_mm_dims(a, b, k, &m, &n, line);
    dan_arr *c = dan_arr_new(DAN_AF32, m * n, line);
    dan_vk_matmat_f32((const float *)a->data, (const float *)b->data, (float *)c->data, m, n, k);
    return c;
}
static dan_arr *dan_k_matmat_f64(const dan_arr *a, const dan_arr *b, int64_t k, int line) {
    int64_t m, n;
    dan_k_mm_dims(a, b, k, &m, &n, line);
    dan_arr *c = dan_arr_new(DAN_AF64, m * n, line);
    dan_vk_matmat_f64((const double *)a->data, (const double *)b->data, (double *)c->data, m, n, k);
    return c;
}

static dan_arr *dan_k_softmax_f32(const dan_arr *x) {
    dan_arr *y = dan_arr_new(DAN_AF32, x->len, 0);
    memcpy(y->data, x->data, (size_t)x->len * 4);
    dan_vk_softmax_f32((float *)y->data, x->len);
    return y;
}
static dan_arr *dan_k_softmax_f64(const dan_arr *x) {
    dan_arr *y = dan_arr_new(DAN_AF64, x->len, 0);
    memcpy(y->data, x->data, (size_t)x->len * 8);
    dan_vk_softmax_f64((double *)y->data, x->len);
    return y;
}

/* weights: empty or of the length of x */
static void dan_k_same_or_empty(const char *what, const dan_arr *x, const dan_arr *w, int line) {
    if (w->len != 0 && w->len != x->len) {
        char buf[300];
        snprintf(buf, sizeof buf, "%s: длина весов или сдвигов (%lld) должна быть 0 или длиной массива (%lld)", what,
                 (long long)w->len, (long long)x->len);
        dan_error(buf, line);
    }
}

static dan_arr *dan_k_rmsnorm_f32(const dan_arr *x, const dan_arr *w, double eps, int line) {
    dan_k_same_or_empty("нормировать_скв", x, w, line);
    dan_arr *y = dan_arr_new(DAN_AF32, x->len, line);
    dan_vk_rmsnorm_f32((float *)y->data, (const float *)x->data, w->len ? (const float *)w->data : NULL, x->len, (float)eps);
    return y;
}
static dan_arr *dan_k_rmsnorm_f64(const dan_arr *x, const dan_arr *w, double eps, int line) {
    dan_k_same_or_empty("нормировать_скв", x, w, line);
    dan_arr *y = dan_arr_new(DAN_AF64, x->len, line);
    dan_vk_rmsnorm_f64((double *)y->data, (const double *)x->data, w->len ? (const double *)w->data : NULL, x->len, eps);
    return y;
}

static dan_arr *dan_k_layernorm_f32(const dan_arr *x, const dan_arr *w, const dan_arr *b, double eps, int line) {
    dan_k_same_or_empty("нормировать_слой", x, w, line);
    dan_k_same_or_empty("нормировать_слой", x, b, line);
    dan_arr *y = dan_arr_new(DAN_AF32, x->len, line);
    dan_vk_layernorm_f32((float *)y->data, (const float *)x->data, w->len ? (const float *)w->data : NULL,
                         b->len ? (const float *)b->data : NULL, x->len, (float)eps);
    return y;
}
static dan_arr *dan_k_layernorm_f64(const dan_arr *x, const dan_arr *w, const dan_arr *b, double eps, int line) {
    dan_k_same_or_empty("нормировать_слой", x, w, line);
    dan_k_same_or_empty("нормировать_слой", x, b, line);
    dan_arr *y = dan_arr_new(DAN_AF64, x->len, line);
    dan_vk_layernorm_f64((double *)y->data, (const double *)x->data, w->len ? (const double *)w->data : NULL,
                         b->len ? (const double *)b->data : NULL, x->len, eps);
    return y;
}

static dan_arr *dan_k_axpy_f32(const dan_arr *y, double a, const dan_arr *x, int line) {
    if (y->len != x->len) dan_k_error("прибавить_кратное: массивы разной длины (%lld и %lld)", y->len, x->len, line);
    dan_arr *r = dan_arr_new(DAN_AF32, y->len, line);
    memcpy(r->data, y->data, (size_t)y->len * 4);
    dan_vk_axpy_f32((float *)r->data, (float)a, (const float *)x->data, x->len);
    return r;
}
static dan_arr *dan_k_axpy_f64(const dan_arr *y, double a, const dan_arr *x, int line) {
    if (y->len != x->len) dan_k_error("прибавить_кратное: массивы разной длины (%lld и %lld)", y->len, x->len, line);
    dan_arr *r = dan_arr_new(DAN_AF64, y->len, line);
    memcpy(r->data, y->data, (size_t)y->len * 8);
    dan_vk_axpy_f64((double *)r->data, a, (const double *)x->data, x->len);
    return r;
}

static void dan_k_q8_len(const char *what, int64_t n, int line) {
    if (n % DAN_Q8_BLOCK != 0) dan_k_error(what, n, DAN_Q8_BLOCK, line);
}

#define DAN_Q8_MAX (127.0f * 65504.0f)   /* the scale still fits half precision */

static dan_arr *dan_k_q8_quantize(const dan_arr *x, int line) {
    dan_k_q8_len("квантовать_q8: длина массива (%lld) должна делиться на %lld", x->len, line);
    const float *v = (const float *)x->data;
    for (int64_t i = 0; i < x->len; i++)
        if (!(fabsf(v[i]) <= DAN_Q8_MAX)) {   /* also NaN */
            char buf[300];
            snprintf(buf, sizeof buf, "квантовать_q8: элемент %lld равен %g, а в весах в 8 битах могут быть только "
                     "конечные числа не больше 8319008 по модулю", (long long)i, (double)v[i]);
            dan_error(buf, line);
        }
    dan_arr *q = dan_arr_new(DAN_AU8, x->len / DAN_Q8_BLOCK * DAN_Q8_BYTES, line);
    dan_vk_q8_quantize((const float *)x->data, q->data, x->len);
    return q;
}

static dan_arr *dan_k_q8_dequantize(const dan_arr *q, int line) {
    if (q->len % DAN_Q8_BYTES != 0)
        dan_k_error("из_q8: длина массива байтов (%lld) должна делиться на %lld (размер блока)", q->len, DAN_Q8_BYTES, line);
    int64_t n = q->len / DAN_Q8_BYTES * DAN_Q8_BLOCK;
    dan_arr *x = dan_arr_new(DAN_AF32, n, line);
    dan_vk_q8_dequantize(q->data, (float *)x->data, n);
    return x;
}

static dan_arr *dan_k_matvec_q8(const dan_arr *w, const dan_arr *x, int line) {
    dan_k_q8_len("матвек_q8: длина вектора (%lld) должна делиться на %lld", x->len, line);
    for (int64_t i = 0; i < x->len; i++)
        if (!(fabsf(((const float *)x->data)[i]) <= DAN_Q8_MAX)) {
            char buf[300];
            snprintf(buf, sizeof buf, "матвек_q8: элемент вектора %lld равен %g, а вектор переводится в 8 бит: нужны "
                     "конечные числа не больше 8319008 по модулю", (long long)i, (double)((const float *)x->data)[i]);
            dan_error(buf, line);
        }
    int64_t row_bytes = x->len / DAN_Q8_BLOCK * DAN_Q8_BYTES;
    if (row_bytes == 0 || w->len % row_bytes != 0)
        dan_k_error("матвек_q8: длина весов в байтах (%lld) должна делиться на длину строки (%lld)", w->len, row_bytes, line);
    int64_t rows = w->len / row_bytes;
    uint8_t *xq = malloc((size_t)row_bytes);
    if (!xq) dan_error(DAN_MSG_NO_MEMORY, line);
    dan_vk_q8_quantize((const float *)x->data, xq, x->len);
    dan_arr *y = dan_arr_new(DAN_AF32, rows, line);
    dan_vk_matvec_q8(w->data, xq, (float *)y->data, rows, x->len);
    free(xq);
    return y;
}

/* weights of llama.cpp: names of the functions by format */
static const char *const dan_k_fmt_mv[DAN_NFMT] = {"матвек_q4_0", "матвек_q4k", "матвек_q5k", "матвек_q6k",
                                                   "матвек_q5_0", "матвек_q4_1", "матвек_q5_1", "матвек_q8"};
static const char *const dan_k_fmt_mm[DAN_NFMT] = {"матвеки_q4_0", "матвеки_q4k", "матвеки_q5k", "матвеки_q6k",
                                                   "матвеки_q5_0", "матвеки_q4_1", "матвеки_q5_1", "матвеки_q8"};
static const char *const dan_k_fmt_from[DAN_NFMT] = {"из_q4_0", "из_q4k", "из_q5k", "из_q6k", "из_q5_0", "из_q4_1", "из_q5_1", "из_q8"};

static void dan_k_fmt_error(const char *name, const char *what, long long a, long long b, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s: ", name);
    size_t k = strlen(buf);
    snprintf(buf + k, sizeof buf - k, what, a, b);
    dan_error(buf, line);
}

/* the numbers of the vectors go into 8 bits: Q8_K needs finite numbers, Q8_0 - a scale that fits half precision */
static void dan_k_fmt_check_x(int fmt, const char *name, const float *xv, int64_t n, int line) {
    int k = fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K || fmt == DAN_FMT_Q6K;
    for (int64_t i = 0; i < n; i++)
        if (k ? !isfinite(xv[i]) : !(fabsf(xv[i]) <= DAN_Q8_MAX)) {
            char buf[300];
            snprintf(buf, sizeof buf, k
                     ? "%s: элемент вектора %lld равен %g, а вектор переводится в 8 бит: нужны конечные числа"
                     : "%s: элемент вектора %lld равен %g, а вектор переводится в 8 бит: нужны конечные числа не больше 8319008 по модулю",
                     name, (long long)i, (double)xv[i]);
            dan_error(buf, line);
        }
}

static dan_arr *dan_k_matvec_fmt(int fmt, const dan_arr *w, const dan_arr *x, int line) {
    const char *name = dan_k_fmt_mv[fmt];
    int64_t nb = dan_vk_fmt_n[fmt], bytes = dan_vk_fmt_bytes[fmt];
    if (x->len % nb != 0)
        dan_k_fmt_error(name, "длина вектора (%lld) должна делиться на %lld", (long long)x->len, (long long)nb, line);
    const float *xv = (const float *)x->data;
    dan_k_fmt_check_x(fmt, name, xv, x->len, line);
    int64_t row_bytes = x->len / nb * bytes;
    if (row_bytes == 0 || w->len % row_bytes != 0)
        dan_k_fmt_error(name, "длина весов в байтах (%lld) должна делиться на длину строки (%lld)", (long long)w->len,
                        (long long)row_bytes, line);
    int64_t rows = w->len / row_bytes;
    uint8_t *xq = malloc((size_t)(x->len / nb * dan_vk_fmt_xbytes[fmt]));
    if (!xq) dan_error(DAN_MSG_NO_MEMORY, line);
    dan_vk_fmt_quantize_x(fmt, xv, xq, x->len);
    dan_arr *y = dan_arr_new(DAN_AF32, rows, line);
    dan_vk_matvec_fmt(fmt, w->data, xq, (float *)y->data, rows, x->len);
    free(xq);
    return y;
}

/* the matrix by n vectors (x - n vectors one after another): n results one after another */
static dan_arr *dan_k_matmul_fmt(int fmt, const dan_arr *w, const dan_arr *x, int64_t n, int line) {
    const char *name = dan_k_fmt_mm[fmt];
    int64_t nb = dan_vk_fmt_n[fmt], bytes = dan_vk_fmt_bytes[fmt];
    if (n < 1) dan_k_fmt_error(name, "векторов (%lld) должно быть хотя бы %lld", (long long)n, 1, line);
    if (x->len % n != 0)
        dan_k_fmt_error(name, "длина векторов (%lld) должна делиться на их число (%lld)", (long long)x->len, (long long)n, line);
    int64_t cols = x->len / n;
    if (cols % nb != 0)
        dan_k_fmt_error(name, "длина вектора (%lld) должна делиться на %lld", (long long)cols, (long long)nb, line);
    const float *xv = (const float *)x->data;
    dan_k_fmt_check_x(fmt, name, xv, x->len, line);
    int64_t row_bytes = cols / nb * bytes;
    if (row_bytes == 0 || w->len % row_bytes != 0)
        dan_k_fmt_error(name, "длина весов в байтах (%lld) должна делиться на длину строки (%lld)", (long long)w->len,
                        (long long)row_bytes, line);
    int64_t rows = w->len / row_bytes;
    if (rows > 0 && n > INT64_MAX / rows) dan_error(DAN_MSG_NO_MEMORY, line);
    /* the vectors into 8 bits once for the whole matrix */
    uint8_t *xq = malloc((size_t)(x->len / nb * dan_vk_fmt_xbytes[fmt]));
    if (!xq) dan_error(DAN_MSG_NO_MEMORY, line);
    dan_vk_fmt_quantize_x(fmt, xv, xq, x->len);
    dan_arr *y = dan_arr_new(DAN_AF32, rows * n, line);
    dan_vk_matmul_fmt(fmt, w->data, xq, (float *)y->data, rows, cols, n);
    free(xq);
    return y;
}

static dan_arr *dan_k_dequantize_fmt(int fmt, const dan_arr *q, int line) {
    int64_t bytes = dan_vk_fmt_bytes[fmt];
    if (q->len % bytes != 0)
        dan_k_fmt_error(dan_k_fmt_from[fmt], "длина массива байтов (%lld) должна делиться на %lld (размер блока)",
                        (long long)q->len, (long long)bytes, line);
    int64_t blocks = q->len / bytes;
    dan_arr *x = dan_arr_new(DAN_AF32, blocks * dan_vk_fmt_n[fmt], line);
    dan_vk_fmt_dequantize(fmt, q->data, (float *)x->data, blocks);
    return x;
}

static dan_arr *dan_k_matvec_q4_0(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q40, w, x, line); }
static dan_arr *dan_k_matvec_q4k(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q4K, w, x, line); }
static dan_arr *dan_k_matvec_q5k(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q5K, w, x, line); }
static dan_arr *dan_k_matvec_q6k(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q6K, w, x, line); }
static dan_arr *dan_k_matvec_q5_0(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q50, w, x, line); }
static dan_arr *dan_k_matvec_q4_1(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q41, w, x, line); }
static dan_arr *dan_k_matvec_q5_1(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q51, w, x, line); }
static dan_arr *dan_k_q4_0_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q40, q, line); }
static dan_arr *dan_k_q4k_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q4K, q, line); }
static dan_arr *dan_k_q5k_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q5K, q, line); }
static dan_arr *dan_k_q6k_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q6K, q, line); }
static dan_arr *dan_k_q5_0_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q50, q, line); }
static dan_arr *dan_k_q4_1_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q41, q, line); }
static dan_arr *dan_k_q5_1_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q51, q, line); }
static dan_arr *dan_k_matmul_q4_0(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q40, w, x, n, line); }
static dan_arr *dan_k_matmul_q4k(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q4K, w, x, n, line); }
static dan_arr *dan_k_matmul_q5k(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q5K, w, x, n, line); }
static dan_arr *dan_k_matmul_q6k(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q6K, w, x, n, line); }
static dan_arr *dan_k_matmul_q5_0(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q50, w, x, n, line); }
static dan_arr *dan_k_matmul_q4_1(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q41, w, x, n, line); }
static dan_arr *dan_k_matmul_q5_1(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q51, w, x, n, line); }
static dan_arr *dan_k_matmul_q8(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q80, w, x, n, line); }

/* «приближённо» */
static double dan_k_dot_f32_fast(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len != b->len) dan_k_error("скаляр: массивы разной длины (%lld и %lld)", a->len, b->len, line);
    return dan_vk()->dot_f32_fast((const float *)a->data, (const float *)b->data, a->len);
}
static double dan_k_dot_f64_fast(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len != b->len) dan_k_error("скаляр: массивы разной длины (%lld и %lld)", a->len, b->len, line);
    return dan_vk()->dot_f64_fast((const double *)a->data, (const double *)b->data, a->len);
}
static dan_arr *dan_k_matvec_f32_fast(const dan_arr *w, const dan_arr *x, int line) {
    int64_t rows = dan_k_rows(w, x, line);
    dan_arr *y = dan_arr_new(DAN_AF32, rows, line);
    dan_vk_matvec_f32_fast((const float *)w->data, (const float *)x->data, (float *)y->data, rows, x->len);
    return y;
}
static dan_arr *dan_k_matvec_f64_fast(const dan_arr *w, const dan_arr *x, int line) {
    int64_t rows = dan_k_rows(w, x, line);
    dan_arr *y = dan_arr_new(DAN_AF64, rows, line);
    dan_vk_matvec_f64_fast((const double *)w->data, (const double *)x->data, (double *)y->data, rows, x->len);
    return y;
}
static dan_arr *dan_k_matmat_f32_fast(const dan_arr *a, const dan_arr *b, int64_t k, int line) {
    int64_t m, n;
    dan_k_mm_dims(a, b, k, &m, &n, line);
    dan_arr *c = dan_arr_new(DAN_AF32, m * n, line);
    dan_vk_matmat_f32_fast((const float *)a->data, (const float *)b->data, (float *)c->data, m, n, k);
    return c;
}
static dan_arr *dan_k_matmat_f64_fast(const dan_arr *a, const dan_arr *b, int64_t k, int line) {
    int64_t m, n;
    dan_k_mm_dims(a, b, k, &m, &n, line);
    dan_arr *c = dan_arr_new(DAN_AF64, m * n, line);
    dan_vk_matmat_f64_fast((const double *)a->data, (const double *)b->data, (double *)c->data, m, n, k);
    return c;
}

static double dan_k_dot_f16(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len != b->len) dan_k_error("скаляр: массивы разной длины (%lld и %lld)", a->len, b->len, line);
    return dan_vk_dot_f16((const uint16_t *)a->data, (const uint16_t *)b->data, a->len);
}

static dan_arr *dan_k_matvec_f16(const dan_arr *w, const dan_arr *x, int line) {
    int64_t rows = dan_k_rows(w, x, line);
    dan_arr *y = dan_arr_new(DAN_AF32, rows, line);
    dan_vk_matvec_f16((const uint16_t *)w->data, (const float *)x->data, (float *)y->data, rows, x->len);
    return y;
}

static dan_arr *dan_k_to_f32(const dan_arr *h, int line) {
    dan_arr *f = dan_arr_new(DAN_AF32, h->len, line);
    dan_vk()->h2f((const uint16_t *)h->data, (float *)f->data, h->len);
    return f;
}

static dan_arr *dan_k_to_f16(const dan_arr *f, int line) {
    const float *x = (const float *)f->data;
    for (int64_t i = 0; i < f->len; i++)
        if (isfinite(x[i]) && fabsf(x[i]) >= 65520.0f) dan_arr_float_range_error(DAN_AF16, x[i], line);
    dan_arr *h = dan_arr_new(DAN_AF16, f->len, line);
    dan_vk_f2h(x, (uint16_t *)h->data, f->len);
    return h;
}

static dan_str dan_k_variant(void) { return dan_vk_name(); }
static int64_t dan_k_threads(void) { return dan_pool_threads(); }
#endif
