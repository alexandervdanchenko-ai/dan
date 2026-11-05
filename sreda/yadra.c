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
   bits as run on one core. DAN_POTOKI=N limits the number of threads. */

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
    void (*mm_f32)(const float *, const float *, float *, int64_t, int64_t, int64_t, int64_t, int64_t);
    void (*mm_f64)(const double *, const double *, double *, int64_t, int64_t, int64_t, int64_t, int64_t);
    int32_t (*q8_block)(const int8_t *, const int8_t *);
    float (*q8_dot)(const uint8_t *, const uint8_t *, int64_t);
} dan_vk_table;

/* sum over blocks of (d_a * d_b) * (integer dot product of the block), blocks in order */
#define DAN_VK_Q8_DOT(NAME, ATTR, BLOCK) \
ATTR static float NAME(const uint8_t *a, const uint8_t *b, int64_t blocks) { \
    float r = 0.0f; \
    for (int64_t i = 0; i < blocks; i++) { \
        const uint8_t *pa = a + i * DAN_Q8_BYTES, *pb = b + i * DAN_Q8_BYTES; \
        uint16_t ha, hb; \
        memcpy(&ha, pa, 2); \
        memcpy(&hb, pb, 2); \
        float d = dan_f16_to_f32(ha) * dan_f16_to_f32(hb), p = d * (float)BLOCK((const int8_t *)pa + 2, (const int8_t *)pb + 2); \
        r = r + p; \
    } \
    return r; \
}
DAN_VK_Q8_DOT(dan_vk_q8_dot_c, , dan_vk_q8_block_c)

static const dan_vk_table dan_vk_plain = {"c", dan_vk_dot_f32_c, dan_vk_dot_f64_c, dan_vk_mm_f32_c,
                                          dan_vk_mm_f64_c, dan_vk_q8_block_c, dan_vk_q8_dot_c};

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

#ifdef DAN_VK_X86
#define DAN_VK_AVX2 __attribute__((target("avx2")))
#define DAN_VK_AVX512 __attribute__((target("avx512f,avx512bw")))

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

/* tiles of 4 rows by 16 columns (float) or 4 by 8 (double) */
DAN_VK_AVX2 static void dan_vk_mm_f32_avx2(const float *A, const float *B, float *C, int64_t m, int64_t n,
                                           int64_t k, int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 16 * 16, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 16)
        for (int64_t i = 0; i < ie; i += 4) {
            __m256 c[4][2];
            for (int r = 0; r < 4; r++) c[r][0] = c[r][1] = _mm256_setzero_ps();
            for (int64_t p = 0; p < k; p++) {
                __m256 b0 = _mm256_loadu_ps(B + p * n + j), b1 = _mm256_loadu_ps(B + p * n + j + 8);
                for (int r = 0; r < 4; r++) {
                    __m256 a = _mm256_set1_ps(A[(i + r) * k + p]);
                    c[r][0] = _mm256_add_ps(c[r][0], _mm256_mul_ps(a, b0));
                    c[r][1] = _mm256_add_ps(c[r][1], _mm256_mul_ps(a, b1));
                }
            }
            for (int r = 0; r < 4; r++) {
                _mm256_storeu_ps(C + (i + r) * n + j, c[r][0]);
                _mm256_storeu_ps(C + (i + r) * n + j + 8, c[r][1]);
            }
        }
    DAN_VK_MM_EDGE(float, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(float, A, B, C, n, k, 0, m, je, j1);
}

