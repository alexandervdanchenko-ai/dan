/* ==== Vector kernels and the thread pool of the Dan language. ====

   Kernels for large computations: dot product, sum, element-wise
   operations, matrix by vector, matrix by matrix, softmax, RMS and layer
   normalisation for float (32-bit) and double numbers, and matrix by vector
   for 8-bit weights in blocks of 32 with a scale (the Q8_0 format of GGUF).

   The variant is chosen once, when the program first uses a kernel, by what
   the processor can do: plain C (always present, the reference), AVX2,
   AVX-512 (F and BW) on x86-64, NEON and NEON with dot product instructions
   on 64-bit ARM. The avx512 variant takes VNNI where the processor has it, on
   AMD Zen 5 - VNNI on 512 bits, on Intel with AMX in Linux - the tiles of
   AMX for a matrix by many vectors. The environment variable
   DAN_YADRA=c|avx2|avx512|neon|dotprod asks for a variant (an unsupported one
   is ignored; avx512vnni, avx512z and amx - the kinds of avx512, for
   measurements). На ARM с i8mm вариант dotprod умножает Q8_0 на 8 и больше
   векторов командой smmla (DAN_YADRA=dotprod - без неё, i8mm - с ней).

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
/* vectors of the kernels of a row by a few vectors at most (small batches) */
#define DAN_VK_SM_MAX 8
/* formats of weights of llama.cpp (sreda/yadra.c below) */
enum { DAN_FMT_Q40, DAN_FMT_Q4K, DAN_FMT_Q5K, DAN_FMT_Q6K, DAN_FMT_Q50, DAN_FMT_Q41, DAN_FMT_Q51, DAN_FMT_Q80,
       DAN_FMT_Q2K, DAN_FMT_Q3K, DAN_FMT_IQ4XS, DAN_NFMT };

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
    /* several rows by several vectors at once (below, «rows by vectors»):
       rn_rows rows unpacked by unpack, rn_vecs vectors prepared by
       dan_vk_rn_prep; rn_mode - DAN_VK_RN_SIGNED: the numbers of weights
       unpacked with their sign (for the instructions of signed bytes);
       rn[fmt] NULL - the kernels of a row by 4 vectors */
    int rn_rows, rn_vecs, rn_mode;
    void (*unpack)(int, const uint8_t *, int64_t, uint8_t *, int);
    void (*rn[DAN_NFMT])(const uint8_t *, int64_t, const uint8_t *, int64_t, int64_t, float *);
    /* a matrix by many vectors at once (AMX): (fmt, w, xq, y, rows, cols, nv)
       as dan_vk_matmul_fmt, 1 if done; NULL - not in this variant */
    int (*many)(int, const uint8_t *, const uint8_t *, float *, int64_t, int64_t, int64_t);
    /* a row by a few vectors (small batches): (fmt, nv, w, blocks, x, us,
       out) - the row w of «blocks» blocks by nv vectors in units (prepared by
       dan_vk_rn_prep in the sign mode 0, us bytes apart), out[k] - the bits
       of the row kernel; 0 if there is no kernel for fmt and nv (out NULL -
       only the question); NULL - none */
    int (*sm)(int, int, const uint8_t *, int64_t, const uint8_t *, int64_t, float *);
    /* строки весов Q2_K, Q3_K, IQ4_XS на вектор в Q8_K */
    float (*q2k_dot)(const uint8_t *, const void *, int64_t);
    float (*q3k_dot)(const uint8_t *, const void *, int64_t);
    float (*iq4xs_dot)(const uint8_t *, const void *, int64_t);
    /* раскладка строк по 16 (поток 26А): ядро групп строк в раскладке на
       векторы по формату (tile, xp, xs, blocks, out; NULL - нет) и
       раскладка 16 строк одного блока (fmt, p[16], o) */
    void (*tl[DAN_NFMT])(const uint8_t *, const uint8_t *, int64_t, int64_t, float *);
    void (*tl_unpack)(int, const uint8_t *const *, uint8_t *);
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

/* ---- веса в 2, 3 и 4 бита: Q2_K, Q3_K, IQ4_XS ----

   Байты блока в точности как в ggml (ggml/src/ggml-common.h llama.cpp):
   Q2_K   - 256 чисел в 16 частях по 16: 16 байтов масштабов (младшие 4 бита -
            масштаб sc части, старшие - её наименьшее m), 64 байта 2-битных q,
            половинные d и dmin; число d * sc * q - dmin * m;
   Q3_K   - 256 чисел в 16 частях по 16: 32 байта старших битов (hmask), 64 байта
            младших 2 битов, 12 байтов 6-битных масштабов, половинное d; число
            d * (sc - 32) * q, q - младшие 2 бита минус 4, если старший бит 0;
   IQ4_XS - 256 чисел в 8 частях по 32: половинное d, 16 битов старших 2 битов
            масштабов, 4 байта их младших 4 битов, 128 байтов 4-битных номеров
            в таблице 16 значений kvalues_iq4nl; число d * (ls - 32) * kv.
   В Q2_K и Q3_K число 128n + 32j + l (n < 2, j < 4, l < 32) - биты 2j, 2j + 1
   байта 32n + l 2-битных q (и в Q3_K бит 4n + j байта l hmask); оно в части
   8n + 2j + l / 16, то есть в части номер / 16. В IQ4_XS число 32b + l - младшие
   (l < 16) или старшие 4 бита байта 16b + l % 16.
   Вектор - в Q8_K. Целая часть блока точная; блок даёт
   Q2_K:           p = (d_x * d) * isum - (d_x * dmin) * imin, imin = сумма m * bsums;
   Q3_K, IQ4_XS:   p = (d_x * d) * isum,
   и блоки строки складываются в 8 дорожках, как у ядра Q8_0. */

#define DAN_Q2K_BYTES 84
#define DAN_Q3K_BYTES 110
#define DAN_IQ4XS_BYTES 136

/* kvalues_iq4nl из ggml */
static const int8_t dan_vk_iq4nl[16] = {-127, -104, -83, -65, -49, -35, -22, -10, 1, 13, 25, 38, 53, 69, 89, 113};

/* 16 масштабов Q3_K минус 32 (маски dequantize_row_q3_K, порядок байтов от младшего) */
static inline void dan_vk_q3k_scales(const uint8_t *q, int8_t *sc) {
    uint32_t a[4];
    uint8_t b[16];
    memcpy(a, q, 12);
    uint32_t t = a[2];
    a[2] = ((a[0] >> 4) & 0x0f0f0f0fu) | (((t >> 4) & 0x03030303u) << 4);
    a[3] = ((a[1] >> 4) & 0x0f0f0f0fu) | (((t >> 6) & 0x03030303u) << 4);
    a[0] = (a[0] & 0x0f0f0f0fu) | ((t & 0x03030303u) << 4);
    a[1] = (a[1] & 0x0f0f0f0fu) | (((t >> 2) & 0x03030303u) << 4);
    memcpy(b, a, 16);
    for (int j = 0; j < 16; j++) sc[j] = (int8_t)(b[j] - 32);
}

/* 8 масштабов IQ4_XS минус 32 */
static inline void dan_vk_iq4xs_scales(const uint8_t *w, int8_t *sc) {
    uint16_t h;
    memcpy(&h, w + 2, 2);
    for (int b = 0; b < 8; b++)
        sc[b] = (int8_t)((((w[4 + b / 2] >> (4 * (b % 2))) & 0xF) | (((h >> (2 * b)) & 3) << 4)) - 32);
}

/* число 128n + 32j + l блока Q3_K (-4..3) */
static inline int dan_vk_q3k_q(const uint8_t *w, int n, int j, int l) {
    return ((w[32 + 32 * n + l] >> (2 * j)) & 3) - ((w[l] >> (4 * n + j)) & 1 ? 0 : 4);
}

static void dan_vk_q2k_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    const int8_t *v = y->qs;
    int32_t s = 0, m = 0;
    for (int n = 0; n < 2; n++)
        for (int j = 0; j < 4; j++, v += 32) {
            const uint8_t *q = w + 16 + 32 * n;
            int32_t a = 0, b = 0;
            for (int l = 0; l < 16; l++) {
                a += ((q[l] >> (2 * j)) & 3) * v[l];
                b += ((q[l + 16] >> (2 * j)) & 3) * v[l + 16];
            }
            s += (w[8 * n + 2 * j] & 0xF) * a + (w[8 * n + 2 * j + 1] & 0xF) * b;
        }
    for (int j = 0; j < 16; j++) m += (w[j] >> 4) * y->bsums[j];
    *is = s;
    *im = m;
}

static void dan_vk_q3k_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    int8_t sc[16];
    dan_vk_q3k_scales(w + 96, sc);
    const int8_t *v = y->qs;
    int32_t s = 0;
    for (int n = 0; n < 2; n++)
        for (int j = 0; j < 4; j++, v += 32) {
            int32_t a = 0, b = 0;
            for (int l = 0; l < 16; l++) {
                a += dan_vk_q3k_q(w, n, j, l) * v[l];
                b += dan_vk_q3k_q(w, n, j, l + 16) * v[l + 16];
            }
            s += sc[8 * n + 2 * j] * a + sc[8 * n + 2 * j + 1] * b;
        }
    *is = s;
    *im = 0;
}

static void dan_vk_iq4xs_ints_c(const uint8_t *w, const dan_q8k *y, int32_t *is, int32_t *im) {
    int8_t sc[8];
    dan_vk_iq4xs_scales(w, sc);
    const uint8_t *q = w + 8;
    const int8_t *v = y->qs;
    int32_t s = 0;
    for (int b = 0; b < 8; b++, q += 16, v += 32) {
        int32_t a = 0;
        for (int l = 0; l < 16; l++) a += dan_vk_iq4nl[q[l] & 0xF] * v[l] + dan_vk_iq4nl[q[l] >> 4] * v[l + 16];
        s += sc[b] * a;
    }
    *is = s;
    *im = 0;
}

static inline float dan_vk_q2k_p(const uint8_t *w, const dan_q8k *y, int32_t is, int32_t im) {
    float a = y->d * dan_vk_half(w + 80), b = y->d * dan_vk_half(w + 82);
    float t = a * (float)is, u = b * (float)im;
    return t - u;
}
static inline float dan_vk_q3k_p(const uint8_t *w, const dan_q8k *y, int32_t is, int32_t im) {
    (void)im;
    float a = y->d * dan_vk_half(w + 108);
    return a * (float)is;
}
static inline float dan_vk_iq4xs_p(const uint8_t *w, const dan_q8k *y, int32_t is, int32_t im) {
    (void)im;
    float a = y->d * dan_vk_half(w);
    return a * (float)is;
}
DAN_VK_KDOT(dan_vk_q2k_dot_c, , dan_vk_q2k_ints_c, DAN_Q2K_BYTES, dan_vk_q2k_p)
DAN_VK_KDOT(dan_vk_q3k_dot_c, , dan_vk_q3k_ints_c, DAN_Q3K_BYTES, dan_vk_q3k_p)
DAN_VK_KDOT(dan_vk_iq4xs_dot_c, , dan_vk_iq4xs_ints_c, DAN_IQ4XS_BYTES, dan_vk_iq4xs_p)

/* the formats: bytes and numbers of a block of weights, bytes of a block of the vector in 8 bits */
static const int dan_vk_fmt_bytes[DAN_NFMT] = {DAN_Q40_BYTES, DAN_Q4K_BYTES, DAN_Q5K_BYTES, DAN_Q6K_BYTES,
                                               DAN_Q50_BYTES, DAN_Q41_BYTES, DAN_Q51_BYTES, DAN_Q8_BYTES,
                                               DAN_Q2K_BYTES, DAN_Q3K_BYTES, DAN_IQ4XS_BYTES};
static const int dan_vk_fmt_n[DAN_NFMT] = {32, DAN_QK, DAN_QK, DAN_QK, 32, 32, 32, 32, DAN_QK, DAN_QK, DAN_QK};
static const int dan_vk_fmt_xbytes[DAN_NFMT] = {DAN_Q8_BYTES, (int)sizeof(dan_q8k), (int)sizeof(dan_q8k), (int)sizeof(dan_q8k),
                                                DAN_Q8_BYTES, DAN_Q81_BYTES, DAN_Q81_BYTES, DAN_Q8_BYTES,
                                                (int)sizeof(dan_q8k), (int)sizeof(dan_q8k), (int)sizeof(dan_q8k)};

/* ---- rows by vectors: unpacked rows and prepared vectors ----

   The kernels of several rows by several vectors (the batch of a prompt)
   multiply 4 numbers of a weight and 4 of a vector by one instruction
   (vpdpbusd of AVX-512 VNNI, sdot of ARM) or by vpmaddubsw with the sums of
   16 bits kept until they could overflow (AVX2). For that the numbers of a
   row are unpacked into bytes, and the numbers of a row and of a vector are
   laid out so that every lane of 4 bytes keeps adding one part of a block:

   - the K formats: a unit is a block of 256 numbers; the lane L of 8 holds
     the part of 32 numbers dan_vk_rn_part[L] (Q4_K, Q5_K), for Q6_K the part
     of 16 numbers dan_vk_rn_part[L] of each half; step i (32 bytes) - the
     numbers 4i..4i+3 of the parts of the lanes;
   - Q4_0, Q5_0, Q8_0: a unit is a group of 8 blocks of 32 numbers, the lane
     L - block L, step i - its numbers 4i..4i+3; the last group of a row is
     filled up with zero blocks.

   A unit of a row (DAN_VK_RN_UNIT bytes): 256 bytes of numbers; then for
   Q4_K and Q5_K 8 scales and 8 mins of the parts (32 bits, by lanes), the
   floats d and dmin; for Q6_K 16 scales (8 of each half), d; for blocks of 32
   the 8 floats d. The numbers are unsigned (Q4_0: q, Q5_0: q, Q8_0: q + 128,
   Q6_K: q) unless the variant takes them signed (q - 8, q - 16, q, q - 32).
   A unit of a vector: 256 bytes in the same order; then 32-bit sums that
   take the shift of unsigned numbers off (Q6_K: 32 * the sum of the part,
   blocks of 32: the shift times the sum of the block; 0 for signed numbers),
   for Q4_K and Q5_K the sums of the parts (for the mins); then d of the
   block (K formats) or the 8 d of the blocks.

   The integer sums are exact, so each part gives what the row kernel gives,
   and the numbers of a block are made by the same float operations in the
   same order: the bits of every row by every vector are those of the row
   kernels (the order of lanes of the parts is free, the integer sum does not
   depend on it). */

/* the steps of the kernels of rows by vectors with 16-bit sums (AVX2) and
   of ARM are not unrolled: unrolled, gcc computes the products of all steps
   first and keeps them in memory (VNNI unrolled is faster: Xeon 8370C 1.3
   times, замеры/результаты.md) */
#ifndef DAN_VK_ROLLED
#define DAN_VK_ROLLED _Pragma("GCC unroll 1")
#endif
#define DAN_VK_RN_UNIT 352
#define DAN_VK_RN_SC 256   /* 32-bit scales and mins (weights) or sums (vectors) */
#define DAN_VK_RN_D 320    /* floats */
static const int dan_vk_rn_part[8] = {0, 2, 4, 6, 1, 3, 5, 7};

static int dan_vk_rn_k(int fmt) { return dan_vk_fmt_n[fmt] == DAN_QK; }
static int64_t dan_vk_rn_units(int fmt, int64_t blocks) { return dan_vk_rn_k(fmt) ? blocks : (blocks + 7) / 8; }

static inline void dan_vk_put32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }
static inline void dan_vk_putf(uint8_t *p, float v) { memcpy(p, &v, 4); }

/* the mode of the layout: the numbers of rows with their sign */
#define DAN_VK_RN_SIGNED 1

/* the numbers of a block of 32 as in a row (q of the format; signed - minus the shift) */
static void dan_vk_rn_b32_nat(int fmt, const uint8_t *w, int sgn, uint8_t *nat) {
    switch (fmt) {
    case DAN_FMT_Q40:
        for (int j = 0; j < 16; j++) {
            nat[j] = (uint8_t)((w[2 + j] & 0xF) - (sgn ? 8 : 0));
            nat[j + 16] = (uint8_t)((w[2 + j] >> 4) - (sgn ? 8 : 0));
        }
        break;
    case DAN_FMT_Q50: {
        uint32_t qh = dan_vk_u32(w + 2);
        for (int j = 0; j < 16; j++) {
            nat[j] = (uint8_t)(((w[6 + j] & 0xF) | (((qh >> j) & 1) << 4)) - (sgn ? 16 : 0));
            nat[j + 16] = (uint8_t)(((w[6 + j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) - (sgn ? 16 : 0));
        }
        break;
    }
    default:
        for (int j = 0; j < 32; j++) nat[j] = (uint8_t)(w[2 + j] ^ (sgn ? 0 : 0x80));
        break;
    }
}

/* блок Q2_K, Q3_K или IQ4_XS в единицу (как Q6_K: 16 частей по 16, в дорожке L
   половины n - часть 8n + dan_vk_rn_part[L]): числа без знака - Q2_K q, Q3_K q + 4,
   IQ4_XS kv + 128, со знаком - q, q, kv; 16 масштабов частей (у IQ4_XS - масштаб
   части 32, в которой часть 16), d; у Q2_K ещё dmin и 16 наименьших байтами
   после него в том же порядке */
static void dan_vk_rn_lowbit_c(int fmt, const uint8_t *p, uint8_t *o, int sgn) {
    uint8_t nat[256];
    int8_t sc[16];
    if (fmt == DAN_FMT_Q2K) {
        for (int e = 0; e < 256; e++) nat[e] = (uint8_t)((p[16 + 32 * (e / 128) + e % 32] >> (2 * (e / 32 % 4))) & 3);
        for (int j = 0; j < 16; j++) sc[j] = (int8_t)(p[j] & 0xF);
    } else if (fmt == DAN_FMT_Q3K) {
        for (int e = 0; e < 256; e++) nat[e] = (uint8_t)(dan_vk_q3k_q(p, e / 128, e / 32 % 4, e % 32) + (sgn ? 0 : 4));
        dan_vk_q3k_scales(p + 96, sc);
    } else {
        int8_t s8[8];
        for (int e = 0; e < 256; e++) {
            uint8_t b = p[8 + 16 * (e / 32) + e % 16];
            nat[e] = (uint8_t)(dan_vk_iq4nl[e % 32 < 16 ? b & 0xF : b >> 4] + (sgn ? 0 : 128));
        }
        dan_vk_iq4xs_scales(p, s8);
        for (int j = 0; j < 16; j++) sc[j] = s8[j / 2];
    }
    for (int n = 0; n < 2; n++)
        for (int i = 0; i < 4; i++)
            for (int L = 0; L < 8; L++)
                for (int t = 0; t < 4; t++) o[128 * n + 32 * i + 4 * L + t] = nat[128 * n + 16 * dan_vk_rn_part[L] + 4 * i + t];
    for (int n = 0; n < 2; n++)
        for (int L = 0; L < 8; L++) dan_vk_put32(o + DAN_VK_RN_SC + 4 * (8 * n + L), sc[8 * n + dan_vk_rn_part[L]]);
    dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + (fmt == DAN_FMT_Q2K ? 80 : fmt == DAN_FMT_Q3K ? 108 : 0)));
    if (fmt == DAN_FMT_Q2K) {
        dan_vk_putf(o + DAN_VK_RN_D + 4, dan_vk_half(p + 82));
        for (int n = 0; n < 2; n++)
            for (int L = 0; L < 8; L++) o[DAN_VK_RN_D + 8 + 8 * n + L] = (uint8_t)(p[8 * n + dan_vk_rn_part[L]] >> 4);
    }
}

/* units of a row of «blocks» blocks of the format into «out»: the plain C reference
   (and the last group of blocks of 32 for the variants) */
static void dan_vk_rn_unpack_c(int fmt, const uint8_t *w, int64_t blocks, uint8_t *out, int mode) {
    int wb = dan_vk_fmt_bytes[fmt], sgn = mode & DAN_VK_RN_SIGNED;
    if (dan_vk_rn_k(fmt)) {
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = w + b * wb;
            uint8_t *o = out + b * DAN_VK_RN_UNIT, nat[256];
            memset(o, 0, DAN_VK_RN_UNIT);
            if (fmt >= DAN_FMT_Q2K) {
                dan_vk_rn_lowbit_c(fmt, p, o, sgn);
                continue;
            }
            if (fmt == DAN_FMT_Q6K) {
                const uint8_t *ql = p, *qh = p + 128;
                for (int n = 0; n < 2; n++, ql += 64, qh += 32)
                    for (int l = 0; l < 32; l++) {
                        nat[128 * n + l] = (uint8_t)((ql[l] & 0xF) | ((qh[l] & 3) << 4));
                        nat[128 * n + l + 32] = (uint8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4));
                        nat[128 * n + l + 64] = (uint8_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4));
                        nat[128 * n + l + 96] = (uint8_t)((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4));
                    }
                for (int n = 0; n < 2; n++)
                    for (int i = 0; i < 4; i++)
                        for (int L = 0; L < 8; L++)
                            for (int t = 0; t < 4; t++)
                                o[128 * n + 32 * i + 4 * L + t] =
                                    (uint8_t)(nat[128 * n + 16 * dan_vk_rn_part[L] + 4 * i + t] - (sgn ? 32 : 0));
                for (int n = 0; n < 2; n++)
                    for (int L = 0; L < 8; L++)
                        dan_vk_put32(o + DAN_VK_RN_SC + 4 * (8 * n + L), ((const int8_t *)p)[192 + 8 * n + dan_vk_rn_part[L]]);
                dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + 208));
                continue;
            }
            uint8_t sc[8], m[8];
            dan_vk_k4_scales(p + 4, sc, m);
            const uint8_t *q = p + (fmt == DAN_FMT_Q5K ? 48 : 16), *qh = p + 16;
            for (int c = 0; c < 4; c++)
                for (int l = 0; l < 32; l++) {
                    int h0 = fmt == DAN_FMT_Q5K ? ((qh[l] >> (2 * c)) & 1) << 4 : 0;
                    int h1 = fmt == DAN_FMT_Q5K ? ((qh[l] >> (2 * c + 1)) & 1) << 4 : 0;
                    nat[64 * c + l] = (uint8_t)((q[32 * c + l] & 0xF) | h0);
                    nat[64 * c + 32 + l] = (uint8_t)((q[32 * c + l] >> 4) | h1);
                }
            for (int i = 0; i < 8; i++)
                for (int L = 0; L < 8; L++) memcpy(o + 32 * i + 4 * L, nat + 32 * dan_vk_rn_part[L] + 4 * i, 4);
            for (int L = 0; L < 8; L++) {
                dan_vk_put32(o + DAN_VK_RN_SC + 4 * L, sc[dan_vk_rn_part[L]]);
                dan_vk_put32(o + DAN_VK_RN_SC + 32 + 4 * L, m[dan_vk_rn_part[L]]);
            }
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p));
            dan_vk_putf(o + DAN_VK_RN_D + 4, dan_vk_half(p + 2));
        }
        return;
    }
    for (int64_t g = 0; g < (blocks + 7) / 8; g++) {
        uint8_t *o = out + g * DAN_VK_RN_UNIT, nat[32];
        memset(o, 0, DAN_VK_RN_UNIT);
        for (int L = 0; L < 8 && 8 * g + L < blocks; L++) {
            const uint8_t *p = w + (8 * g + L) * wb;
            dan_vk_rn_b32_nat(fmt, p, sgn, nat);
            for (int i = 0; i < 8; i++) memcpy(o + 32 * i + 4 * L, nat + 4 * i, 4);
            dan_vk_putf(o + DAN_VK_RN_D + 4 * L, dan_vk_half(p));
        }
    }
}

/* a vector in 8 bits (dan_vk_fmt_quantize_x) of «blocks» blocks into units */
static void dan_vk_rn_prep(int fmt, const uint8_t *x, int64_t blocks, uint8_t *out, int mode) {
    int sgn = mode & DAN_VK_RN_SIGNED;
    if (dan_vk_rn_k(fmt)) {
        for (int64_t b = 0; b < blocks; b++) {
            const dan_q8k *y = (const dan_q8k *)(const void *)x + b;
            uint8_t *o = out + b * DAN_VK_RN_UNIT;
            memset(o, 0, DAN_VK_RN_UNIT);
            if (fmt == DAN_FMT_Q6K || fmt >= DAN_FMT_Q2K) {
                /* суммы, снимающие сдвиг чисел без знака (Q2_K - суммы частей для наименьших) */
                int k = fmt == DAN_FMT_Q2K ? 1 : sgn ? 0 : fmt == DAN_FMT_Q3K ? 4 : fmt == DAN_FMT_IQ4XS ? 128 : 32;
                for (int n = 0; n < 2; n++)
                    for (int i = 0; i < 4; i++)
                        for (int L = 0; L < 8; L++)
                            memcpy(o + 128 * n + 32 * i + 4 * L, y->qs + 128 * n + 16 * dan_vk_rn_part[L] + 4 * i, 4);
                for (int n = 0; n < 2; n++)
                    for (int L = 0; L < 8; L++)
                        dan_vk_put32(o + DAN_VK_RN_SC + 4 * (8 * n + L), k * y->bsums[8 * n + dan_vk_rn_part[L]]);
            } else {
                for (int i = 0; i < 8; i++)
                    for (int L = 0; L < 8; L++) memcpy(o + 32 * i + 4 * L, y->qs + 32 * dan_vk_rn_part[L] + 4 * i, 4);
                for (int L = 0; L < 8; L++)
                    dan_vk_put32(o + DAN_VK_RN_SC + 32 + 4 * L, y->bsums[2 * dan_vk_rn_part[L]] + y->bsums[2 * dan_vk_rn_part[L] + 1]);
            }
            dan_vk_putf(o + DAN_VK_RN_D, y->d);
        }
        return;
    }
    int shift = sgn ? 0 : fmt == DAN_FMT_Q40 ? 8 : fmt == DAN_FMT_Q50 ? 16 : 128;
    for (int64_t g = 0; g < (blocks + 7) / 8; g++) {
        uint8_t *o = out + g * DAN_VK_RN_UNIT;
        memset(o, 0, DAN_VK_RN_UNIT);
        for (int L = 0; L < 8 && 8 * g + L < blocks; L++) {
            const uint8_t *p = x + (8 * g + L) * DAN_Q8_BYTES;
            int32_t s = 0;
            for (int i = 0; i < 32; i++) s += (int8_t)p[2 + i];
            for (int i = 0; i < 8; i++) memcpy(o + 32 * i + 4 * L, p + 2 + 4 * i, 4);
            dan_vk_put32(o + DAN_VK_RN_SC + 4 * L, shift * s);
            dan_vk_putf(o + DAN_VK_RN_D + 4 * L, dan_vk_half(p));
        }
    }
}

/* ---- раскладка строк по 16 (поток 26А) ----

   Ядра строк на векторы на AVX-512 VNNI с раскладкой, как у «tiled»-ядер
   llama.cpp: дорожка регистра из 16 чисел по 32 бита - строка, а не часть
   блока. Шестнадцать строк одного блока раскладываются в единицу: шаг s (64
   байта) - числа 4s..4s+3 каждой из 16 строк (строка r - байты 4r..4r+3), и
   одна команда vpdpbusd умножает четыре числа 16 строк на четыре числа
   вектора, размноженные по дорожкам. Сумма блока для каждой строки и вектора
   оказывается в своей дорожке: не нужно складывать дорожки частей, масштабы
   частей и d строк - тоже по дорожкам.

   Числа строк - со знаком (vpdpbusd: байты вектора без знака, строки - со
   знаком), числа вектора сдвинуты на 128 (x + 128). Сдвиг снимает число
   строки C, посчитанное при раскладке: 128, умноженное на сумму чисел строки
   (с масштабами частей). Числа строки в единице:
   Q8_0 - q; Q2_K - sc * q (масштаб части уже умножен: 0..45); Q3_K - минус
   (sc - 32) * q (-128..124: (sc - 32) * q бывает и 128), сумма блока - C
   минус сумма ядра; IQ4_XS - kv; Q4_K, Q5_K - q; Q6_K - q - 32.
   У Q4_K, Q5_K, IQ4_XS, Q6_K сумма части умножается на масштаб части в
   дорожках (vpmulld), у Q2_K и Q3_K масштаб уже в числах. Наименьшие (Q2_K,
   Q4_K, Q5_K) - vpdpwssd: пары наименьших соседних частей строки на пары
   сумм частей вектора.

   Единица 16 строк одного блока (у Q8_0 блок - 32 числа, у остальных 256):
   числа (ступеней по 64 байта: 8 или 64), масштабы частей (int32 по
   дорожкам, у Q4_K, Q5_K, IQ4_XS - 8 частей, у Q6_K - 16), пары наименьших
   (int16, у Q2_K - 8 пар, у Q4_K и Q5_K - 4), C (int32), d и dmin (float).
   Единица вектора: блок K - 256 байтов x + 128, 16 сумм частей по 16
   (int16, как в Q8_K), 8 сумм частей по 32, d; блок Q8_0 - 32 байта x + 128
   и d.

   Целые суммы блока точны и равны суммам ядра строки C, числа блока
   получаются теми же действиями с плавающей точкой в том же порядке, и
   блоки складываются в 8 дорожках, как у ядра строки: биты те же. Раскладка
   делается при каждом умножении на пакет (как у llama.cpp «tiled»): веса
   остаются отображёнными из файла, лишней памяти на модель нет (буфер -
   строки одной группы, замеры/результаты.md, поток 26А). */

static int dan_vk_tl_steps(int fmt) { return fmt == DAN_FMT_Q80 ? 8 : 64; }
static int dan_vk_tl_nsc(int fmt) {
    return fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K || fmt == DAN_FMT_IQ4XS ? 8 : fmt == DAN_FMT_Q6K ? 16 : 0;
}
static int dan_vk_tl_nmp(int fmt) { return fmt == DAN_FMT_Q2K ? 8 : fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K ? 4 : 0; }
static int dan_vk_tl_mins(int fmt) { return fmt == DAN_FMT_Q2K || fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K; }
/* смещения в единице 16 строк: масштабы, пары наименьших, C, d, dmin; размер */
static int dan_vk_tl_sc(int fmt) { return 64 * dan_vk_tl_steps(fmt); }
static int dan_vk_tl_mp(int fmt) { return dan_vk_tl_sc(fmt) + 64 * dan_vk_tl_nsc(fmt); }
static int dan_vk_tl_c(int fmt) { return dan_vk_tl_mp(fmt) + 64 * dan_vk_tl_nmp(fmt); }
static int dan_vk_tl_d(int fmt) { return dan_vk_tl_c(fmt) + 64; }
static int dan_vk_tl_size(int fmt) { return dan_vk_tl_d(fmt) + (dan_vk_tl_mins(fmt) ? 128 : 64); }
/* единица вектора: байты одного блока */
#define DAN_VK_TL_XK 320
#define DAN_VK_TL_X8 64
#define DAN_VK_TL_XBS 256   /* суммы частей по 16 */
#define DAN_VK_TL_XB32 288  /* суммы частей по 32 */
#define DAN_VK_TL_XD 304    /* d (у Q8_0 - 32) */
static int dan_vk_tl_xu(int fmt) { return fmt == DAN_FMT_Q80 ? DAN_VK_TL_X8 : DAN_VK_TL_XK; }

/* числа строки в раскладке (со знаком, см. выше), масштабы (sc - по частям, у
   Q2_K и Q3_K - 0), наименьшие частей, d и dmin одного блока формата */
static void dan_vk_tl_nat_c(int fmt, const uint8_t *p, int8_t *nat, int32_t *sc, int32_t *mn, float *d, float *dm) {
    *dm = 0.0f;
    if (fmt == DAN_FMT_Q80) {
        for (int i = 0; i < 32; i++) nat[i] = (int8_t)p[2 + i];
        *d = dan_vk_half(p);
        return;
    }
    if (fmt == DAN_FMT_Q2K) {
        for (int e = 0; e < 256; e++)
            nat[e] = (int8_t)((p[e / 16] & 0xF) * ((p[16 + 32 * (e / 128) + e % 32] >> (2 * (e / 32 % 4))) & 3));
        for (int j = 0; j < 16; j++) mn[j] = p[j] >> 4;
        *d = dan_vk_half(p + 80);
        *dm = dan_vk_half(p + 82);
    } else if (fmt == DAN_FMT_Q3K) {
        int8_t s[16];
        dan_vk_q3k_scales(p + 96, s);
        for (int e = 0; e < 256; e++) nat[e] = (int8_t)(-(s[e / 16] * dan_vk_q3k_q(p, e / 128, e / 32 % 4, e % 32)));
        *d = dan_vk_half(p + 108);
    } else if (fmt == DAN_FMT_IQ4XS) {
        int8_t s[8];
        dan_vk_iq4xs_scales(p, s);
        for (int e = 0; e < 256; e++) {
            uint8_t b = p[8 + 16 * (e / 32) + e % 16];
            nat[e] = dan_vk_iq4nl[e % 32 < 16 ? b & 0xF : b >> 4];
        }
        for (int j = 0; j < 8; j++) sc[j] = s[j];
        *d = dan_vk_half(p);
    } else if (fmt == DAN_FMT_Q6K) {
        const uint8_t *ql = p, *qh = p + 128;
        for (int n = 0; n < 2; n++, ql += 64, qh += 32)
            for (int l = 0; l < 32; l++) {
                nat[128 * n + l] = (int8_t)(((ql[l] & 0xF) | ((qh[l] & 3) << 4)) - 32);
                nat[128 * n + l + 32] = (int8_t)(((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32);
                nat[128 * n + l + 64] = (int8_t)(((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32);
                nat[128 * n + l + 96] = (int8_t)(((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4)) - 32);
            }
        for (int j = 0; j < 16; j++) sc[j] = ((const int8_t *)p)[192 + j];
        *d = dan_vk_half(p + 208);
    } else {   /* Q4_K, Q5_K */
        uint8_t s[8], m[8];
        dan_vk_k4_scales(p + 4, s, m);
        const uint8_t *q = p + (fmt == DAN_FMT_Q5K ? 48 : 16), *qh = p + 16;
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 32; l++) {
                int h0 = fmt == DAN_FMT_Q5K ? ((qh[l] >> (2 * c)) & 1) << 4 : 0;
                int h1 = fmt == DAN_FMT_Q5K ? ((qh[l] >> (2 * c + 1)) & 1) << 4 : 0;
                nat[64 * c + l] = (int8_t)((q[32 * c + l] & 0xF) | h0);
                nat[64 * c + 32 + l] = (int8_t)((q[32 * c + l] >> 4) | h1);
            }
        for (int j = 0; j < 8; j++) { sc[j] = s[j]; mn[j] = m[j]; }
        *d = dan_vk_half(p);
        *dm = dan_vk_half(p + 2);
    }
}

/* 16 строк (p[r] - блок строки r) в единицу раскладки o: образец на C */
static void dan_vk_tl_unpack_c(int fmt, const uint8_t *const *p, uint8_t *o) {
    int steps = dan_vk_tl_steps(fmt), nsc = dan_vk_tl_nsc(fmt), nmp = dan_vk_tl_nmp(fmt);
    int part = nsc ? 4 * steps / nsc : 4 * steps;
    for (int r = 0; r < 16; r++) {
        int8_t nat[256];
        int32_t sc[16] = {0}, mn[16] = {0};
        float d, dm;
        dan_vk_tl_nat_c(fmt, p[r], nat, sc, mn, &d, &dm);
        for (int s = 0; s < steps; s++) memcpy(o + 64 * s + 4 * r, nat + 4 * s, 4);
        int32_t c = 0;
        for (int e = 0; e < 4 * steps; e++) c += (nsc ? sc[e / part] : 1) * nat[e];
        for (int j = 0; j < nsc; j++) dan_vk_put32(o + dan_vk_tl_sc(fmt) + 64 * j + 4 * r, sc[j]);
        for (int j = 0; j < nmp; j++) {
            int16_t pr[2] = {(int16_t)mn[2 * j], (int16_t)mn[2 * j + 1]};
            memcpy(o + dan_vk_tl_mp(fmt) + 64 * j + 4 * r, pr, 4);
        }
        dan_vk_put32(o + dan_vk_tl_c(fmt) + 4 * r, 128 * c);
        dan_vk_putf(o + dan_vk_tl_d(fmt) + 4 * r, d);
        if (dan_vk_tl_mins(fmt)) dan_vk_putf(o + dan_vk_tl_d(fmt) + 64 + 4 * r, dm);
    }
}

/* вектор в 8 битах (dan_vk_fmt_quantize_x) из «blocks» блоков в единицы раскладки */
static void dan_vk_tl_prep(int fmt, const uint8_t *x, int64_t blocks, uint8_t *out) {
    if (fmt == DAN_FMT_Q80) {
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = x + b * DAN_Q8_BYTES;
            uint8_t *o = out + b * DAN_VK_TL_X8;
            for (int i = 0; i < 32; i++) o[i] = (uint8_t)(p[2 + i] ^ 0x80);
            dan_vk_putf(o + 32, dan_vk_half(p));
            memset(o + 36, 0, DAN_VK_TL_X8 - 36);
        }
        return;
    }
    for (int64_t b = 0; b < blocks; b++) {
        const dan_q8k *y = (const dan_q8k *)(const void *)x + b;
        uint8_t *o = out + b * DAN_VK_TL_XK;
        for (int i = 0; i < 256; i++) o[i] = (uint8_t)((uint8_t)y->qs[i] ^ 0x80);
        memcpy(o + DAN_VK_TL_XBS, y->bsums, 32);
        for (int j = 0; j < 8; j++) {
            int16_t s = (int16_t)(y->bsums[2 * j] + y->bsums[2 * j + 1]);
            memcpy(o + DAN_VK_TL_XB32 + 2 * j, &s, 2);
        }
        dan_vk_putf(o + DAN_VK_TL_XD, y->d);
        memset(o + DAN_VK_TL_XD + 4, 0, DAN_VK_TL_XK - DAN_VK_TL_XD - 4);
    }
}

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
                                          dan_vk_q50_dot_c, dan_vk_q41_dot_c, dan_vk_q51_dot_c, {0}, 0, 0, 0, NULL, {0}, NULL, NULL,
                                          dan_vk_q2k_dot_c, dan_vk_q3k_dot_c, dan_vk_iq4xs_dot_c};

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

#if defined(__MINGW32__) && defined(__GNUC__) && !defined(__clang__)
/* gcc MinGW (15.2, -O3): клон функции под постоянный аргумент (ipa-cp), который возвращает
   __m256, падает при обращении к стеку (Windows x64 не выравнивает стек по 32 байтам);
   без клонов ядра те же, а падения нет */
#pragma GCC optimize("no-ipa-cp-clone")
#endif
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
/* 32 products of a block as 8 partial sums: |x| (у -128 - 128 без знака) на y со знаком x одной
   vpmaddubsw, пара не больше 2 * 128 * 127 (у вектора b, квантованного Даном, чисел -128 нет; без
   расширения до 16 бит - вдвое меньше перестановок на EPYC 7763) */
DAN_VK_AVX2F16 static inline __m256i dan_vk_q8_part_avx2(const uint8_t *a, const uint8_t *b) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(a + 2)), y = _mm256_loadu_si256((const __m256i *)(b + 2));
    return _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_abs_epi8(x), _mm256_sign_epi8(y, x)), _mm256_set1_epi16(1));
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

