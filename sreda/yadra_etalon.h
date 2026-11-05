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
