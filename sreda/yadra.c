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
                                          dan_vk_mm_f64_c, dan_vk_q8_block_c, dan_vk_q8_dot_c, dan_vk_dot32_acc_c, dan_vk_h2f_c, dan_vk_dot_f32_c, dan_vk_dot_f64_c, dan_vk_mm_f32_c, dan_vk_mm_f64_c};

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

DAN_VK_AVX2F16 static inline __m256 dan_vk_q8_scales_avx2(const uint8_t *p) {
    uint16_t h[8];
    for (int l = 0; l < 8; l++) memcpy(&h[l], p + l * DAN_Q8_BYTES, 2);
    return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)h));
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
        __m256 d = _mm256_mul_ps(dan_vk_q8_scales_avx2(pa), dan_vk_q8_scales_avx2(pb));
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

static const dan_vk_table dan_vk_avx2 = {"avx2", dan_vk_dot_f32_avx2, dan_vk_dot_f64_avx2, dan_vk_sum_f32_avx2, dan_vk_sum_f64_avx2, dan_vk_mm_f32_avx2,
                                         dan_vk_mm_f64_avx2, dan_vk_q8_block_avx2, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx2, dan_vk_h2f_avx2, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx2_fast, dan_vk_mm_f64_avx2_fast};

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
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx2, dan_vk_dot32_acc_avx512, dan_vk_h2f_avx512, dan_vk_dot_f32_avx2_fast, dan_vk_dot_f64_avx2_fast, dan_vk_mm_f32_avx512_fast, dan_vk_mm_f64_avx512_fast};
#endif

#ifdef DAN_VK_ARM
/* ---- NEON ---- */
#define DAN_VK_FMA32(a, b, c) vfmaq_f32(c, a, b)
#define DAN_VK_FMA64(a, b, c) vfmaq_f64(c, a, b)

static float dan_vk_dot_f32_neon(const float *a, const float *b, int64_t n) {
    float32x4_t s[16];
    for (int j = 0; j < 16; j++) s[j] = vdupq_n_f32(0.0f);
    int64_t k = 0;
    for (; k + 64 <= n; k += 64)
        for (int j = 0; j < 16; j++)
            s[j] = vaddq_f32(s[j], vmulq_f32(vld1q_f32(a + k + 4 * j), vld1q_f32(b + k + 4 * j)));
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
    for (int64_t k = 0; k + 64 <= n; k += 64)
        for (int j = 0; j < 16; j++)
            s[j] = vaddq_f32(s[j], vmulq_f32(vld1q_f32(a + k + 4 * j), vld1q_f32(b + k + 4 * j)));
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

static const dan_vk_table dan_vk_neon = {"neon", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon, dan_vk_mm_f32_neon,
                                         dan_vk_mm_f64_neon, dan_vk_q8_block_neon, dan_vk_q8_dot_neon, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast};

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

static const dan_vk_table dan_vk_dotprod = {"dotprod", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_sum_f32_neon, dan_vk_sum_f64_neon,
                                            dan_vk_mm_f32_neon, dan_vk_mm_f64_neon, dan_vk_q8_block_dotprod, dan_vk_q8_dot_dotprod, dan_vk_dot32_acc_neon, dan_vk_h2f_neon, dan_vk_dot_f32_neon_fast, dan_vk_dot_f64_neon_fast, dan_vk_mm_f32_neon_fast, dan_vk_mm_f64_neon_fast};

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
        *t = &dan_vk_avx512; return 1;
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
    uint64_t generation;
    int left;           /* parts not finished */
    int64_t n;
    dan_pool_fn fn;
    void *ctx;
} dan_pool;

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
        dan_pool_lock(&dan_pool.mx);
        while (dan_pool.generation == seen) dan_pool_wait(&dan_pool.start, &dan_pool.mx);
        seen = dan_pool.generation;
        dan_pool_unlock(&dan_pool.mx);
        dan_pool_part(part);
        dan_pool_lock(&dan_pool.mx);
        if (--dan_pool.left == 0) dan_pool_wake_all(&dan_pool.done);
        dan_pool_unlock(&dan_pool.mx);
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
    dan_pool_lock(&dan_pool.mx);
    dan_pool.n = n;
    dan_pool.fn = fn;
    dan_pool.ctx = ctx;
    dan_pool.left = t - 1;
    dan_pool.generation++;
    dan_pool_wake_all(&dan_pool.start);
    dan_pool_unlock(&dan_pool.mx);
    dan_pool_part(0);
    dan_pool_lock(&dan_pool.mx);
    while (dan_pool.left > 0) dan_pool_wait(&dan_pool.done, &dan_pool.mx);
    dan_pool_unlock(&dan_pool.mx);
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