DAN_VK_AVX2 static inline int32_t dan_vk_hsum_i32_avx2(__m256i s);
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
        float p = dan_vk_q8_p(pa, pb, dan_vk_hsum_i32_avx2(dan_vk_q8_part_avx2(pa, pb)));
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

/* то же, что dan_vk_k4_sm, в регистре (q - 12 байтов масштабов блока Q4_K или Q5_K, за ними ещё
   есть 4 байта блока): слова u0 u2 u1 u2 (младшие 6 или 4 бита) и u0 u0 u1 u1 (старшие 2 бита),
   без записи байтов в память и чтения их регистром (запись по частям не передаётся чтению
   целиком и задерживает его) */
DAN_VK_AVX2 static inline __m128i dan_vk_k4_sm128_avx2(const uint8_t *q) {
    __m128i u = _mm_loadu_si128((const __m128i *)(const void *)q);
    __m128i lo = _mm_and_si128(_mm_srlv_epi32(_mm_shuffle_epi32(u, 0x98), _mm_setr_epi32(0, 0, 0, 4)),
                               _mm_setr_epi32(0x3f3f3f3f, 0x0f0f0f0f, 0x3f3f3f3f, 0x0f0f0f0f));
    __m128i hi = _mm_and_si128(_mm_srli_epi32(_mm_shuffle_epi32(u, 0x50), 6), _mm_setr_epi32(0, 0x03030303, 0, 0x03030303));
    return _mm_or_si128(lo, _mm_slli_epi32(hi, 4));
}
/* scales and mins of a Q4_K or Q5_K block as 16 numbers of 16 bits: sc0..sc7, m0..m7 */
DAN_VK_AVX2 static inline __m256i dan_vk_k4_sm_avx2(const uint8_t *q) {
    return _mm256_cvtepu8_epi16(dan_vk_k4_sm128_avx2(q));
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

/* Хвост строки K-формата (меньше 8 блоков после последних полных 8: у строк из 1536 чисел, Qwen2.5
   1.5B, - все 6 блоков): целые суммы n блоков складываются по дорожкам, как у полных 8 (дорожки
   n..7 - нули), числа p блоков считаются по дорожкам теми же действиями, что у ядра строки, и
   прибавляются к r по одному по порядку. Прежде каждый блок хвоста складывался отдельно и его p
   считалось на скалярах. */
DAN_VK_AVX2 static inline __m256i dan_vk_hadd8_avx2(const __m256i *v);
/* n (1..7) половинных чисел через stride байтов как числа с плавающей точкой (остальные - 0),
   без чтения за n-м */
DAN_VK_AVX2F16 static inline __m256 dan_vk_halvesn_avx2(const uint8_t *p, int stride, int n) {
    __m128i h = _mm_setzero_si128();
    uint16_t v;
    switch (n) {
#define DAN_VK_HALF_N(l) memcpy(&v, p + (l) * stride, 2); h = _mm_insert_epi16(h, v, (l));
    case 7: DAN_VK_HALF_N(6) /* fallthrough */
    case 6: DAN_VK_HALF_N(5) /* fallthrough */
    case 5: DAN_VK_HALF_N(4) /* fallthrough */
    case 4: DAN_VK_HALF_N(3) /* fallthrough */
    case 3: DAN_VK_HALF_N(2) /* fallthrough */
    case 2: DAN_VK_HALF_N(1) /* fallthrough */
    default: DAN_VK_HALF_N(0)
#undef DAN_VK_HALF_N
    }
    return _mm256_cvtph_ps(h);
}
/* d n блоков вектора (остальные - 0) */
DAN_VK_AVX2 static inline __m256 dan_vk_ydn_avx2(const dan_q8k *px, int n) {
    float d[8];
    for (int l = 0; l < 8; l++) d[l] = l < n ? px[l].d : 0.0f;
    return _mm256_setr_ps(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}
/* хвост: PARTS(w, y, &mins) - 8 частичных сумм isum блока и imin (VEC: вектор частичных сумм, иначе
   int32_t), d по смещению DOFF, dmin (MINS) по MOFF */
#define DAN_VK_KTAIL_AVX2(PARTS, WB, DOFF, MINS, MOFF, VEC) \
    if (i < blocks) { \
        int n = (int)(blocks - i); \
        const uint8_t *pw = w + i * (WB); \
        const dan_q8k *px = x + i; \
        __m256i v[8], mv[8]; \
        int32_t ex[8] = {0}; \
        for (int l = 0; l < 8; l++) { \
            v[l] = _mm256_setzero_si256(); \
            mv[l] = _mm256_setzero_si256(); \
        } \
        for (int l = 0; l < n; l++) v[l] = VEC ? PARTS(pw + l * (WB), px + l, (void *)&mv[l]) : PARTS(pw + l * (WB), px + l, (void *)&ex[l]); \
        __m256 yd = dan_vk_ydn_avx2(px, n); \
        __m256 t = _mm256_mul_ps(_mm256_mul_ps(yd, dan_vk_halvesn_avx2(pw + (DOFF), WB, n)), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(v))); \
        if (MINS) { \
            __m256 b = _mm256_mul_ps(yd, dan_vk_halvesn_avx2(pw + (MOFF), WB, n)); \
            __m256i m = VEC ? dan_vk_hadd8_avx2(mv) : _mm256_setr_epi32(ex[0], ex[1], ex[2], ex[3], ex[4], ex[5], ex[6], ex[7]); \
            t = _mm256_sub_ps(t, _mm256_mul_ps(b, _mm256_cvtepi32_ps(m))); \
        } \
        float pf[8]; \
        _mm256_storeu_ps(pf, t); \
        for (int l = 0; l < n; l++) r = r + pf[l]; \
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
            __m256i m = _mm256_setr_epi32(ex[0], ex[1], ex[2], ex[3], ex[4], ex[5], ex[6], ex[7]); \
            t = _mm256_sub_ps(t, _mm256_mul_ps(b, _mm256_cvtepi32_ps(m))); \
        } \
        acc = _mm256_add_ps(acc, t); \
    } \
    __m128 v4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1)); \
    v4 = _mm_add_ps(v4, _mm_movehl_ps(v4, v4)); \
    v4 = _mm_add_ss(v4, _mm_shuffle_ps(v4, v4, 1)); \
    float r = _mm_cvtss_f32(v4); \
    DAN_VK_KTAIL_AVX2(PARTS, WB, DOFF, MINS, 2, 0) \
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

/* ---- rows by vectors on x86 (the units: see «rows by vectors» above) ----

   AVX-512 VNNI: 4 rows by 4 vectors, 4 numbers of a row by 4 of a vector by
   one vpdpbusd (on 256 bits); AVX2: 2 rows by 4 vectors, vpmaddubsw with the
   16-bit sums added until they could overflow (the number of steps G: Q4_K
   and Q4_0 - 8, Q5_K and Q5_0 - 4, Q6_K - 2; numbers of a vector within
   -128..127), then multiplied by the scales of the parts by vpmaddwd. */
#define DAN_VK_RN_R_VNNI 4
#define DAN_VK_RN_R_AVX2 2
#define DAN_VK_RN_V 4
#define DAN_VK_LD(p) _mm256_loadu_si256((const __m256i *)(const void *)(p))

/* the end of a block of a K format: T and M - the integer sums by parts of
   the (row, vector) pairs r * NV + k; the 8 pairs of group g become the lanes
   of one vector, whose numbers are made as in the row kernel */
#define DAN_VK_RNK_END(R, NV, MINS) \
        for (int g = 0; g < (R) * (NV) / 8; g++) { \
            float dw[8], dm[8], yd[8]; \
            for (int l = 0; l < 8; l++) { \
                int r = (8 * g + l) / (NV), k = (8 * g + l) % (NV); \
                memcpy(&dw[l], wb[r] + DAN_VK_RN_D, 4); \
                memcpy(&dm[l], wb[r] + DAN_VK_RN_D + 4, 4); \
                memcpy(&yd[l], xb[k] + DAN_VK_RN_D, 4); \
            } \
            __m256 Y = _mm256_loadu_ps(yd); \
            __m256 t = _mm256_mul_ps(_mm256_mul_ps(Y, _mm256_loadu_ps(dw)), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(T + 8 * g))); \
            if (MINS) { \
                __m256 bm = _mm256_mul_ps(Y, _mm256_loadu_ps(dm)); \
                t = _mm256_sub_ps(t, _mm256_mul_ps(bm, _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(M + 8 * g)))); \
            } \
            if (b < full) _mm256_storeu_ps(&accf[b & 7][8 * g], _mm256_add_ps(_mm256_loadu_ps(&accf[b & 7][8 * g]), t)); \
            else _mm256_storeu_ps(&tail[8 * g], _mm256_add_ps(_mm256_loadu_ps(&tail[8 * g]), t)); \
        } \
        if (b + 1 == full) \
            for (int g = 0; g < (R) * (NV) / 8; g++) { \
                __m256 a[8]; \
                for (int l = 0; l < 8; l++) a[l] = _mm256_loadu_ps(&accf[l][8 * g]); \
                __m256 s = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(a[0], a[4]), _mm256_add_ps(a[2], a[6])), \
                                         _mm256_add_ps(_mm256_add_ps(a[1], a[5]), _mm256_add_ps(a[3], a[7]))); \
                _mm256_storeu_ps(&tail[8 * g], s); \
            }

/* the same for NV = 4 with the floats of the 8 pairs of a group made by
   broadcasts (rows 2g and 2g + 1 - halves, vectors - lanes 0..3 of each
   half) and not through memory: 8 stores of 4 bytes read back as one vector
   of 32 bytes wait for the stores (the same numbers, the same bits) */
#define DAN_VK_RNK_END4(R, MINS) \
        { \
            float yv[4]; \
            for (int k = 0; k < 4; k++) memcpy(&yv[k], xb[k] + DAN_VK_RN_D, 4); \
            __m128 y4 = _mm_setr_ps(yv[0], yv[1], yv[2], yv[3]); \
            __m256 Y = _mm256_set_m128(y4, y4); \
            for (int g = 0; g < (R) / 2; g++) { \
                float d0, d1, m0, m1; \
                memcpy(&d0, wb[2 * g] + DAN_VK_RN_D, 4); \
                memcpy(&d1, wb[2 * g + 1] + DAN_VK_RN_D, 4); \
                __m256 dw = _mm256_set_m128(_mm_set1_ps(d1), _mm_set1_ps(d0)); \
                __m256 t = _mm256_mul_ps(_mm256_mul_ps(Y, dw), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(T + 8 * g))); \
                if (MINS) { \
                    memcpy(&m0, wb[2 * g] + DAN_VK_RN_D + 4, 4); \
                    memcpy(&m1, wb[2 * g + 1] + DAN_VK_RN_D + 4, 4); \
                    __m256 bm = _mm256_mul_ps(Y, _mm256_set_m128(_mm_set1_ps(m1), _mm_set1_ps(m0))); \
                    t = _mm256_sub_ps(t, _mm256_mul_ps(bm, _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(M + 8 * g)))); \
                } \
                if (b < full) _mm256_storeu_ps(&accf[b & 7][8 * g], _mm256_add_ps(_mm256_loadu_ps(&accf[b & 7][8 * g]), t)); \
                else _mm256_storeu_ps(&tail[8 * g], _mm256_add_ps(_mm256_loadu_ps(&tail[8 * g]), t)); \
            } \
        } \
        if (b + 1 == full) \
            for (int g = 0; g < (R) / 2; g++) { \
                __m256 a[8]; \
                for (int l = 0; l < 8; l++) a[l] = _mm256_loadu_ps(&accf[l][8 * g]); \
                __m256 s = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(a[0], a[4]), _mm256_add_ps(a[2], a[6])), \
                                         _mm256_add_ps(_mm256_add_ps(a[1], a[5]), _mm256_add_ps(a[3], a[7]))); \
                _mm256_storeu_ps(&tail[8 * g], s); \
            }
#define DAN_VK_RNK4_X86(NAME, ATTR, R, MINS, INTS) \
ATTR static void NAME(const uint8_t *w, int64_t ws, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    float accf[8][(R) * 4], tail[(R) * 4]; \
    memset(accf, 0, sizeof accf); \
    for (int l = 0; l < (R) * 4; l++) tail[l] = 0.0f; \
    const int64_t full = blocks / 8 * 8; \
    for (int64_t b = 0; b < blocks; b++) { \
        const uint8_t *wb[R], *xb[4]; \
        for (int r = 0; r < (R); r++) wb[r] = w + r * ws + b * DAN_VK_RN_UNIT; \
        for (int k = 0; k < 4; k++) xb[k] = x + k * xs + b * DAN_VK_RN_UNIT; \
        __m256i T[(R) * 4], M[(R) * 4]; \
        INTS(wb, xb, T, M); \
        DAN_VK_RNK_END4(R, MINS) \
    } \
    for (int l = 0; l < (R) * 4; l++) out[l] = tail[l]; \
}

/* R rows (units ws bytes apart) by NV vectors (xs bytes apart) of a K format:
   INTS(wb, xb, T, M) - the integer sums of a block; out[r * NV + k] */
#define DAN_VK_RNK_X86(NAME, ATTR, R, NV, MINS, INTS) \
ATTR static void NAME(const uint8_t *w, int64_t ws, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    float accf[8][(R) * (NV)], tail[(R) * (NV)]; \
    memset(accf, 0, sizeof accf); \
    for (int l = 0; l < (R) * (NV); l++) tail[l] = 0.0f; \
    const int64_t full = blocks / 8 * 8; \
    for (int64_t b = 0; b < blocks; b++) { \
        const uint8_t *wb[R], *xb[NV]; \
        for (int r = 0; r < (R); r++) wb[r] = w + r * ws + b * DAN_VK_RN_UNIT; \
        for (int k = 0; k < (NV); k++) xb[k] = x + k * xs + b * DAN_VK_RN_UNIT; \
        __m256i T[(R) * (NV)], M[(R) * (NV)]; \
        INTS(wb, xb, T, M); \
        DAN_VK_RNK_END(R, NV, MINS) \
    } \
    for (int l = 0; l < (R) * (NV); l++) out[l] = tail[l]; \
}

/* R rows by NV vectors of groups of 8 blocks of 32: INTS(wb, xb, S) - the
   integer sums of the 8 blocks (lane - block) before the shift is taken off */
#define DAN_VK_RNB_X86(NAME, ATTR, R, NV, INTS) \
ATTR static void NAME(const uint8_t *w, int64_t ws, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    __m256 facc[(R) * (NV)]; \
    int32_t last[(R) * (NV)][8]; \
    for (int l = 0; l < (R) * (NV); l++) facc[l] = _mm256_setzero_ps(); \
    const int64_t units = (blocks + 7) / 8, fullu = blocks / 8; \
    for (int64_t u = 0; u < units; u++) { \
        const uint8_t *wb[R], *xb[NV]; \
        for (int r = 0; r < (R); r++) wb[r] = w + r * ws + u * DAN_VK_RN_UNIT; \
        for (int k = 0; k < (NV); k++) xb[k] = x + k * xs + u * DAN_VK_RN_UNIT; \
        __m256i S[(R) * (NV)]; \
        INTS(wb, xb, S); \
        for (int r = 0; r < (R); r++) { \
            __m256 dw = _mm256_loadu_ps((const float *)(const void *)(wb[r] + DAN_VK_RN_D)); \
            for (int k = 0; k < (NV); k++) { \
                __m256i is = _mm256_sub_epi32(S[r * (NV) + k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC)); \
                if (u < fullu) { \
                    __m256 d = _mm256_mul_ps(dw, _mm256_loadu_ps((const float *)(const void *)(xb[k] + DAN_VK_RN_D))); \
                    facc[r * (NV) + k] = _mm256_add_ps(facc[r * (NV) + k], _mm256_mul_ps(d, _mm256_cvtepi32_ps(is))); \
                } else _mm256_storeu_si256((__m256i *)(void *)last[r * (NV) + k], is); \
            } \
        } \
    } \
    for (int r = 0; r < (R); r++) \
        for (int k = 0; k < (NV); k++) { \
            float res = dan_vk_hsum8_lanes_avx2(facc[r * (NV) + k]); \
            const uint8_t *wl = w + r * ws + fullu * DAN_VK_RN_UNIT + DAN_VK_RN_D, *xl = x + k * xs + fullu * DAN_VK_RN_UNIT + DAN_VK_RN_D; \
            for (int l = 0; l < blocks - 8 * fullu; l++) { \
                float a, c; \
                memcpy(&a, wl + 4 * l, 4); \
                memcpy(&c, xl + 4 * l, 4); \
                float d = a * c, p = d * (float)last[r * (NV) + k][l]; \
                res = res + p; \
            } \
            out[r * (NV) + k] = res; \
        } \
}

/* -- AVX-512 VNNI -- */
#define DAN_VK_RNV_STEPS(OFF, N) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) for (int k = 0; k < DAN_VK_RN_V; k++) acc[r][k] = _mm256_setzero_si256(); \
    for (int i = 0; i < (N); i++) { \
        __m256i wv[DAN_VK_RN_R_VNNI]; \
        for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) wv[r] = DAN_VK_LD(wb[r] + (OFF) + 32 * i); \
        for (int k = 0; k < DAN_VK_RN_V; k++) { \
            __m256i yv = DAN_VK_LD(xb[k] + (OFF) + 32 * i); \
            for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) acc[r][k] = _mm256_dpbusd_epi32(acc[r][k], wv[r], yv); \
        } \
    }
DAN_VK_VNNI static inline void dan_vk_rn_k4_vnni(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    DAN_VK_RNV_STEPS(0, 8)
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) {
        __m256i sc = DAN_VK_LD(wb[r] + DAN_VK_RN_SC), m = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32);
        for (int k = 0; k < DAN_VK_RN_V; k++) {
            T[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(acc[r][k], sc);
            M[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(m, DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32));
        }
    }
}
DAN_VK_VNNI static inline void dan_vk_rn_q6k_vnni(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    (void)M;
    for (int n = 0; n < 2; n++) {
        DAN_VK_RNV_STEPS(128 * n, 4)
        for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) {
            __m256i sc = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32 * n);
            for (int k = 0; k < DAN_VK_RN_V; k++) {
                __m256i t = _mm256_mullo_epi32(_mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32 * n)), sc);
                T[r * DAN_VK_RN_V + k] = n ? _mm256_add_epi32(T[r * DAN_VK_RN_V + k], t) : t;
            }
        }
    }
}
DAN_VK_VNNI static inline void dan_vk_rn_b32_vnni(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *S) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    DAN_VK_RNV_STEPS(0, 8)
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++)
        for (int k = 0; k < DAN_VK_RN_V; k++) S[r * DAN_VK_RN_V + k] = acc[r][k];
}
DAN_VK_RNK_X86(dan_vk_rn_q4k_vnni, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, DAN_VK_RN_V, 1, dan_vk_rn_k4_vnni)
DAN_VK_RNK_X86(dan_vk_rn_q6k_vnni_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, DAN_VK_RN_V, 0, dan_vk_rn_q6k_vnni)
DAN_VK_RNB_X86(dan_vk_rn_b32_vnni_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, DAN_VK_RN_V, dan_vk_rn_b32_vnni)

/* -- AVX-512 VNNI on 512 bits: two steps by one vpdpbusd (the halves of a
   register - steps i and i + 1 - added at the end of the part: the integer
   sums are the same). On AMD Zen 5 (full 512-bit units); on the processors
   of Intel and on Zen 4 the 256-bit kernels are faster or the same
   (замеры/результаты.md) -- */
#define DAN_VK_RNZ_STEPS(OFF, N) \
    { \
        __m512i az[DAN_VK_RN_R_VNNI][DAN_VK_RN_V]; \
        for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) for (int k = 0; k < DAN_VK_RN_V; k++) az[r][k] = _mm512_setzero_si512(); \
        for (int i = 0; i < (N); i += 2) { \
            __m512i wv[DAN_VK_RN_R_VNNI]; \
            for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) wv[r] = _mm512_loadu_si512((const void *)(wb[r] + (OFF) + 32 * i)); \
            for (int k = 0; k < DAN_VK_RN_V; k++) { \
                __m512i yv = _mm512_loadu_si512((const void *)(xb[k] + (OFF) + 32 * i)); \
                for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) az[r][k] = _mm512_dpbusd_epi32(az[r][k], wv[r], yv); \
            } \
        } \
        for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) \
            for (int k = 0; k < DAN_VK_RN_V; k++) \
                acc[r][k] = _mm256_add_epi32(_mm512_castsi512_si256(az[r][k]), _mm512_extracti64x4_epi64(az[r][k], 1)); \
    }
DAN_VK_VNNI static inline void dan_vk_rn_k4_vnniz(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    DAN_VK_RNZ_STEPS(0, 8)
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) {
        __m256i sc = DAN_VK_LD(wb[r] + DAN_VK_RN_SC), m = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32);
        for (int k = 0; k < DAN_VK_RN_V; k++) {
            T[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(acc[r][k], sc);
            M[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(m, DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32));
        }
    }
}
DAN_VK_VNNI static inline void dan_vk_rn_q6k_vnniz(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    (void)M;
    for (int n = 0; n < 2; n++) {
        DAN_VK_RNZ_STEPS(128 * n, 4)
        for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) {
            __m256i sc = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32 * n);
            for (int k = 0; k < DAN_VK_RN_V; k++) {
                __m256i t = _mm256_mullo_epi32(_mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32 * n)), sc);
                T[r * DAN_VK_RN_V + k] = n ? _mm256_add_epi32(T[r * DAN_VK_RN_V + k], t) : t;
            }
        }
    }
}
DAN_VK_VNNI static inline void dan_vk_rn_b32_vnniz(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *S) {
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V];
    DAN_VK_RNZ_STEPS(0, 8)
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++)
        for (int k = 0; k < DAN_VK_RN_V; k++) S[r * DAN_VK_RN_V + k] = acc[r][k];
}
DAN_VK_RNK4_X86(dan_vk_rn_q4k_vnniz, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 1, dan_vk_rn_k4_vnniz)
DAN_VK_RNK4_X86(dan_vk_rn_q6k_vnniz_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 0, dan_vk_rn_q6k_vnniz)
DAN_VK_RNB_X86(dan_vk_rn_b32_vnniz_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, DAN_VK_RN_V, dan_vk_rn_b32_vnniz)

/* -- AVX2: 16-bit sums of G steps, then multiplied by MUL16(r) -- */
#define DAN_VK_RNA_STEPS(OFF, N, G, MUL16) \
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) for (int k = 0; k < DAN_VK_RN_V; k++) acc[r][k] = _mm256_setzero_si256(); \
    for (int i0 = 0; i0 < (N); i0 += (G)) { \
        __m256i a16[DAN_VK_RN_R_AVX2][DAN_VK_RN_V]; \
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) for (int k = 0; k < DAN_VK_RN_V; k++) a16[r][k] = _mm256_setzero_si256(); \
        DAN_VK_ROLLED \
        for (int i = i0; i < i0 + (G); i++) { \
            __m256i wv[DAN_VK_RN_R_AVX2]; \
            for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) wv[r] = DAN_VK_LD(wb[r] + (OFF) + 32 * i); \
            for (int k = 0; k < DAN_VK_RN_V; k++) { \
                __m256i yv = DAN_VK_LD(xb[k] + (OFF) + 32 * i); \
                for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) \
                    a16[r][k] = _mm256_add_epi16(a16[r][k], _mm256_maddubs_epi16(wv[r], yv)); \
            } \
        } \
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) \
            for (int k = 0; k < DAN_VK_RN_V; k++) acc[r][k] = _mm256_add_epi32(acc[r][k], _mm256_madd_epi16(a16[r][k], (MUL16))); \
    }
/* 32-bit scales as pairs of 16 bits (the scales of Q6_K are negative too) */
DAN_VK_AVX2 static inline __m256i dan_vk_sc16_avx2(const uint8_t *p) {
    __m256i s = DAN_VK_LD(p);
    return _mm256_blend_epi16(s, _mm256_slli_epi32(s, 16), 0xAA);
}
#define DAN_VK_RNA_K(SUF, G) \
DAN_VK_AVX2 static inline void dan_vk_rn_##SUF##_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) { \
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V], s16[DAN_VK_RN_R_AVX2]; \
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) s16[r] = dan_vk_sc16_avx2(wb[r] + DAN_VK_RN_SC); \
    DAN_VK_RNA_STEPS(0, 8, G, s16[r]) \
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) { \
        __m256i m = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32); \
        for (int k = 0; k < DAN_VK_RN_V; k++) { \
            T[r * DAN_VK_RN_V + k] = acc[r][k]; \
            M[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(m, DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32)); \
        } \
    } \
}
DAN_VK_RNA_K(q4k, 8)
DAN_VK_RNA_K(q5k, 4)
DAN_VK_AVX2 static inline void dan_vk_rn_q6k_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V], s16[DAN_VK_RN_R_AVX2];
    (void)M;
    for (int n = 0; n < 2; n++) {
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) s16[r] = dan_vk_sc16_avx2(wb[r] + DAN_VK_RN_SC + 32 * n);
        DAN_VK_RNA_STEPS(128 * n, 4, 2, s16[r])
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) {
            __m256i sc = DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32 * n);
            for (int k = 0; k < DAN_VK_RN_V; k++) {
                __m256i t = _mm256_sub_epi32(acc[r][k], _mm256_mullo_epi32(DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32 * n), sc));
                T[r * DAN_VK_RN_V + k] = n ? _mm256_add_epi32(T[r * DAN_VK_RN_V + k], t) : t;
            }
        }
    }
}
#define DAN_VK_RNA_B(SUF, G) \
DAN_VK_AVX2 static inline void dan_vk_rn_##SUF##_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *S) { \
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V]; \
    const __m256i one = _mm256_set1_epi16(1); \
    DAN_VK_RNA_STEPS(0, 8, G, one) \
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) \
        for (int k = 0; k < DAN_VK_RN_V; k++) S[r * DAN_VK_RN_V + k] = acc[r][k]; \
}
DAN_VK_RNA_B(q40, 8)
DAN_VK_RNA_B(q50, 4)
DAN_VK_RNK_X86(dan_vk_rn_q4k_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, 1, dan_vk_rn_q4k_avx2)
DAN_VK_RNK_X86(dan_vk_rn_q5k_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, 1, dan_vk_rn_q5k_avx2)
DAN_VK_RNK_X86(dan_vk_rn_q6k_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, 0, dan_vk_rn_q6k_avx2)
DAN_VK_RNB_X86(dan_vk_rn_q40_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, dan_vk_rn_q40_avx2)
DAN_VK_RNB_X86(dan_vk_rn_q50_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, dan_vk_rn_q50_avx2)

/* unpacking of rows: the numbers of 8 parts (a[j] - part j, dword i - its
   numbers 4i..4i+3) transposed: step i, lane j */
DAN_VK_AVX2 static inline void dan_vk_tr8_avx2(const __m256i *a, uint8_t *o) {
    __m256i t[8], u[8];
    for (int j = 0; j < 8; j += 2) { t[j] = _mm256_unpacklo_epi32(a[j], a[j + 1]); t[j + 1] = _mm256_unpackhi_epi32(a[j], a[j + 1]); }
    for (int h = 0; h < 8; h += 4) {
        u[h] = _mm256_unpacklo_epi64(t[h], t[h + 2]);
        u[h + 1] = _mm256_unpackhi_epi64(t[h], t[h + 2]);
        u[h + 2] = _mm256_unpacklo_epi64(t[h + 1], t[h + 3]);
        u[h + 3] = _mm256_unpackhi_epi64(t[h + 1], t[h + 3]);
    }
    for (int i = 0; i < 4; i++) {
        _mm256_storeu_si256((__m256i *)(void *)(o + 32 * i), _mm256_permute2x128_si256(u[i], u[i + 4], 0x20));
        _mm256_storeu_si256((__m256i *)(void *)(o + 32 * (i + 4)), _mm256_permute2x128_si256(u[i], u[i + 4], 0x31));
    }
}
/* 8 bytes picked by idx (the order of the lanes) as 32-bit numbers */
DAN_VK_AVX2 static inline void dan_vk_rn_put8_avx2(uint8_t *o, __m128i b, __m128i idx, int sgn) {
    __m128i v = _mm_shuffle_epi8(b, idx);
    _mm256_storeu_si256((__m256i *)(void *)o, sgn ? _mm256_cvtepi8_epi32(v) : _mm256_cvtepu8_epi32(v));
}
/* 16 масштабов Q3_K минус 32 (то же, что dan_vk_q3k_scales) в регистре: слова a0 a1 a0 a1 сдвигаются
   на 0 0 4 4 (младшие 4 бита), слово a2 - на 0 2 4 6 (старшие 2); без записи байтов в память и
   чтения их регистром (запись по частям не передаётся чтению целиком и задерживает его) */
DAN_VK_AVX2 static inline __m128i dan_vk_q3k_sc_avx2(const uint8_t *q) {
    uint32_t a2;
    memcpy(&a2, q + 8, 4);
    __m128i v = _mm_shuffle_epi32(_mm_loadl_epi64((const __m128i *)(const void *)q), 0x44);
    __m128i lo = _mm_and_si128(_mm_srlv_epi32(v, _mm_setr_epi32(0, 0, 4, 4)), _mm_set1_epi8(0x0f));
    __m128i hi = _mm_and_si128(_mm_srlv_epi32(_mm_set1_epi32((int)a2), _mm_setr_epi32(0, 2, 4, 6)), _mm_set1_epi8(3));
    return _mm_sub_epi8(_mm_or_si128(lo, _mm_slli_epi32(hi, 4)), _mm_set1_epi8(32));
}
/* kvalues_iq4nl + 128 - числа IQ4_XS без знака */
static const uint8_t dan_vk_iq4nl_u[16] = {1, 24, 45, 63, 79, 93, 106, 118, 129, 141, 153, 166, 181, 197, 217, 241};
/* Единицы строки Q2_K, Q3_K, IQ4_XS без знака (то же, что dan_vk_rn_lowbit_c в режиме 0): в половине n
   q[j] - числа 128n + 32j + l (l < 32), их 32-битные слова переставляются, как у Q6_K. */