DAN_VK_AVX2 static void dan_vk_mm_f64_avx2(const double *A, const double *B, double *C, int64_t m, int64_t n,
                                           int64_t k, int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 8 * 8, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 8)
        for (int64_t i = 0; i < ie; i += 4) {
            __m256d c[4][2];
            for (int r = 0; r < 4; r++) c[r][0] = c[r][1] = _mm256_setzero_pd();
            for (int64_t p = 0; p < k; p++) {
                __m256d b0 = _mm256_loadu_pd(B + p * n + j), b1 = _mm256_loadu_pd(B + p * n + j + 4);
                for (int r = 0; r < 4; r++) {
                    __m256d a = _mm256_set1_pd(A[(i + r) * k + p]);
                    c[r][0] = _mm256_add_pd(c[r][0], _mm256_mul_pd(a, b0));
                    c[r][1] = _mm256_add_pd(c[r][1], _mm256_mul_pd(a, b1));
                }
            }
            for (int r = 0; r < 4; r++) {
                _mm256_storeu_pd(C + (i + r) * n + j, c[r][0]);
                _mm256_storeu_pd(C + (i + r) * n + j + 4, c[r][1]);
            }
        }
    DAN_VK_MM_EDGE(double, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(double, A, B, C, n, k, 0, m, je, j1);
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

DAN_VK_Q8_DOT(dan_vk_q8_dot_avx2, DAN_VK_AVX2, dan_vk_q8_block_avx2)

static const dan_vk_table dan_vk_avx2 = {"avx2", dan_vk_dot_f32_avx2, dan_vk_dot_f64_avx2, dan_vk_mm_f32_avx2,
                                         dan_vk_mm_f64_avx2, dan_vk_q8_block_avx2, dan_vk_q8_dot_avx2};

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

/* tiles of 8 rows by 16 columns (float) or 8 by 8 (double) */
DAN_VK_AVX512 static void dan_vk_mm_f32_avx512(const float *A, const float *B, float *C, int64_t m, int64_t n,
                                               int64_t k, int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 32 * 32, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 32)
        for (int64_t i = 0; i < ie; i += 4) {
            __m512 c[4][2];
            for (int r = 0; r < 4; r++) c[r][0] = c[r][1] = _mm512_setzero_ps();
            for (int64_t p = 0; p < k; p++) {
                __m512 b0 = _mm512_loadu_ps(B + p * n + j), b1 = _mm512_loadu_ps(B + p * n + j + 16);
                for (int r = 0; r < 4; r++) {
                    __m512 a = _mm512_set1_ps(A[(i + r) * k + p]);
                    c[r][0] = _mm512_add_ps(c[r][0], _mm512_mul_ps(a, b0));
                    c[r][1] = _mm512_add_ps(c[r][1], _mm512_mul_ps(a, b1));
                }
            }
            for (int r = 0; r < 4; r++) {
                _mm512_storeu_ps(C + (i + r) * n + j, c[r][0]);
                _mm512_storeu_ps(C + (i + r) * n + j + 16, c[r][1]);
            }
        }
    DAN_VK_MM_EDGE(float, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(float, A, B, C, n, k, 0, m, je, j1);
}

DAN_VK_AVX512 static void dan_vk_mm_f64_avx512(const double *A, const double *B, double *C, int64_t m, int64_t n,
                                               int64_t k, int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 16 * 16, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 16)
        for (int64_t i = 0; i < ie; i += 4) {
            __m512d c[4][2];
            for (int r = 0; r < 4; r++) c[r][0] = c[r][1] = _mm512_setzero_pd();
            for (int64_t p = 0; p < k; p++) {
                __m512d b0 = _mm512_loadu_pd(B + p * n + j), b1 = _mm512_loadu_pd(B + p * n + j + 8);
                for (int r = 0; r < 4; r++) {
                    __m512d a = _mm512_set1_pd(A[(i + r) * k + p]);
                    c[r][0] = _mm512_add_pd(c[r][0], _mm512_mul_pd(a, b0));
                    c[r][1] = _mm512_add_pd(c[r][1], _mm512_mul_pd(a, b1));
                }
            }
            for (int r = 0; r < 4; r++) {
                _mm512_storeu_pd(C + (i + r) * n + j, c[r][0]);
                _mm512_storeu_pd(C + (i + r) * n + j + 8, c[r][1]);
            }
        }
    DAN_VK_MM_EDGE(double, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(double, A, B, C, n, k, 0, m, je, j1);
}

DAN_VK_AVX512 static int32_t dan_vk_q8_block_avx512(const int8_t *a, const int8_t *b) {
    __m512i x = _mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i *)a));
    __m512i y = _mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i *)b));
    return _mm512_reduce_add_epi32(_mm512_madd_epi16(x, y));
}

DAN_VK_Q8_DOT(dan_vk_q8_dot_avx512, DAN_VK_AVX512, dan_vk_q8_block_avx512)

static const dan_vk_table dan_vk_avx512 = {"avx512", dan_vk_dot_f32_avx512, dan_vk_dot_f64_avx512,
                                           dan_vk_mm_f32_avx512, dan_vk_mm_f64_avx512, dan_vk_q8_block_avx512, dan_vk_q8_dot_avx512};
