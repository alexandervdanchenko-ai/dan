/* Fixed inputs and hashes of the results of sums, softmax and normalisations
   (sreda/yadra_test.c). The hashes were taken from the plain C functions
   before the variants of sums appeared: every variant on every processor must
   give these bits (softmax - where exp is that of glibc). */

static uint64_t etalon_rng;
static double etalon_rnd(void) {
    etalon_rng ^= etalon_rng << 13; etalon_rng ^= etalon_rng >> 7; etalon_rng ^= etalon_rng << 17;
    return (double)(etalon_rng >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static uint64_t etalon_fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *c = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ull; }
    return h;
}

/* numbers of mixed size and sign, with subnormal ones */
static void etalon_input(float *x, double *y, int64_t n, uint64_t seed) {
    etalon_rng = seed;
    for (int64_t i = 0; i < n; i++) {
        double v = etalon_rnd() * pow(10.0, (double)(i % 13) - 6.0);
        if (i % 17 == 5) v = 1e-41 * (double)(i % 3 + 1) * (i % 2 ? -1 : 1);   /* subnormal float */
        if (i % 19 == 7) v = -v * 1e3;
        x[i] = (float)v;
        y[i] = v * (1.0 + 1e-9 * (double)i);
    }
}

#define ETALON_CASES 8
static const int64_t etalon_sizes[ETALON_CASES] = {1, 7, 63, 64, 65, 100, 1000, 4097};

/* hashes: [0] sums, [1] layer normalisation, [2] RMS normalisation, [3] softmax */
static void etalon_hashes(uint64_t out[4]) {
    static float x[4097], y[4097], w[4097], b[4097];
    static double xd[4097], yd[4097], wd[4097], bd[4097];
    uint64_t h[4] = {14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull};
    for (int c = 0; c < ETALON_CASES; c++) {
        int64_t n = etalon_sizes[c];
        etalon_input(x, xd, n, 1234567 + (uint64_t)c);
        etalon_input(w, wd, n, 7654321 + (uint64_t)c);
        etalon_input(b, bd, n, 1111111 + (uint64_t)c);
        float s = dan_vk_sum_f32(x, n);
        double sd = dan_vk_sum_f64(xd, n);
        h[0] = etalon_fnv(h[0], &s, 4);
        h[0] = etalon_fnv(h[0], &sd, 8);
        dan_vk_layernorm_f32(y, x, w, b, n, 1e-5f);
        h[1] = etalon_fnv(h[1], y, (size_t)n * 4);
        dan_vk_layernorm_f64(yd, xd, NULL, bd, n, 1e-9);
        h[1] = etalon_fnv(h[1], yd, (size_t)n * 8);
        memcpy(y, x, (size_t)n * 4);
        dan_vk_layernorm_f32(y, y, NULL, NULL, n, 1e-5f);   /* in place */
        h[1] = etalon_fnv(h[1], y, (size_t)n * 4);
        dan_vk_rmsnorm_f32(y, x, w, n, 1e-6f);
        h[2] = etalon_fnv(h[2], y, (size_t)n * 4);
        dan_vk_rmsnorm_f64(yd, xd, wd, n, 1e-12);
        h[2] = etalon_fnv(h[2], yd, (size_t)n * 8);
        for (int64_t i = 0; i < n; i++) { y[i] = x[i] * 1e5f; yd[i] = xd[i] * 1e5; }   /* exp of big differences */
        dan_vk_softmax_f32(y, n);
        h[3] = etalon_fnv(h[3], y, (size_t)n * 4);
        dan_vk_softmax_f64(yd, n);
        h[3] = etalon_fnv(h[3], yd, (size_t)n * 8);
    }
    memcpy(out, h, sizeof h);
}