DAN_VK_AVX2F16 static void dan_vk_rn_unpack_lowbit_x86(int fmt, const uint8_t *w, int64_t blocks, uint8_t *out) {
    const int wb = dan_vk_fmt_bytes[fmt];
    const __m256i m3 = _mm256_set1_epi8(3), one = _mm256_set1_epi8(1), m4 = _mm256_set1_epi8(0xF);
    const __m256i kv = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(const void *)dan_vk_iq4nl_u));
    const __m128i order = _mm_setr_epi8(0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15);
    for (int64_t b = 0; b < blocks; b++) {
        const uint8_t *p = w + b * wb;
        uint8_t *o = out + b * DAN_VK_RN_UNIT;
        __m256i h = fmt == DAN_FMT_Q3K ? DAN_VK_LD(p) : _mm256_setzero_si256();
        for (int n = 0; n < 2; n++) {
            __m256i q[4];
            if (fmt == DAN_FMT_IQ4XS) {
                for (int j = 0; j < 4; j++) {
                    __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + 8 + 16 * (4 * n + j)));
                    __m256i nb = _mm256_inserti128_si256(_mm256_castsi128_si256(v), _mm_srli_epi16(v, 4), 1);
                    q[j] = _mm256_shuffle_epi8(kv, _mm256_and_si256(nb, m4));
                }
            } else {
                __m256i v = DAN_VK_LD(p + (fmt == DAN_FMT_Q2K ? 16 : 32) + 32 * n);
                for (int j = 0; j < 4; j++) {
                    q[j] = _mm256_and_si256(v, m3);
                    v = _mm256_srli_epi16(v, 2);
                    if (fmt == DAN_FMT_Q3K) {
                        /* старший бит 4n + j байта hmask - как 4; h сдвигается на 1 каждый шаг */
                        q[j] = _mm256_or_si256(q[j], _mm256_slli_epi16(_mm256_and_si256(h, one), 2));
                        h = _mm256_srli_epi16(h, 1);
                    }
                }
            }
            __m256i t0 = _mm256_unpacklo_epi32(q[0], q[1]), t1 = _mm256_unpackhi_epi32(q[0], q[1]);
            __m256i t2 = _mm256_unpacklo_epi32(q[2], q[3]), t3 = _mm256_unpackhi_epi32(q[2], q[3]);
            uint8_t *on = o + 128 * n;
            _mm256_storeu_si256((__m256i *)(void *)on, _mm256_unpacklo_epi64(t0, t2));
            _mm256_storeu_si256((__m256i *)(void *)(on + 32), _mm256_unpackhi_epi64(t0, t2));
            _mm256_storeu_si256((__m256i *)(void *)(on + 64), _mm256_unpacklo_epi64(t1, t3));
            _mm256_storeu_si256((__m256i *)(void *)(on + 96), _mm256_unpackhi_epi64(t1, t3));
        }
        memset(o + DAN_VK_RN_D, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D);
        if (fmt == DAN_FMT_Q2K) {
            __m128i s = _mm_loadu_si128((const __m128i *)(const void *)p);
            __m128i sc = _mm_and_si128(s, _mm256_castsi256_si128(m4));
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC, sc, order, 0);
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC + 32, _mm_srli_si128(sc, 8), order, 0);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + 80));
            dan_vk_putf(o + DAN_VK_RN_D + 4, dan_vk_half(p + 82));
            __m128i mn = _mm_shuffle_epi8(_mm_and_si128(_mm_srli_epi16(s, 4), _mm256_castsi256_si128(m4)), order);
            _mm_storeu_si128((__m128i *)(void *)(o + DAN_VK_RN_D + 8), mn);
        } else {
            __m128i s;
            if (fmt == DAN_FMT_Q3K) s = dan_vk_q3k_sc_avx2(p + 96);
            else {
                int8_t s8[8], sc[16];
                dan_vk_iq4xs_scales(p, s8);
                for (int j = 0; j < 16; j++) sc[j] = s8[j / 2];
                s = _mm_loadu_si128((const __m128i *)(const void *)sc);
            }
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC, s, order, 1);
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC + 32, _mm_srli_si128(s, 8), order, 1);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + (fmt == DAN_FMT_Q3K ? 108 : 0)));
        }
    }
}
DAN_VK_AVX2F16 static void dan_vk_rn_unpack_x86(int fmt, const uint8_t *w, int64_t blocks, uint8_t *out, int sgn) {
    /* sgn - the mode: only 0 here, the rest by plain C */
    const int wb = dan_vk_fmt_bytes[fmt];
    const __m256i m4 = _mm256_set1_epi8(0xF), one = _mm256_set1_epi8(1), m3 = _mm256_set1_epi8(3);
    const __m128i order = _mm_setr_epi8(0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15);
    if (!sgn && fmt >= DAN_FMT_Q2K) { dan_vk_rn_unpack_lowbit_x86(fmt, w, blocks, out); return; }
    if (sgn || fmt >= DAN_FMT_Q2K) { dan_vk_rn_unpack_c(fmt, w, blocks, out, sgn); return; }
    if (fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K) {
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = w + b * wb;
            uint8_t *o = out + b * DAN_VK_RN_UNIT;
            __m256i a[8];
            __m256i hb = fmt == DAN_FMT_Q5K ? DAN_VK_LD(p + 16) : _mm256_setzero_si256();
            const uint8_t *q = p + (fmt == DAN_FMT_Q5K ? 48 : 16);
            for (int c = 0; c < 4; c++) {
                __m256i v = DAN_VK_LD(q + 32 * c);
                __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(hb, one), 4);
                __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hb, 1), one), 4);
                hb = _mm256_srli_epi16(hb, 2);
                a[c] = _mm256_or_si256(_mm256_and_si256(v, m4), h0);
                a[c + 4] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4), h1);
            }
            dan_vk_tr8_avx2(a, o);
            __m128i s = dan_vk_k4_sm128_avx2(p + 4);
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC, s, order, 0);
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC + 32, _mm_srli_si128(s, 8), order, 0);
            memset(o + DAN_VK_RN_D, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p));
            dan_vk_putf(o + DAN_VK_RN_D + 4, dan_vk_half(p + 2));
        }
        return;
    }
    if (fmt == DAN_FMT_Q6K) {
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = w + b * wb;
            uint8_t *o = out + b * DAN_VK_RN_UNIT;
            for (int n = 0; n < 2; n++) {
                const uint8_t *ql = p + 64 * n, *qh = p + 128 + 32 * n;
                __m256i l0 = DAN_VK_LD(ql), l1 = DAN_VK_LD(ql + 32), h = DAN_VK_LD(qh);
                __m256i q1 = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m3), 4));
                __m256i q2 = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4));
                __m256i q3 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                                             _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4));
                __m256i q4 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                                             _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4));
                __m256i t0 = _mm256_unpacklo_epi32(q1, q2), t1 = _mm256_unpackhi_epi32(q1, q2);
                __m256i t2 = _mm256_unpacklo_epi32(q3, q4), t3 = _mm256_unpackhi_epi32(q3, q4);
                uint8_t *on = o + 128 * n;
                _mm256_storeu_si256((__m256i *)(void *)on, _mm256_unpacklo_epi64(t0, t2));
                _mm256_storeu_si256((__m256i *)(void *)(on + 32), _mm256_unpackhi_epi64(t0, t2));
                _mm256_storeu_si256((__m256i *)(void *)(on + 64), _mm256_unpacklo_epi64(t1, t3));
                _mm256_storeu_si256((__m256i *)(void *)(on + 96), _mm256_unpackhi_epi64(t1, t3));
            }
            __m128i s = _mm_loadu_si128((const __m128i *)(const void *)(p + 192));
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC, s, order, 1);
            dan_vk_rn_put8_avx2(o + DAN_VK_RN_SC + 32, _mm_srli_si128(s, 8), order, 1);
            memset(o + DAN_VK_RN_D, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + 208));
        }
        return;
    }
    int64_t g = 0;
    for (; 8 * g + 8 <= blocks; g++) {
        const uint8_t *p = w + 8 * g * wb;
        uint8_t *o = out + g * DAN_VK_RN_UNIT;
        __m256i a[8];
        for (int j = 0; j < 8; j++) {
            const uint8_t *pj = p + j * wb;
            a[j] = fmt == DAN_FMT_Q40 ? dan_vk_nib32_avx2(pj + 2)
                 : fmt == DAN_FMT_Q50 ? _mm256_or_si256(dan_vk_nib32_avx2(pj + 6), dan_vk_bits32_avx2(pj + 2))
                 : _mm256_xor_si256(DAN_VK_LD(pj + 2), _mm256_set1_epi8((char)0x80));
        }
        dan_vk_tr8_avx2(a, o);
        _mm256_storeu_ps((float *)(void *)(o + DAN_VK_RN_D), dan_vk_halves8_avx2(p, wb));
        memset(o + DAN_VK_RN_D + 32, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D - 32);
        memset(o + DAN_VK_RN_SC, 0, DAN_VK_RN_D - DAN_VK_RN_SC);
    }
    if (8 * g < blocks) dan_vk_rn_unpack_c(fmt, w + 8 * g * wb, blocks - 8 * g, out + g * DAN_VK_RN_UNIT, sgn);
}
/* ---- AVX2: Q2_K, Q3_K, IQ4_XS ---- */

/* целая часть блока Q2_K: 8 частичных сумм isum, *mv - 8 частичных сумм imin */
DAN_VK_AVX2 static inline __m256i dan_vk_q2k_parts2_avx2(const uint8_t *w, const dan_q8k *y, __m256i *mv) {
    const __m128i m4 = _mm_set1_epi8(0xF);
    __m128i sb = _mm_loadu_si128((const __m128i *)(const void *)w);
    __m256i s16 = _mm256_cvtepu8_epi16(_mm_and_si128(sb, m4));
    __m256i m16 = _mm256_cvtepu8_epi16(_mm_and_si128(_mm_srli_epi16(sb, 4), m4));
    *mv = _mm256_madd_epi16(m16, _mm256_loadu_si256((const __m256i *)y->bsums));   /* меньше 16 * 15 * 2048 */
    const __m256i m3 = _mm256_set1_epi8(3);
    __m256i acc = _mm256_setzero_si256();
    for (int n = 0; n < 2; n++) {
        __m256i q = _mm256_loadu_si256((const __m256i *)(const void *)(w + 16 + 32 * n));
        __m256i s = n == 0 ? _mm256_broadcastsi128_si256(_mm256_castsi256_si128(s16))
                           : _mm256_broadcastsi128_si256(_mm256_extracti128_si256(s16, 1));
        const int8_t *v = y->qs + 128 * n;
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(q, m3), v, DAN_VK_BC16X2(s, 0, 1)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(_mm256_srli_epi16(q, 2), m3), v + 32, DAN_VK_BC16X2(s, 2, 3)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(_mm256_srli_epi16(q, 4), m3), v + 64, DAN_VK_BC16X2(s, 4, 5)));
        acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(_mm256_and_si256(_mm256_srli_epi16(q, 6), m3), v + 96, DAN_VK_BC16X2(s, 6, 7)));
    }
    return acc;
}

/* целая часть блока Q3_K: 8 частичных сумм isum, *extra = 0; числа q + 4 (0..7) без знака, сдвиг
   4 * сумма sc * bsums снимается здесь */
DAN_VK_AVX2 static inline __m256i dan_vk_q3k_parts_avx2(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    __m256i s16 = _mm256_cvtepi8_epi16(dan_vk_q3k_sc_avx2(w + 96));
    __m256i bias = _mm256_madd_epi16(s16, _mm256_loadu_si256((const __m256i *)y->bsums));
    __m256i acc = _mm256_sub_epi32(_mm256_setzero_si256(), _mm256_slli_epi32(bias, 2));
    const __m256i m3 = _mm256_set1_epi8(3), one = _mm256_set1_epi8(1);
    __m256i hm = _mm256_loadu_si256((const __m256i *)(const void *)w);
    for (int n = 0; n < 2; n++) {
        __m256i q = _mm256_loadu_si256((const __m256i *)(const void *)(w + 32 + 32 * n));
        __m256i s = n == 0 ? _mm256_broadcastsi128_si256(_mm256_castsi256_si128(s16))
                           : _mm256_broadcastsi128_si256(_mm256_extracti128_si256(s16, 1));
        const int8_t *v = y->qs + 128 * n;
        for (int j = 0; j < 4; j++) {
            /* hm сдвигается на 1 каждый шаг: бит 4n + j в младшем */
            __m256i u = _mm256_or_si256(_mm256_and_si256(q, m3), _mm256_slli_epi16(_mm256_and_si256(hm, one), 2));
            __m256i sj = j == 0 ? DAN_VK_BC16X2(s, 0, 1) : j == 1 ? DAN_VK_BC16X2(s, 2, 3)
                       : j == 2 ? DAN_VK_BC16X2(s, 4, 5) : DAN_VK_BC16X2(s, 6, 7);
            acc = _mm256_add_epi32(acc, dan_vk_mulsc_avx2(u, v + 32 * j, sj));
            q = _mm256_srli_epi16(q, 2);
            hm = _mm256_srli_epi16(hm, 1);
        }
    }
    *extra = 0;
    return acc;
}

/* IQ4_XS: числа kv со знаком, произведение пары - |kv| на y со знаком kv (не больше 2 * 127 * 127);
   суммы 8 частей (дорожка - часть) умножаются на масштабы сразу */
DAN_VK_AVX2 static inline __m256i dan_vk_iq4xs_parts_avx2(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    uint16_t h;
    uint32_t l;
    memcpy(&h, w + 2, 2);
    memcpy(&l, w + 4, 4);
    __m256i lo = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)l), _mm256_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28)),
                                  _mm256_set1_epi32(0xF));
    __m256i hi = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32(h), _mm256_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14)),
                                  _mm256_set1_epi32(3));
    __m256i sc = _mm256_sub_epi32(_mm256_or_si256(lo, _mm256_slli_epi32(hi, 4)), _mm256_set1_epi32(32));
    /* 8 масштабов 16-битными числами в обеих половинах: пары произведений части b сразу умножаются
       на масштаб b (vpmaddwd), без сложения частей по дорожкам в каждом блоке */
    __m128i s8 = _mm_packs_epi32(_mm256_castsi256_si128(sc), _mm256_extracti128_si256(sc, 1));
    __m256i s16 = _mm256_broadcastsi128_si256(s8);
    const __m256i kv = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(const void *)dan_vk_iq4nl));
    const __m128i m4 = _mm_set1_epi8(0xF);
    __m256i acc = _mm256_setzero_si256();
    for (int b = 0; b < 8; b++) {
        __m128i q = _mm_loadu_si128((const __m128i *)(const void *)(w + 8 + 16 * b));
        __m256i a = _mm256_shuffle_epi8(kv, _mm256_set_m128i(_mm_and_si128(_mm_srli_epi16(q, 4), m4), _mm_and_si128(q, m4)));
        __m256i yv = _mm256_loadu_si256((const __m256i *)(const void *)(y->qs + 32 * b));
        /* пара не больше 2 * 127 * 127, на масштаб до 32 по модулю - сумма блока помещается в 32 бита */
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_abs_epi8(a), _mm256_sign_epi8(yv, a)),
                                                      DAN_VK_BC16(s16, b)));
    }
    *extra = 0;
    return acc;
}

/* по 8 блоков, как DAN_VK_KDOT_AVX2: d по смещению DOFF, dmin (MINS) - по MOFF, хвост - PF */
#define DAN_VK_KDOT2_AVX2(NAME, PARTS, WB, DOFF, MINS, MOFF, PF) DAN_VK_KDOT2_X86(NAME, DAN_VK_AVX2F16, PARTS, WB, DOFF, MINS, MOFF, PF)
#define DAN_VK_KDOT2_X86(NAME, ATTR, PARTS, WB, DOFF, MINS, MOFF, PF) \
ATTR static float NAME(const uint8_t *w, const void *xv, int64_t blocks) { \
    const dan_q8k *x = (const dan_q8k *)xv; \
    __m256 acc = _mm256_setzero_ps(); \
    int64_t i = 0; \
    for (; i + 8 <= blocks; i += 8) { \
        const uint8_t *pw = w + i * (WB); \
        const dan_q8k *px = x + i; \
        __m256i v[8]; \
        int32_t ex[8]; \
        for (int l = 0; l < 8; l++) v[l] = PARTS(pw + l * (WB), px + l, &ex[l]); \
        __m256i isum = dan_vk_hadd8_avx2(v); \
        __m256 yd = _mm256_setr_ps(px[0].d, px[1].d, px[2].d, px[3].d, px[4].d, px[5].d, px[6].d, px[7].d); \
        __m256 t = _mm256_mul_ps(_mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + (DOFF), WB)), _mm256_cvtepi32_ps(isum)); \
        if (MINS) { \
            __m256 b = _mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + (MOFF), WB)); \
            __m256i m = _mm256_setr_epi32(ex[0], ex[1], ex[2], ex[3], ex[4], ex[5], ex[6], ex[7]); \
            t = _mm256_sub_ps(t, _mm256_mul_ps(b, _mm256_cvtepi32_ps(m))); \
        } \
        acc = _mm256_add_ps(acc, t); \
    } \
    float r = dan_vk_hsum8_lanes_avx2(acc); \
    (void)PF; \
    DAN_VK_KTAIL_AVX2(PARTS, WB, DOFF, MINS, MOFF, 0) \
    return r; \
}
/* строка Q2_K: наименьшие 8 блоков, как и isum, собираются сложением по дорожкам (dan_vk_hadd8_avx2),
   а не суммой каждого блока, записанной в память и прочитанной регистром */
DAN_VK_AVX2F16 static float dan_vk_q2k_dot_avx2(const uint8_t *w, const void *xv, int64_t blocks) {
    const dan_q8k *x = (const dan_q8k *)xv;
    __m256 acc = _mm256_setzero_ps();
    int64_t i = 0;
    for (; i + 8 <= blocks; i += 8) {
        const uint8_t *pw = w + i * DAN_Q2K_BYTES;
        const dan_q8k *px = x + i;
        __m256i v[8], mv[8];
        for (int l = 0; l < 8; l++) v[l] = dan_vk_q2k_parts2_avx2(pw + l * DAN_Q2K_BYTES, px + l, &mv[l]);
        __m256 yd = _mm256_setr_ps(px[0].d, px[1].d, px[2].d, px[3].d, px[4].d, px[5].d, px[6].d, px[7].d);
        __m256 t = _mm256_mul_ps(_mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + 80, DAN_Q2K_BYTES)), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(v)));
        __m256 b = _mm256_mul_ps(yd, dan_vk_halves8_avx2(pw + 82, DAN_Q2K_BYTES));
        t = _mm256_sub_ps(t, _mm256_mul_ps(b, _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(mv))));
        acc = _mm256_add_ps(acc, t);
    }
    float r = dan_vk_hsum8_lanes_avx2(acc);
    DAN_VK_KTAIL_AVX2(dan_vk_q2k_parts2_avx2, DAN_Q2K_BYTES, 80, 1, 82, 1)
    return r;
}
DAN_VK_KDOT2_AVX2(dan_vk_q3k_dot_avx2, dan_vk_q3k_parts_avx2, DAN_Q3K_BYTES, 108, 0, 0, dan_vk_q3k_p)
DAN_VK_KDOT2_AVX2(dan_vk_iq4xs_dot_avx2, dan_vk_iq4xs_parts_avx2, DAN_IQ4XS_BYTES, 0, 0, 0, dan_vk_iq4xs_p)

/* ---- строка IQ4_XS на AVX-512 VNNI (варианты avx512 с VNNI) ----
   Числа - kv + 128 без знака из таблицы (vpshufb на 512 битах, 64 числа - две части), на числа
   вектора одной vpdpbusd; суммы части - на её масштаб (vpmulld), сдвиг 128 * сумма частей вектора
   на масштабы снимается один раз на блок (vpmaddwd по суммам 16 чисел Q8_K). Целая сумма блока та
   же, что у ядра AVX2, - те же биты. */
DAN_VK_VNNI static inline __m256i dan_vk_iq4xs_parts_vnni(const uint8_t *w, const dan_q8k *y, int32_t *extra) {
    uint16_t h;
    uint32_t l;
    memcpy(&h, w + 2, 2);
    memcpy(&l, w + 4, 4);
    __m256i lo = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)l), _mm256_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28)),
                                  _mm256_set1_epi32(0xF));
    __m256i hi = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32(h), _mm256_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14)),
                                  _mm256_set1_epi32(3));
    __m256i sc = _mm256_sub_epi32(_mm256_or_si256(lo, _mm256_slli_epi32(hi, 4)), _mm256_set1_epi32(32));
    __m512i sc512 = _mm512_castsi256_si512(sc);
    const __m512i kv = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)(const void *)dan_vk_iq4nl_u));
    const __m256i m4 = _mm256_set1_epi8(0xF);
    __m512i T = _mm512_setzero_si512();
    for (int k = 0; k < 4; k++) {   /* части 2k и 2k + 1: 64 числа */
        __m256i q = _mm256_loadu_si256((const __m256i *)(const void *)(w + 8 + 32 * k));
        __m256i a = _mm256_and_si256(q, m4), b = _mm256_and_si256(_mm256_srli_epi16(q, 4), m4);
        __m512i ix = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_permute2x128_si256(a, b, 0x20)),
                                        _mm256_permute2x128_si256(a, b, 0x31), 1);
        __m512i u = _mm512_shuffle_epi8(kv, ix);
        __m512i d = _mm512_dpbusd_epi32(_mm512_setzero_si512(), u, _mm512_loadu_si512((const void *)(y->qs + 64 * k)));
        __m512i s = _mm512_permutexvar_epi32(_mm512_set_epi32(2 * k + 1, 2 * k + 1, 2 * k + 1, 2 * k + 1, 2 * k + 1, 2 * k + 1,
                                                              2 * k + 1, 2 * k + 1, 2 * k, 2 * k, 2 * k, 2 * k, 2 * k, 2 * k, 2 * k, 2 * k),
                                             sc512);
        T = _mm512_add_epi32(T, _mm512_mullo_epi32(d, s));
    }
    /* сдвиг: 128 * сумма по частям b масштаба b на суммы двух групп по 16 чисел вектора */
    __m256i sp = _mm256_or_si256(_mm256_and_si256(sc, _mm256_set1_epi32(0xFFFF)), _mm256_slli_epi32(sc, 16));
    __m256i bias = _mm256_madd_epi16(sp, _mm256_loadu_si256((const __m256i *)(const void *)y->bsums));
    *extra = 0;
    return _mm256_sub_epi32(_mm256_add_epi32(_mm512_castsi512_si256(T), _mm512_extracti64x4_epi64(T, 1)), _mm256_slli_epi32(bias, 7));
}
DAN_VK_KDOT2_X86(dan_vk_iq4xs_dot_vnni, DAN_VK_VNNI, dan_vk_iq4xs_parts_vnni, DAN_IQ4XS_BYTES, 0, 0, 0, dan_vk_iq4xs_p)


/* строки на векторы Q2_K (единицы - dan_vk_rn_lowbit_c): T - части с масштабами, M - наименьшие на
   суммы частей вектора.
   Суммы части Q2_K (16 чисел до 3 на числа вектора) и Q3_K без сдвига (до 4 по модулю) помещаются в
   16 бит: суммы двух половин сжимаются в один регистр (vpackssdw) и умножаются на масштабы, также
   сжатые, одной vpmaddwd вместо vpmulld на каждую половину; наименьшие Q2_K - vpmaddwd 16 байтов
   наименьших строки на 16 сумм частей вектора. Порядок 16-битных чисел после vpackssdw двух
   регистров по 8: половина 0 дорожки 0-3, половина 1 дорожки 0-3, затем то же для дорожек 4-7. */
#define DAN_VK_PK_ORDER _mm_setr_epi8(0, 1, 2, 3, 8, 9, 10, 11, 4, 5, 6, 7, 12, 13, 14, 15)
/* 16 наименьших строки (байты после d и dmin единицы) в порядке vpackssdw как 16-битные числа */
DAN_VK_AVX2 static inline __m256i dan_vk_q2k_mins16_avx2(const uint8_t *u) {
    return _mm256_cvtepu8_epi16(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(const void *)(u + DAN_VK_RN_D + 8)), DAN_VK_PK_ORDER));
}
/* 32-битные числа двух половин (масштабы строки или суммы вектора) как 16-битные в порядке vpackssdw */
DAN_VK_AVX2 static inline __m256i dan_vk_pk16_avx2(const uint8_t *p) {
    return _mm256_packs_epi32(DAN_VK_LD(p), DAN_VK_LD(p + 32));
}
/* IQ4_XS на VNNI: числа единиц kv + 128, суммы частей со снятым сдвигом не помещаются в 16 бит;
   но у дорожек L и L + 4 половины один масштаб (части 2b и 2b + 1 - одна часть IQ4_XS из 32
   чисел), поэтому они складываются, и на масштабы обеих половин - одна vpmulld вместо двух */
/* STEPS - шаги VNNI на 256 битах (DAN_VK_RNV_STEPS) или по два шага на 512 битах (DAN_VK_RNZ_STEPS, Zen 5) */
#define DAN_VK_RN_LOWBIT_VNNI(SUF, STEPS) \
DAN_VK_VNNI static inline void dan_vk_rn_q2k_##SUF(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) { \
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V], h0[DAN_VK_RN_R_VNNI][DAN_VK_RN_V]; \
    STEPS(0, 4) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) \
        for (int k = 0; k < DAN_VK_RN_V; k++) h0[r][k] = acc[r][k]; \
    STEPS(128, 4) \
    __m256i bs[DAN_VK_RN_V]; \
    for (int k = 0; k < DAN_VK_RN_V; k++) bs[k] = dan_vk_pk16_avx2(xb[k] + DAN_VK_RN_SC); \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) { \
        __m256i sc = dan_vk_pk16_avx2(wb[r] + DAN_VK_RN_SC), mn = dan_vk_q2k_mins16_avx2(wb[r]); \
        for (int k = 0; k < DAN_VK_RN_V; k++) { \
            T[r * DAN_VK_RN_V + k] = _mm256_madd_epi16(_mm256_packs_epi32(h0[r][k], acc[r][k]), sc); \
            M[r * DAN_VK_RN_V + k] = _mm256_madd_epi16(mn, bs[k]); \
        } \
    } \
} \
DAN_VK_VNNI static inline void dan_vk_rn_q3k_##SUF(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) { \
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V], h0[DAN_VK_RN_R_VNNI][DAN_VK_RN_V]; \
    (void)M; \
    STEPS(0, 4) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) \
        for (int k = 0; k < DAN_VK_RN_V; k++) h0[r][k] = _mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC)); \
    STEPS(128, 4) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) { \
        __m256i sc = dan_vk_pk16_avx2(wb[r] + DAN_VK_RN_SC); \
        for (int k = 0; k < DAN_VK_RN_V; k++) { \
            __m256i h1 = _mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32)); \
            T[r * DAN_VK_RN_V + k] = _mm256_madd_epi16(_mm256_packs_epi32(h0[r][k], h1), sc); \
        } \
    } \
} \
DAN_VK_VNNI static inline void dan_vk_rn_iq4xs_##SUF(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) { \
    __m256i acc[DAN_VK_RN_R_VNNI][DAN_VK_RN_V], h0[DAN_VK_RN_R_VNNI][DAN_VK_RN_V]; \
    (void)M; \
    STEPS(0, 4) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) \
        for (int k = 0; k < DAN_VK_RN_V; k++) h0[r][k] = _mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC)); \
    STEPS(128, 4) \
    for (int r = 0; r < DAN_VK_RN_R_VNNI; r++) { \
        __m256i sc = _mm256_permute2x128_si256(DAN_VK_LD(wb[r] + DAN_VK_RN_SC), DAN_VK_LD(wb[r] + DAN_VK_RN_SC + 32), 0x20); \
        for (int k = 0; k < DAN_VK_RN_V; k++) { \
            __m256i h1 = _mm256_sub_epi32(acc[r][k], DAN_VK_LD(xb[k] + DAN_VK_RN_SC + 32)); \
            __m256i z = _mm256_add_epi32(_mm256_permute2x128_si256(h0[r][k], h1, 0x20), _mm256_permute2x128_si256(h0[r][k], h1, 0x31)); \
            T[r * DAN_VK_RN_V + k] = _mm256_mullo_epi32(z, sc); \
        } \
    } \
}
DAN_VK_RN_LOWBIT_VNNI(vnni, DAN_VK_RNV_STEPS)
DAN_VK_RN_LOWBIT_VNNI(vnniz, DAN_VK_RNZ_STEPS)
DAN_VK_AVX2 static inline void dan_vk_rn_q2k_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V], s16[DAN_VK_RN_R_AVX2];
    for (int n = 0; n < 2; n++) {
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) s16[r] = dan_vk_sc16_avx2(wb[r] + DAN_VK_RN_SC + 32 * n);
        DAN_VK_RNA_STEPS(128 * n, 4, 4, s16[r])   /* числа до 3: 16-битные суммы 4 шагов не больше 4 * 2 * 3 * 128 */
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
            for (int k = 0; k < DAN_VK_RN_V; k++)
                T[r * DAN_VK_RN_V + k] = n ? _mm256_add_epi32(T[r * DAN_VK_RN_V + k], acc[r][k]) : acc[r][k];
    }
    __m256i bs[DAN_VK_RN_V];
    for (int k = 0; k < DAN_VK_RN_V; k++) bs[k] = dan_vk_pk16_avx2(xb[k] + DAN_VK_RN_SC);
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) {
        __m256i mn = dan_vk_q2k_mins16_avx2(wb[r]);
        for (int k = 0; k < DAN_VK_RN_V; k++) M[r * DAN_VK_RN_V + k] = _mm256_madd_epi16(mn, bs[k]);
    }
}
DAN_VK_RNK4_X86(dan_vk_rn_q2k_vnni_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 1, dan_vk_rn_q2k_vnni)
DAN_VK_RNK4_X86(dan_vk_rn_q2k_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, 1, dan_vk_rn_q2k_avx2)
DAN_VK_RNK4_X86(dan_vk_rn_q3k_vnni_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 0, dan_vk_rn_q3k_vnni)
DAN_VK_RNK4_X86(dan_vk_rn_iq4xs_vnni_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 0, dan_vk_rn_iq4xs_vnni)
DAN_VK_RNK4_X86(dan_vk_rn_q2k_vnniz_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 1, dan_vk_rn_q2k_vnniz)
DAN_VK_RNK4_X86(dan_vk_rn_q3k_vnniz_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 0, dan_vk_rn_q3k_vnniz)
DAN_VK_RNK4_X86(dan_vk_rn_iq4xs_vnniz_f, DAN_VK_VNNI, DAN_VK_RN_R_VNNI, 0, dan_vk_rn_iq4xs_vnniz)

/* Q3_K на AVX2: числа q + 4 не больше 7, 16-битные суммы всех 4 шагов половины не больше
   4 * 2 * 7 * 128 - одно умножение на масштабы на половину (у ядра Q6_K - на 2 шага) */
DAN_VK_AVX2 static inline void dan_vk_rn_q3k_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V], s16[DAN_VK_RN_R_AVX2];
    (void)M;
    for (int n = 0; n < 2; n++) {
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) s16[r] = dan_vk_sc16_avx2(wb[r] + DAN_VK_RN_SC + 32 * n);
        DAN_VK_RNA_STEPS(128 * n, 4, 4, s16[r])
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
            for (int k = 0; k < DAN_VK_RN_V; k++)
                T[r * DAN_VK_RN_V + k] = n ? _mm256_add_epi32(T[r * DAN_VK_RN_V + k], acc[r][k]) : acc[r][k];
    }
    /* сдвиг 4 * сумма частей вектора на масштабы: суммы и масштабы в 16 битах, одна vpmaddwd */
    __m256i xs[DAN_VK_RN_V];
    for (int k = 0; k < DAN_VK_RN_V; k++) xs[k] = dan_vk_pk16_avx2(xb[k] + DAN_VK_RN_SC);
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) {
        __m256i sc = dan_vk_pk16_avx2(wb[r] + DAN_VK_RN_SC);
        for (int k = 0; k < DAN_VK_RN_V; k++)
            T[r * DAN_VK_RN_V + k] = _mm256_sub_epi32(T[r * DAN_VK_RN_V + k], _mm256_madd_epi16(sc, xs[k]));
    }
}
DAN_VK_RNK4_X86(dan_vk_rn_q3k_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, 0, dan_vk_rn_q3k_avx2)

/* IQ4_XS на AVX2: числа единиц kv + 128 (до 241) не годятся для vpmaddubsw (сумма пары
   переполнила бы 16 бит), поэтому число берётся со знаком (kv = u ^ 0x80): |kv| на y со знаком
   kv, пара не больше 2 * 127 * 127, и сразу умножается на масштабы части; суммы вектора со
   сдвигом 128 не нужны */
DAN_VK_AVX2 static inline void dan_vk_rn_iq4xs_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *T, __m256i *M) {
    (void)M;
    const __m256i x80 = _mm256_set1_epi8((char)0x80);
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
        for (int k = 0; k < DAN_VK_RN_V; k++) T[r * DAN_VK_RN_V + k] = _mm256_setzero_si256();
    for (int n = 0; n < 2; n++) {
        __m256i s16[DAN_VK_RN_R_AVX2];
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) s16[r] = dan_vk_sc16_avx2(wb[r] + DAN_VK_RN_SC + 32 * n);
        DAN_VK_ROLLED
        for (int i = 0; i < 4; i++) {
            __m256i sv[DAN_VK_RN_R_AVX2], av[DAN_VK_RN_R_AVX2];
            for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) {
                sv[r] = _mm256_xor_si256(DAN_VK_LD(wb[r] + 128 * n + 32 * i), x80);
                av[r] = _mm256_abs_epi8(sv[r]);
            }
            for (int k = 0; k < DAN_VK_RN_V; k++) {
                __m256i yv = DAN_VK_LD(xb[k] + 128 * n + 32 * i);
                for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
                    T[r * DAN_VK_RN_V + k] = _mm256_add_epi32(T[r * DAN_VK_RN_V + k],
                        _mm256_madd_epi16(_mm256_maddubs_epi16(av[r], _mm256_sign_epi8(yv, sv[r])), s16[r]));
            }
        }
    }
}
DAN_VK_RNK4_X86(dan_vk_rn_iq4xs_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, 0, dan_vk_rn_iq4xs_avx2)

/* Q8_0 на AVX2: числа единиц q + 128 переполнили бы 16-битные суммы пары, поэтому, как у
   IQ4_XS, |q| (у -128 - 128 без знака) на y со знаком q, пара не больше 2 * 128 * 127, и
   сразу в 32 бита; DAN_VK_RNB_X86 вычитает из суммы сдвиг 128 * сумма блока вектора, поэтому
   он прибавляется здесь (S = сумма (q + 128) y, та же, что у VNNI) */
DAN_VK_AVX2 static inline void dan_vk_rn_q80_avx2(const uint8_t *const *wb, const uint8_t *const *xb, __m256i *S) {
    const __m256i x80 = _mm256_set1_epi8((char)0x80), one = _mm256_set1_epi16(1);
    __m256i acc[DAN_VK_RN_R_AVX2][DAN_VK_RN_V];
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
        for (int k = 0; k < DAN_VK_RN_V; k++) acc[r][k] = DAN_VK_LD(xb[k] + DAN_VK_RN_SC);
    DAN_VK_ROLLED
    for (int i = 0; i < 8; i++) {
        __m256i sv[DAN_VK_RN_R_AVX2], av[DAN_VK_RN_R_AVX2];
        for (int r = 0; r < DAN_VK_RN_R_AVX2; r++) {
            sv[r] = _mm256_xor_si256(DAN_VK_LD(wb[r] + 32 * i), x80);
            av[r] = _mm256_abs_epi8(sv[r]);
        }
        for (int k = 0; k < DAN_VK_RN_V; k++) {
            __m256i yv = DAN_VK_LD(xb[k] + 32 * i);
            for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
                acc[r][k] = _mm256_add_epi32(acc[r][k], _mm256_madd_epi16(_mm256_maddubs_epi16(av[r], _mm256_sign_epi8(yv, sv[r])), one));
        }
    }
    for (int r = 0; r < DAN_VK_RN_R_AVX2; r++)
        for (int k = 0; k < DAN_VK_RN_V; k++) S[r * DAN_VK_RN_V + k] = acc[r][k];
}
DAN_VK_RNB_X86(dan_vk_rn_q80_avx2f, DAN_VK_AVX2F16, DAN_VK_RN_R_AVX2, DAN_VK_RN_V, dan_vk_rn_q80_avx2)

#define DAN_VK_RN_AVX2_TAIL , DAN_VK_RN_R_AVX2, DAN_VK_RN_V, 0, dan_vk_rn_unpack_x86, \
    {dan_vk_rn_q40_avx2f, dan_vk_rn_q4k_avx2f, dan_vk_rn_q5k_avx2f, dan_vk_rn_q6k_avx2f, dan_vk_rn_q50_avx2f, NULL, NULL, dan_vk_rn_q80_avx2f, \
     dan_vk_rn_q2k_avx2f, dan_vk_rn_q3k_avx2f, dan_vk_rn_iq4xs_avx2f}
#define DAN_VK_RN_VNNI_TAIL , DAN_VK_RN_R_VNNI, DAN_VK_RN_V, 0, dan_vk_rn_unpack_x86, \
    {dan_vk_rn_b32_vnni_f, dan_vk_rn_q4k_vnni, dan_vk_rn_q4k_vnni, dan_vk_rn_q6k_vnni_f, dan_vk_rn_b32_vnni_f, NULL, NULL, dan_vk_rn_b32_vnni_f, \
     dan_vk_rn_q2k_vnni_f, dan_vk_rn_q3k_vnni_f, dan_vk_rn_iq4xs_vnni_f}
#define DAN_VK_RN_VNNIZ_TAIL , DAN_VK_RN_R_VNNI, DAN_VK_RN_V, 0, dan_vk_rn_unpack_x86, \
    {dan_vk_rn_b32_vnniz_f, dan_vk_rn_q4k_vnniz, dan_vk_rn_q4k_vnniz, dan_vk_rn_q6k_vnniz_f, dan_vk_rn_b32_vnniz_f, NULL, NULL, dan_vk_rn_b32_vnniz_f, \
     dan_vk_rn_q2k_vnniz_f, dan_vk_rn_q3k_vnniz_f, dan_vk_rn_iq4xs_vnniz_f}

/* ---- AMX: 16 rows by 16 vectors by tiles (Intel Xeon of 2023 and later, Linux) ----

   The K formats (Q4_K, Q5_K, Q6_K). A number of a row times the scale of its
   part, s = sc * q (Q6_K: sc * (q - 32)), fits 16 bits; it is split into a
   low unsigned byte and a high byte, s = lo + 256 hi, and the integer sum of
   a block for 16 rows by 16 vectors is C_lo + 256 C_hi, where C_lo and C_hi
   are made by tdpbusd (tdpbssd for the signed high bytes of Q6_K) over the
   256 numbers of the block in 4 steps of 64. The sum of the mins (Q4_K and
   Q5_K) is made by vpdpwssd: the mins of the parts of a row by the sums of
   the parts of 16 vectors. Both integer sums are those of the row kernel
   (exact), and the numbers of each block are made by the same float
   operations in the same order (lanes: «blocks of llama.cpp» above), so the
   bits are those of plain C.

   16 rows unpacked, a block (DAN_VK_AMX_AU bytes): the low bytes - 4 tiles of
   16 rows by 64 numbers (step c: the numbers 64c..64c+63 of the block), the
   high bytes - 4 tiles the same; 16 floats d, 16 floats dmin; the mins of the
   parts of 16 rows as 4 pairs of 16 bits (mins 2j and 2j + 1).
   16 vectors prepared, a block (DAN_VK_AMX_XU bytes): 4 tiles of 16 rows by 16
   vectors of 4 numbers (row i of step c: the numbers 64c + 4i..64c + 4i + 3 of
   each vector), 16 floats d, the sums of the parts of 32 as 4 rows of 16
   pairs of 16 bits.

   Так же Q2_K, Q3_K, IQ4_XS: s = sc * q (Q2_K - не больше 45, только младшие
   байты, старшие тайлы не считаются), sc * q у Q3_K и sc * kv у IQ4_XS - со
   знаком (старшие байты - tdpbssd, как у Q6_K); у Q2_K 16 наименьших частей
   по 16 чисел - 8 пар на строку, у векторов - суммы частей по 16 (8 строк пар).

   AMX needs the permission of the system: in Linux it is asked once
   (arch_prctl ARCH_REQ_XCOMP_PERM); without it (or in Windows, where it is
   not done) the variant is avx512 with VNNI. */