#endif

#ifdef DAN_VK_ARM
/* ---- NEON ---- */

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

/* tiles of 4 rows by 16 columns (float) or 4 by 8 (double) */
static void dan_vk_mm_f32_neon(const float *A, const float *B, float *C, int64_t m, int64_t n, int64_t k,
                               int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 16 * 16, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 16)
        for (int64_t i = 0; i < ie; i += 4) {
            float32x4_t c[4][4];
            for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) c[r][q] = vdupq_n_f32(0.0f);
            for (int64_t p = 0; p < k; p++) {
                float32x4_t b[4];
                for (int q = 0; q < 4; q++) b[q] = vld1q_f32(B + p * n + j + 4 * q);
                for (int r = 0; r < 4; r++) {
                    float32x4_t a = vdupq_n_f32(A[(i + r) * k + p]);
                    for (int q = 0; q < 4; q++) c[r][q] = vaddq_f32(c[r][q], vmulq_f32(a, b[q]));
                }
            }
            for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) vst1q_f32(C + (i + r) * n + j + 4 * q, c[r][q]);
        }
    DAN_VK_MM_EDGE(float, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(float, A, B, C, n, k, 0, m, je, j1);
}

static void dan_vk_mm_f64_neon(const double *A, const double *B, double *C, int64_t m, int64_t n, int64_t k,
                               int64_t j0, int64_t j1) {
    int64_t je = j0 + (j1 - j0) / 8 * 8, ie = m / 4 * 4;
    for (int64_t j = j0; j < je; j += 8)
        for (int64_t i = 0; i < ie; i += 4) {
            float64x2_t c[4][4];
            for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) c[r][q] = vdupq_n_f64(0.0);
            for (int64_t p = 0; p < k; p++) {
                float64x2_t b[4];
                for (int q = 0; q < 4; q++) b[q] = vld1q_f64(B + p * n + j + 2 * q);
                for (int r = 0; r < 4; r++) {
                    float64x2_t a = vdupq_n_f64(A[(i + r) * k + p]);
                    for (int q = 0; q < 4; q++) c[r][q] = vaddq_f64(c[r][q], vmulq_f64(a, b[q]));
                }
            }
            for (int r = 0; r < 4; r++) for (int q = 0; q < 4; q++) vst1q_f64(C + (i + r) * n + j + 2 * q, c[r][q]);
        }
    DAN_VK_MM_EDGE(double, A, B, C, n, k, ie, m, j0, je);
    DAN_VK_MM_EDGE(double, A, B, C, n, k, 0, m, je, j1);
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

DAN_VK_Q8_DOT(dan_vk_q8_dot_neon, , dan_vk_q8_block_neon)

static const dan_vk_table dan_vk_neon = {"neon", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon, dan_vk_mm_f32_neon,
                                         dan_vk_mm_f64_neon, dan_vk_q8_block_neon, dan_vk_q8_dot_neon};

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

DAN_VK_Q8_DOT(dan_vk_q8_dot_dotprod, DAN_VK_DOTPROD, dan_vk_q8_block_dotprod)

static const dan_vk_table dan_vk_dotprod = {"dotprod", dan_vk_dot_f32_neon, dan_vk_dot_f64_neon,
                                            dan_vk_mm_f32_neon, dan_vk_mm_f64_neon, dan_vk_q8_block_dotprod, dan_vk_q8_dot_dotprod};

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

static const dan_vk_table *dan_vk_cur;