/* ---- weights of llama.cpp: blocks for the comparison with ggml ----

   Random blocks of Q4_0, Q4_K, Q5_K, Q6_K, Q5_0, Q4_1 and Q5_1 (random bytes, the scales in
   half precision - finite numbers of every kind: zero, subnormal, up to
   65504, both signs). etalon_kvanty_ggml - hashes of what dequantize_row_*
   of ggml gives on them (llama.cpp b12190, ggml/src/ggml-quants.c built by
   gcc 13.3 with -O2 -ffp-contract=off on x86-64; sreda/yadra_ggml.c checks
   them when the sources of ggml are at hand, see test_yadra.py). */

static uint16_t etalon_half(void) {
    etalon_rng ^= etalon_rng << 13; etalon_rng ^= etalon_rng >> 7; etalon_rng ^= etalon_rng << 17;
    uint64_t r = etalon_rng;
    uint16_t sign = (uint16_t)((r >> 20) & 1) << 15;
    switch (r % 8) {
    case 0: return sign;                                          /* zero */
    case 1: return (uint16_t)(sign | (1 + (r >> 30) % 1023));     /* subnormal */
    default: return (uint16_t)(sign | ((1 + (r >> 30) % 30) << 10) | ((r >> 40) & 0x3ff));
    }
}

#define ETALON_KVANT_FORMATS 7
static const int etalon_kvant_bytes[ETALON_KVANT_FORMATS] = {18, 144, 176, 210, 22, 20, 24};
static const int etalon_kvant_n[ETALON_KVANT_FORMATS] = {32, 256, 256, 256, 32, 32, 32};

static void etalon_kvant_blocks(int fmt, uint8_t *out, int64_t blocks, uint64_t seed) {
    etalon_rng = seed;
    int64_t b = etalon_kvant_bytes[fmt];
    for (int64_t i = 0; i < blocks * b; i++) {
        etalon_rng ^= etalon_rng << 13; etalon_rng ^= etalon_rng >> 7; etalon_rng ^= etalon_rng << 17;
        out[i] = (uint8_t)(etalon_rng >> 32);
    }
    for (int64_t i = 0; i < blocks; i++) {
        uint8_t *o = out + i * b;
        uint16_t h = etalon_half();
        if (fmt == 3) memcpy(o + 208, &h, 2);   /* Q6_K: d at the end */
        else memcpy(o, &h, 2);
        if (fmt == 1 || fmt == 2 || fmt == 5 || fmt == 6) { h = etalon_half(); memcpy(o + 2, &h, 2); }   /* dmin, m */
    }
}

#define ETALON_KVANT_BLOCKS 256
static const uint64_t etalon_kvant_seed = 20261108;

/* hashes of the numbers of ETALON_KVANT_BLOCKS blocks of each format given
   by dequant(fmt, blocks, numbers, count of blocks) */
typedef void (*etalon_dequant_fn)(int fmt, const uint8_t *in, float *out, int64_t blocks);
static void etalon_kvant_hashes(etalon_dequant_fn dequant, uint64_t out[ETALON_KVANT_FORMATS]) {
    static uint8_t in[ETALON_KVANT_BLOCKS * 210];
    static float y[ETALON_KVANT_BLOCKS * 256];
    for (int f = 0; f < ETALON_KVANT_FORMATS; f++) {
        etalon_kvant_blocks(f, in, ETALON_KVANT_BLOCKS, etalon_kvant_seed + (uint64_t)f);
        dequant(f, in, y, ETALON_KVANT_BLOCKS);
        out[f] = etalon_fnv(14695981039346656037ull, y, (size_t)ETALON_KVANT_BLOCKS * etalon_kvant_n[f] * 4);
    }
}

/* filled in from ggml by sreda/yadra_ggml.c («печать») */
static const uint64_t etalon_kvanty_ggml[ETALON_KVANT_FORMATS] = {0x31a03d62345957d2ull, 0x7ed0a9ea476c4c1cull, 0x4ed495776db8ec2dull, 0x88762c8b76053e5dull,
    0x572f49526ea26a59ull, 0x0c4f9a73532bcd33ull, 0x67b1256109f26e01ull};