#if defined(__linux__) && ((defined(__clang__) && __clang_major__ >= 12) || (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 11))
#define DAN_VK_AMX_ON 1
#include <cpuid.h>
#include <sys/syscall.h>
long syscall(long, ...);
#define DAN_VK_AMX __attribute__((target("avx2,f16c,fma,avx512f,avx512bw,avx512vl,avx512vnni,amx-tile,amx-int8")))
#define DAN_VK_AMX_AU (8192 + 64 + 64 + 512)
#define DAN_VK_AMX_XU (4096 + 64 + 512)

typedef struct { uint8_t palette, start, res[14]; uint16_t colsb[16]; uint8_t rows[16]; } dan_vk_tilecfg;

/* 1 if the processor has AMX for bytes and the system lets this program use it */
static int dan_vk_amx_ok(void) {
    static _Atomic int ok = -1;
    int r = atomic_load(&ok);
    if (r >= 0) return r;
    unsigned a, b, c, d;
    r = 0;
    if (__get_cpuid_count(1, 0, &a, &b, &c, &d) && (c & (1u << 27))   /* OSXSAVE */
        && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (d & (1u << 24)) && (d & (1u << 25))) {   /* AMX-TILE, AMX-INT8 */
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        (void)hi;
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif
        /* the system keeps the tiles (XCR0 bits 17 and 18); ARCH_REQ_XCOMP_PERM for XTILEDATA */
        if ((lo & (3u << 17)) == (3u << 17) && syscall(SYS_arch_prctl, 0x1023L, 18L) == 0) r = 1;
    }
    atomic_store(&ok, r);
    return r;
}

DAN_VK_AMX static void dan_vk_amx_begin(void) {
    _Alignas(64) dan_vk_tilecfg c;
    memset(&c, 0, sizeof c);
    c.palette = 1;
    for (int i = 0; i < 8; i++) { c.colsb[i] = 64; c.rows[i] = 16; }
    _tile_loadconfig(&c);
}
DAN_VK_AMX static void dan_vk_amx_end(void) { _tile_release(); }

/* the 16-bit numbers s of 32 numbers as low and high bytes */
DAN_VK_AMX static inline void dan_vk_amx_put(uint8_t *lo, uint8_t *hi, __m512i s) {
    _mm256_storeu_si256((__m256i *)(void *)lo, _mm512_cvtepi16_epi8(s));
    _mm256_storeu_si256((__m256i *)(void *)hi, _mm512_cvtepi16_epi8(_mm512_srai_epi16(s, 8)));
}

/* Блок Q2_K, Q3_K или IQ4_XS строки m: числа, умноженные на масштаб своей части, s (Q2_K - sc q до 45,
   только младшие байты; Q3_K - sc q, IQ4_XS - sc kv, со знаком), в младшие и старшие байты, d, у Q2_K -
   dmin и 16 наименьших парами (части 2j и 2j + 1) */
DAN_VK_AMX static void dan_vk_amx_unpack_lowbit(int fmt, const uint8_t *p, uint8_t *lo, uint8_t *hi, float *df, int32_t *mp, int m) {
    const __m256i m3 = _mm256_set1_epi8(3), one = _mm256_set1_epi8(1), m4 = _mm256_set1_epi8(0xF);
    if (fmt == DAN_FMT_IQ4XS) {
        int8_t sc[8];
        dan_vk_iq4xs_scales(p, sc);
        const __m256i kv = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(const void *)dan_vk_iq4nl));
        for (int b = 0; b < 8; b++) {   /* числа 32 b..32 b + 31 */
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + 8 + 16 * b));
            __m256i nb = _mm256_and_si256(_mm256_inserti128_si256(_mm256_castsi128_si256(v), _mm_srli_epi16(v, 4), 1), m4);
            __m512i s = _mm512_mullo_epi16(_mm512_cvtepi8_epi16(_mm256_shuffle_epi8(kv, nb)), _mm512_set1_epi16(sc[b]));
            int off = 1024 * (b / 2) + 32 * (b % 2);
            dan_vk_amx_put(lo + off, hi + off, s);
        }
        df[m] = dan_vk_half(p);
        return;
    }
    int8_t sc[16];
    __m256i h = fmt == DAN_FMT_Q3K ? DAN_VK_LD(p) : _mm256_setzero_si256();
    if (fmt == DAN_FMT_Q3K) dan_vk_q3k_scales(p + 96, sc);
    else for (int j = 0; j < 16; j++) sc[j] = (int8_t)(p[j] & 0xF);
    for (int n2 = 0; n2 < 2; n2++) {
        __m256i v = DAN_VK_LD(p + (fmt == DAN_FMT_Q2K ? 16 : 32) + 32 * n2);
        for (int j = 0; j < 4; j++) {   /* числа 128 n2 + 32 j..: части 8 n2 + 2 j и 8 n2 + 2 j + 1 */
            __m256i q = _mm256_and_si256(v, m3);
            v = _mm256_srli_epi16(v, 2);
            __m512i q16 = _mm512_cvtepu8_epi16(q);
            if (fmt == DAN_FMT_Q3K) {
                /* q = младшие 2 бита - 4, если бит 4 n2 + j байта hmask равен 0 */
                q16 = _mm512_sub_epi16(_mm512_or_si512(q16, _mm512_cvtepu8_epi16(_mm256_slli_epi16(_mm256_and_si256(h, one), 2))),
                                       _mm512_set1_epi16(4));
                h = _mm256_srli_epi16(h, 1);
            }
            __m512i sv = _mm512_mask_blend_epi16(0xFFFF0000u, _mm512_set1_epi16(sc[8 * n2 + 2 * j]), _mm512_set1_epi16(sc[8 * n2 + 2 * j + 1]));
            int off = 1024 * (2 * n2 + j / 2) + 32 * (j % 2);
            dan_vk_amx_put(lo + off, hi + off, _mm512_mullo_epi16(q16, sv));
        }
    }
    if (fmt == DAN_FMT_Q3K) {
        df[m] = dan_vk_half(p + 108);
        return;
    }
    df[m] = dan_vk_half(p + 80);
    df[16 + m] = dan_vk_half(p + 82);
    for (int j = 0; j < 8; j++) mp[8 * m + j] = (int32_t)((uint32_t)(p[2 * j] >> 4) | ((uint32_t)(p[2 * j + 1] >> 4) << 16));
}

/* 16 rows (row bytes apart; from «n» on - the last row again) of «blocks» K blocks into a */
DAN_VK_AMX static void dan_vk_amx_unpack(int fmt, const uint8_t *w, int64_t row, int n, int64_t blocks, uint8_t *a) {
    const int wb = dan_vk_fmt_bytes[fmt];
    const __m256i m4 = _mm256_set1_epi8(0xF), one = _mm256_set1_epi8(1), m3 = _mm256_set1_epi8(3);
    for (int m = 0; m < 16; m++) {
        const uint8_t *wr = w + (m < n ? m : n - 1) * row;
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = wr + b * wb;
            uint8_t *o = a + b * DAN_VK_AMX_AU, *lo = o + 64 * m, *hi = o + 4096 + 64 * m;
            float *df = (float *)(void *)(o + 8192);
            if (fmt >= DAN_FMT_Q2K) {
                dan_vk_amx_unpack_lowbit(fmt, p, lo, hi, df, (int32_t *)(void *)(o + 8192 + 128), m);
                continue;
            }
            if (fmt == DAN_FMT_Q6K) {
                const int8_t *sc = (const int8_t *)p + 192;
                for (int n2 = 0; n2 < 2; n2++) {
                    const uint8_t *ql = p + 64 * n2, *qh = p + 128 + 32 * n2;
                    __m256i l0 = DAN_VK_LD(ql), l1 = DAN_VK_LD(ql + 32), h = DAN_VK_LD(qh), q[4];
                    q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m3), 4));
                    q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4));
                    q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                                           _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4));
                    q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                                           _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4));
                    for (int j = 0; j < 4; j++) {   /* numbers 128 n2 + 32 j..: parts 8 n2 + 2 j and 8 n2 + 2 j + 1 */
                        __m512i s = _mm512_mask_blend_epi16(0xFFFF0000u, _mm512_set1_epi16(sc[8 * n2 + 2 * j]),
                                                            _mm512_set1_epi16(sc[8 * n2 + 2 * j + 1]));
                        __m512i v = _mm512_mullo_epi16(_mm512_sub_epi16(_mm512_cvtepu8_epi16(q[j]), _mm512_set1_epi16(32)), s);
                        int off = 1024 * (2 * n2 + j / 2) + 32 * (j % 2);
                        dan_vk_amx_put(lo + off, hi + off, v);
                    }
                }
                df[m] = dan_vk_half(p + 208);
                continue;
            }
            uint8_t sc[8], mn[8];
            dan_vk_k4_scales(p + 4, sc, mn);
            __m256i hb = fmt == DAN_FMT_Q5K ? DAN_VK_LD(p + 16) : _mm256_setzero_si256();
            const uint8_t *q = p + (fmt == DAN_FMT_Q5K ? 48 : 16);
            for (int c = 0; c < 4; c++) {   /* numbers 64 c..: parts 2 c and 2 c + 1 */
                __m256i v = DAN_VK_LD(q + 32 * c);
                __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(hb, one), 4);
                __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hb, 1), one), 4);
                hb = _mm256_srli_epi16(hb, 2);
                __m256i l = _mm256_or_si256(_mm256_and_si256(v, m4), h0);
                __m256i u = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4), h1);
                dan_vk_amx_put(lo + 1024 * c, hi + 1024 * c, _mm512_mullo_epi16(_mm512_cvtepu8_epi16(l), _mm512_set1_epi16(sc[2 * c])));
                dan_vk_amx_put(lo + 1024 * c + 32, hi + 1024 * c + 32,
                               _mm512_mullo_epi16(_mm512_cvtepu8_epi16(u), _mm512_set1_epi16(sc[2 * c + 1])));
            }
            df[m] = dan_vk_half(p);
            df[16 + m] = dan_vk_half(p + 2);
            int32_t *mp = (int32_t *)(void *)(o + 8192 + 128);
            for (int j = 0; j < 4; j++) mp[8 * m + j] = (int32_t)((uint32_t)mn[2 * j] | ((uint32_t)mn[2 * j + 1] << 16));
        }
    }
}

/* the vectors 16 g..16 g + 15 of nv (xs bytes apart, in Q8_K; past nv - zeros) into o */
static void dan_vk_amx_prep(int fmt, const uint8_t *x, int64_t xs, int64_t nv, int64_t blocks, int64_t g, uint8_t *o) {
    memset(o, 0, (size_t)(blocks * DAN_VK_AMX_XU));
    for (int v = 0; v < 16 && 16 * g + v < nv; v++) {
        const dan_q8k *y = (const dan_q8k *)(const void *)(x + (16 * g + v) * xs);
        for (int64_t b = 0; b < blocks; b++, y++) {
            uint8_t *ob = o + b * DAN_VK_AMX_XU;
            for (int c = 0; c < 4; c++)
                for (int i = 0; i < 16; i++) memcpy(ob + 1024 * c + 64 * i + 4 * v, y->qs + 64 * c + 4 * i, 4);
            memcpy(ob + 4096 + 4 * v, &y->d, 4);
            if (fmt == DAN_FMT_Q2K)   /* суммы частей по 16 парами (части 2j и 2j + 1) */
                for (int j = 0; j < 8; j++) {
                    uint32_t pr = (uint32_t)(uint16_t)y->bsums[2 * j] | ((uint32_t)(uint16_t)y->bsums[2 * j + 1] << 16);
                    memcpy(ob + 4096 + 64 + 64 * j + 4 * v, &pr, 4);
                }
            else
                for (int j = 0; j < 4; j++) {
                    uint32_t s0 = (uint16_t)(int16_t)(y->bsums[4 * j] + y->bsums[4 * j + 1]);
                    uint32_t s1 = (uint16_t)(int16_t)(y->bsums[4 * j + 2] + y->bsums[4 * j + 3]);
                    uint32_t pr = s0 | (s1 << 16);
                    memcpy(ob + 4096 + 64 + 64 * j + 4 * v, &pr, 4);
                }
        }
    }
}

/* the float part of a block for 16 rows by the vectors of one group: c - the
   integer sums C_lo, C_hi (16 by 16), acc - 8 lanes and the tail, 16 by 16 */
DAN_VK_AMX static inline void dan_vk_amx_fin(int mins, int hi, const uint8_t *ab, const uint8_t *xb, const int32_t *c, float *acc, int64_t b,
                                             int64_t full) {
    const float *dw = (const float *)(const void *)(ab + 8192), *dm = dw + 16;
    const int32_t *mp = (const int32_t *)(const void *)(ab + 8192 + 128);
    __m512 Y = _mm512_loadu_ps((const float *)(const void *)(xb + 4096));
    __m512i ps[8];
    for (int j = 0; j < mins; j++) ps[j] = _mm512_loadu_si512((const void *)(xb + 4096 + 64 + 64 * j));
    float *dst = b < full ? acc + 256 * (b & 7) : acc + 256 * 8;
    for (int m = 0; m < 16; m++) {
        __m512i T = _mm512_loadu_si512((const void *)(c + 16 * m));
        if (hi) T = _mm512_add_epi32(T, _mm512_slli_epi32(_mm512_loadu_si512((const void *)(c + 256 + 16 * m)), 8));
        __m512 t = _mm512_mul_ps(_mm512_mul_ps(Y, _mm512_set1_ps(dw[m])), _mm512_cvtepi32_ps(T));
        if (mins) {
            __m512i M = _mm512_setzero_si512();
            for (int j = 0; j < mins; j++) M = _mm512_dpwssd_epi32(M, _mm512_set1_epi32(mp[8 * m + j]), ps[j]);
            t = _mm512_sub_ps(t, _mm512_mul_ps(_mm512_mul_ps(Y, _mm512_set1_ps(dm[m])), _mm512_cvtepi32_ps(M)));
        }
        _mm512_storeu_ps(dst + 16 * m, _mm512_add_ps(_mm512_loadu_ps(dst + 16 * m), t));
    }
    if (b + 1 == full)
        for (int m = 0; m < 16; m++) {
            __m512 a[8];
            for (int l = 0; l < 8; l++) a[l] = _mm512_loadu_ps(acc + 256 * l + 16 * m);
            __m512 s = _mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a[0], a[4]), _mm512_add_ps(a[2], a[6])),
                                     _mm512_add_ps(_mm512_add_ps(a[1], a[5]), _mm512_add_ps(a[3], a[7])));
            _mm512_storeu_ps(acc + 256 * 8 + 16 * m, s);
        }
}

/* 16 unpacked rows (a) by the groups g0..g1 - 1 of 16 vectors (x, gs bytes
   apart) over «blocks» blocks: block by block, two groups at a time; acc -
   9 * 256 floats for each group (8 lanes and the result, row m - 16
   vectors), c - 1024 ints */
DAN_VK_AMX static void dan_vk_amx_k(int fmt, const uint8_t *a, const uint8_t *x, int64_t gs, int64_t g0, int64_t g1, int64_t blocks,
                                    float *acc, int32_t *c) {
    /* старшие байты со знаком (Q6_K, Q3_K, IQ4_XS); у Q2_K старших байтов нет; пары наименьших */
    const int sgn = fmt == DAN_FMT_Q6K || fmt == DAN_FMT_Q3K || fmt == DAN_FMT_IQ4XS, hi = fmt != DAN_FMT_Q2K;
    const int mins = fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K ? 4 : fmt == DAN_FMT_Q2K ? 8 : 0;
    const int64_t full = blocks / 8 * 8;
    memset(acc, 0, (size_t)(9 * 256 * 4 * (g1 - g0)));
    for (int64_t b = 0; b < blocks; b++) {
        const uint8_t *ab = a + b * DAN_VK_AMX_AU;
        for (int64_t g = g0; g < g1; g += 2) {
            const uint8_t *xb0 = x + g * gs + b * DAN_VK_AMX_XU;
            float *acc0 = acc + 9 * 256 * (g - g0);
            if (g + 1 < g1) {
                const uint8_t *xb1 = xb0 + gs;
                _tile_zero(0);
                _tile_zero(2);
                if (hi) {
                    _tile_zero(1);
                    _tile_zero(3);
                }
                for (int s = 0; s < 4; s++) {
                    _tile_loadd(4, ab + 1024 * s, 64);
                    _tile_loadd(6, xb0 + 1024 * s, 64);
                    _tile_loadd(7, xb1 + 1024 * s, 64);
                    _tile_dpbusd(0, 4, 6);
                    _tile_dpbusd(2, 4, 7);
                    if (hi) {
                        _tile_loadd(5, ab + 4096 + 1024 * s, 64);
                        if (sgn) { _tile_dpbssd(1, 5, 6); _tile_dpbssd(3, 5, 7); }
                        else { _tile_dpbusd(1, 5, 6); _tile_dpbusd(3, 5, 7); }
                    }
                }
                _tile_stored(0, c, 64);
                _tile_stored(2, c + 512, 64);
                if (hi) {
                    _tile_stored(1, c + 256, 64);
                    _tile_stored(3, c + 768, 64);
                }
                dan_vk_amx_fin(mins, hi, ab, xb0, c, acc0, b, full);
                dan_vk_amx_fin(mins, hi, ab, xb1, c + 512, acc0 + 9 * 256, b, full);
            } else {
                _tile_zero(0);
                if (hi) _tile_zero(1);
                for (int s = 0; s < 4; s++) {
                    _tile_loadd(4, ab + 1024 * s, 64);
                    _tile_loadd(6, xb0 + 1024 * s, 64);
                    _tile_dpbusd(0, 4, 6);
                    if (hi) {
                        _tile_loadd(5, ab + 4096 + 1024 * s, 64);
                        if (sgn) _tile_dpbssd(1, 5, 6); else _tile_dpbusd(1, 5, 6);
                    }
                }
                _tile_stored(0, c, 64);
                if (hi) _tile_stored(1, c + 256, 64);
                dan_vk_amx_fin(mins, hi, ab, xb0, c, acc0, b, full);
            }
        }
    }
}
static int dan_vk_matmul_amx(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv);
#endif

/* ---- small batches on x86: a row of weights by 1..8 vectors ----

   For a batch of 2-9 vectors (the check of the proposals of a draft) the
   unpacking of the rows into units costs as much as the products: here a K
   block of a row is taken apart in registers into the layout of a unit
   (dan_vk_rn_unpack_x86, sign mode 0) and multiplied at once by up to 8
   vectors prepared by dan_vk_rn_prep, as the kernels of rows by vectors do
   (vpdpbusd, or vpmaddubsw with 16-bit sums on AVX2). The integer sums of a
   block go into the float part by 8 blocks: lane l - block 8i + l, as in the
   row kernel; the blocks after the last full 8 - one by one. The bits are
   those of the row kernel. */

/* dan_vk_tr8_avx2 into registers: o[i] - step i */
DAN_VK_AVX2 static inline void dan_vk_tr8r_avx2(const __m256i *a, __m256i *o) {
    __m256i t[8], u[8];
    for (int j = 0; j < 8; j += 2) { t[j] = _mm256_unpacklo_epi32(a[j], a[j + 1]); t[j + 1] = _mm256_unpackhi_epi32(a[j], a[j + 1]); }
    for (int h = 0; h < 8; h += 4) {
        u[h] = _mm256_unpacklo_epi64(t[h], t[h + 2]);
        u[h + 1] = _mm256_unpackhi_epi64(t[h], t[h + 2]);
        u[h + 2] = _mm256_unpacklo_epi64(t[h + 1], t[h + 3]);
        u[h + 3] = _mm256_unpackhi_epi64(t[h + 1], t[h + 3]);
    }
    for (int i = 0; i < 4; i++) {
        o[i] = _mm256_permute2x128_si256(u[i], u[i + 4], 0x20);
        o[i + 4] = _mm256_permute2x128_si256(u[i], u[i + 4], 0x31);
    }
}
/* a block of Q4_K or Q5_K: w - the 8 steps, sc and mn - the scales and mins by lanes */
DAN_VK_AVX2 static inline void dan_vk_sm_k4_dec(int q5, const uint8_t *p, __m256i *w, __m256i *sc, __m256i *mn) {
    const __m256i m4 = _mm256_set1_epi8(0xF), one = _mm256_set1_epi8(1);
    const __m128i order = _mm_setr_epi8(0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15);
    __m256i a[8], hb = q5 ? DAN_VK_LD(p + 16) : _mm256_setzero_si256();
    const uint8_t *q = p + (q5 ? 48 : 16);
    for (int c = 0; c < 4; c++) {
        __m256i v = DAN_VK_LD(q + 32 * c);
        __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(hb, one), 4);
        __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hb, 1), one), 4);
        hb = _mm256_srli_epi16(hb, 2);
        a[c] = _mm256_or_si256(_mm256_and_si256(v, m4), h0);
        a[c + 4] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4), h1);
    }
    dan_vk_tr8r_avx2(a, w);
    __m128i s = dan_vk_k4_sm128_avx2(p + 4);
    *sc = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(s, order));
    *mn = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(_mm_srli_si128(s, 8), order));
}
/* a block of Q6_K: w - the 8 steps (4 of each half), sc - the scales of the halves by lanes */
DAN_VK_AVX2 static inline void dan_vk_sm_q6k_dec(const uint8_t *p, __m256i *w, __m256i *sc) {
    const __m256i m4 = _mm256_set1_epi8(0xF), m3 = _mm256_set1_epi8(3);
    const __m128i order = _mm_setr_epi8(0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15);
    for (int n = 0; n < 2; n++) {
        const uint8_t *ql = p + 64 * n, *qh = p + 128 + 32 * n;
        __m256i l0 = DAN_VK_LD(ql), l1 = DAN_VK_LD(ql + 32), h = DAN_VK_LD(qh);
        __m256i q1 = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m3), 4));
        __m256i q2 = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4));
        __m256i q3 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                                     _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4));
        __m256i q4 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                                     _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4));
        __m256i t0 = _mm256_unpacklo_epi32(q1, q2), t1 = _mm256_unpackhi_epi32(q1, q2);
        __m256i t2 = _mm256_unpacklo_epi32(q3, q4), t3 = _mm256_unpackhi_epi32(q3, q4);
        w[4 * n] = _mm256_unpacklo_epi64(t0, t2);
        w[4 * n + 1] = _mm256_unpackhi_epi64(t0, t2);
        w[4 * n + 2] = _mm256_unpacklo_epi64(t1, t3);
        w[4 * n + 3] = _mm256_unpackhi_epi64(t1, t3);
    }
    __m128i s = _mm_loadu_si128((const __m128i *)(const void *)(p + 192));
    sc[0] = _mm256_cvtepi8_epi32(_mm_shuffle_epi8(s, order));
    sc[1] = _mm256_cvtepi8_epi32(_mm_shuffle_epi8(_mm_srli_si128(s, 8), order));
}

/* the integer sums of a K block p of a row by NV vectors (units of block x, us
   bytes apart): T[k], M[k] - by lanes, as the kernels of rows by vectors */
#define DAN_VK_SM_INTS_VNNI(NV) \
DAN_VK_VNNI static inline void dan_vk_sm_ints##NV##_vnni(int fmt, const uint8_t *p, const uint8_t *x, int64_t us, __m256i *T, __m256i *M) { \
    __m256i wv[8], sc[2], mn; \
    if (fmt == DAN_FMT_Q6K) { \
        dan_vk_sm_q6k_dec(p, wv, sc); \
        for (int k = 0; k < (NV); k++) { \
            const uint8_t *xb = x + k * us; \
            for (int n = 0; n < 2; n++) { \
                __m256i a = _mm256_setzero_si256(); \
                for (int i = 0; i < 4; i++) a = _mm256_dpbusd_epi32(a, wv[4 * n + i], DAN_VK_LD(xb + 128 * n + 32 * i)); \
                __m256i t = _mm256_mullo_epi32(_mm256_sub_epi32(a, DAN_VK_LD(xb + DAN_VK_RN_SC + 32 * n)), sc[n]); \
                T[k] = n ? _mm256_add_epi32(T[k], t) : t; \
            } \
            M[k] = _mm256_setzero_si256(); \
        } \
        return; \
    } \
    dan_vk_sm_k4_dec(fmt == DAN_FMT_Q5K, p, wv, sc, &mn); \
    for (int k = 0; k < (NV); k++) { \
        const uint8_t *xb = x + k * us; \
        __m256i a = _mm256_setzero_si256(); \
        for (int i = 0; i < 8; i++) a = _mm256_dpbusd_epi32(a, wv[i], DAN_VK_LD(xb + 32 * i)); \
        T[k] = _mm256_mullo_epi32(a, sc[0]); \
        M[k] = _mm256_mullo_epi32(mn, DAN_VK_LD(xb + DAN_VK_RN_SC + 32)); \
    } \
}
/* AVX2: vpmaddubsw with 16-bit sums of G steps (Q4_K - 8, Q5_K - 4, Q6_K - 2), then vpmaddwd by the scales */
#define DAN_VK_SM_INTS_AVX2(NV) \
DAN_VK_AVX2 static inline void dan_vk_sm_ints##NV##_avx2(int fmt, const uint8_t *p, const uint8_t *x, int64_t us, __m256i *T, __m256i *M) { \
    __m256i wv[8], sc[2], mn; \
    if (fmt == DAN_FMT_Q6K) { \
        dan_vk_sm_q6k_dec(p, wv, sc); \
        for (int n = 0; n < 2; n++) { \
            __m256i s16 = _mm256_blend_epi16(sc[n], _mm256_slli_epi32(sc[n], 16), 0xAA); \
            for (int k = 0; k < (NV); k++) { \
                const uint8_t *xb = x + k * us + 128 * n; \
                __m256i a0 = _mm256_add_epi16(_mm256_maddubs_epi16(wv[4 * n], DAN_VK_LD(xb)), _mm256_maddubs_epi16(wv[4 * n + 1], DAN_VK_LD(xb + 32))); \
                __m256i a1 = _mm256_add_epi16(_mm256_maddubs_epi16(wv[4 * n + 2], DAN_VK_LD(xb + 64)), _mm256_maddubs_epi16(wv[4 * n + 3], DAN_VK_LD(xb + 96))); \
                __m256i a = _mm256_add_epi32(_mm256_madd_epi16(a0, s16), _mm256_madd_epi16(a1, s16)); \
                __m256i t = _mm256_sub_epi32(a, _mm256_mullo_epi32(DAN_VK_LD(x + k * us + DAN_VK_RN_SC + 32 * n), sc[n])); \
                T[k] = n ? _mm256_add_epi32(T[k], t) : t; \
                M[k] = _mm256_setzero_si256(); \
            } \
        } \
        return; \
    } \
    dan_vk_sm_k4_dec(fmt == DAN_FMT_Q5K, p, wv, sc, &mn); \
    __m256i s16 = _mm256_blend_epi16(sc[0], _mm256_slli_epi32(sc[0], 16), 0xAA); \
    for (int k = 0; k < (NV); k++) { \
        const uint8_t *xb = x + k * us; \
        __m256i a; \
        if (fmt == DAN_FMT_Q4K) { \
            __m256i a16 = _mm256_maddubs_epi16(wv[0], DAN_VK_LD(xb)); \
            for (int i = 1; i < 8; i++) a16 = _mm256_add_epi16(a16, _mm256_maddubs_epi16(wv[i], DAN_VK_LD(xb + 32 * i))); \
            a = _mm256_madd_epi16(a16, s16); \
        } else { \
            __m256i a0 = _mm256_maddubs_epi16(wv[0], DAN_VK_LD(xb)), a1 = _mm256_maddubs_epi16(wv[4], DAN_VK_LD(xb + 128)); \
            for (int i = 1; i < 4; i++) { \
                a0 = _mm256_add_epi16(a0, _mm256_maddubs_epi16(wv[i], DAN_VK_LD(xb + 32 * i))); \
                a1 = _mm256_add_epi16(a1, _mm256_maddubs_epi16(wv[i + 4], DAN_VK_LD(xb + 32 * (i + 4)))); \
            } \
            a = _mm256_add_epi32(_mm256_madd_epi16(a0, s16), _mm256_madd_epi16(a1, s16)); \
        } \
        T[k] = a; \
        M[k] = _mm256_mullo_epi32(mn, DAN_VK_LD(xb + DAN_VK_RN_SC + 32)); \
    } \
}

/* a row w of «blocks» K blocks by NV vectors (units, us bytes apart): out[k] */
#define DAN_VK_SMK_X86(NAME, ATTR, NV, INTS) \
ATTR static void NAME(int fmt, const uint8_t *w, int64_t blocks, const uint8_t *x, int64_t us, float *out) { \
    const int wb = dan_vk_fmt_bytes[fmt], q6 = fmt == DAN_FMT_Q6K, doff = q6 ? 208 : 0; \
    const int64_t full = blocks / 8 * 8; \
    __m256i tb[NV][8], mb[NV][8]; \
    __m256 acc[NV]; \
    float r[NV]; \
    for (int k = 0; k < (NV); k++) { acc[k] = _mm256_setzero_ps(); r[k] = 0.0f; } \
    for (int64_t b = 0; b < blocks; b++) { \
        const uint8_t *p = w + b * wb, *xb = x + b * DAN_VK_RN_UNIT; \
        __m256i T[NV], M[NV]; \
        INTS(fmt, p, xb, us, T, M); \
        if (b < full) { \
            for (int k = 0; k < (NV); k++) { tb[k][b & 7] = T[k]; mb[k][b & 7] = M[k]; } \
            if ((b & 7) == 7) { \
                const uint8_t *p0 = p - 7 * wb, *x0 = xb - 7 * DAN_VK_RN_UNIT + DAN_VK_RN_D; \
                __m256 dw = dan_vk_halves8_avx2(p0 + doff, wb), dm = q6 ? _mm256_setzero_ps() : dan_vk_halves8_avx2(p0 + 2, wb); \
                for (int k = 0; k < (NV); k++) { \
                    const float *f = (const float *)(const void *)(x0 + k * us); \
                    __m256 Y = _mm256_setr_ps(f[0], f[88], f[176], f[264], f[352], f[440], f[528], f[616]); \
                    __m256 t = _mm256_mul_ps(_mm256_mul_ps(Y, dw), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(tb[k]))); \
                    if (!q6) t = _mm256_sub_ps(t, _mm256_mul_ps(_mm256_mul_ps(Y, dm), _mm256_cvtepi32_ps(dan_vk_hadd8_avx2(mb[k])))); \
                    acc[k] = _mm256_add_ps(acc[k], t); \
                } \
                if (b + 1 == full) for (int k = 0; k < (NV); k++) r[k] = dan_vk_hsum8_lanes_avx2(acc[k]); \
            } \
        } else { \
            float dw = dan_vk_half(p + doff), dm = q6 ? 0.0f : dan_vk_half(p + 2); \
            for (int k = 0; k < (NV); k++) { \
                float yd; \
                memcpy(&yd, xb + k * us + DAN_VK_RN_D, 4); \
                float a = yd * dw, t = a * (float)dan_vk_hsum_i32_avx2(T[k]); \
                if (!q6) { float bq = yd * dm, u = bq * (float)dan_vk_hsum_i32_avx2(M[k]); t = t - u; } \
                r[k] = r[k] + t; \
            } \
        } \
    } \
    for (int k = 0; k < (NV); k++) out[k] = r[k]; \
}
DAN_VK_SM_INTS_VNNI(1) DAN_VK_SM_INTS_VNNI(2) DAN_VK_SM_INTS_VNNI(3) DAN_VK_SM_INTS_VNNI(4)
DAN_VK_SM_INTS_VNNI(5) DAN_VK_SM_INTS_VNNI(6) DAN_VK_SM_INTS_VNNI(7) DAN_VK_SM_INTS_VNNI(8)
DAN_VK_SM_INTS_AVX2(1) DAN_VK_SM_INTS_AVX2(2) DAN_VK_SM_INTS_AVX2(3) DAN_VK_SM_INTS_AVX2(4)
DAN_VK_SMK_X86(dan_vk_sm1_vnni, DAN_VK_VNNI, 1, dan_vk_sm_ints1_vnni)
DAN_VK_SMK_X86(dan_vk_sm2_vnni, DAN_VK_VNNI, 2, dan_vk_sm_ints2_vnni)
DAN_VK_SMK_X86(dan_vk_sm3_vnni, DAN_VK_VNNI, 3, dan_vk_sm_ints3_vnni)
DAN_VK_SMK_X86(dan_vk_sm4_vnni, DAN_VK_VNNI, 4, dan_vk_sm_ints4_vnni)
DAN_VK_SMK_X86(dan_vk_sm5_vnni, DAN_VK_VNNI, 5, dan_vk_sm_ints5_vnni)
DAN_VK_SMK_X86(dan_vk_sm6_vnni, DAN_VK_VNNI, 6, dan_vk_sm_ints6_vnni)
DAN_VK_SMK_X86(dan_vk_sm7_vnni, DAN_VK_VNNI, 7, dan_vk_sm_ints7_vnni)
DAN_VK_SMK_X86(dan_vk_sm8_vnni, DAN_VK_VNNI, 8, dan_vk_sm_ints8_vnni)
DAN_VK_SMK_X86(dan_vk_sm1_avx2, DAN_VK_AVX2F16, 1, dan_vk_sm_ints1_avx2)
DAN_VK_SMK_X86(dan_vk_sm2_avx2, DAN_VK_AVX2F16, 2, dan_vk_sm_ints2_avx2)
DAN_VK_SMK_X86(dan_vk_sm3_avx2, DAN_VK_AVX2F16, 3, dan_vk_sm_ints3_avx2)
DAN_VK_SMK_X86(dan_vk_sm4_avx2, DAN_VK_AVX2F16, 4, dan_vk_sm_ints4_avx2)
/* a row by nv (1..sm_max) vectors: 0 if the format has no such kernel */
static int dan_vk_sm_vnni(int fmt, int nv, const uint8_t *w, int64_t blocks, const uint8_t *x, int64_t us, float *out) {
    static void (*const f[8])(int, const uint8_t *, int64_t, const uint8_t *, int64_t, float *) = {
        dan_vk_sm1_vnni, dan_vk_sm2_vnni, dan_vk_sm3_vnni, dan_vk_sm4_vnni, dan_vk_sm5_vnni, dan_vk_sm6_vnni, dan_vk_sm7_vnni, dan_vk_sm8_vnni};
    if ((fmt != DAN_FMT_Q4K && fmt != DAN_FMT_Q5K && fmt != DAN_FMT_Q6K) || nv < 1 || nv > 8) return 0;
    if (out) f[nv - 1](fmt, w, blocks, x, us, out);
    return 1;
}
static int dan_vk_sm_avx2(int fmt, int nv, const uint8_t *w, int64_t blocks, const uint8_t *x, int64_t us, float *out) {
    static void (*const f[4])(int, const uint8_t *, int64_t, const uint8_t *, int64_t, float *) = {
        dan_vk_sm1_avx2, dan_vk_sm2_avx2, dan_vk_sm3_avx2, dan_vk_sm4_avx2};
    if ((fmt != DAN_FMT_Q4K && fmt != DAN_FMT_Q5K && fmt != DAN_FMT_Q6K) || nv < 1 || nv > 4) return 0;
    if (out) f[nv - 1](fmt, w, blocks, x, us, out);
    return 1;
}

/* -- раскладка строк по 16 на AVX-512 VNNI (поток 26А, «раскладка строк по 16» выше) -- */

/* 16 x 16 чисел по 32 бита: строка r на входе - r[i], на выходе r[s] - числа s всех строк (дорожка - строка) */
DAN_VK_VNNI static inline void dan_vk_tl_tr16(__m512i *r) {
    __m512i t[16], u[16];
    for (int i = 0; i < 8; i++) {
        t[2 * i] = _mm512_unpacklo_epi32(r[2 * i], r[2 * i + 1]);
        t[2 * i + 1] = _mm512_unpackhi_epi32(r[2 * i], r[2 * i + 1]);
    }
    for (int i = 0; i < 4; i++) {
        u[4 * i] = _mm512_unpacklo_epi64(t[4 * i], t[4 * i + 2]);
        u[4 * i + 1] = _mm512_unpackhi_epi64(t[4 * i], t[4 * i + 2]);
        u[4 * i + 2] = _mm512_unpacklo_epi64(t[4 * i + 1], t[4 * i + 3]);
        u[4 * i + 3] = _mm512_unpackhi_epi64(t[4 * i + 1], t[4 * i + 3]);
    }
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 4; j++) {
            t[8 * i + j] = _mm512_shuffle_i32x4(u[8 * i + j], u[8 * i + j + 4], 0x88);
            t[8 * i + j + 4] = _mm512_shuffle_i32x4(u[8 * i + j], u[8 * i + j + 4], 0xDD);
        }
    for (int j = 0; j < 8; j++) {
        r[j] = _mm512_shuffle_i32x4(t[j], t[j + 8], 0x88);
        r[j + 8] = _mm512_shuffle_i32x4(t[j], t[j + 8], 0xDD);
    }
}