/* 1 if the processor can run the variant with this name */
static int dan_vk_supported(const char *name, const dan_vk_table **t) {
    if (strcmp(name, "c") == 0) { *t = &dan_vk_plain; return 1; }
#ifdef DAN_VK_X86
    __builtin_cpu_init();
    if (strcmp(name, "avx2") == 0 && __builtin_cpu_supports("avx2")) { *t = &dan_vk_avx2; return 1; }
    if (strcmp(name, "avx512") == 0 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
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
    const dan_vk_table *t = dan_vk_cur;
    if (t) return t;
    static const char *const best[] = {"avx512", "avx2", "dotprod", "neon", "c"};
    const char *want = getenv("DAN_YADRA");
    if (!(want && dan_vk_supported(want, &t)))
        for (int i = 0; i < 5; i++) if (dan_vk_supported(best[i], &t)) break;
    dan_vk_cur = t;   /* a race here is harmless: every thread finds the same table */
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
    int state;          /* 0 - not started, 1 - working, -1 - one thread only */
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

static void dan_pool_part(int part) {
    int64_t n = dan_pool.n, t = dan_pool.threads;
    int64_t b = n / t * part + (part < n % t ? part : n % t);
    int64_t e = b + n / t + (part < n % t ? 1 : 0);
    if (b < e) dan_pool.fn(dan_pool.ctx, b, e);
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
        if (v >= 1 && v < t) t = (int)v;
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
    dan_pool.state = made > 1 ? 1 : -1;
}

static int dan_pool_ready;
#ifndef _WIN32
static void dan_pool_init_busy(void) { dan_pool_mx_init(&dan_pool.busy); dan_pool_ready = 1; }
#endif

static void dan_pool_init_once(void) {
    if (dan_pool_ready) return;
#ifdef _WIN32
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) { dan_pool_mx_init(&dan_pool.busy); dan_pool_ready = 1; }
    else while (!dan_pool_ready) Sleep(0);
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
    dan_pool_init_once();
    if (n < 2 * min_part || !dan_pool_trylock(&dan_pool.busy)) { fn(ctx, 0, n); return; }
    if (dan_pool.state == 0) dan_pool_start();
    if (dan_pool.state < 0) { dan_pool_unlock(&dan_pool.busy); fn(ctx, 0, n); return; }
    int t = dan_pool.threads;
    if (n / min_part < t) {   /* fewer parts: still one call per thread, some get nothing */
        dan_pool_unlock(&dan_pool.busy);
        /* keep the parts a function of n and the thread count only: run whole */
        fn(ctx, 0, n);
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
    return dan_pool.state > 0 ? dan_pool.threads : 1;
}
#endif

/* ---- kernels for the program ---- */

static float dan_vk_dot_f32(const float *a, const float *b, int64_t n) { return dan_vk()->dot_f32(a, b, n); }
static double dan_vk_dot_f64(const double *a, const double *b, int64_t n) { return dan_vk()->dot_f64(a, b, n); }

static float dan_vk_sum_f32(const float *a, int64_t n) {
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

static double dan_vk_sum_f64(const double *a, int64_t n) {
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
    float acc[DAN_VK_L32] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L32 <= n; k += DAN_VK_L32)
        for (int l = 0; l < DAN_VK_L32; l++) { float d = x[k + l] - mean, p = d * d; acc[l] = acc[l] + p; }
    for (int s = DAN_VK_L32 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    float v = acc[0];
    for (; k < n; k++) { float d = x[k] - mean, p = d * d; v = v + p; }
    float sc = 1.0f / sqrtf(v / (float)n + eps);
    for (int64_t i = 0; i < n; i++) {
        float t = (x[i] - mean) * sc;
        if (w) t = t * w[i];
        if (b) t = t + b[i];
        y[i] = t;
    }
}
static void dan_vk_layernorm_f64(double *y, const double *x, const double *w, const double *b, int64_t n,
                                 double eps) {
    if (n <= 0) return;
    double mean = dan_vk_sum_f64(x, n) / (double)n;
    double acc[DAN_VK_L64] = {0};
    int64_t k = 0;
    for (; k + DAN_VK_L64 <= n; k += DAN_VK_L64)
        for (int l = 0; l < DAN_VK_L64; l++) { double d = x[k + l] - mean, p = d * d; acc[l] = acc[l] + p; }
    for (int s = DAN_VK_L64 / 2; s >= 1; s /= 2)
        for (int l = 0; l < s; l++) acc[l] = acc[l] + acc[l + s];
    double v = acc[0];
    for (; k < n; k++) { double d = x[k] - mean, p = d * d; v = v + p; }
    double sc = 1.0 / sqrt(v / (double)n + eps);
    for (int64_t i = 0; i < n; i++) {
        double t = (x[i] - mean) * sc;
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
        uint16_t h = dan_f32_to_f16(d);
        memcpy(o, &h, 2);
        for (int i = 0; i < DAN_Q8_BLOCK; i++) o[2 + i] = (uint8_t)(int8_t)lrintf(v[i] * id);
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

static float dan_vk_q8_dot(const uint8_t *a, const uint8_t *b, int64_t blocks) { return dan_vk()->q8_dot(a, b, blocks); }

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