#define DAN_VK_TL_LD(p) _mm256_loadu_si256((const __m256i *)(const void *)(p))
#define DAN_VK_TL_ST(p, v) _mm256_storeu_si256((__m256i *)(void *)(p), (v))

/* числа одного блока строки в раскладке (как dan_vk_tl_nat_c) командами SIMD */
DAN_VK_VNNI static void dan_vk_tl_nat_vnni(int fmt, const uint8_t *p, int8_t *nat) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m3 = _mm256_set1_epi8(3), m1 = _mm256_set1_epi8(1);
    switch (fmt) {
    case DAN_FMT_Q80:
        memcpy(nat, p + 2, 32);
        break;
    case DAN_FMT_Q4K:
        for (int c = 0; c < 4; c++) {
            __m256i v = DAN_VK_TL_LD(p + 16 + 32 * c);
            DAN_VK_TL_ST(nat + 64 * c, _mm256_and_si256(v, m4));
            DAN_VK_TL_ST(nat + 64 * c + 32, _mm256_and_si256(_mm256_srli_epi16(v, 4), m4));
        }
        break;
    case DAN_FMT_Q5K: {
        __m256i qh = DAN_VK_TL_LD(p + 16);
        for (int c = 0; c < 4; c++) {
            __m256i v = DAN_VK_TL_LD(p + 48 + 32 * c);
            __m256i h0 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srl_epi16(qh, _mm_cvtsi32_si128(2 * c)), m1), 4);
            __m256i h1 = _mm256_slli_epi16(_mm256_and_si256(_mm256_srl_epi16(qh, _mm_cvtsi32_si128(2 * c + 1)), m1), 4);
            DAN_VK_TL_ST(nat + 64 * c, _mm256_or_si256(_mm256_and_si256(v, m4), h0));
            DAN_VK_TL_ST(nat + 64 * c + 32, _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4), h1));
        }
        break;
    }
    case DAN_FMT_Q6K: {
        const __m256i k32 = _mm256_set1_epi8(32);
        for (int n = 0; n < 2; n++) {
            __m256i l0 = DAN_VK_TL_LD(p + 64 * n), l1 = DAN_VK_TL_LD(p + 64 * n + 32), qh = DAN_VK_TL_LD(p + 128 + 32 * n);
            __m256i a = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(qh, m3), 4));
            __m256i b = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(qh, 2), m3), 4));
            __m256i c = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(qh, 4), m3), 4));
            __m256i d = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(qh, 6), m3), 4));
            DAN_VK_TL_ST(nat + 128 * n, _mm256_sub_epi8(a, k32));
            DAN_VK_TL_ST(nat + 128 * n + 32, _mm256_sub_epi8(b, k32));
            DAN_VK_TL_ST(nat + 128 * n + 64, _mm256_sub_epi8(c, k32));
            DAN_VK_TL_ST(nat + 128 * n + 96, _mm256_sub_epi8(d, k32));
        }
        break;
    }
    case DAN_FMT_Q2K:
        for (int n = 0; n < 2; n++) {
            __m256i v = DAN_VK_TL_LD(p + 16 + 32 * n);
            for (int j = 0; j < 4; j++) {
                __m256i q = _mm256_and_si256(_mm256_srl_epi16(v, _mm_cvtsi32_si128(2 * j)), m3);
                __m512i s = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set1_epi16((int16_t)(p[8 * n + 2 * j] & 0xF))),
                                               _mm256_set1_epi16((int16_t)(p[8 * n + 2 * j + 1] & 0xF)), 1);
                DAN_VK_TL_ST(nat + 128 * n + 32 * j, _mm512_cvtepi16_epi8(_mm512_mullo_epi16(_mm512_cvtepu8_epi16(q), s)));
            }
        }
        break;
    case DAN_FMT_Q3K: {
        int8_t sc[16];
        dan_vk_q3k_scales(p + 96, sc);
        __m256i hm = DAN_VK_TL_LD(p);
        const __m256i k4 = _mm256_set1_epi8(4);
        for (int n = 0; n < 2; n++) {
            __m256i v = DAN_VK_TL_LD(p + 32 + 32 * n);
            for (int j = 0; j < 4; j++) {
                __m256i q2 = _mm256_and_si256(_mm256_srl_epi16(v, _mm_cvtsi32_si128(2 * j)), m3);
                __m256i h = _mm256_and_si256(_mm256_srl_epi16(hm, _mm_cvtsi32_si128(4 * n + j)), m1);
                __m256i q = _mm256_sub_epi8(_mm256_or_si256(q2, _mm256_slli_epi16(h, 2)), k4);
                __m512i s = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set1_epi16(sc[8 * n + 2 * j])),
                                               _mm256_set1_epi16(sc[8 * n + 2 * j + 1]), 1);
                __m512i pr = _mm512_sub_epi16(_mm512_setzero_si512(), _mm512_mullo_epi16(_mm512_cvtepi8_epi16(q), s));
                DAN_VK_TL_ST(nat + 128 * n + 32 * j, _mm512_cvtepi16_epi8(pr));
            }
        }
        break;
    }
    default: {   /* IQ4_XS */
        const __m128i t = _mm_loadu_si128((const __m128i *)(const void *)dan_vk_iq4nl), k = _mm_set1_epi8(0x0F);
        for (int b = 0; b < 8; b++) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + 8 + 16 * b));
            _mm_storeu_si128((__m128i *)(void *)(nat + 32 * b), _mm_shuffle_epi8(t, _mm_and_si128(v, k)));
            _mm_storeu_si128((__m128i *)(void *)(nat + 32 * b + 16), _mm_shuffle_epi8(t, _mm_and_si128(_mm_srli_epi16(v, 4), k)));
        }
        break;
    }
    }
}

/* 16 строк (p[r] - блок строки r) в единицу раскладки o: те же байты, что у dan_vk_tl_unpack_c */
DAN_VK_VNNI static void dan_vk_tl_unpack_vnni(int fmt, const uint8_t *const *p, uint8_t *o) {
    _Alignas(64) int8_t nat[16][256];
    int steps = dan_vk_tl_steps(fmt), nsc = dan_vk_tl_nsc(fmt), nmp = dan_vk_tl_nmp(fmt);
    int osc = dan_vk_tl_sc(fmt), omp = dan_vk_tl_mp(fmt), od = dan_vk_tl_d(fmt);
    for (int r = 0; r < 16; r++) {
        const uint8_t *q = p[r];
        dan_vk_tl_nat_vnni(fmt, q, nat[r]);
        float d, dm = 0.0f;
        switch (fmt) {
        case DAN_FMT_Q80: d = dan_vk_half(q); break;
        case DAN_FMT_Q2K:
            for (int j = 0; j < 8; j++) {
                int16_t pr[2] = {(int16_t)(q[2 * j] >> 4), (int16_t)(q[2 * j + 1] >> 4)};
                memcpy(o + omp + 64 * j + 4 * r, pr, 4);
            }
            d = dan_vk_half(q + 80);
            dm = dan_vk_half(q + 82);
            break;
        case DAN_FMT_Q3K: d = dan_vk_half(q + 108); break;
        case DAN_FMT_Q6K:
            for (int j = 0; j < 16; j++) dan_vk_put32(o + osc + 64 * j + 4 * r, ((const int8_t *)q)[192 + j]);
            d = dan_vk_half(q + 208);
            break;
        case DAN_FMT_IQ4XS: {
            int8_t s[8];
            dan_vk_iq4xs_scales(q, s);
            for (int j = 0; j < 8; j++) dan_vk_put32(o + osc + 64 * j + 4 * r, s[j]);
            d = dan_vk_half(q);
            break;
        }
        default: {   /* Q4_K, Q5_K */
            uint8_t s[8], m[8];
            dan_vk_k4_scales(q + 4, s, m);
            for (int j = 0; j < 8; j++) dan_vk_put32(o + osc + 64 * j + 4 * r, s[j]);
            for (int j = 0; j < 4; j++) {
                int16_t pr[2] = {m[2 * j], m[2 * j + 1]};
                memcpy(o + omp + 64 * j + 4 * r, pr, 4);
            }
            d = dan_vk_half(q);
            dm = dan_vk_half(q + 2);
            break;
        }
        }
        dan_vk_putf(o + od + 4 * r, d);
        if (dan_vk_tl_mins(fmt)) dan_vk_putf(o + od + 64 + 4 * r, dm);
    }
    (void)nmp;
    /* числа: по 16 ступеней (у Q8_0 - 8 ступеней, верхняя половина пуста) */
    for (int c = 0; c < (steps + 15) / 16; c++) {
        __m512i v[16];
        for (int r = 0; r < 16; r++)
            v[r] = steps < 16 ? _mm512_castsi256_si512(DAN_VK_TL_LD(nat[r])) : _mm512_load_si512((const void *)(nat[r] + 64 * c));
        if (steps < 16)
            for (int r = 0; r < 16; r++) v[r] = _mm512_inserti64x4(v[r], _mm256_setzero_si256(), 1);
        dan_vk_tl_tr16(v);
        for (int s = 0; s < 16 && 16 * c + s < steps; s++) _mm512_storeu_si512((void *)(o + 64 * (16 * c + s)), v[s]);
    }
    /* C: 128 x сумма чисел строки (по частям - с масштабом части) */
    const __m512i one = _mm512_set1_epi8(1);
    __m512i c = _mm512_setzero_si512();
    int sp = nsc ? steps / nsc : steps;
    for (int j = 0; j < (nsc ? nsc : 1); j++) {
        __m512i a = _mm512_setzero_si512();
        for (int s = j * sp; s < (j + 1) * sp; s++) a = _mm512_dpbusd_epi32(a, one, _mm512_loadu_si512((const void *)(o + 64 * s)));
        c = nsc ? _mm512_add_epi32(c, _mm512_mullo_epi32(a, _mm512_loadu_si512((const void *)(o + osc + 64 * j)))) : a;
    }
    _mm512_storeu_si512((void *)(o + dan_vk_tl_c(fmt)), _mm512_slli_epi32(c, 7));
}

/* ядра: DAN_VK_TL_R групп по 16 строк на DAN_VK_TL_V векторов; единицы
   групп одного блока - подряд (tile + (b R + g) единиц), векторы - через xs
   байтов; out[k 16 R + i] - строка i на вектор k */
#ifndef DAN_VK_TL_R
#define DAN_VK_TL_R 2
#define DAN_VK_TL_V 4
#endif

DAN_VK_VNNI static inline __m512i dan_vk_tl_bc(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return _mm512_set1_epi32(v);
}
DAN_VK_VNNI static inline __m512i dan_vk_tl_ld(const uint8_t *p) { return _mm512_loadu_si512((const void *)p); }

/* суммы ступеней S0..S1 - 1 блока в A[r][k] */
#define DAN_VK_TL_DP(A, S0, S1) \
    for (int r = 0; r < DAN_VK_TL_R; r++) for (int k = 0; k < DAN_VK_TL_V; k++) A[r][k] = _mm512_setzero_si512(); \
    _Pragma("GCC unroll 8") \
    for (int s = (S0); s < (S1); s++) { \
        __m512i W[DAN_VK_TL_R]; \
        for (int r = 0; r < DAN_VK_TL_R; r++) W[r] = dan_vk_tl_ld(tw[r] + 64 * s); \
        for (int k = 0; k < DAN_VK_TL_V; k++) { \
            __m512i X = dan_vk_tl_bc(xv[k] + 4 * s); \
            for (int r = 0; r < DAN_VK_TL_R; r++) A[r][k] = _mm512_dpbusd_epi32(A[r][k], X, W[r]); \
        } \
    }

/* целые суммы блока: числа с масштабами (Q8_0, Q2_K: IS = A - C, Q3_K: C - A) */
#define DAN_VK_TL_INTS_PLAIN(FMT, NEG) \
    { \
        __m512i A[DAN_VK_TL_R][DAN_VK_TL_V]; \
        DAN_VK_TL_DP(A, 0, dan_vk_tl_steps(FMT)) \
        for (int r = 0; r < DAN_VK_TL_R; r++) { \
            __m512i C = dan_vk_tl_ld(tw[r] + dan_vk_tl_c(FMT)); \
            for (int k = 0; k < DAN_VK_TL_V; k++) IS[r][k] = (NEG) ? _mm512_sub_epi32(C, A[r][k]) : _mm512_sub_epi32(A[r][k], C); \
        } \
    }
/* суммы частей на масштабы частей (vpmulld): NP частей */
#define DAN_VK_TL_INTS_MUL(FMT, NP) \
    { \
        __m512i A[DAN_VK_TL_R][DAN_VK_TL_V]; \
        for (int r = 0; r < DAN_VK_TL_R; r++) for (int k = 0; k < DAN_VK_TL_V; k++) IS[r][k] = _mm512_setzero_si512(); \
        for (int j = 0; j < (NP); j++) { \
            DAN_VK_TL_DP(A, j * (64 / (NP)), (j + 1) * (64 / (NP))) \
            for (int r = 0; r < DAN_VK_TL_R; r++) { \
                __m512i S = dan_vk_tl_ld(tw[r] + dan_vk_tl_sc(FMT) + 64 * j); \
                for (int k = 0; k < DAN_VK_TL_V; k++) IS[r][k] = _mm512_add_epi32(IS[r][k], _mm512_mullo_epi32(A[r][k], S)); \
            } \
        } \
        for (int r = 0; r < DAN_VK_TL_R; r++) { \
            __m512i C = dan_vk_tl_ld(tw[r] + dan_vk_tl_c(FMT)); \
            for (int k = 0; k < DAN_VK_TL_V; k++) IS[r][k] = _mm512_sub_epi32(IS[r][k], C); \
        } \
    }
/* наименьшие: NM пар частей строки на пары сумм частей вектора (смещение XO) */
#define DAN_VK_TL_MINS(FMT, NM, XO) \
    for (int r = 0; r < DAN_VK_TL_R; r++) \
        for (int k = 0; k < DAN_VK_TL_V; k++) { \
            __m512i m = _mm512_setzero_si512(); \
            for (int j = 0; j < (NM); j++) \
                m = _mm512_dpwssd_epi32(m, dan_vk_tl_ld(tw[r] + dan_vk_tl_mp(FMT) + 64 * j), dan_vk_tl_bc(xv[k] + (XO) + 4 * j)); \
            IM[r][k] = m; \
        }

/* ядро формата FMT: INTS - целые суммы IS (и IM) блока */
#define DAN_VK_TL_KERNEL(NAME, FMT, INTS) \
DAN_VK_VNNI static void NAME(const uint8_t *tile, const uint8_t *xp, int64_t xs, int64_t blocks, float *out) { \
    enum { R = DAN_VK_TL_R, V = DAN_VK_TL_V }; \
    const int tu = dan_vk_tl_size(FMT), xu = dan_vk_tl_xu(FMT), od = dan_vk_tl_d(FMT), mins = dan_vk_tl_mins(FMT); \
    const int xd = (FMT) == DAN_FMT_Q80 ? 32 : DAN_VK_TL_XD; \
    __m512 accf[8][R][V], tail[R][V]; \
    for (int l = 0; l < 8; l++) for (int r = 0; r < R; r++) for (int k = 0; k < V; k++) accf[l][r][k] = _mm512_setzero_ps(); \
    for (int r = 0; r < R; r++) for (int k = 0; k < V; k++) tail[r][k] = _mm512_setzero_ps(); \
    const int64_t full = blocks / 8 * 8; \
    for (int64_t b = 0; b < blocks; b++) { \
        const uint8_t *tw[R], *xv[V]; \
        for (int r = 0; r < R; r++) tw[r] = tile + (b * R + r) * tu; \
        for (int k = 0; k < V; k++) xv[k] = xp + k * xs + b * xu; \
        __m512i IS[R][V], IM[R][V]; \
        INTS \
        (void)IM; \
        for (int k = 0; k < V; k++) { \
            float yd; \
            memcpy(&yd, xv[k] + xd, 4); \
            __m512 Y = _mm512_set1_ps(yd); \
            for (int r = 0; r < R; r++) { \
                __m512 t = _mm512_mul_ps(_mm512_mul_ps(Y, _mm512_loadu_ps((const float *)(const void *)(tw[r] + od))), \
                                         _mm512_cvtepi32_ps(IS[r][k])); \
                if (mins) { \
                    __m512 bm = _mm512_mul_ps(Y, _mm512_loadu_ps((const float *)(const void *)(tw[r] + od + 64))); \
                    t = _mm512_sub_ps(t, _mm512_mul_ps(bm, _mm512_cvtepi32_ps(IM[r][k]))); \
                } \
                if (b < full) accf[b & 7][r][k] = _mm512_add_ps(accf[b & 7][r][k], t); \
                else tail[r][k] = _mm512_add_ps(tail[r][k], t); \
            } \
        } \
        if (b + 1 == full) \
            for (int r = 0; r < R; r++) \
                for (int k = 0; k < V; k++) { \
                    __m512 (*a)[R][V] = accf; \
                    tail[r][k] = _mm512_add_ps(_mm512_add_ps(_mm512_add_ps(a[0][r][k], a[4][r][k]), _mm512_add_ps(a[2][r][k], a[6][r][k])), \
                                               _mm512_add_ps(_mm512_add_ps(a[1][r][k], a[5][r][k]), _mm512_add_ps(a[3][r][k], a[7][r][k]))); \
                } \
    } \
    for (int k = 0; k < V; k++) \
        for (int r = 0; r < R; r++) _mm512_storeu_ps(out + k * 16 * R + 16 * r, tail[r][k]); \
}
DAN_VK_TL_KERNEL(dan_vk_tl_q80_vnni, DAN_FMT_Q80, DAN_VK_TL_INTS_PLAIN(DAN_FMT_Q80, 0))
DAN_VK_TL_KERNEL(dan_vk_tl_q2k_vnni, DAN_FMT_Q2K, DAN_VK_TL_INTS_PLAIN(DAN_FMT_Q2K, 0) DAN_VK_TL_MINS(DAN_FMT_Q2K, 8, DAN_VK_TL_XBS))
DAN_VK_TL_KERNEL(dan_vk_tl_q3k_vnni, DAN_FMT_Q3K, DAN_VK_TL_INTS_PLAIN(DAN_FMT_Q3K, 1))
DAN_VK_TL_KERNEL(dan_vk_tl_iq4xs_vnni, DAN_FMT_IQ4XS, DAN_VK_TL_INTS_MUL(DAN_FMT_IQ4XS, 8))
DAN_VK_TL_KERNEL(dan_vk_tl_q4k_vnni, DAN_FMT_Q4K, DAN_VK_TL_INTS_MUL(DAN_FMT_Q4K, 8) DAN_VK_TL_MINS(DAN_FMT_Q4K, 4, DAN_VK_TL_XB32))
DAN_VK_TL_KERNEL(dan_vk_tl_q5k_vnni, DAN_FMT_Q5K, DAN_VK_TL_INTS_MUL(DAN_FMT_Q5K, 8) DAN_VK_TL_MINS(DAN_FMT_Q5K, 4, DAN_VK_TL_XB32))
DAN_VK_TL_KERNEL(dan_vk_tl_q6k_vnni, DAN_FMT_Q6K, DAN_VK_TL_INTS_MUL(DAN_FMT_Q6K, 16))

#define DAN_VK_TL_VNNI , {NULL, dan_vk_tl_q4k_vnni, dan_vk_tl_q5k_vnni, dan_vk_tl_q6k_vnni, NULL, NULL, NULL, dan_vk_tl_q80_vnni, \
                          dan_vk_tl_q2k_vnni, dan_vk_tl_q3k_vnni, dan_vk_tl_iq4xs_vnni}, dan_vk_tl_unpack_vnni

#define DAN_VK_LOWBIT_AVX2 dan_vk_q2k_dot_avx2, dan_vk_q3k_dot_avx2, dan_vk_iq4xs_dot_avx2
#define DAN_VK_LOWBIT_VNNI dan_vk_q2k_dot_avx2, dan_vk_q3k_dot_avx2, dan_vk_iq4xs_dot_vnni

/* AVX2 (and avx512 without VNNI) */
#define DAN_VK_AVX2_EXTRA dan_vk_q50_dot_avx2, dan_vk_q41_dot_avx2, dan_vk_q51_dot_avx2, \
    {dan_vk_q40_dot4_avx2, dan_vk_q4k_dot4_avx2, dan_vk_q5k_dot4_avx2, dan_vk_q6k_dot4_avx2, \
     dan_vk_q50_dot4_avx2, dan_vk_q41_dot4_avx2, dan_vk_q51_dot4_avx2, dan_vk_q80_dot4_avx2}

static const dan_vk_table dan_vk_avx2 = {"avx2",dan_vk_dot_f32_avx2, dan_vk_dot_f64_avx2, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2, dan_vk_mm_f32_avx2,
                                         dan_vk_mm_f64_avx2, dan_vk_q8_block_avx2, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx2, dan_vk_h2f_avx2, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx2_fast, dan_vk_mm_f64_avx2_fast,
                                         dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_AVX2_EXTRA DAN_VK_RN_AVX2_TAIL, NULL, dan_vk_sm_avx2, DAN_VK_LOWBIT_AVX2};

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
                                         DAN_VK_AVX2_EXTRA DAN_VK_RN_AVX2_TAIL, NULL, dan_vk_sm_avx2, DAN_VK_LOWBIT_AVX2};

/* avx512 with VNNI: the kernels of rows by several vectors use its instructions */
#define DAN_VK_VNNI_EXTRA dan_vk_q50_dot_vnni, dan_vk_q41_dot_vnni, dan_vk_q51_dot_vnni, \
    {dan_vk_q40_dot4_vnni, dan_vk_q4k_dot4_vnni, dan_vk_q5k_dot4_vnni, dan_vk_q6k_dot4_vnni, \
     dan_vk_q50_dot4_vnni, dan_vk_q41_dot4_vnni, dan_vk_q51_dot4_vnni, dan_vk_q80_dot4_vnni}
static const dan_vk_table dan_vk_avx512_vnni = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast,
                                           dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_VNNI_EXTRA DAN_VK_RN_VNNI_TAIL, NULL, dan_vk_sm_vnni, DAN_VK_LOWBIT_VNNI DAN_VK_TL_VNNI};
/* avx512 with VNNI on 512 bits (AMD Zen 5): the kernels of rows by vectors two steps at a time */
static const dan_vk_table dan_vk_avx512_zen5 = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast,
                                           dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_VNNI_EXTRA DAN_VK_RN_VNNIZ_TAIL, NULL, dan_vk_sm_vnni, DAN_VK_LOWBIT_VNNI DAN_VK_TL_VNNI};
#ifdef DAN_VK_AMX_ON
/* avx512 with VNNI and AMX: a matrix by many vectors by tiles */
static const dan_vk_table dan_vk_avx512_amx = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast,
                                           dan_vk_q40_dot_avx2, dan_vk_q4k_dot_avx2, dan_vk_q5k_dot_avx2, dan_vk_q6k_dot_avx2,
                                         DAN_VK_VNNI_EXTRA DAN_VK_RN_VNNI_TAIL, dan_vk_matmul_amx, dan_vk_sm_vnni, DAN_VK_LOWBIT_VNNI DAN_VK_TL_VNNI};
#endif
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

/* ---- NEON: Q2_K, Q3_K, IQ4_XS ---- */

/* 16 частей по 16 (d - суммы частей в 4 дорожках) на масштабы lo (части 0..7) и hi (8..15) */
static inline int32x4_t dan_vk_apply16_neon(const int32x4_t *d, int16x8_t lo, int16x8_t hi) {
    int32x4_t acc = vmulq_s32(dan_vk_sum4_neon(d), vmovl_s16(vget_low_s16(lo)));
    acc = vmlaq_s32(acc, dan_vk_sum4_neon(d + 4), vmovl_s16(vget_high_s16(lo)));
    acc = vmlaq_s32(acc, dan_vk_sum4_neon(d + 8), vmovl_s16(vget_low_s16(hi)));
    return vmlaq_s32(acc, dan_vk_sum4_neon(d + 12), vmovl_s16(vget_high_s16(hi)));
}

/* 16 масштабов Q3_K минус 32 в регистре (без записи в память и чтения обратно) */
static inline int8x16_t dan_vk_q3k_scales_neon(const uint8_t *q) {
    uint32_t a[3];
    memcpy(a, q, 12);
    uint32_t t = a[2];
    uint64_t lo = (uint64_t)((a[0] & 0x0f0f0f0fu) | ((t & 0x03030303u) << 4))
                | (uint64_t)((a[1] & 0x0f0f0f0fu) | (((t >> 2) & 0x03030303u) << 4)) << 32;
    uint64_t hi = (uint64_t)(((a[0] >> 4) & 0x0f0f0f0fu) | (((t >> 4) & 0x03030303u) << 4))
                | (uint64_t)(((a[1] >> 4) & 0x0f0f0f0fu) | (((t >> 6) & 0x03030303u) << 4)) << 32;
    return vsubq_s8(vreinterpretq_s8_u8(vcombine_u8(vcreate_u8(lo), vcreate_u8(hi))), vdupq_n_s8(32));
}

/* по 8 блоков, как DAN_VK_KDOT_NEON: d по смещению DOFF, dmin (MINS) - по MOFF, хвост - PF */
#define DAN_VK_KDOT2_NEON(NAME, ATTR, PARTS, WB, DOFF, MINS, MOFF, PF) \
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
            float32x4_t b0 = vmulq_f32(y0, dan_vk_halves4_neon(pw + (MOFF), WB)); \
            float32x4_t b1 = vmulq_f32(y1, dan_vk_halves4_neon(pw + 4 * (WB) + (MOFF), WB)); \
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
        float p = PF(w + i * (WB), x + i, is, ex); \
        r = r + p; \
    } \
    return r; \
}

/* ядра строк Q2_K, Q3_K, IQ4_XS для способа умножить 16 чисел (DOT16) */
#define DAN_VK_NEON_LOWBIT(SUF, ATTR, DOT16) \
ATTR static inline int32x4_t dan_vk_q2k_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    const uint8x16_t m3 = vdupq_n_u8(3); \
    int32x4_t d[16]; \
    for (int n = 0; n < 2; n++) { \
        uint8x16_t q0 = vld1q_u8(w + 16 + 32 * n), q1 = vld1q_u8(w + 32 + 32 * n); \
        const int8_t *v = y->qs + 128 * n; \
        for (int j = 0; j < 4; j++) { \
            d[8 * n + 2 * j] = DOT16(vdupq_n_s32(0), DAN_VK_S8(vandq_u8(q0, m3)), vld1q_s8(v + 32 * j)); \
            d[8 * n + 2 * j + 1] = DOT16(vdupq_n_s32(0), DAN_VK_S8(vandq_u8(q1, m3)), vld1q_s8(v + 32 * j + 16)); \
            q0 = vshrq_n_u8(q0, 2); \
            q1 = vshrq_n_u8(q1, 2); \
        } \
    } \
    uint8x16_t s = vld1q_u8(w); \
    int16x8_t sl = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vandq_u8(s, vdupq_n_u8(0xF))))); \
    int16x8_t sh = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(vandq_u8(s, vdupq_n_u8(0xF))))); \
    uint8x16_t m = vshrq_n_u8(s, 4); \
    int16x8_t ml = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(m))), mh = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(m))); \
    int16x8_t b0 = vld1q_s16(y->bsums), b1 = vld1q_s16(y->bsums + 8); \
    int32x4_t mi = vmull_s16(vget_low_s16(ml), vget_low_s16(b0)); \
    mi = vmlal_s16(mi, vget_high_s16(ml), vget_high_s16(b0)); \
    mi = vmlal_s16(mi, vget_low_s16(mh), vget_low_s16(b1)); \
    mi = vmlal_s16(mi, vget_high_s16(mh), vget_high_s16(b1)); \
    *extra = vaddvq_s32(mi); \
    return dan_vk_apply16_neon(d, sl, sh); \
} \
/* Q3_K: числа -4..3 со знаком - младшие 2 бита минус 4, где старший бит 0 */ \
ATTR static inline int32x4_t dan_vk_q3k_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    int8x16_t s8 = dan_vk_q3k_scales_neon(w + 96); \
    const uint8x16_t m3 = vdupq_n_u8(3), four = vdupq_n_u8(4); \
    uint8x16_t h0 = vld1q_u8(w), h1 = vld1q_u8(w + 16); \
    int32x4_t d[16]; \
    for (int n = 0; n < 2; n++) { \
        uint8x16_t q0 = vld1q_u8(w + 32 + 32 * n), q1 = vld1q_u8(w + 48 + 32 * n); \
        const int8_t *v = y->qs + 128 * n; \
        for (int j = 0; j < 4; j++) { \
            uint8x16_t mb = vdupq_n_u8((uint8_t)(1u << (4 * n + j))); \
            int8x16_t a0 = vsubq_s8(DAN_VK_S8(vandq_u8(q0, m3)), DAN_VK_S8(vbicq_u8(four, vtstq_u8(h0, mb)))); \
            int8x16_t a1 = vsubq_s8(DAN_VK_S8(vandq_u8(q1, m3)), DAN_VK_S8(vbicq_u8(four, vtstq_u8(h1, mb)))); \
            d[8 * n + 2 * j] = DOT16(vdupq_n_s32(0), a0, vld1q_s8(v + 32 * j)); \
            d[8 * n + 2 * j + 1] = DOT16(vdupq_n_s32(0), a1, vld1q_s8(v + 32 * j + 16)); \
            q0 = vshrq_n_u8(q0, 2); \
            q1 = vshrq_n_u8(q1, 2); \
        } \
    } \
    *extra = 0; \
    return dan_vk_apply16_neon(d, vmovl_s8(vget_low_s8(s8)), vmovl_s8(vget_high_s8(s8))); \
} \
ATTR static inline int32x4_t dan_vk_iq4xs_parts_##SUF(const uint8_t *w, const dan_q8k *y, int32_t *extra) { \
    int8_t sc[8]; \
    dan_vk_iq4xs_scales(w, sc); \
    const int8x16_t kv = vld1q_s8(dan_vk_iq4nl); \
    const uint8x16_t m4 = vdupq_n_u8(0xF); \
    int32x4_t d[8]; \
    for (int b = 0; b < 8; b++) { \
        uint8x16_t q = vld1q_u8(w + 8 + 16 * b); \
        int8x16_t lo = vqtbl1q_s8(kv, vandq_u8(q, m4)), hi = vqtbl1q_s8(kv, vshrq_n_u8(q, 4)); \
        d[b] = DOT16(DOT16(vdupq_n_s32(0), lo, vld1q_s8(y->qs + 32 * b)), hi, vld1q_s8(y->qs + 32 * b + 16)); \
    } \
    int16x8_t s16 = vmovl_s8(vld1_s8(sc)); \
    *extra = 0; \
    return vmlaq_s32(vmulq_s32(dan_vk_sum4_neon(d), vmovl_s16(vget_low_s16(s16))), dan_vk_sum4_neon(d + 4), \
                     vmovl_s16(vget_high_s16(s16))); \
} \
DAN_VK_KDOT2_NEON(dan_vk_q2k_dot_##SUF, ATTR, dan_vk_q2k_parts_##SUF, DAN_Q2K_BYTES, 80, 1, 82, dan_vk_q2k_p) \
DAN_VK_KDOT2_NEON(dan_vk_q3k_dot_##SUF, ATTR, dan_vk_q3k_parts_##SUF, DAN_Q3K_BYTES, 108, 0, 0, dan_vk_q3k_p) \
DAN_VK_KDOT2_NEON(dan_vk_iq4xs_dot_##SUF, ATTR, dan_vk_iq4xs_parts_##SUF, DAN_IQ4XS_BYTES, 0, 0, 0, dan_vk_iq4xs_p)
DAN_VK_NEON_LOWBIT(neon, , dan_vk_dot16_neon)

static const dan_vk_table dan_vk_neon = {"neon",dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon, dan_vk_mm_f32_neon,
                                         dan_vk_mm_f64_neon, dan_vk_q8_block_neon, dan_vk_q8_dot_neon, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast,
                                         dan_vk_q40_dot_neon, dan_vk_q4k_dot_neon, dan_vk_q5k_dot_neon, dan_vk_q6k_dot_neon,
                                         dan_vk_q50_dot_neon, dan_vk_q41_dot_neon, dan_vk_q51_dot_neon,
                                         {dan_vk_q40_dot4_neon, dan_vk_q4k_dot4_neon, dan_vk_q5k_dot4_neon, dan_vk_q6k_dot4_neon,
                                          dan_vk_q50_dot4_neon, dan_vk_q41_dot4_neon, dan_vk_q51_dot4_neon, dan_vk_q80_dot4_neon},
                                         0, 0, 0, NULL, {0}, NULL, NULL, dan_vk_q2k_dot_neon, dan_vk_q3k_dot_neon, dan_vk_iq4xs_dot_neon};

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
DAN_VK_NEON_LOWBIT(dotprod, DAN_VK_DOTPROD, dan_vk_dot16_dotprod)

/* ---- rows by vectors on ARM with sdot (the units: see «rows by vectors»
   above): 2 rows by 4 vectors, the numbers of rows unpacked with their sign,
   a step of 32 bytes - two registers (lanes 0-3 and 4-7) ---- */
#define DAN_VK_RN_R_NEON 2
#define DAN_VK_RN_V_NEON 4
#define DAN_VK_LDS8(p) vld1q_s8((const int8_t *)(const void *)(p))
#define DAN_VK_LDS32(p) vld1q_s32((const int32_t *)(const void *)(p))
#define DAN_VK_RNN_STEPS(OFF, N) \
    for (int r = 0; r < DAN_VK_RN_R_NEON; r++) \
        for (int k = 0; k < DAN_VK_RN_V_NEON; k++) acc[r][k][0] = acc[r][k][1] = vdupq_n_s32(0); \
    DAN_VK_ROLLED \
    for (int i = 0; i < (N); i++) { \
        int8x16_t wv[DAN_VK_RN_R_NEON][2]; \
        for (int r = 0; r < DAN_VK_RN_R_NEON; r++) { \
            wv[r][0] = DAN_VK_LDS8(wb[r] + (OFF) + 32 * i); \
            wv[r][1] = DAN_VK_LDS8(wb[r] + (OFF) + 32 * i + 16); \
        } \
        for (int k = 0; k < DAN_VK_RN_V_NEON; k++) { \
            int8x16_t y0 = DAN_VK_LDS8(xb[k] + (OFF) + 32 * i), y1 = DAN_VK_LDS8(xb[k] + (OFF) + 32 * i + 16); \
            for (int r = 0; r < DAN_VK_RN_R_NEON; r++) { \
                acc[r][k][0] = vdotq_s32(acc[r][k][0], wv[r][0], y0); \
                acc[r][k][1] = vdotq_s32(acc[r][k][1], wv[r][1], y1); \
            } \
        } \
    }

/* K formats: T, M - 8 parts of each (row, vector) pair p = r * 4 + k as two registers */
DAN_VK_DOTPROD static inline void dan_vk_rn_k4_dotprod(const uint8_t *const *wb, const uint8_t *const *xb, int32x4_t (*T)[2],
                                                      int32x4_t (*M)[2]) {
    int32x4_t acc[DAN_VK_RN_R_NEON][DAN_VK_RN_V_NEON][2];
    DAN_VK_RNN_STEPS(0, 8)
    for (int r = 0; r < DAN_VK_RN_R_NEON; r++)
        for (int h = 0; h < 2; h++) {
            int32x4_t sc = DAN_VK_LDS32(wb[r] + DAN_VK_RN_SC + 16 * h), m = DAN_VK_LDS32(wb[r] + DAN_VK_RN_SC + 32 + 16 * h);
            for (int k = 0; k < DAN_VK_RN_V_NEON; k++) {
                T[r * DAN_VK_RN_V_NEON + k][h] = vmulq_s32(acc[r][k][h], sc);
                M[r * DAN_VK_RN_V_NEON + k][h] = vmulq_s32(m, DAN_VK_LDS32(xb[k] + DAN_VK_RN_SC + 32 + 16 * h));
            }
        }
}
DAN_VK_DOTPROD static inline void dan_vk_rn_q6k_dotprod(const uint8_t *const *wb, const uint8_t *const *xb, int32x4_t (*T)[2],
                                                       int32x4_t (*M)[2]) {
    int32x4_t acc[DAN_VK_RN_R_NEON][DAN_VK_RN_V_NEON][2];
    (void)M;
    for (int n = 0; n < 2; n++) {
        DAN_VK_RNN_STEPS(128 * n, 4)
        for (int r = 0; r < DAN_VK_RN_R_NEON; r++)
            for (int h = 0; h < 2; h++) {
                int32x4_t sc = DAN_VK_LDS32(wb[r] + DAN_VK_RN_SC + 32 * n + 16 * h);
                for (int k = 0; k < DAN_VK_RN_V_NEON; k++) {
                    int32x4_t t = vmulq_s32(vsubq_s32(acc[r][k][h], DAN_VK_LDS32(xb[k] + DAN_VK_RN_SC + 32 * n + 16 * h)), sc);
                    T[r * DAN_VK_RN_V_NEON + k][h] = n ? vaddq_s32(T[r * DAN_VK_RN_V_NEON + k][h], t) : t;
                }
            }
    }
}
#define DAN_VK_RNK_NEON(NAME, MINS, INTS) \
DAN_VK_DOTPROD static void NAME(const uint8_t *w, int64_t ws, const uint8_t *x, int64_t xs, int64_t blocks, float *out) { \
    enum { R = DAN_VK_RN_R_NEON, NV = DAN_VK_RN_V_NEON }; \
    float accf[8][R * NV], tail[R * NV]; \
    memset(accf, 0, sizeof accf); \
    for (int l = 0; l < R * NV; l++) tail[l] = 0.0f; \
    const int64_t full = blocks / 8 * 8; \
    for (int64_t b = 0; b < blocks; b++) { \
        const uint8_t *wb[R], *xb[NV]; \
        for (int r = 0; r < R; r++) wb[r] = w + r * ws + b * DAN_VK_RN_UNIT; \
        for (int k = 0; k < NV; k++) xb[k] = x + k * xs + b * DAN_VK_RN_UNIT; \
        int32x4_t T[R * NV][2], M[R * NV][2]; \
        INTS(wb, xb, T, M); \
        float yd[NV]; \
        for (int k = 0; k < NV; k++) memcpy(&yd[k], xb[k] + DAN_VK_RN_D, 4); \
        float32x4_t Y = vld1q_f32(yd); \
        for (int r = 0; r < R; r++) { \
            int32x4_t s[NV], ms[NV]; \
            for (int k = 0; k < NV; k++) { \
                s[k] = vaddq_s32(T[r * NV + k][0], T[r * NV + k][1]); \
                if (MINS) ms[k] = vaddq_s32(M[r * NV + k][0], M[r * NV + k][1]); \
            } \
            float dw, dm; \
            memcpy(&dw, wb[r] + DAN_VK_RN_D, 4); \
            memcpy(&dm, wb[r] + DAN_VK_RN_D + 4, 4); \
            float32x4_t t = vmulq_f32(vmulq_f32(Y, vdupq_n_f32(dw)), vcvtq_f32_s32(dan_vk_sum4_neon(s))); \
            if (MINS) { \
                float32x4_t bm = vmulq_f32(Y, vdupq_n_f32(dm)); \
                t = vsubq_f32(t, vmulq_f32(bm, vcvtq_f32_s32(dan_vk_sum4_neon(ms)))); \
            } \
            float *dst = b < full ? &accf[b & 7][r * NV] : &tail[r * NV]; \
            vst1q_f32(dst, vaddq_f32(vld1q_f32(dst), t)); \
        } \
        if (b + 1 == full) \
            for (int g = 0; g < R * NV; g += 4) { \
                float32x4_t a[8]; \
                for (int l = 0; l < 8; l++) a[l] = vld1q_f32(&accf[l][g]); \
                float32x4_t s = vaddq_f32(vaddq_f32(vaddq_f32(a[0], a[4]), vaddq_f32(a[2], a[6])), \
                                          vaddq_f32(vaddq_f32(a[1], a[5]), vaddq_f32(a[3], a[7]))); \
                vst1q_f32(&tail[g], s); \
            } \
    } \
    for (int l = 0; l < R * NV; l++) out[l] = tail[l]; \
}
DAN_VK_RNK_NEON(dan_vk_rn_q4k_dotprod, 1, dan_vk_rn_k4_dotprod)
DAN_VK_RNK_NEON(dan_vk_rn_q6k_dotprodf, 0, dan_vk_rn_q6k_dotprod)

/* Q2_K (единицы - dan_vk_rn_lowbit_c): T - части с масштабами, M - наименьшие на суммы частей вектора;
   Q3_K и IQ4_XS считает ядро Q6_K (числа со знаком, суммы вектора нулевые) */
DAN_VK_DOTPROD static inline void dan_vk_rn_q2k_dotprod(const uint8_t *const *wb, const uint8_t *const *xb, int32x4_t (*T)[2],
                                                       int32x4_t (*M)[2]) {
    int32x4_t acc[DAN_VK_RN_R_NEON][DAN_VK_RN_V_NEON][2];
    for (int n = 0; n < 2; n++) {
        DAN_VK_RNN_STEPS(128 * n, 4)
        for (int r = 0; r < DAN_VK_RN_R_NEON; r++)
            for (int h = 0; h < 2; h++) {
                int32x4_t sc = DAN_VK_LDS32(wb[r] + DAN_VK_RN_SC + 32 * n + 16 * h);
                for (int k = 0; k < DAN_VK_RN_V_NEON; k++) {
                    int32x4_t t = vmulq_s32(acc[r][k][h], sc);
                    T[r * DAN_VK_RN_V_NEON + k][h] = n ? vaddq_s32(T[r * DAN_VK_RN_V_NEON + k][h], t) : t;
                }
            }
    }
    for (int r = 0; r < DAN_VK_RN_R_NEON; r++)
        for (int h = 0; h < 2; h++) {
            int32x4_t m0 = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(wb[r] + DAN_VK_RN_D + 8 + 4 * h)))));
            int32x4_t m1 = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(wb[r] + DAN_VK_RN_D + 16 + 4 * h)))));
            for (int k = 0; k < DAN_VK_RN_V_NEON; k++)
                M[r * DAN_VK_RN_V_NEON + k][h] = vmlaq_s32(vmulq_s32(m0, DAN_VK_LDS32(xb[k] + DAN_VK_RN_SC + 16 * h)), m1,
                                                           DAN_VK_LDS32(xb[k] + DAN_VK_RN_SC + 32 + 16 * h));
        }
}
DAN_VK_RNK_NEON(dan_vk_rn_q2k_dotprodf, 1, dan_vk_rn_q2k_dotprod)

/* blocks of 32: lanes - the blocks of a group (0-3 and 4-7) */
DAN_VK_DOTPROD static void dan_vk_rn_b32_dotprod(const uint8_t *w, int64_t ws, const uint8_t *x, int64_t xs, int64_t blocks, float *out) {
    enum { R = DAN_VK_RN_R_NEON, NV = DAN_VK_RN_V_NEON };
    float32x4_t facc[R * NV][2];
    int32_t last[R * NV][8];
    for (int l = 0; l < R * NV; l++) facc[l][0] = facc[l][1] = vdupq_n_f32(0.0f);
    const int64_t units = (blocks + 7) / 8, fullu = blocks / 8;
    for (int64_t u = 0; u < units; u++) {
        const uint8_t *wb[R], *xb[NV];
        for (int r = 0; r < R; r++) wb[r] = w + r * ws + u * DAN_VK_RN_UNIT;
        for (int k = 0; k < NV; k++) xb[k] = x + k * xs + u * DAN_VK_RN_UNIT;
        int32x4_t acc[R][NV][2];
        DAN_VK_RNN_STEPS(0, 8)
        for (int r = 0; r < R; r++)
            for (int h = 0; h < 2; h++) {
                float32x4_t dw = vld1q_f32((const float *)(const void *)(wb[r] + DAN_VK_RN_D + 16 * h));
                for (int k = 0; k < NV; k++) {
                    int32x4_t is = vsubq_s32(acc[r][k][h], DAN_VK_LDS32(xb[k] + DAN_VK_RN_SC + 16 * h));
                    if (u < fullu) {
                        float32x4_t d = vmulq_f32(dw, vld1q_f32((const float *)(const void *)(xb[k] + DAN_VK_RN_D + 16 * h)));
                        facc[r * NV + k][h] = vaddq_f32(facc[r * NV + k][h], vmulq_f32(d, vcvtq_f32_s32(is)));
                    } else vst1q_s32(last[r * NV + k] + 4 * h, is);
                }
            }
    }
    for (int r = 0; r < R; r++)
        for (int k = 0; k < NV; k++) {
            float32x4_t v4 = vaddq_f32(facc[r * NV + k][0], facc[r * NV + k][1]);
            float32x2_t h2 = vadd_f32(vget_low_f32(v4), vget_high_f32(v4));
            float res = vget_lane_f32(h2, 0) + vget_lane_f32(h2, 1);
            const uint8_t *wl = w + r * ws + fullu * DAN_VK_RN_UNIT + DAN_VK_RN_D, *xl = x + k * xs + fullu * DAN_VK_RN_UNIT + DAN_VK_RN_D;
            for (int l = 0; l < blocks - 8 * fullu; l++) {
                float a, c;
                memcpy(&a, wl + 4 * l, 4);
                memcpy(&c, xl + 4 * l, 4);
                float d = a * c, p = d * (float)last[r * NV + k][l];
                res = res + p;
            }
            out[r * NV + k] = res;
        }
}

/* unpacking of rows: 4 parts (16 bytes = dwords d..d+3 each) transposed,
   stored as 16 bytes of 4 steps from o, 32 bytes apart */
static inline void dan_vk_tr4_neon(int8x16_t a0, int8x16_t a1, int8x16_t a2, int8x16_t a3, uint8_t *o) {
    uint32x4_t t0 = vtrn1q_u32(vreinterpretq_u32_s8(a0), vreinterpretq_u32_s8(a1));
    uint32x4_t t1 = vtrn2q_u32(vreinterpretq_u32_s8(a0), vreinterpretq_u32_s8(a1));
    uint32x4_t t2 = vtrn1q_u32(vreinterpretq_u32_s8(a2), vreinterpretq_u32_s8(a3));
    uint32x4_t t3 = vtrn2q_u32(vreinterpretq_u32_s8(a2), vreinterpretq_u32_s8(a3));
    vst1q_u8(o, vreinterpretq_u8_u64(vtrn1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))));
    vst1q_u8(o + 32, vreinterpretq_u8_u64(vtrn1q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))));
    vst1q_u8(o + 64, vreinterpretq_u8_u64(vtrn2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))));
    vst1q_u8(o + 96, vreinterpretq_u8_u64(vtrn2q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))));
}
/* 8 parts of 32 numbers (a[j]: numbers 0..15 and 16..31): steps 0..7 */
static inline void dan_vk_tr8_neon(const dan_vk_w32_neon *a, uint8_t *o) {
    for (int G = 0; G < 2; G++) {
        const dan_vk_w32_neon *g = a + 4 * G;
        dan_vk_tr4_neon(g[0].lo, g[1].lo, g[2].lo, g[3].lo, o + 16 * G);
        dan_vk_tr4_neon(g[0].hi, g[1].hi, g[2].hi, g[3].hi, o + 128 + 16 * G);
    }
}
/* 8 bytes picked by the order of the lanes as 32-bit numbers */
static inline void dan_vk_rn_put8_neon(uint8_t *o, uint8x16_t b, int sgn) {
    static const uint8_t order[16] = {0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15};
    uint8x8_t v = vget_low_u8(vqtbl1q_u8(b, vld1q_u8(order)));
    if (sgn) {
        int16x8_t h = vmovl_s8(vreinterpret_s8_u8(v));
        vst1q_s32((int32_t *)(void *)o, vmovl_s16(vget_low_s16(h)));
        vst1q_s32((int32_t *)(void *)(o + 16), vmovl_s16(vget_high_s16(h)));
    } else {
        uint16x8_t h = vmovl_u8(v);
        vst1q_u32((uint32_t *)(void *)o, vmovl_u16(vget_low_u16(h)));
        vst1q_u32((uint32_t *)(void *)(o + 16), vmovl_u16(vget_high_u16(h)));
    }
}
static void dan_vk_rn_unpack_neon(int fmt, const uint8_t *w, int64_t blocks, uint8_t *out, int sgn) {
    /* sgn - the mode: DAN_VK_RN_SIGNED here, the rest by plain C */
    const int wb = dan_vk_fmt_bytes[fmt];
    const uint8x16_t m4 = vdupq_n_u8(0xF), m3 = vdupq_n_u8(3), b16 = vdupq_n_u8(16);
    if (sgn != DAN_VK_RN_SIGNED || fmt >= DAN_FMT_Q2K) { dan_vk_rn_unpack_c(fmt, w, blocks, out, sgn); return; }
    if (fmt == DAN_FMT_Q4K || fmt == DAN_FMT_Q5K) {
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = w + b * wb;
            uint8_t *o = out + b * DAN_VK_RN_UNIT, sm[16];
            dan_vk_w32_neon a[8];
            const uint8_t *q = p + (fmt == DAN_FMT_Q5K ? 48 : 16);
            uint8x16_t h0 = vld1q_u8(p + 16), h1 = vld1q_u8(p + 32);
            for (int c = 0; c < 4; c++) {
                uint8x16_t v0 = vld1q_u8(q + 32 * c), v1 = vld1q_u8(q + 32 * c + 16);
                uint8x16_t l0 = vandq_u8(v0, m4), l1 = vandq_u8(v1, m4), u0 = vshrq_n_u8(v0, 4), u1 = vshrq_n_u8(v1, 4);
                if (fmt == DAN_FMT_Q5K) {
                    uint8x16_t e1 = vdupq_n_u8((uint8_t)(1u << (2 * c))), e2 = vdupq_n_u8((uint8_t)(2u << (2 * c)));
                    l0 = vorrq_u8(l0, vandq_u8(vtstq_u8(h0, e1), b16));
                    l1 = vorrq_u8(l1, vandq_u8(vtstq_u8(h1, e1), b16));
                    u0 = vorrq_u8(u0, vandq_u8(vtstq_u8(h0, e2), b16));
                    u1 = vorrq_u8(u1, vandq_u8(vtstq_u8(h1, e2), b16));
                }
                a[c].lo = DAN_VK_S8(l0); a[c].hi = DAN_VK_S8(l1);
                a[c + 4].lo = DAN_VK_S8(u0); a[c + 4].hi = DAN_VK_S8(u1);
            }
            dan_vk_tr8_neon(a, o);
            dan_vk_k4_sm(p + 4, sm);
            uint8x16_t s = vld1q_u8(sm);
            dan_vk_rn_put8_neon(o + DAN_VK_RN_SC, s, 0);
            dan_vk_rn_put8_neon(o + DAN_VK_RN_SC + 32, vextq_u8(s, s, 8), 0);
            memset(o + DAN_VK_RN_D, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p));
            dan_vk_putf(o + DAN_VK_RN_D + 4, dan_vk_half(p + 2));
        }
        return;
    }
    if (fmt == DAN_FMT_Q6K) {
        const int8x16_t s32 = vdupq_n_s8(32);
        for (int64_t b = 0; b < blocks; b++) {
            const uint8_t *p = w + b * wb;
            uint8_t *o = out + b * DAN_VK_RN_UNIT;
            for (int n = 0; n < 2; n++) {
                const uint8_t *ql = p + 64 * n, *qh = p + 128 + 32 * n;
                int8x16_t S[8];
                for (int t = 0; t < 2; t++) {
                    uint8x16_t l0 = vld1q_u8(ql + 16 * t), l1 = vld1q_u8(ql + 32 + 16 * t), h = vld1q_u8(qh + 16 * t);
                    S[t] = vsubq_s8(DAN_VK_S8(vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(h, m3), 4))), s32);
                    S[2 + t] = vsubq_s8(DAN_VK_S8(vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 2), m3), 4))), s32);
                    S[4 + t] = vsubq_s8(DAN_VK_S8(vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(h, 4), m3), 4))), s32);
                    S[6 + t] = vsubq_s8(DAN_VK_S8(vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(h, 6), 4))), s32);
                }
                dan_vk_tr4_neon(S[0], S[2], S[4], S[6], o + 128 * n);
                dan_vk_tr4_neon(S[1], S[3], S[5], S[7], o + 128 * n + 16);
            }
            uint8x16_t s = vld1q_u8(p + 192);
            dan_vk_rn_put8_neon(o + DAN_VK_RN_SC, s, 1);
            dan_vk_rn_put8_neon(o + DAN_VK_RN_SC + 32, vextq_u8(s, s, 8), 1);
            memset(o + DAN_VK_RN_D, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D);
            dan_vk_putf(o + DAN_VK_RN_D, dan_vk_half(p + 208));
        }
        return;
    }
    int64_t g = 0;
    for (; 8 * g + 8 <= blocks; g++) {
        const uint8_t *p = w + 8 * g * wb;
        uint8_t *o = out + g * DAN_VK_RN_UNIT;
        dan_vk_w32_neon a[8];
        for (int j = 0; j < 8; j++)
            a[j] = fmt == DAN_FMT_Q40 ? dan_vk_w_q40_neon(p + j * wb) : fmt == DAN_FMT_Q50 ? dan_vk_w_q50_neon(p + j * wb)
                                                                                         : dan_vk_w_q80_neon(p + j * wb);
        dan_vk_tr8_neon(a, o);
        vst1q_f32((float *)(void *)(o + DAN_VK_RN_D), dan_vk_halves4_neon(p, wb));
        vst1q_f32((float *)(void *)(o + DAN_VK_RN_D + 16), dan_vk_halves4_neon(p + 4 * wb, wb));
        memset(o + DAN_VK_RN_D + 32, 0, DAN_VK_RN_UNIT - DAN_VK_RN_D - 32);
        memset(o + DAN_VK_RN_SC, 0, DAN_VK_RN_D - DAN_VK_RN_SC);
    }
    if (8 * g < blocks) dan_vk_rn_unpack_c(fmt, w + 8 * g * wb, blocks - 8 * g, out + g * DAN_VK_RN_UNIT, sgn);
}
#define DAN_VK_RN_DOTPROD_TAIL , DAN_VK_RN_R_NEON, DAN_VK_RN_V_NEON, 1, dan_vk_rn_unpack_neon, \
    {dan_vk_rn_b32_dotprod, dan_vk_rn_q4k_dotprod, dan_vk_rn_q4k_dotprod, dan_vk_rn_q6k_dotprodf, dan_vk_rn_b32_dotprod, NULL, NULL, dan_vk_rn_b32_dotprod, \
     dan_vk_rn_q2k_dotprodf, dan_vk_rn_q6k_dotprodf, dan_vk_rn_q6k_dotprodf}

static const dan_vk_table dan_vk_dotprod = {"dotprod", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon,
                                            dan_vk_mm_f32_neon, dan_vk_mm_f64_neon, dan_vk_q8_block_dotprod, dan_vk_q8_dot_dotprod, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast,
                                            dan_vk_q40_dot_dotprod, dan_vk_q4k_dot_dotprod, dan_vk_q5k_dot_dotprod, dan_vk_q6k_dot_dotprod,
                                         dan_vk_q50_dot_dotprod, dan_vk_q41_dot_dotprod, dan_vk_q51_dot_dotprod,
                                         {dan_vk_q40_dot4_dotprod, dan_vk_q4k_dot4_dotprod, dan_vk_q5k_dot4_dotprod, dan_vk_q6k_dot4_dotprod,
                                          dan_vk_q50_dot4_dotprod, dan_vk_q41_dot4_dotprod, dan_vk_q51_dot4_dotprod, dan_vk_q80_dot4_dotprod} DAN_VK_RN_DOTPROD_TAIL, NULL, NULL,
                                         dan_vk_q2k_dot_dotprod, dan_vk_q3k_dot_dotprod, dan_vk_iq4xs_dot_dotprod};

#if defined(__clang__)
#define DAN_VK_I8MM __attribute__((target("dotprod,i8mm")))
#else
#define DAN_VK_I8MM __attribute__((target("+dotprod+i8mm")))
#endif

/* ---- ARM с i8mm: Q8_0 на много векторов командой smmla ----

   smmla умножает матрицу 2 x 8 байтов на 8 x 2 (две строки на два вектора по 8 чисел) и
   прибавляет 4 суммы: вдвое больше произведений на команду, чем sdot. Строки и векторы
   берутся парами: блок пары (DAN_VK_MMLA_RB байтов) - 4 куска по 16 байтов [первая: числа
   8c..8c+7 блока | вторая: те же], затем 4 числа d: у пары строк - d0 d0 d1 d1, у пары
   векторов - d0 d1 d0 d1, так что произведения d четырёх пар (строка, вектор) блока - одно
   умножение в том же порядке, что у ядра строки (d строки на d вектора). Целая сумма блока
   точна, p = d * сумма, дорожки и хвост - как у ядра строки Q8_0: каждое число то же.
   Группа - 4 строки (2 пары) на 8 векторов (4 пары). */
#define DAN_VK_MMLA_RB 80
#define DAN_VK_MMLA_VG 8   /* векторов в группе */

static int dan_vk_has_i8mm(void) {
#if defined(__linux__)
#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1 << 13)
#endif
    return (getauxval(AT_HWCAP2) & HWCAP2_I8MM) != 0;
#elif defined(__APPLE__)
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_I8MM", &v, &len, NULL, 0) != 0) return 0;
    return v != 0;
#else
    return 0;
#endif
}

/* пара блоков Q8_0 (p0, p1; второй может быть NULL - нули) в блок пары; rows - d0 d0 d1 d1, иначе d0 d1 d0 d1 */
static void dan_vk_mmla_pair(const uint8_t *p0, const uint8_t *p1, int64_t blocks, int64_t stride, int rows, uint8_t *o) {
    for (int64_t b = 0; b < blocks; b++, o += DAN_VK_MMLA_RB) {
        const uint8_t *a = p0 ? p0 + b * stride : NULL, *c = p1 ? p1 + b * stride : NULL;
        for (int k = 0; k < 4; k++) {
            if (a) memcpy(o + 16 * k, a + 2 + 8 * k, 8); else memset(o + 16 * k, 0, 8);
            if (c) memcpy(o + 16 * k + 8, c + 2 + 8 * k, 8); else memset(o + 16 * k + 8, 0, 8);
        }
        float d0 = a ? dan_vk_half(a) : 0.0f, d1 = c ? dan_vk_half(c) : 0.0f;
        float d[4] = {d0, rows ? d0 : d1, rows ? d1 : d0, d1};
        memcpy(o + 64, d, 16);
    }
}

/* 4 строки (пары a0, a1) на 8 векторов (пары x, gs байтов между парами) по blocks блоков: out[r * 8 + v] */
DAN_VK_I8MM static void dan_vk_mmla_k(const uint8_t *a0, const uint8_t *a1, const uint8_t *x, int64_t gs, int64_t blocks, float *out) {
    float acc[8][32], res[32];
    memset(acc, 0, sizeof acc);
    const int64_t full = blocks / 8 * 8;
    for (int64_t b = 0; b < blocks; b++) {
        const uint8_t *pa[2] = {a0 + b * DAN_VK_MMLA_RB, a1 + b * DAN_VK_MMLA_RB};
        const uint8_t *px[4];
        for (int q = 0; q < 4; q++) px[q] = x + q * gs + b * DAN_VK_MMLA_RB;
        int32x4_t s[2][4];
        for (int p = 0; p < 2; p++)
            for (int q = 0; q < 4; q++) s[p][q] = vdupq_n_s32(0);
        for (int c = 0; c < 4; c++) {
            int8x16_t A0 = vld1q_s8((const int8_t *)(const void *)(pa[0] + 16 * c));
            int8x16_t A1 = vld1q_s8((const int8_t *)(const void *)(pa[1] + 16 * c));
            for (int q = 0; q < 4; q++) {
                int8x16_t X = vld1q_s8((const int8_t *)(const void *)(px[q] + 16 * c));
                s[0][q] = vmmlaq_s32(s[0][q], A0, X);
                s[1][q] = vmmlaq_s32(s[1][q], A1, X);
            }
        }
        float *dst = b < full ? acc[b & 7] : res;
        if (b == full) {
            /* дорожки: (l, l + 4), (l, l + 2), (l, l + 1) - как у ядра строки */
            for (int i = 0; i < 32; i += 4) {
                float32x4_t a[8];
                for (int l = 0; l < 8; l++) a[l] = vld1q_f32(&acc[l][i]);
                vst1q_f32(&res[i], vaddq_f32(vaddq_f32(vaddq_f32(a[0], a[4]), vaddq_f32(a[2], a[6])),
                                             vaddq_f32(vaddq_f32(a[1], a[5]), vaddq_f32(a[3], a[7]))));
            }
        }
        for (int p = 0; p < 2; p++) {
            float32x4_t dw = vld1q_f32((const float *)(const void *)(pa[p] + 64));
            for (int q = 0; q < 4; q++) {
                float32x4_t t = vmulq_f32(vmulq_f32(dw, vld1q_f32((const float *)(const void *)(px[q] + 64))), vcvtq_f32_s32(s[p][q]));
                float *o = dst + 16 * p + 4 * q;
                vst1q_f32(o, vaddq_f32(vld1q_f32(o), t));
            }
        }
    }
    if (blocks == full)
        for (int i = 0; i < 32; i += 4) {
            float32x4_t a[8];
            for (int l = 0; l < 8; l++) a[l] = vld1q_f32(&acc[l][i]);
            vst1q_f32(&res[i], vaddq_f32(vaddq_f32(vaddq_f32(a[0], a[4]), vaddq_f32(a[2], a[6])),
                                         vaddq_f32(vaddq_f32(a[1], a[5]), vaddq_f32(a[3], a[7]))));
        }
    /* res: пара строк p, пара векторов q - [r0 v0, r0 v1, r1 v0, r1 v1] */
    for (int p = 0; p < 2; p++)
        for (int q = 0; q < 4; q++)
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) out[(2 * p + i) * 8 + 2 * q + j] = res[16 * p + 4 * q + 2 * i + j];
}

static int dan_vk_matmul_mmla(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv);
/* dotprod с i8mm: то же, а Q8_0 на 8 и больше векторов - командой smmla (имя варианта то же - «dotprod») */
static const dan_vk_table dan_vk_dotprod_i8mm = {"dotprod", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon,
                                            dan_vk_mm_f32_neon, dan_vk_mm_f64_neon, dan_vk_q8_block_dotprod, dan_vk_q8_dot_dotprod, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast,
                                            dan_vk_q40_dot_dotprod, dan_vk_q4k_dot_dotprod, dan_vk_q5k_dot_dotprod, dan_vk_q6k_dot_dotprod,
                                         dan_vk_q50_dot_dotprod, dan_vk_q41_dot_dotprod, dan_vk_q51_dot_dotprod,
                                         {dan_vk_q40_dot4_dotprod, dan_vk_q4k_dot4_dotprod, dan_vk_q5k_dot4_dotprod, dan_vk_q6k_dot4_dotprod,
                                          dan_vk_q50_dot4_dotprod, dan_vk_q41_dot4_dotprod, dan_vk_q51_dot4_dotprod, dan_vk_q80_dot4_dotprod} DAN_VK_RN_DOTPROD_TAIL, dan_vk_matmul_mmla, NULL,
                                         dan_vk_q2k_dot_dotprod, dan_vk_q3k_dot_dotprod, dan_vk_iq4xs_dot_dotprod};


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

#ifdef DAN_VK_X86
/* AMD Zen 5 and later (family 0x1A): units of 512 bits */
static int dan_vk_zen5(void) {
    __builtin_cpu_init();
    if (!__builtin_cpu_is("amd")) return 0;
    unsigned a = 0, b, c, d;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
#endif
    unsigned fam = (a >> 8) & 0xF;
    if (fam == 0xF) fam += (a >> 20) & 0xFF;
    return fam >= 0x1A;
}
#endif

/* 1 if the processor can run the variant with this name */
static int dan_vk_supported(const char *name, const dan_vk_table **t) {
    if (strcmp(name, "c") == 0) { *t = &dan_vk_plain; return 1; }
#ifdef DAN_VK_X86
    __builtin_cpu_init();
    if (strcmp(name, "avx2") == 0 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("f16c")
        && __builtin_cpu_supports("fma")) { *t = &dan_vk_avx2; return 1; }
    /* «avx512» - the best of the variants below; «avx512vnni» and «amx» are
       asked for by DAN_YADRA for measurements and tests */
    int avx512 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("f16c")
                 && __builtin_cpu_supports("fma");
    int vnni = avx512 && __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vnni");
#ifdef DAN_VK_AMX_ON
    if ((strcmp(name, "avx512") == 0 || strcmp(name, "amx") == 0) && vnni && dan_vk_amx_ok()) { *t = &dan_vk_avx512_amx; return 1; }
#endif
    if (((strcmp(name, "avx512") == 0 && dan_vk_zen5()) || strcmp(name, "avx512z") == 0) && vnni) { *t = &dan_vk_avx512_zen5; return 1; }
    if ((strcmp(name, "avx512") == 0 || strcmp(name, "avx512vnni") == 0) && vnni) { *t = &dan_vk_avx512_vnni; return 1; }
    if (strcmp(name, "avx512") == 0 && avx512) { *t = &dan_vk_avx512; return 1; }
#endif
#ifdef DAN_VK_ARM
    if (strcmp(name, "neon") == 0) { *t = &dan_vk_neon; return 1; }
    if (strcmp(name, "dotprod") == 0 && dan_vk_has_dotprod()) { *t = &dan_vk_dotprod; return 1; }
    /* «i8mm» - dotprod со smmla, лучший вариант, где он есть */
    if (strcmp(name, "i8mm") == 0 && dan_vk_has_dotprod() && dan_vk_has_i8mm()) { *t = &dan_vk_dotprod_i8mm; return 1; }
#endif
    return 0;
}

static const dan_vk_table *dan_vk(void) {
    const dan_vk_table *t = atomic_load_explicit(&dan_vk_cur, memory_order_relaxed);
    if (t) return t;
    static const char *const best[] = {"avx512", "avx2", "i8mm", "dotprod", "neon", "c"};
    const char *want = getenv("DAN_YADRA");
    if (!(want && dan_vk_supported(want, &t)))
        for (int i = 0; i < 6; i++) if (dan_vk_supported(best[i], &t)) break;
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

static int dan_vk_few_f32(const float *a, const float *b, float *c, int64_t m, int64_t n, int64_t k);
static void dan_vk_matmat_f32(const float *a, const float *b, float *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    if (dan_vk_few_f32(a, b, c, m, n, k)) return;   /* a few rows: below */
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f32_part, &j);
}
static void dan_vk_matmat_f64(const double *a, const double *b, double *c, int64_t m, int64_t n, int64_t k) {
    dan_vk_mm_job j = {a, b, c, m, n, k};
    if (m <= 0 || n <= 0) return;
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_mm_f64_part, &j);
}

/* ---- rows of floats by several vectors (attention: the keys by the queries of
   a group of heads) and a matrix of few rows by a matrix (the weighted sums of
   the values) ----

   dan_vk_rows_f32: y[v * rows + i] = row i of w (rows x cols) . vector v of x
   (nv vectors of cols numbers one after another), each number with the bits of
   the variant's dot_f32 of the row and the vector. A row is read once for all
   vectors: the products of a pair go into the 64 lanes of a float dot product
   and are added pairwise down to 8 (4 on ARM) lanes as in dot_f32, the last
   pairwise sums of 8 (4) pairs are done at once by transposing their lanes
   (the same additions of the same numbers), then the remaining cols mod 64
   products one by one. The variant is chosen by its dot_f32, so the bits are
   those of dan_vk_dot_f32 with any variant. */

static void dan_vk_rows_f32_c(const float *w, const float *x, float *y, int64_t rows, int64_t cols, int64_t nv,
                              int64_t i0, int64_t i1, float (*dot)(const float *, const float *, int64_t)) {
    for (int64_t i = i0; i < i1; i++)
        for (int64_t v = 0; v < nv; v++) y[v * rows + i] = dot(w + i * cols, x + v * cols, cols);
}

/* the sum r of a pair after the lanes: the products of the last cols mod 64 numbers added one by one */
static inline float dan_vk_rows_tail(const float *row, const float *x, int64_t cols, float r) {
    for (int64_t k = cols / 64 * 64; k < cols; k++) { float p = row[k] * x[k]; r = r + p; }
    return r;
}

#ifdef DAN_VK_X86
/* 8 lanes of a pair (lanes l + 32, then l + 16, then l + 8 added, as in dan_vk_dot_f32_avx2) */
DAN_VK_AVX2 static inline __m256 dan_vk_rows_lanes8_avx2(const float *a, const float *b, int64_t n) {
    __m256 s[8];
    for (int j = 0; j < 8; j++) s[j] = _mm256_setzero_ps();
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 8; j++)
            s[j] = _mm256_add_ps(s[j], _mm256_mul_ps(_mm256_loadu_ps(a + k + 8 * j), _mm256_loadu_ps(b + k + 8 * j)));
    for (int j = 0; j < 4; j++) s[j] = _mm256_add_ps(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = _mm256_add_ps(s[j], s[j + 2]);
    return _mm256_add_ps(s[0], s[1]);
}

/* the sums of the 8 lanes of 8 registers (l + 4, then l + 2, then l + 1 - as
   dan_vk_hsum8_f32 for each): r[q] is the sum of the register q */
DAN_VK_AVX2 static inline void dan_vk_rows_hsum8x8(const __m256 *s, float *r) {
    /* the pairs (q, q + 4) in one register: the low half - register q, the high one - q + 4 */
    __m256 x[4];
    for (int q = 0; q < 4; q++)
        x[q] = _mm256_add_ps(_mm256_permute2f128_ps(s[q], s[q + 4], 0x20), _mm256_permute2f128_ps(s[q], s[q + 4], 0x31));
    /* x[q]: lanes 0-3 - X of register q, 4-7 - of q + 4, where X_l = lane l + lane l + 4 */
    __m256 c = _mm256_add_ps(_mm256_shuffle_ps(x[0], x[1], 0x44), _mm256_shuffle_ps(x[0], x[1], 0xEE));
    __m256 d = _mm256_add_ps(_mm256_shuffle_ps(x[2], x[3], 0x44), _mm256_shuffle_ps(x[2], x[3], 0xEE));
    /* c: [Y0 of 0, Y1 of 0, Y0 of 1, Y1 of 1 | the same of 4, 5], Y_l = X_l + X_l+2 */
    __m256 e = _mm256_add_ps(_mm256_shuffle_ps(c, d, 0x88), _mm256_shuffle_ps(c, d, 0xDD));
    /* e: [0, 1, 2, 3 | 4, 5, 6, 7] */
    _mm256_storeu_ps(r, e);
}

DAN_VK_AVX2 static void dan_vk_rows_f32_avx2(const float *w, const float *x, float *y, int64_t rows, int64_t cols,
                                             int64_t nv, int64_t i0, int64_t i1) {
    /* the pairs (row, vector) by rows, then by vectors, 8 at a time */
    __m256 s[8];
    int64_t pi[8], pv[8];
    int c = 0;
    for (int64_t i = i0; i < i1; i++)
        for (int64_t v = 0; v < nv; v++) {
            s[c] = dan_vk_rows_lanes8_avx2(w + i * cols, x + v * cols, cols);
            pi[c] = i;
            pv[c] = v;
            if (++c == 8 || (i == i1 - 1 && v == nv - 1)) {
                float r[8];
                for (int q = c; q < 8; q++) s[q] = _mm256_setzero_ps();
                dan_vk_rows_hsum8x8(s, r);
                for (int q = 0; q < c; q++)
                    y[pv[q] * rows + pi[q]] = dan_vk_rows_tail(w + pi[q] * cols, x + pv[q] * cols, cols, r[q]);
                c = 0;
            }
        }
}

/* 8 lanes of a pair as in dan_vk_dot_f32_avx512: 64 lanes in 4 registers, l + 32, l + 16, then l + 8 */
DAN_VK_AVX512 static inline __m256 dan_vk_rows_lanes8_avx512(const float *a, const float *b, int64_t n) {
    __m512 s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm512_setzero_ps();
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 4; j++)
            s[j] = _mm512_add_ps(s[j], _mm512_mul_ps(_mm512_loadu_ps(a + k + 16 * j), _mm512_loadu_ps(b + k + 16 * j)));
    for (int j = 0; j < 2; j++) s[j] = _mm512_add_ps(s[j], s[j + 2]);
    __m512 t = _mm512_add_ps(s[0], s[1]);
    return _mm256_add_ps(_mm512_castps512_ps256(t), _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(t), 1)));
}

DAN_VK_AVX512 static void dan_vk_rows_f32_avx512(const float *w, const float *x, float *y, int64_t rows, int64_t cols,
                                                 int64_t nv, int64_t i0, int64_t i1) {
    /* the pairs (row, vector) by rows, then by vectors, 8 at a time */
    __m256 s[8];
    int64_t pi[8], pv[8];
    int c = 0;
    for (int64_t i = i0; i < i1; i++)
        for (int64_t v = 0; v < nv; v++) {
            s[c] = dan_vk_rows_lanes8_avx512(w + i * cols, x + v * cols, cols);
            pi[c] = i;
            pv[c] = v;
            if (++c == 8 || (i == i1 - 1 && v == nv - 1)) {
                float r[8];
                for (int q = c; q < 8; q++) s[q] = _mm256_setzero_ps();
                dan_vk_rows_hsum8x8(s, r);
                for (int q = 0; q < c; q++)
                    y[pv[q] * rows + pi[q]] = dan_vk_rows_tail(w + pi[q] * cols, x + pv[q] * cols, cols, r[q]);
                c = 0;
            }
        }
}
#endif

#ifdef DAN_VK_ARM
/* 4 lanes of a pair as in dan_vk_dot_f32_neon: 64 lanes in 16 registers, l + 32, l + 16, l + 8, then l + 4 */
static inline float32x4_t dan_vk_rows_lanes4_neon(const float *a, const float *b, int64_t n) {
    float32x4_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f32(0.0f);
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 16; j++) s[j] = vaddq_f32(s[j], vmulq_f32(vld1q_f32(a + k + 4 * j), vld1q_f32(b + k + 4 * j)));
    for (int j = 0; j < 8; j++) s[j] = vaddq_f32(s[j], s[j + 8]);
    for (int j = 0; j < 4; j++) s[j] = vaddq_f32(s[j], s[j + 4]);
    for (int j = 0; j < 2; j++) s[j] = vaddq_f32(s[j], s[j + 2]);
    return vaddq_f32(s[0], s[1]);
}

static void dan_vk_rows_f32_neon(const float *w, const float *x, float *y, int64_t rows, int64_t cols,
                                 int64_t nv, int64_t i0, int64_t i1) {
    /* the pairs (row, vector) by rows, then by vectors, 4 at a time */
    float32x4_t s[4];
    int64_t pi[4], pv[4];
    int c = 0;
    for (int64_t i = i0; i < i1; i++)
        for (int64_t v = 0; v < nv; v++) {
            s[c] = dan_vk_rows_lanes4_neon(w + i * cols, x + v * cols, cols);
            pi[c] = i;
            pv[c] = v;
            if (++c == 4 || (i == i1 - 1 && v == nv - 1)) {
                float r[4];
                for (int q = c; q < 4; q++) s[q] = vdupq_n_f32(0.0f);
                float32x2_t h[4];
                for (int q = 0; q < 4; q++) h[q] = vadd_f32(vget_low_f32(s[q]), vget_high_f32(s[q]));   /* l + 2 */
                /* l + 1 of the 4 registers at once: pairwise sums of adjacent lanes */
                vst1q_f32(r, vpaddq_f32(vcombine_f32(h[0], h[1]), vcombine_f32(h[2], h[3])));
                for (int q = 0; q < c; q++)
                    y[pv[q] * rows + pi[q]] = dan_vk_rows_tail(w + pi[q] * cols, x + pv[q] * cols, cols, r[q]);
                c = 0;
            }
        }
}
#endif

/* ---- SwiGLU: y[i] = z / (1 + e^-z) * u ----

   The result must be the same as the loop of the program «з / (1.0 + экспонента(-з)) * в» over float
   arrays: every number in double, exp - the exp of the C library (the function «экспонента» of Dan),
   the result rounded to float (and an error if it does not fit). Calling exp for every number costs most
   of the time, so the vector kernels compute e^-z themselves - a table of 2^(j/32) and a polynomial (relative
   error below 2^-50, checked by the test against the exp of the library), then the same operations in double, and keep the rounded float
   only when it cannot differ from the loop's: the exp of a library differs from e^-z by far less than 2^-45
   (glibc, musl, macOS, Windows - below 1 unit of the last place, 2^-52), so the double result of the loop
   differs from ours by less than 2^-44 of it; if ours is farther than 2^-40 of it from every midpoint
   between neighbouring floats, the rounding of both gives the same float. Otherwise (once in tens of
   thousands of numbers), and for |z| > 700, zero, non-finite numbers and results outside 2^-125..2^127,
   the number is computed as the loop does, with the exp of the library. So every variant gives the bits
   of the loop. */

#define DAN_VK_FLT_MAX 3.4028234663852886e38
#define DAN_VK_LOG2E 1.4426950408889634
#define DAN_VK_LN2HI 6.93147180369123816490e-01   /* 32 significant bits: t * LN2HI is exact for |t| < 2^21 */
#define DAN_VK_LN2LO 1.90821492927058770002e-10
#define DAN_VK_EXP_DEG 6
#define DAN_VK_EXP_MAX 700.0
/* 1/k!, k = 0..8 */
static const double dan_vk_exp_c[9] = {1.0, 1.0, 1.0 / 2, 1.0 / 6, 1.0 / 24, 1.0 / 120, 1.0 / 720, 1.0 / 5040, 1.0 / 40320};
/* 2^(j/32), j = 0..31 (nearest doubles) */
static const double dan_vk_exp2t[32] = {1.0, 1.0218971486541166, 1.0442737824274138, 1.0671404006768237, 1.0905077326652577, 1.1143867425958924,
    1.1387886347566916, 1.1637248587775775, 1.189207115002721, 1.215247359980469, 1.241857812073484, 1.2690509571917332,
    1.2968395546510096, 1.3252366431597413, 1.3542555469368927, 1.383909881963832, 1.4142135623730951, 1.4451808069770467,
    1.4768261459394993, 1.5091644275934228, 1.5422108254079407, 1.5759808451078865, 1.6104903319492543, 1.645755478153965,
    1.681792830507429, 1.718619298122478, 1.7562521603732995, 1.7947090750031072, 1.8340080864093424, 1.8741676341103,
    1.9152065613971474, 1.9571441241754002};

/* e^y for |y| <= 700 (other y and NaN give NaN, so the result is computed as the loop does): y = t ln2 / 32 + r,
   |r| <= ln2 / 64, t = 32 k + j, e^y = 2^k 2^(j/32) e^r, e^r - Taylor polynomial of degree deg (DAN_VK_EXP_DEG;
   the test checks that a smaller degree is caught) */
static inline double dan_vk_exp_poly(double y, int deg) {
    if (!(fabs(y) <= DAN_VK_EXP_MAX)) return NAN;
    /* rounded to the nearest integer, ties to even (as the vector variants): |32 y log2 e| < 2^51 */
    double t = (y * (32 * DAN_VK_LOG2E) + 0x1.8p52) - 0x1.8p52;
    double r = (y - t * (DAN_VK_LN2HI / 32)) - t * (DAN_VK_LN2LO / 32);
    double p = dan_vk_exp_c[deg];
    for (int k = deg - 1; k >= 0; k--) p = p * r + dan_vk_exp_c[k];
    int64_t ti = (int64_t)t, j = ti & 31;
    uint64_t bits = (uint64_t)((ti - j) / 32 + 1023) << 52;
    double s;
    memcpy(&s, &bits, 8);
    return p * dan_vk_exp2t[j] * s;
}

/* the number as the loop of the program computes it; *bad - the first index whose result does not fit a float */
static inline void dan_vk_swiglu_exact(const float *z, const float *u, float *y, int64_t i, int64_t *bad) {
    double x = (double)z[i];
    double r = x / (1.0 + exp(-x)) * (double)u[i];
    if (*bad < 0 && isfinite(r) && fabs(r) > DAN_VK_FLT_MAX) *bad = i;
    y[i] = (float)r;
}

/* e[i] = e^-z[i] by the polynomial (plain C; the test also calls it with a smaller degree) */
static void dan_vk_expneg_c(const float *z, double *e, int64_t n, int deg) {
    for (int64_t i = 0; i < n; i++) e[i] = dan_vk_exp_poly(-(double)z[i], deg);
}

/* plain C: the loop with the exp of the library (the polynomial without vector instructions is not faster) */
static int64_t dan_vk_swiglu_f32_c(const float *z, const float *u, float *y, int64_t b, int64_t e) {
    int64_t bad = -1;
    for (int64_t i = b; i < e; i++) dan_vk_swiglu_exact(z, u, y, i, &bad);
    return bad;
}

#define DAN_VK_SWIGLU_CHUNK 64   /* чисел за проход векторных вариантов swiglu */
#ifdef DAN_VK_X86
/* e[i] = e^-z[i], 16 numbers at a time (4 independent vectors keep the units busy): the operations of
   dan_vk_exp_poly in every lane */
DAN_VK_AVX2 static void dan_vk_expneg_avx2(const float *z, double *e, int64_t n) {
    const __m256d mx = _mm256_set1_pd(DAN_VK_EXP_MAX), mn = _mm256_set1_pd(-DAN_VK_EXP_MAX), nan = _mm256_set1_pd(NAN);
    int64_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256d y[4], in[4], t[4], r[4], p[4];
        for (int q = 0; q < 4; q++) {
            y[q] = _mm256_sub_pd(_mm256_setzero_pd(), _mm256_cvtps_pd(_mm_loadu_ps(z + i + 4 * q)));
            in[q] = _mm256_and_pd(_mm256_cmp_pd(y[q], mx, _CMP_LE_OQ), _mm256_cmp_pd(y[q], mn, _CMP_GE_OQ));
            y[q] = _mm256_and_pd(y[q], in[q]);   /* outside (and NaN) - 0, the result - NaN */
            t[q] = _mm256_round_pd(_mm256_mul_pd(y[q], _mm256_set1_pd(32 * DAN_VK_LOG2E)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            r[q] = _mm256_sub_pd(_mm256_sub_pd(y[q], _mm256_mul_pd(t[q], _mm256_set1_pd(DAN_VK_LN2HI / 32))),
                                 _mm256_mul_pd(t[q], _mm256_set1_pd(DAN_VK_LN2LO / 32)));
            p[q] = _mm256_set1_pd(dan_vk_exp_c[DAN_VK_EXP_DEG]);
        }
        for (int k = DAN_VK_EXP_DEG - 1; k >= 0; k--) {
            __m256d c = _mm256_set1_pd(dan_vk_exp_c[k]);
            for (int q = 0; q < 4; q++) p[q] = _mm256_add_pd(_mm256_mul_pd(p[q], r[q]), c);
        }
        for (int q = 0; q < 4; q++) {
            __m128i ti = _mm256_cvtpd_epi32(t[q]);
            __m256d tab = _mm256_i32gather_pd(dan_vk_exp2t, _mm_and_si128(ti, _mm_set1_epi32(31)), 8);
            __m256i s = _mm256_slli_epi64(_mm256_add_epi64(_mm256_cvtepi32_epi64(_mm_srai_epi32(ti, 5)), _mm256_set1_epi64x(1023)), 52);
            __m256d v = _mm256_mul_pd(_mm256_mul_pd(p[q], tab), _mm256_castsi256_pd(s));
            _mm256_storeu_pd(e + i + 4 * q, _mm256_blendv_pd(nan, v, in[q]));
        }
    }
    dan_vk_expneg_c(z + i, e + i, n - i, DAN_VK_EXP_DEG);
}

/* mask of the lanes whose float is sure: |r| in 2^-125..2^127, and r is farther than 2^-40 |r| from the
   midpoints between the float nearest to it and its neighbours, so every number that close rounds to it */
DAN_VK_AVX2 static int dan_vk_sure_avx2(__m256d r) {
    const __m256d nosign = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7fffffffffffffffLL));
    __m256d a = _mm256_and_pd(r, nosign);
    __m256d in = _mm256_and_pd(_mm256_cmp_pd(a, _mm256_set1_pd(0x1p-125), _CMP_GE_OQ),
                               _mm256_cmp_pd(a, _mm256_set1_pd(0x1p127), _CMP_LE_OQ));
    __m128i fb = _mm_castps_si128(_mm256_cvtpd_ps(a));
    __m256d f = _mm256_cvtps_pd(_mm_castsi128_ps(fb));
    __m256d fu = _mm256_cvtps_pd(_mm_castsi128_ps(_mm_add_epi32(fb, _mm_set1_epi32(1))));
    __m256d fd = _mm256_cvtps_pd(_mm_castsi128_ps(_mm_sub_epi32(fb, _mm_set1_epi32(1))));
    __m256d tol = _mm256_mul_pd(a, _mm256_set1_pd(0x1p-40)), half = _mm256_set1_pd(0.5);
    __m256d lo = _mm256_add_pd(_mm256_mul_pd(_mm256_add_pd(f, fd), half), tol);
    __m256d hi = _mm256_sub_pd(_mm256_mul_pd(_mm256_add_pd(f, fu), half), tol);
    in = _mm256_and_pd(in, _mm256_and_pd(_mm256_cmp_pd(a, lo, _CMP_GT_OQ), _mm256_cmp_pd(a, hi, _CMP_LT_OQ)));
    return _mm256_movemask_pd(in);
}

DAN_VK_AVX2 static int64_t dan_vk_swiglu_f32_avx2(const float *z, const float *u, float *y, int64_t b, int64_t e) {
    int64_t bad = -1, i = b;
    double ex[DAN_VK_SWIGLU_CHUNK];
    const __m256d one = _mm256_set1_pd(1.0);
    while (i + 4 <= e) {
        int64_t m = (e - i) / 4 * 4;
        if (m > DAN_VK_SWIGLU_CHUNK) m = DAN_VK_SWIGLU_CHUNK;
        dan_vk_expneg_avx2(z + i, ex, m);
        for (int64_t k = 0; k < m; k += 4, i += 4) {
            __m256d x = _mm256_cvtps_pd(_mm_loadu_ps(z + i));
            __m256d r = _mm256_mul_pd(_mm256_div_pd(x, _mm256_add_pd(one, _mm256_loadu_pd(ex + k))), _mm256_cvtps_pd(_mm_loadu_ps(u + i)));
            _mm_storeu_ps(y + i, _mm256_cvtpd_ps(r));
            int ok = dan_vk_sure_avx2(r);
            if (ok != 15)
                for (int l = 0; l < 4; l++) if (!(ok >> l & 1)) dan_vk_swiglu_exact(z, u, y, i + l, &bad);
        }
    }
    for (; i < e; i++) dan_vk_swiglu_exact(z, u, y, i, &bad);
    return bad;
}

DAN_VK_AVX512 static void dan_vk_expneg_avx512(const float *z, double *e, int64_t n) {
    const __m512d mx = _mm512_set1_pd(DAN_VK_EXP_MAX), nan = _mm512_set1_pd(NAN);
    int64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m512d y[4], t[4], r[4], p[4];
        __mmask8 in[4];
        for (int q = 0; q < 4; q++) {
            y[q] = _mm512_sub_pd(_mm512_setzero_pd(), _mm512_cvtps_pd(_mm256_loadu_ps(z + i + 8 * q)));
            in[q] = _mm512_cmp_pd_mask(_mm512_abs_pd(y[q]), mx, _CMP_LE_OQ);
            y[q] = _mm512_maskz_mov_pd(in[q], y[q]);
            t[q] = _mm512_roundscale_pd(_mm512_mul_pd(y[q], _mm512_set1_pd(32 * DAN_VK_LOG2E)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            r[q] = _mm512_sub_pd(_mm512_sub_pd(y[q], _mm512_mul_pd(t[q], _mm512_set1_pd(DAN_VK_LN2HI / 32))),
                                 _mm512_mul_pd(t[q], _mm512_set1_pd(DAN_VK_LN2LO / 32)));
            p[q] = _mm512_set1_pd(dan_vk_exp_c[DAN_VK_EXP_DEG]);
        }
        for (int k = DAN_VK_EXP_DEG - 1; k >= 0; k--) {
            __m512d c = _mm512_set1_pd(dan_vk_exp_c[k]);
            for (int q = 0; q < 4; q++) p[q] = _mm512_add_pd(_mm512_mul_pd(p[q], r[q]), c);
        }
        for (int q = 0; q < 4; q++) {
            __m256i ti = _mm512_cvtpd_epi32(t[q]);
            __m512d tab = _mm512_i32gather_pd(_mm256_and_si256(ti, _mm256_set1_epi32(31)), dan_vk_exp2t, 8);
            __m512i s = _mm512_slli_epi64(_mm512_add_epi64(_mm512_cvtepi32_epi64(_mm256_srai_epi32(ti, 5)), _mm512_set1_epi64(1023)), 52);
            __m512d v = _mm512_mul_pd(_mm512_mul_pd(p[q], tab), _mm512_castsi512_pd(s));
            _mm512_storeu_pd(e + i + 8 * q, _mm512_mask_mov_pd(nan, in[q], v));
        }
    }
    dan_vk_expneg_c(z + i, e + i, n - i, DAN_VK_EXP_DEG);
}

DAN_VK_AVX512 static __mmask8 dan_vk_sure_avx512(__m512d r) {
    __m512d a = _mm512_abs_pd(r);
    __mmask8 in = _mm512_cmp_pd_mask(a, _mm512_set1_pd(0x1p-125), _CMP_GE_OQ) & _mm512_cmp_pd_mask(a, _mm512_set1_pd(0x1p127), _CMP_LE_OQ);
    __m256i fb = _mm256_castps_si256(_mm512_cvtpd_ps(a));
    __m512d f = _mm512_cvtps_pd(_mm256_castsi256_ps(fb));
    __m512d fu = _mm512_cvtps_pd(_mm256_castsi256_ps(_mm256_add_epi32(fb, _mm256_set1_epi32(1))));
    __m512d fd = _mm512_cvtps_pd(_mm256_castsi256_ps(_mm256_sub_epi32(fb, _mm256_set1_epi32(1))));
    __m512d tol = _mm512_mul_pd(a, _mm512_set1_pd(0x1p-40)), half = _mm512_set1_pd(0.5);
    __m512d lo = _mm512_add_pd(_mm512_mul_pd(_mm512_add_pd(f, fd), half), tol);
    __m512d hi = _mm512_sub_pd(_mm512_mul_pd(_mm512_add_pd(f, fu), half), tol);
    return in & _mm512_cmp_pd_mask(a, lo, _CMP_GT_OQ) & _mm512_cmp_pd_mask(a, hi, _CMP_LT_OQ);
}

DAN_VK_AVX512 static int64_t dan_vk_swiglu_f32_avx512(const float *z, const float *u, float *y, int64_t b, int64_t e) {
    int64_t bad = -1, i = b;
    double ex[DAN_VK_SWIGLU_CHUNK];
    const __m512d one = _mm512_set1_pd(1.0);
    while (i + 8 <= e) {
        int64_t m = (e - i) / 8 * 8;
        if (m > DAN_VK_SWIGLU_CHUNK) m = DAN_VK_SWIGLU_CHUNK;
        dan_vk_expneg_avx512(z + i, ex, m);
        for (int64_t k = 0; k < m; k += 8, i += 8) {
            __m512d x = _mm512_cvtps_pd(_mm256_loadu_ps(z + i));
            __m512d r = _mm512_mul_pd(_mm512_div_pd(x, _mm512_add_pd(one, _mm512_loadu_pd(ex + k))), _mm512_cvtps_pd(_mm256_loadu_ps(u + i)));
            _mm256_storeu_ps(y + i, _mm512_cvtpd_ps(r));
            unsigned ok = dan_vk_sure_avx512(r);
            if (ok != 255)
                for (int l = 0; l < 8; l++) if (!(ok >> l & 1)) dan_vk_swiglu_exact(z, u, y, i + l, &bad);
        }
    }
    for (; i < e; i++) dan_vk_swiglu_exact(z, u, y, i, &bad);
    return bad;
}
#endif

#ifdef DAN_VK_ARM
static void dan_vk_expneg_neon(const float *z, double *e, int64_t n) {
    const float64x2_t mx = vdupq_n_f64(DAN_VK_EXP_MAX), nan = vdupq_n_f64(NAN);
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        float64x2_t y[4], t[4], r[4], p[4];
        uint64x2_t in[4];
        for (int q = 0; q < 4; q++) {
            y[q] = vnegq_f64(vcvt_f64_f32(vld1_f32(z + i + 2 * q)));
            in[q] = vcleq_f64(vabsq_f64(y[q]), mx);
            y[q] = vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(y[q]), in[q]));
            t[q] = vrndnq_f64(vmulq_f64(y[q], vdupq_n_f64(32 * DAN_VK_LOG2E)));
            r[q] = vsubq_f64(vsubq_f64(y[q], vmulq_f64(t[q], vdupq_n_f64(DAN_VK_LN2HI / 32))),
                             vmulq_f64(t[q], vdupq_n_f64(DAN_VK_LN2LO / 32)));
            p[q] = vdupq_n_f64(dan_vk_exp_c[DAN_VK_EXP_DEG]);
        }
        for (int k = DAN_VK_EXP_DEG - 1; k >= 0; k--) {
            float64x2_t c = vdupq_n_f64(dan_vk_exp_c[k]);
            for (int q = 0; q < 4; q++) p[q] = vaddq_f64(vmulq_f64(p[q], r[q]), c);
        }
        for (int q = 0; q < 4; q++) {
            int64x2_t ti = vcvtq_s64_f64(t[q]);
            int64x2_t j = vandq_s64(ti, vdupq_n_s64(31));
            double tv[2] = {dan_vk_exp2t[vgetq_lane_s64(j, 0)], dan_vk_exp2t[vgetq_lane_s64(j, 1)]};
            int64x2_t s = vshlq_n_s64(vaddq_s64(vshrq_n_s64(ti, 5), vdupq_n_s64(1023)), 52);
            float64x2_t v = vmulq_f64(vmulq_f64(p[q], vld1q_f64(tv)), vreinterpretq_f64_s64(s));
            vst1q_f64(e + i + 2 * q, vbslq_f64(in[q], v, nan));
        }
    }
    dan_vk_expneg_c(z + i, e + i, n - i, DAN_VK_EXP_DEG);
}

static uint64x2_t dan_vk_sure_neon(float64x2_t r) {
    float64x2_t a = vabsq_f64(r);
    uint64x2_t in = vandq_u64(vcgeq_f64(a, vdupq_n_f64(0x1p-125)), vcleq_f64(a, vdupq_n_f64(0x1p127)));
    uint32x2_t fb = vreinterpret_u32_f32(vcvt_f32_f64(a));
    float64x2_t f = vcvt_f64_f32(vreinterpret_f32_u32(fb));
    float64x2_t fu = vcvt_f64_f32(vreinterpret_f32_u32(vadd_u32(fb, vdup_n_u32(1))));
    float64x2_t fd = vcvt_f64_f32(vreinterpret_f32_u32(vsub_u32(fb, vdup_n_u32(1))));
    float64x2_t tol = vmulq_f64(a, vdupq_n_f64(0x1p-40)), half = vdupq_n_f64(0.5);
    float64x2_t lo = vaddq_f64(vmulq_f64(vaddq_f64(f, fd), half), tol);
    float64x2_t hi = vsubq_f64(vmulq_f64(vaddq_f64(f, fu), half), tol);
    return vandq_u64(in, vandq_u64(vcgtq_f64(a, lo), vcltq_f64(a, hi)));
}

static int64_t dan_vk_swiglu_f32_neon(const float *z, const float *u, float *y, int64_t b, int64_t e) {
    int64_t bad = -1, i = b;
    double ex[DAN_VK_SWIGLU_CHUNK];
    const float64x2_t one = vdupq_n_f64(1.0);
    while (i + 2 <= e) {
        int64_t m = (e - i) / 2 * 2;
        if (m > DAN_VK_SWIGLU_CHUNK) m = DAN_VK_SWIGLU_CHUNK;
        dan_vk_expneg_neon(z + i, ex, m);
        for (int64_t k = 0; k < m; k += 2, i += 2) {
            float64x2_t x = vcvt_f64_f32(vld1_f32(z + i));
            float64x2_t r = vmulq_f64(vdivq_f64(x, vaddq_f64(one, vld1q_f64(ex + k))), vcvt_f64_f32(vld1_f32(u + i)));
            vst1_f32(y + i, vcvt_f32_f64(r));
            uint64x2_t ok = dan_vk_sure_neon(r);
            if (!vgetq_lane_u64(ok, 0)) dan_vk_swiglu_exact(z, u, y, i, &bad);
            if (!vgetq_lane_u64(ok, 1)) dan_vk_swiglu_exact(z, u, y, i + 1, &bad);
        }
    }
    for (; i < e; i++) dan_vk_swiglu_exact(z, u, y, i, &bad);
    return bad;
}
#endif

/* the variant of the processor (by the dot product of the chosen table, as the rows below) */
static int64_t dan_vk_swiglu_f32_part_v(const float *z, const float *u, float *y, int64_t b, int64_t e) {
    float (*dot)(const float *, const float *, int64_t) = dan_vk()->dot_f32;
#ifdef DAN_VK_X86
    if (dot == dan_vk_dot_f32_avx2) return dan_vk_swiglu_f32_avx2(z, u, y, b, e);
    if (dot == dan_vk_dot_f32_avx512) return dan_vk_swiglu_f32_avx512(z, u, y, b, e);
#endif
#ifdef DAN_VK_ARM
    if (dot == dan_vk_dot_f32_neon) return dan_vk_swiglu_f32_neon(z, u, y, b, e);
#endif
    (void)dot;
    return dan_vk_swiglu_f32_c(z, u, y, b, e);
}

#define DAN_VK_SWIGLU_PART 2048   /* numbers in a part of the pool */
typedef struct { const float *z, *u; float *y; int64_t n; _Atomic int64_t bad; } dan_vk_swiglu_job;

static void dan_vk_swiglu_f32_part(void *c, int64_t b, int64_t e) {
    dan_vk_swiglu_job *j = (dan_vk_swiglu_job *)c;
    int64_t to = e * DAN_VK_SWIGLU_PART < j->n ? e * DAN_VK_SWIGLU_PART : j->n;
    int64_t bad = dan_vk_swiglu_f32_part_v(j->z, j->u, j->y, b * DAN_VK_SWIGLU_PART, to);
    if (bad < 0) return;
    int64_t was = atomic_load_explicit(&j->bad, memory_order_relaxed);
    while ((was < 0 || bad < was) && !atomic_compare_exchange_weak_explicit(&j->bad, &was, bad, memory_order_relaxed, memory_order_relaxed)) {}
}

/* y = SwiGLU of z and u (n numbers each), on all cores; the first index of a result out of float, or -1 */
static int64_t dan_vk_swiglu_f32(const float *z, const float *u, float *y, int64_t n) {
    dan_vk_swiglu_job j = {z, u, y, n, -1};
    if (n <= 0) return -1;
    dan_pool_run((n + DAN_VK_SWIGLU_PART - 1) / DAN_VK_SWIGLU_PART, 4, dan_vk_swiglu_f32_part, &j);
    return atomic_load_explicit(&j.bad, memory_order_relaxed);
}

static int64_t dan_vk_swiglu_f64(const double *z, const double *u, double *y, int64_t n) {
    for (int64_t i = 0; i < n; i++) { double x = z[i]; y[i] = x / (1.0 + exp(-x)) * u[i]; }
    return -1;
}

typedef struct { const float *w, *x; float *y; int64_t rows, cols, nv; } dan_vk_rows_job;

/* DAN_YADRA_STROKI=0 - the kernels below are off (a dot_f32 for each pair, matmat by the tiles of the
   variant); read once: -1 - not read yet (the test sets it directly) */
static _Atomic int dan_vk_stroki = -1;
static int dan_vk_stroki_on(void) {
    int v = atomic_load_explicit(&dan_vk_stroki, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("DAN_YADRA_STROKI");
        v = e == NULL || atoi(e) != 0;
        atomic_store_explicit(&dan_vk_stroki, v, memory_order_relaxed);
    }
    return v;
}

static void dan_vk_rows_f32_part(void *c, int64_t b, int64_t e) {
    dan_vk_rows_job *j = (dan_vk_rows_job *)c;
    float (*dot)(const float *, const float *, int64_t) = dan_vk()->dot_f32;
    if (dan_vk_stroki_on()) {
#ifdef DAN_VK_X86
        if (dot == dan_vk_dot_f32_avx2) { dan_vk_rows_f32_avx2(j->w, j->x, j->y, j->rows, j->cols, j->nv, b, e); return; }
        if (dot == dan_vk_dot_f32_avx512) { dan_vk_rows_f32_avx512(j->w, j->x, j->y, j->rows, j->cols, j->nv, b, e); return; }
#endif
#ifdef DAN_VK_ARM
        if (dot == dan_vk_dot_f32_neon) { dan_vk_rows_f32_neon(j->w, j->x, j->y, j->rows, j->cols, j->nv, b, e); return; }
#endif
    }
    dan_vk_rows_f32_c(j->w, j->x, j->y, j->rows, j->cols, j->nv, b, e, dot);
}

/* y[v * rows + i] = row i of w . vector v of x; DAN_YADRA_STROKI=0 - a dot_f32 for each pair */
static void dan_vk_rows_f32(const float *w, const float *x, float *y, int64_t rows, int64_t cols, int64_t nv) {
    dan_vk_rows_job j = {w, x, y, rows, cols, nv};
    int64_t work = cols * (nv > 0 ? nv : 1);
    dan_pool_run(rows, work >= 65536 ? 1 : 65536 / work, dan_vk_rows_f32_part, &j);
}

/* C (m x n) = A (m x k) B (k x n) for a few rows m (DAN_VK_FEW at most), columns
   [j0, j1): every element adds its k products in order from zero, as
   dan_vk_mm_f32_c, but the rows are not left to plain C when there are fewer
   of them than the tile of the variant has, and the strip of B is not copied
   (matmat of the weights of a group of heads by the values in the attention) */
#define DAN_VK_FEW 8

#ifdef DAN_VK_X86
/* MR rows by 16 columns */
#define DAN_VK_FEW_AVX2(MR) do { \
    __m256 c_[MR][2]; \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 2; q_++) c_[r_][q_] = _mm256_setzero_ps(); \
    for (int64_t p_ = 0; p_ < k; p_++) { \
        __m256 b_[2]; \
        for (int q_ = 0; q_ < 2; q_++) b_[q_] = _mm256_loadu_ps(B + p_ * n + j + 8 * q_); \
        for (int r_ = 0; r_ < MR; r_++) { \
            __m256 a_ = _mm256_set1_ps(A[(i + r_) * k + p_]); \
            for (int q_ = 0; q_ < 2; q_++) c_[r_][q_] = _mm256_add_ps(c_[r_][q_], _mm256_mul_ps(a_, b_[q_])); \
        } \
    } \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 2; q_++) _mm256_storeu_ps(C + (i + r_) * n + j + 8 * q_, c_[r_][q_]); \
} while (0)

DAN_VK_AVX2 static int64_t dan_vk_few_f32_avx2(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k,
                                               int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 16 * 16;
    for (int64_t j = j0; j < je; j += 16) {
        int64_t i = 0;
        for (; i + 4 <= m; i += 4) DAN_VK_FEW_AVX2(4);
        if (m - i == 3) DAN_VK_FEW_AVX2(3);
        else if (m - i == 2) DAN_VK_FEW_AVX2(2);
        else if (m - i == 1) DAN_VK_FEW_AVX2(1);
    }
    return je;
}

/* MR rows by 64 columns */
#define DAN_VK_FEW_AVX512(MR) do { \
    __m512 c_[MR][4]; \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 4; q_++) c_[r_][q_] = _mm512_setzero_ps(); \
    for (int64_t p_ = 0; p_ < k; p_++) { \
        __m512 b_[4]; \
        for (int q_ = 0; q_ < 4; q_++) b_[q_] = _mm512_loadu_ps(B + p_ * n + j + 16 * q_); \
        for (int r_ = 0; r_ < MR; r_++) { \
            __m512 a_ = _mm512_set1_ps(A[(i + r_) * k + p_]); \
            for (int q_ = 0; q_ < 4; q_++) c_[r_][q_] = _mm512_add_ps(c_[r_][q_], _mm512_mul_ps(a_, b_[q_])); \
        } \
    } \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 4; q_++) _mm512_storeu_ps(C + (i + r_) * n + j + 16 * q_, c_[r_][q_]); \
} while (0)

DAN_VK_AVX512 static int64_t dan_vk_few_f32_avx512(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k,
                                                   int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 64 * 64;
    for (int64_t j = j0; j < je; j += 64) {
        int64_t i = 0;
        for (; i + 4 <= m; i += 4) DAN_VK_FEW_AVX512(4);
        if (m - i == 3) DAN_VK_FEW_AVX512(3);
        else if (m - i == 2) DAN_VK_FEW_AVX512(2);
        else if (m - i == 1) DAN_VK_FEW_AVX512(1);
    }
    /* the rest by 16 columns (AVX2 instructions of the same sums) */
    return dan_vk_few_f32_avx2(A, B, C, m, n, k, je, j1);
}
#endif

#ifdef DAN_VK_ARM
/* MR rows by 16 columns */
#define DAN_VK_FEW_NEON(MR) do { \
    float32x4_t c_[MR][4]; \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 4; q_++) c_[r_][q_] = vdupq_n_f32(0.0f); \
    for (int64_t p_ = 0; p_ < k; p_++) { \
        float32x4_t b_[4]; \
        for (int q_ = 0; q_ < 4; q_++) b_[q_] = vld1q_f32(B + p_ * n + j + 4 * q_); \
        for (int r_ = 0; r_ < MR; r_++) { \
            float32x4_t a_ = vdupq_n_f32(A[(i + r_) * k + p_]); \
            for (int q_ = 0; q_ < 4; q_++) c_[r_][q_] = vaddq_f32(c_[r_][q_], vmulq_f32(a_, b_[q_])); \
        } \
    } \
    for (int r_ = 0; r_ < MR; r_++) for (int q_ = 0; q_ < 4; q_++) vst1q_f32(C + (i + r_) * n + j + 4 * q_, c_[r_][q_]); \
} while (0)

static int64_t dan_vk_few_f32_neon(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k,
                                   int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 16 * 16;
    for (int64_t j = j0; j < je; j += 16) {
        int64_t i = 0;
        for (; i + 4 <= m; i += 4) DAN_VK_FEW_NEON(4);
        if (m - i == 3) DAN_VK_FEW_NEON(3);
        else if (m - i == 2) DAN_VK_FEW_NEON(2);
        else if (m - i == 1) DAN_VK_FEW_NEON(1);
    }
    return je;
}
#endif

static void dan_vk_few_f32_part(void *c, int64_t b, int64_t e) {
    dan_vk_mm_job *jb = (dan_vk_mm_job *)c;
    const float *A = (const float *)jb->a, *B = (const float *)jb->b;
    float *C = (float *)jb->c;
    int64_t m = jb->m, n = jb->n, k = jb->k, j0 = b * 64, j1 = e * 64 < n ? e * 64 : n, je = j0;
    const dan_vk_table *t = dan_vk();
#ifdef DAN_VK_X86
    if (t->dot_f32 == dan_vk_dot_f32_avx512) je = dan_vk_few_f32_avx512(A, B, C, m, n, k, j0, j1);
    else if (t->dot_f32 == dan_vk_dot_f32_avx2) je = dan_vk_few_f32_avx2(A, B, C, m, n, k, j0, j1);
#endif
#ifdef DAN_VK_ARM
    if (t->dot_f32 == dan_vk_dot_f32_neon) je = dan_vk_few_f32_neon(A, B, C, m, n, k, j0, j1);
#endif
    (void)t;
    DAN_VK_MM_EDGE(float, A, B, C, n, k, 0, m, je, j1);
}

/* 1 if done: m is at most DAN_VK_FEW (DAN_YADRA_STROKI=0 - not used) */
static int dan_vk_few_f32(const float *a, const float *b, float *c, int64_t m, int64_t n, int64_t k) {
    if (m > DAN_VK_FEW || !dan_vk_stroki_on()) return 0;
    dan_vk_mm_job j = {a, b, c, m, n, k};
    dan_pool_run((n + 63) / 64, dan_vk_min_strips(m, k), dan_vk_few_f32_part, &j);
    return 1;
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

/* x (n numbers, n a multiple of 32) -> n / 32 blocks of 34 bytes. На x86-64 - командами SSE2 (есть у
   любого x86-64): наибольший модуль - maxps (те же сравнения a > amax), произведение v * id - то же
   умножение, ограничение -127..127 - minps/maxps, округление - cvtps2dq до ближайшего чётного, как lrintf
   (режим округления по умолчанию); NaN - в 0, как ниже. Байты те же, что у кода C (замеры/результаты.md,
   «Поток 26В»: перевод вектора в 8 бит был 5% времени чтения запроса Qwen2.5 0.5B Q8_0 на EPYC 7763). */
#ifdef DAN_VK_X86
static void dan_vk_q8_quantize(const float *x, uint8_t *out, int64_t n) {
    const __m128 sign = _mm_set1_ps(-0.0f), lo = _mm_set1_ps(-127.0f), hi = _mm_set1_ps(127.0f);
    for (int64_t b = 0; b < n / DAN_Q8_BLOCK; b++) {
        const float *v = x + b * DAN_Q8_BLOCK;
        uint8_t *o = out + b * DAN_Q8_BYTES;
        __m128 m = _mm_setzero_ps();
        /* maxps(a, m) - a, если a > m, иначе m: как «если a > amax», NaN не становится наибольшим */
        for (int i = 0; i < DAN_Q8_BLOCK; i += 4) m = _mm_max_ps(_mm_andnot_ps(sign, _mm_loadu_ps(v + i)), m);
        float mv[4];
        _mm_storeu_ps(mv, m);
        float amax = 0.0f;
        for (int i = 0; i < 4; i++) if (mv[i] > amax) amax = mv[i];
        float d = amax / 127.0f, id = d != 0.0f ? 1.0f / d : 0.0f;
        if (!isfinite(id) || !isfinite(d)) { d = 0.0f; id = 0.0f; }   /* scale below or above the range */
        uint16_t h = dan_f32_to_f16(d);
        memcpy(o, &h, 2);
        const __m128 vid = _mm_set1_ps(id);
        __m128i r[8];
        for (int k = 0; k < 8; k++) {
            __m128 q = _mm_mul_ps(_mm_loadu_ps(v + 4 * k), vid);
            __m128 ok = _mm_cmpord_ps(q, q);
            q = _mm_and_ps(_mm_min_ps(_mm_max_ps(q, lo), hi), ok);   /* NaN -> 0 */
            r[k] = _mm_cvtps_epi32(q);
        }
        __m128i a = _mm_packs_epi16(_mm_packs_epi32(r[0], r[1]), _mm_packs_epi32(r[2], r[3]));
        __m128i c = _mm_packs_epi16(_mm_packs_epi32(r[4], r[5]), _mm_packs_epi32(r[6], r[7]));
        _mm_storeu_si128((__m128i *)(void *)(o + 2), a);
        _mm_storeu_si128((__m128i *)(void *)(o + 18), c);
    }
}
#else
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
#endif

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
    int64_t row = j->blocks * DAN_Q8_BYTES;
    for (int64_t i = b; i < e; i++) j->y[i] = t->q8_dot(j->w + i * row, j->x, j->blocks);
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
#ifdef DAN_VK_X86
        /* наибольший модуль - maxps (числа конечны), затем первое число с этим модулем - его знак,
           как в цикле ниже (первое из равных по модулю) */
        {
            const __m128 sign = _mm_set1_ps(-0.0f);
            __m128 m = _mm_setzero_ps();
            for (int j = 0; j < DAN_QK; j += 4) m = _mm_max_ps(_mm_andnot_ps(sign, _mm_loadu_ps(x + j)), m);
            float mv[4];
            _mm_storeu_ps(mv, m);
            for (int j = 0; j < 4; j++) if (mv[j] > amax) amax = mv[j];
            if (amax != 0.0f)
                for (int j = 0; j < DAN_QK; j++)
                    if (fabsf(x[j]) == amax) { max = x[j]; break; }
        }
#else
        for (int j = 0; j < DAN_QK; j++) {
            float ax = fabsf(x[j]);
            if (ax > amax) { amax = ax; max = x[j]; }
        }
#endif
        float iscale = amax != 0.0f ? -127.0f / max : 0.0f;
        if (!isfinite(iscale) || iscale == 0.0f) {
            memset(o, 0, sizeof *o);
            continue;
        }
#ifdef DAN_VK_X86
        /* v = iscale * x, |v| <= 127,5: cvtps2dq - ближайшее целое, у равноудалённых - чётное, как
           прибавление 1,5 * 2^23 ниже; больше 127 - 127 (packs насыщает так же, чисел меньше -128 нет) */
        {
            const __m128 vs = _mm_set1_ps(iscale);
            const __m128i c127 = _mm_set1_epi32(127);
            for (int j = 0; j < DAN_QK; j += 16) {
                __m128i r[4];
                for (int k = 0; k < 4; k++) {
                    __m128i t = _mm_cvtps_epi32(_mm_mul_ps(vs, _mm_loadu_ps(x + j + 4 * k)));
                    __m128i gt = _mm_cmpgt_epi32(t, c127);
                    r[k] = _mm_or_si128(_mm_andnot_si128(gt, t), _mm_and_si128(gt, c127));
                }
                __m128i q = _mm_packs_epi16(_mm_packs_epi32(r[0], r[1]), _mm_packs_epi32(r[2], r[3]));
                _mm_storeu_si128((__m128i *)(void *)(o->qs + j), q);
                /* сумма 16 чисел: знаковые байты в 16 бит и попарно */
                __m128i lo16 = _mm_srai_epi16(_mm_unpacklo_epi8(q, q), 8), hi16 = _mm_srai_epi16(_mm_unpackhi_epi8(q, q), 8);
                __m128i s32 = _mm_madd_epi16(_mm_add_epi16(lo16, hi16), _mm_set1_epi16(1));
                s32 = _mm_add_epi32(s32, _mm_shuffle_epi32(s32, 0x4E));
                s32 = _mm_add_epi32(s32, _mm_shuffle_epi32(s32, 0xB1));
                o->bsums[j / 16] = (int16_t)_mm_cvtsi128_si32(s32);
            }
            o->d = 1.0f / iscale;
            continue;
        }
#endif
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

/* Q2_K, Q3_K, IQ4_XS: те же действия, что у dequantize_row_q2_K, _q3_K, _iq4_xs ggml */
static void dan_vk_q2k_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q2K_BYTES) {
        float d = dan_vk_half(in + 80), min = dan_vk_half(in + 82);
        for (int n = 0; n < 2; n++)
            for (int j = 0; j < 4; j++)
                for (int h = 0; h < 2; h++) {
                    uint8_t sc = in[8 * n + 2 * j + h];
                    float dl = d * (float)(sc & 0xF), ml = min * (float)(sc >> 4);
                    for (int l = 0; l < 16; l++) {
                        float t = dl * (float)((in[16 + 32 * n + 16 * h + l] >> (2 * j)) & 3);
                        *y++ = t - ml;
                    }
                }
    }
}

static void dan_vk_q3k_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_Q3K_BYTES) {
        float d = dan_vk_half(in + 108);
        int8_t sc[16];
        dan_vk_q3k_scales(in + 96, sc);
        for (int n = 0; n < 2; n++)
            for (int j = 0; j < 4; j++)
                for (int h = 0; h < 2; h++) {
                    float dl = d * (float)sc[8 * n + 2 * j + h];
                    for (int l = 0; l < 16; l++) *y++ = dl * (float)dan_vk_q3k_q(in, n, j, 16 * h + l);
                }
    }
}

static void dan_vk_iq4xs_dequantize(const uint8_t *in, float *y, int64_t blocks) {
    for (int64_t i = 0; i < blocks; i++, in += DAN_IQ4XS_BYTES) {
        float d = dan_vk_half(in);
        int8_t sc[8];
        dan_vk_iq4xs_scales(in, sc);
        for (int b = 0; b < 8; b++, y += 32) {
            float dl = d * (float)sc[b];
            for (int j = 0; j < 16; j++) {
                y[j] = dl * (float)dan_vk_iq4nl[in[8 + 16 * b + j] & 0xF];
                y[j + 16] = dl * (float)dan_vk_iq4nl[in[8 + 16 * b + j] >> 4];
            }
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


/* the vector (n numbers, a multiple of the block) into 8 bits for the format */
static void dan_vk_fmt_quantize_x(int fmt, const float *x, uint8_t *out, int64_t n) {
    switch (fmt) {
    case DAN_FMT_Q4K: case DAN_FMT_Q5K: case DAN_FMT_Q6K:
    case DAN_FMT_Q2K: case DAN_FMT_Q3K: case DAN_FMT_IQ4XS: dan_vk_q8k_quantize(x, (dan_q8k *)(void *)out, n); break;
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
    case DAN_FMT_Q2K: dan_vk_q2k_dequantize(in, y, blocks); break;
    case DAN_FMT_Q3K: dan_vk_q3k_dequantize(in, y, blocks); break;
    case DAN_FMT_IQ4XS: dan_vk_iq4xs_dequantize(in, y, blocks); break;
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
    case DAN_FMT_Q2K: return t->q2k_dot(w, x, blocks);
    case DAN_FMT_Q3K: return t->q3k_dot(w, x, blocks);
    case DAN_FMT_IQ4XS: return t->iq4xs_dot(w, x, blocks);
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

static int64_t dan_vk_tile_bytes(int64_t dflt) {
    const char *s = getenv("DAN_YADRA_PLITKA");
    if (s) {
        long long v = atoll(s);
        if (v > 0) return (int64_t)v;
    }
    return dflt;
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

/* Rows by vectors (the kernels t->rn): the vectors are prepared once into
   units (dan_vk_rn_prep, on the pool), padded with zero vectors up to a
   multiple of rn_vecs; each part of the pool takes groups of rn_rows rows
   (the last group repeats its last row), for every tile of vectors unpacks
   the rows of a group into its buffer and multiplies them by all the vectors
   of the tile. The tile is larger than for a row by vectors: the unpacking
   of a group of rows is shared by more vectors (3 MB: on Neoverse N2 and
   Xeon 8573C with rows of 14336 numbers 1.3-1.5 times faster than 192 KB,
   the same for 4096, замеры/результаты.md). */
#define DAN_VK_RN_TILE (3 * 1024 * 1024)
typedef struct { const uint8_t *w, *x; uint8_t *xp; float *y; int64_t blocks, units, rows, nv, nvp, tile; int fmt, sgn; } dan_vk_rn_job;

static void dan_vk_rn_prep_part(void *c, int64_t b, int64_t e) {
    dan_vk_rn_job *j = (dan_vk_rn_job *)c;
    int64_t xs = j->blocks * dan_vk_fmt_xbytes[j->fmt], us = j->units * DAN_VK_RN_UNIT;
    for (int64_t v = b; v < e; v++) dan_vk_rn_prep(j->fmt, j->x + v * xs, j->blocks, j->xp + v * us, j->sgn);
}

static void dan_vk_rn_job_part(void *c, int64_t b, int64_t e) {
    dan_vk_rn_job *j = (dan_vk_rn_job *)c;
    const dan_vk_table *t = dan_vk();
    int fmt = j->fmt, R = t->rn_rows, NV = t->rn_vecs;
    int64_t row = j->blocks * dan_vk_fmt_bytes[fmt], us = j->units * DAN_VK_RN_UNIT;
    uint8_t *buf = (uint8_t *)malloc((size_t)(R * us));
    if (!buf) {   /* no memory for the unpacked rows: a row by each vector */
        for (int64_t i = b * R; i < e * R && i < j->rows; i++)
            for (int64_t v = 0; v < j->nv; v++)
                j->y[v * j->rows + i] = dan_vk_fmt_dot(t, fmt, j->w + i * row, j->x + v * j->blocks * dan_vk_fmt_xbytes[fmt], j->blocks);
        return;
    }
    float out[64];
    for (int64_t v0 = 0; v0 < j->nvp; v0 += j->tile) {
        int64_t v1 = v0 + j->tile < j->nvp ? v0 + j->tile : j->nvp;
        for (int64_t g = b; g < e; g++) {
            int64_t r0 = g * R;
            for (int a = 0; a < R; a++) {
                int64_t r = r0 + a < j->rows ? r0 + a : j->rows - 1;
                t->unpack(fmt, j->w + r * row, j->blocks, buf + a * us, j->sgn);
            }
            for (int64_t v = v0; v < v1; v += NV) {
                t->rn[fmt](buf, us, j->xp + v * us, us, j->blocks, out);
                for (int a = 0; a < R && r0 + a < j->rows; a++)
                    for (int k = 0; k < NV && v + k < j->nv; k++) j->y[(v + k) * j->rows + r0 + a] = out[a * NV + k];
            }
        }
    }
    free(buf);
}

/* 1 if done by the kernels of rows by vectors */
static int dan_vk_matmul_rn(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    const dan_vk_table *t = dan_vk();
    if (!t->rn[fmt] || nv < t->rn_vecs || rows < 1 || cols < 1 || t->rn_rows * t->rn_vecs > 64) return 0;
    int64_t blocks = cols / dan_vk_fmt_n[fmt], units = dan_vk_rn_units(fmt, blocks), us = units * DAN_VK_RN_UNIT;
    int64_t nvp = (nv + t->rn_vecs - 1) / t->rn_vecs * t->rn_vecs;
    uint8_t *xp = (uint8_t *)malloc((size_t)(nvp * us));
    if (!xp) return 0;
    memset(xp + nv * us, 0, (size_t)((nvp - nv) * us));
    int64_t tile = dan_vk_tile_bytes(DAN_VK_RN_TILE) / us;
    tile = tile < t->rn_vecs ? t->rn_vecs : tile / t->rn_vecs * t->rn_vecs;
    dan_vk_rn_job j = {w, xq, xp, y, blocks, units, rows, nv, nvp, tile, fmt, t->rn_mode};
    dan_pool_run(nv, 1, dan_vk_rn_prep_part, &j);
    int64_t groups = (rows + t->rn_rows - 1) / t->rn_rows, work = cols * nv * t->rn_rows;
    dan_pool_run(groups, work >= 65536 ? 1 : 65536 / work, dan_vk_rn_job_part, &j);
    free(xp);
    return 1;
}

/* Раскладка строк по 16 (ядра t->tl, «раскладка строк по 16» выше): векторы
   готовятся один раз (dan_vk_tl_prep, в пуле) и дополняются нулевыми
   векторами до кратного DAN_VK_TL_V; каждая часть пула берёт группы по
   16 DAN_VK_TL_R строк (последняя группа повторяет свою последнюю строку),
   раскладывает строки группы в свой буфер и умножает их на все векторы.
   DAN_YADRA_RASKLADKA=0 - без раскладки (для замеров). */
typedef struct { const uint8_t *w, *x; uint8_t *xp; float *y; int64_t blocks, rows, nv, nvp, xs; int fmt; } dan_vk_tl_job;

static void dan_vk_tl_prep_part(void *c, int64_t b, int64_t e) {
    dan_vk_tl_job *j = (dan_vk_tl_job *)c;
    int64_t xb = j->blocks * dan_vk_fmt_xbytes[j->fmt];
    for (int64_t v = b; v < e; v++) dan_vk_tl_prep(j->fmt, j->x + v * xb, j->blocks, j->xp + v * j->xs);
}

#ifndef DAN_VK_TL_R
#define DAN_VK_TL_R 1
#define DAN_VK_TL_V 1
#endif
static void dan_vk_tl_job_part(void *c, int64_t b, int64_t e) {
    dan_vk_tl_job *j = (dan_vk_tl_job *)c;
    const dan_vk_table *t = dan_vk();
    int fmt = j->fmt;
    const int G = 16 * DAN_VK_TL_R, V = DAN_VK_TL_V;
    int64_t wb = dan_vk_fmt_bytes[fmt], row = j->blocks * wb, tu = dan_vk_tl_size(fmt);
    uint8_t *buf = (uint8_t *)malloc((size_t)(j->blocks * DAN_VK_TL_R * tu));
    if (!buf) {   /* нет памяти на раскладку: строка на каждый вектор */
        int64_t xb = j->blocks * dan_vk_fmt_xbytes[fmt];
        for (int64_t i = b * G; i < e * G && i < j->rows; i++)
            for (int64_t v = 0; v < j->nv; v++) j->y[v * j->rows + i] = dan_vk_fmt_dot(t, fmt, j->w + i * row, j->x + v * xb, j->blocks);
        return;
    }
    float out[16 * DAN_VK_TL_R * DAN_VK_TL_V];
    for (int64_t g = b; g < e; g++) {
        int64_t r0 = g * G;
        for (int64_t bl = 0; bl < j->blocks; bl++)
            for (int h = 0; h < DAN_VK_TL_R; h++) {
                const uint8_t *p[16];
                for (int i = 0; i < 16; i++) {
                    int64_t r = r0 + 16 * h + i < j->rows ? r0 + 16 * h + i : j->rows - 1;
                    p[i] = j->w + r * row + bl * wb;
                }
                (t->tl_unpack ? t->tl_unpack : dan_vk_tl_unpack_c)(fmt, p, buf + (bl * DAN_VK_TL_R + h) * tu);
            }
        for (int64_t v = 0; v < j->nvp; v += V) {
            t->tl[fmt](buf, j->xp + v * j->xs, j->xs, j->blocks, out);
            for (int k = 0; k < V && v + k < j->nv; k++)
                for (int i = 0; i < G && r0 + i < j->rows; i++) j->y[(v + k) * j->rows + r0 + i] = out[k * G + i];
        }
    }
    free(buf);
}

/* 1 if done by the kernels of rows laid out by 16 */
static int dan_vk_matmul_tl(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    const dan_vk_table *t = dan_vk();
    if (!t->tl[fmt] || nv < DAN_VK_TL_V || rows < 1 || cols < 1) return 0;
    const char *s = getenv("DAN_YADRA_RASKLADKA");
    if (s && atoi(s) == 0) return 0;
    int64_t blocks = cols / dan_vk_fmt_n[fmt], xs = blocks * dan_vk_tl_xu(fmt);
    int64_t nvp = (nv + DAN_VK_TL_V - 1) / DAN_VK_TL_V * DAN_VK_TL_V;
    uint8_t *xp = (uint8_t *)malloc((size_t)(nvp * xs));
    if (!xp) return 0;
    memset(xp + nv * xs, 0, (size_t)((nvp - nv) * xs));
    dan_vk_tl_job j = {w, xq, xp, y, blocks, rows, nv, nvp, xs, fmt};
    dan_pool_run(nv, 1, dan_vk_tl_prep_part, &j);
    int64_t G = 16 * DAN_VK_TL_R, groups = (rows + G - 1) / G, work = cols * nv * G;
    dan_pool_run(groups, work >= 65536 ? 1 : 65536 / work, dan_vk_tl_job_part, &j);
    free(xp);
    return 1;
}

#ifdef DAN_VK_AMX_ON
/* AMX (above): the vectors are prepared once by groups of 16 (padded with zero
   vectors, on the pool), each part of the pool takes groups of 16 rows (the
   last group repeats its last row), for every tile of DAN_VK_AMX_TILE bytes
   of prepared vectors unpacks the rows of a group and multiplies them block
   by block by the groups of the tile, two at a time (the block of the rows
   stays in the first level cache). DAN_YADRA_PLITKA sets the tile. */
#define DAN_VK_AMX_TILE (2 * 1024 * 1024)
#define DAN_VK_AMX_MIN 16   /* vectors: fewer - the kernels of rows by vectors */
typedef struct { const uint8_t *w, *x; uint8_t *xp; float *y; int64_t blocks, rows, nv, groups, tile, xs; int fmt; } dan_vk_amx_job;

static void dan_vk_amx_prep_part(void *c, int64_t b, int64_t e) {
    dan_vk_amx_job *j = (dan_vk_amx_job *)c;
    for (int64_t g = b; g < e; g++) dan_vk_amx_prep(j->fmt, j->x, j->xs, j->nv, j->blocks, g, j->xp + g * j->blocks * DAN_VK_AMX_XU);
}

static void dan_vk_amx_job_part(void *c, int64_t b, int64_t e) {
    dan_vk_amx_job *j = (dan_vk_amx_job *)c;
    int64_t row = j->blocks * dan_vk_fmt_bytes[j->fmt], gs = j->blocks * DAN_VK_AMX_XU;
    int64_t tg = j->tile < j->groups ? j->tile : j->groups;
    int64_t abytes = (j->blocks * DAN_VK_AMX_AU + 63) / 64 * 64;
    /* the unpacked rows, 9 x 256 floats for each group of a tile, 1024 ints */
    uint8_t *buf = (uint8_t *)malloc((size_t)(abytes + tg * 9 * 1024 + 4096 + 64));
    if (!buf) {   /* no memory: a row by each vector */
        const dan_vk_table *t = dan_vk();
        for (int64_t i = b * 16; i < e * 16 && i < j->rows; i++)
            for (int64_t v = 0; v < j->nv; v++)
                j->y[v * j->rows + i] = dan_vk_fmt_dot(t, j->fmt, j->w + i * row, j->x + v * j->xs, j->blocks);
        return;
    }
    uint8_t *a = buf;
    float *acc = (float *)(void *)(buf + abytes);
    int32_t *cs = (int32_t *)(void *)(acc + tg * 9 * 256);
    dan_vk_amx_begin();
    for (int64_t g0 = 0; g0 < j->groups; g0 += j->tile) {
        int64_t g1 = g0 + j->tile < j->groups ? g0 + j->tile : j->groups;
        for (int64_t rg = b; rg < e; rg++) {
            int64_t r0 = 16 * rg;
            int n = j->rows - r0 < 16 ? (int)(j->rows - r0) : 16;
            dan_vk_amx_unpack(j->fmt, j->w + r0 * row, row, n, j->blocks, a);
            dan_vk_amx_k(j->fmt, a, j->xp, gs, g0, g1, j->blocks, acc, cs);
            for (int64_t g = g0; g < g1; g++)
                for (int m = 0; m < n; m++)
                    for (int v = 0; v < 16 && 16 * g + v < j->nv; v++)
                        j->y[(16 * g + v) * j->rows + r0 + m] = acc[9 * 256 * (g - g0) + 8 * 256 + 16 * m + v];
        }
    }
    dan_vk_amx_end();
    free(buf);
}

static int dan_vk_matmul_amx(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    if (dan_vk_fmt_n[fmt] != DAN_QK || nv < DAN_VK_AMX_MIN || rows < 1 || cols < DAN_QK) return 0;
    int64_t blocks = cols / DAN_QK, groups = (nv + 15) / 16, gs = blocks * DAN_VK_AMX_XU;
    uint8_t *xp = (uint8_t *)malloc((size_t)(groups * gs));
    if (!xp) return 0;
    int64_t tile = dan_vk_tile_bytes(DAN_VK_AMX_TILE) / gs;
    tile = tile < 2 ? 2 : tile / 2 * 2;
    dan_vk_amx_job j = {w, xq, xp, y, blocks, rows, nv, groups, tile, blocks * (int64_t)sizeof(dan_q8k), fmt};
    dan_pool_run(groups, 1, dan_vk_amx_prep_part, &j);
    int64_t rgroups = (rows + 15) / 16, work = cols * nv * 16;
    dan_pool_run(rgroups, work >= 65536 ? 1 : 65536 / work, dan_vk_amx_job_part, &j);
    free(xp);
    return 1;
}
#endif

#ifdef DAN_VK_ARM
/* Q8_0 на много векторов (i8mm): векторы готовятся парами один раз (на пуле, группы по 8 - нулевые
   векторы после nv), каждая часть пула берёт группы по 4 строки, для каждой плитки векторов
   раскладывает свои строки парами и умножает на все группы векторов плитки */
#define DAN_VK_MMLA_TILE (512 * 1024)
typedef struct { const uint8_t *w, *x; uint8_t *xp; float *y; int64_t blocks, rows, nv, groups, gs, tile; } dan_vk_mmla_job;

static void dan_vk_mmla_prep_part(void *c, int64_t b, int64_t e) {
    dan_vk_mmla_job *j = (dan_vk_mmla_job *)c;
    int64_t xs = j->blocks * DAN_Q8_BYTES;
    for (int64_t q = b; q < e; q++) {   /* пара векторов 2q, 2q + 1 */
        const uint8_t *v0 = 2 * q < j->nv ? j->x + 2 * q * xs : NULL, *v1 = 2 * q + 1 < j->nv ? j->x + (2 * q + 1) * xs : NULL;
        dan_vk_mmla_pair(v0, v1, j->blocks, DAN_Q8_BYTES, 0, j->xp + q * j->gs);
    }
}

static void dan_vk_mmla_part(void *c, int64_t b, int64_t e) {
    dan_vk_mmla_job *j = (dan_vk_mmla_job *)c;
    int64_t row = j->blocks * DAN_Q8_BYTES;
    uint8_t *a = (uint8_t *)malloc((size_t)(2 * j->gs));
    if (!a) {   /* нет памяти: строка на каждый вектор */
        const dan_vk_table *t = dan_vk();
        for (int64_t i = 4 * b; i < 4 * e && i < j->rows; i++)
            for (int64_t v = 0; v < j->nv; v++) j->y[v * j->rows + i] = t->q8_dot(j->w + i * row, j->x + v * row, j->blocks);
        return;
    }
    float out[32];
    for (int64_t g0 = 0; g0 < j->groups; g0 += j->tile) {
        int64_t g1 = g0 + j->tile < j->groups ? g0 + j->tile : j->groups;
        for (int64_t rg = b; rg < e; rg++) {
            int64_t r0 = 4 * rg;
            for (int p = 0; p < 2; p++) {
                int64_t ra = r0 + 2 * p, rb = ra + 1;
                dan_vk_mmla_pair(ra < j->rows ? j->w + ra * row : NULL, rb < j->rows ? j->w + rb * row : NULL, j->blocks, DAN_Q8_BYTES, 1,
                               a + p * j->gs);
            }
            for (int64_t g = g0; g < g1; g++) {
                dan_vk_mmla_k(a, a + j->gs, j->xp + 4 * g * j->gs, j->gs, j->blocks, out);
                for (int r = 0; r < 4 && r0 + r < j->rows; r++)
                    for (int v = 0; v < DAN_VK_MMLA_VG && DAN_VK_MMLA_VG * g + v < j->nv; v++)
                        j->y[(DAN_VK_MMLA_VG * g + v) * j->rows + r0 + r] = out[r * 8 + v];
            }
        }
    }
    free(a);
}

/* 1 если сделано: Q8_0, 8 и больше векторов */
static int dan_vk_matmul_mmla(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    if (fmt != DAN_FMT_Q80 || nv < DAN_VK_MMLA_VG || rows < 1 || cols < DAN_Q8_BLOCK) return 0;
    int64_t blocks = cols / DAN_Q8_BLOCK, groups = (nv + DAN_VK_MMLA_VG - 1) / DAN_VK_MMLA_VG, gs = blocks * DAN_VK_MMLA_RB;
    uint8_t *xp = (uint8_t *)malloc((size_t)(groups * 4 * gs));
    if (!xp) return 0;
    int64_t tile = dan_vk_tile_bytes(DAN_VK_MMLA_TILE) / (4 * gs);
    if (tile < 1) tile = 1;
    dan_vk_mmla_job j = {w, xq, xp, y, blocks, rows, nv, groups, gs, tile};
    dan_pool_run(groups * 4, 1, dan_vk_mmla_prep_part, &j);
    int64_t rgroups = (rows + 3) / 4, work = cols * nv * 4;
    dan_pool_run(rgroups, work >= 65536 ? 1 : 65536 / work, dan_vk_mmla_part, &j);
    free(xp);
    return 1;
}
#endif

/* Small batches (the kernels t->sm): the vectors are prepared once into
   units (sign mode 0), each part of the pool takes rows, and a row is
   multiplied by up to sm vectors at a time (more vectors - by groups of
   equal size: the row stays in the first level cache). For batches of 2 to
   dan_vk_sm_limit vectors (DAN_YADRA_MALYE sets the limit, up to
   DAN_VK_SM_LIMIT is tested); larger - the kernels of rows by vectors. */
#define DAN_VK_SM_LIMIT 16
/* the largest batch for which the kernels of small batches are faster than
   those of rows by vectors (yadra_test zamer_malye, замеры/результаты.md):
   AVX-512 on Intel - 8, on AMD (Zen 4, Zen 5) - 3, AVX2 - 5 */
static int64_t dan_vk_sm_limit(const dan_vk_table *t) {
    const char *e = getenv("DAN_YADRA_MALYE");
    if (e) return atoll(e);
#ifdef DAN_VK_X86
    if (t->sm == dan_vk_sm_avx2) return 5;
    __builtin_cpu_init();
    return __builtin_cpu_is("amd") ? 3 : 8;
#else
    (void)t;
    return DAN_VK_SM_LIMIT;
#endif
}
typedef struct { const uint8_t *w, *xp; float *y; int64_t blocks, rows, nv, us; int fmt, per; } dan_vk_sm_job;

static void dan_vk_sm_part(void *c, int64_t b, int64_t e) {
    dan_vk_sm_job *j = (dan_vk_sm_job *)c;
    const dan_vk_table *t = dan_vk();
    int64_t row = j->blocks * dan_vk_fmt_bytes[j->fmt];
    float out[DAN_VK_SM_MAX];
    for (int64_t i = b; i < e; i++)
        for (int64_t v0 = 0; v0 < j->nv; v0 += j->per) {
            int n = (int)(j->nv - v0 < j->per ? j->nv - v0 : j->per);
            t->sm(j->fmt, n, j->w + i * row, j->blocks, j->xp + v0 * j->us, j->us, out);
            for (int k = 0; k < n; k++) j->y[(v0 + k) * j->rows + i] = out[k];
        }
}

/* 1 if done by the kernels of small batches */
static int dan_vk_matmul_sm(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    const dan_vk_table *t = dan_vk();
    int64_t limit = t->sm ? dan_vk_sm_limit(t) : 0;
    if (!t->sm || nv < 2 || nv > limit || rows < 1 || cols < 1 || t->rn_mode != 0 || !t->sm(fmt, 1, NULL, 0, NULL, 0, NULL)) return 0;
    int max = DAN_VK_SM_MAX;
    while (max > 1 && !t->sm(fmt, max, NULL, 0, NULL, 0, NULL)) max--;
    int64_t groups = (nv + max - 1) / max, per = (nv + groups - 1) / groups;
    int64_t blocks = cols / dan_vk_fmt_n[fmt], us = dan_vk_rn_units(fmt, blocks) * DAN_VK_RN_UNIT, xs = blocks * dan_vk_fmt_xbytes[fmt];
    uint8_t *xp = (uint8_t *)malloc((size_t)(nv * us));
    if (!xp) return 0;
    for (int64_t v = 0; v < nv; v++) dan_vk_rn_prep(fmt, xq + v * xs, blocks, xp + v * us, 0);
    dan_vk_sm_job j = {w, xp, y, blocks, rows, nv, us, fmt, (int)per};
    int64_t work = cols * nv;
    dan_pool_run(rows, work >= 65536 ? 1 : 65536 / work, dan_vk_sm_part, &j);
    free(xp);
    return 1;
}

/* y[v * rows + i] = row i of w (rows x cols in blocks of the format) . vector v
   of xq (nv vectors already in 8 bits, one after another) */
static void dan_vk_matmul_fmt(int fmt, const uint8_t *w, const uint8_t *xq, float *y, int64_t rows, int64_t cols, int64_t nv) {
    if (getenv("DAN_YADRA_RN") == NULL || atoi(getenv("DAN_YADRA_RN")) != 0) {
        const dan_vk_table *t = dan_vk();
        if (t->many && t->many(fmt, w, xq, y, rows, cols, nv)) return;
        if (dan_vk_matmul_sm(fmt, w, xq, y, rows, cols, nv)) return;
        if (dan_vk_matmul_tl(fmt, w, xq, y, rows, cols, nv)) return;
        if (dan_vk_matmul_rn(fmt, w, xq, y, rows, cols, nv)) return;
    }
    int64_t blocks = cols / dan_vk_fmt_n[fmt], xs = blocks * dan_vk_fmt_xbytes[fmt];
    int64_t tile = dan_vk_tile_bytes(DAN_VK_TILE) / (xs > 0 ? xs : 1);
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

/* матвеки: the matrix w (rows of x->len / n numbers) by n vectors of x, the results of the vectors one after another */
static dan_arr *dan_k_matmul_f32(const dan_arr *w, const dan_arr *x, int64_t n, int line) {
    if (n < 1) dan_k_error("матвеки: векторов (%lld) должно быть хотя бы %lld", n, 1, line);
    if (x->len % n != 0) dan_k_error("матвеки: длина векторов (%lld) должна делиться на их число (%lld)", x->len, n, line);
    int64_t cols = x->len / n;
    if (cols == 0 || w->len % cols != 0)
        dan_k_error("матвеки: длина матрицы (%lld) должна делиться на длину вектора (%lld)", w->len, cols, line);
    int64_t rows = w->len / cols;
    if (rows > 0 && n > INT64_MAX / rows) dan_error(DAN_MSG_NO_MEMORY, line);
    dan_arr *y = dan_arr_new(DAN_AF32, rows * n, line);
    dan_vk_rows_f32((const float *)w->data, (const float *)x->data, (float *)y->data, rows, cols, n);
    return y;
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

static dan_arr *dan_k_swiglu_f32(const dan_arr *z, const dan_arr *u, int line) {
    if (z->len != u->len) dan_k_error("swiglu: массивы разной длины (%lld и %lld)", z->len, u->len, line);
    dan_arr *r = dan_arr_new(DAN_AF32, z->len, line);
    int64_t bad = dan_vk_swiglu_f32((const float *)z->data, (const float *)u->data, (float *)r->data, z->len);
    if (bad >= 0) {
        /* as the loop of the program: the number does not fit the element of the array */
        double x = (double)((const float *)z->data)[bad];
        dan_arr_float_range_error(DAN_AF32, x / (1.0 + exp(-x)) * (double)((const float *)u->data)[bad], line);
    }
    return r;
}
static dan_arr *dan_k_swiglu_f64(const dan_arr *z, const dan_arr *u, int line) {
    if (z->len != u->len) dan_k_error("swiglu: массивы разной длины (%lld и %lld)", z->len, u->len, line);
    dan_arr *r = dan_arr_new(DAN_AF64, z->len, line);
    dan_vk_swiglu_f64((const double *)z->data, (const double *)u->data, (double *)r->data, z->len);
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
                                                   "матвек_q5_0", "матвек_q4_1", "матвек_q5_1", "матвек_q8",
                                                   "матвек_q2k", "матвек_q3k", "матвек_iq4_xs"};
static const char *const dan_k_fmt_mm[DAN_NFMT] = {"матвеки_q4_0", "матвеки_q4k", "матвеки_q5k", "матвеки_q6k",
                                                   "матвеки_q5_0", "матвеки_q4_1", "матвеки_q5_1", "матвеки_q8",
                                                   "матвеки_q2k", "матвеки_q3k", "матвеки_iq4_xs"};
static const char *const dan_k_fmt_from[DAN_NFMT] = {"из_q4_0", "из_q4k", "из_q5k", "из_q6k", "из_q5_0", "из_q4_1", "из_q5_1", "из_q8",
                                                     "из_q2k", "из_q3k", "из_iq4_xs"};

static void dan_k_fmt_error(const char *name, const char *what, long long a, long long b, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s: ", name);
    size_t k = strlen(buf);
    snprintf(buf + k, sizeof buf - k, what, a, b);
    dan_error(buf, line);
}

/* the numbers of the vectors go into 8 bits: Q8_K needs finite numbers, Q8_0 - a scale that fits half precision */
static void dan_k_fmt_check_x(int fmt, const char *name, const float *xv, int64_t n, int line) {
    int k = dan_vk_fmt_n[fmt] == DAN_QK;
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

/* the vectors into 8 bits on the pool, vector by vector */
typedef struct { int fmt; const float *x; uint8_t *out; int64_t cols; } dan_vk_quantize_job;
static void dan_vk_quantize_part(void *c, int64_t b, int64_t e) {
    dan_vk_quantize_job *j = (dan_vk_quantize_job *)c;
    int64_t xs = j->cols / dan_vk_fmt_n[j->fmt] * dan_vk_fmt_xbytes[j->fmt];
    for (int64_t v = b; v < e; v++) dan_vk_fmt_quantize_x(j->fmt, j->x + v * j->cols, j->out + v * xs, j->cols);
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
    dan_vk_quantize_job qj = {fmt, xv, xq, cols};
    dan_pool_run(n, 1, dan_vk_quantize_part, &qj);   /* each vector by itself: the same bytes */
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
static dan_arr *dan_k_matvec_q2k(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q2K, w, x, line); }
static dan_arr *dan_k_matvec_q3k(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_Q3K, w, x, line); }
static dan_arr *dan_k_matvec_iq4_xs(const dan_arr *w, const dan_arr *x, int line) { return dan_k_matvec_fmt(DAN_FMT_IQ4XS, w, x, line); }
static dan_arr *dan_k_q2k_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q2K, q, line); }
static dan_arr *dan_k_q3k_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_Q3K, q, line); }
static dan_arr *dan_k_iq4_xs_dequantize(const dan_arr *q, int line) { return dan_k_dequantize_fmt(DAN_FMT_IQ4XS, q, line); }
static dan_arr *dan_k_matmul_q2k(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q2K, w, x, n, line); }
static dan_arr *dan_k_matmul_q3k(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_Q3K, w, x, n, line); }
static dan_arr *dan_k_matmul_iq4_xs(const dan_arr *w, const dan_arr *x, int64_t n, int line) { return dan_k_matmul_fmt(DAN_FMT_IQ4XS, w, x, n, line); }

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
