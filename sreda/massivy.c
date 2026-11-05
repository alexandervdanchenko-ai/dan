/* ==== Dense numeric arrays of the Dan language (массив<дроб32> and others) ====

   An array is a value, like a list: the header counts the places that hold it,
   and a place that changes an array held elsewhere first gets its own copy.
   Unlike a list, the elements lie in one block of memory one after another,
   in the size of their kind (4 bytes for дроб32, 1 byte for цел8), and the
   block is aligned to 64 bytes, so the elements can be read by vector
   instructions and passed to C functions without copying.

   A part of an array (м[а..б]) and an array mapped from a file share the
   elements of the array or of the file; they are read-only, and changing such
   an array first makes its own copy (ro). */

#if !defined(_WIN32) && !defined(__wasi__)
#include <sys/mman.h>
#include <fcntl.h>
#endif

enum { DAN_AF64, DAN_AF32, DAN_AF16, DAN_AI64, DAN_AI32, DAN_AI16, DAN_AI8, DAN_AU8 };
static const uint8_t dan_arr_size[] = { 8, 4, 2, 8, 4, 2, 1, 1 };
static const char *const dan_arr_kind_name[] = {
    "дроб", "дроб32", "дроб16", "цел", "цел32", "цел16", "цел8", "байт"
};

typedef struct dan_arr {
    dan_hdr h;
    int64_t len;
    uint8_t *data;          /* the first element */
    struct dan_arr *owner;  /* a part of this array: its elements are shared */
    void *block;            /* owned memory, or the view of a mapped file */
    size_t map_len;         /* > 0: block is a view of a file of this many bytes */
    int32_t kind;
    int32_t ro;             /* the elements are not this array's own: change only a copy */
} dan_arr;

#define DAN_ARR_ALIGN 64

static void dan_arr_unmap(void *block, size_t n);

static void dan_arr_destroy(dan_hdr *h) {
    dan_arr *a = (dan_arr *)(void *)h;
    if (a->owner) dan_hdr_release(&a->owner->h, dan_arr_destroy);
    else if (a->map_len) dan_arr_unmap(a->block, a->map_len);
    else free(a->block);
    dan_free(a);
}

static void dan_arr_size_error(int64_t n, int line) {
    char buf[200];
    snprintf(buf, sizeof buf, "размер массива не может быть отрицательным: %lld", (long long)n);
    dan_error(buf, line);
}

/* elements of a new array; zero-filled, aligned to 64 bytes */
static uint8_t *dan_arr_block(int32_t kind, int64_t n, void **block, int line) {
    if (n < 0) dan_arr_size_error(n, line);
    size_t bytes;
    if (__builtin_mul_overflow((size_t)n, (size_t)dan_arr_size[kind], &bytes) || bytes > SIZE_MAX - DAN_ARR_ALIGN)
        dan_error("массив слишком большой", line);
    void *p = calloc(1, bytes + DAN_ARR_ALIGN);
    if (!p) dan_error("не хватает памяти для массива", line);
    *block = p;
    return (uint8_t *)(((uintptr_t)p + DAN_ARR_ALIGN - 1) & ~(uintptr_t)(DAN_ARR_ALIGN - 1));
}

static dan_arr *dan_arr_head(int32_t kind, int64_t n) {
    dan_arr *a = dan_alloc(sizeof *a);
    dan_hdr_new(&a->h, dan_arr_destroy);
    a->len = n;
    a->data = NULL;
    a->owner = NULL;
    a->block = NULL;
    a->map_len = 0;
    a->kind = kind;
    a->ro = 0;
    return a;
}

/* new array of n zeros */
static dan_arr *dan_arr_new(int32_t kind, int64_t n, int line) {
    dan_arr *a = dan_arr_head(kind, n < 0 ? 0 : n);
    a->data = dan_arr_block(kind, n, &a->block, line);
    return a;
}

/* ---- half-precision numbers (дроб16): IEEE 754 binary16 ---- */

/* every binary16 number is exactly a binary32 number: the bits are moved over */
static inline double dan_f16_to_d(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = ((uint32_t)h >> 10) & 31u, m = h & 1023u, r;
    if (e == 0) {
        if (m == 0) r = s;
        else {                                        /* subnormal: normalise */
            e = 113;
            while (!(m & 1024u)) { m <<= 1; e--; }
            r = s | (e << 23) | ((m & 1023u) << 13);
        }
    } else if (e == 31) r = s | 0x7F800000u | (m << 13);
    else r = s | ((e + 112u) << 23) | (m << 13);
    float f;
    memcpy(&f, &r, 4);
    return f;
}

/* nearest binary16 number (ties to even); the caller has checked the range */
static uint16_t dan_d_to_f16(double v) {
    uint16_t s = signbit(v) ? 0x8000u : 0;
    if (isnan(v)) return (uint16_t)(s | 0x7E00u);
    double a = fabs(v);
    if (isinf(a)) return (uint16_t)(s | 0x7C00u);
    if (a < ldexp(1.0, -14)) {                       /* subnormal: steps of 2^-24 */
        double q = nearbyint(ldexp(a, 24));
        return (uint16_t)(s | (uint16_t)q);           /* q == 1024 is the smallest normal number */
    }
    int e;
    double f = frexp(a, &e);                          /* a = f * 2^e, 0.5 <= f < 1 */
    double q = nearbyint(ldexp(f, 11));               /* 11 significant bits: 1024..2048 */
    if (q >= 2048.0) { q /= 2.0; e++; }
    int be = e - 1 + 15;
    if (be >= 31) return (uint16_t)(s | 0x7C00u);
    return (uint16_t)(s | (uint16_t)(be << 10) | ((uint16_t)q & 1023u));
}

/* ---- elements ---- */

static void dan_arr_range_error(int32_t kind, const char *value, const char *range, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, "значение %s не помещается в элемент массива<%s> (%s)", value, dan_arr_kind_name[kind], range);
    dan_error(buf, line);
}

static void dan_arr_int_range_error(int32_t kind, int64_t v, int line) {
    static const char *const range[] = { "", "", "", "", "от -2147483648 до 2147483647", "от -32768 до 32767",
                                         "от -128 до 127", "от 0 до 255" };
    char b[24];
    dan_arr_range_error(kind, dan_itoa(v, b + sizeof b), range[kind], line);
}

static void dan_arr_float_range_error(int32_t kind, double v, int line) {
    char b[64];
    dan_format_float(b, sizeof b, v);
    dan_arr_range_error(kind, b, kind == DAN_AF32 ? "наибольшее по модулю 3.4028234663852886e38" : "наибольшее по модулю 65504",
                        line);
}

static inline void dan_arr_check(const dan_arr *a, int64_t i, int line) {
    if ((uint64_t)i >= (uint64_t)a->len) dan_index_error(i, a->len, line);
}

/* the kind is a constant in the program's code, so after inlining only one case is left */
static inline double dan_arr_rdk(int32_t kind, const dan_arr *a, int64_t i) {
    switch (kind) {
    case DAN_AF64: { double v; memcpy(&v, a->data + i * 8, 8); return v; }
    case DAN_AF32: { float v; memcpy(&v, a->data + i * 4, 4); return v; }
    default: { uint16_t v; memcpy(&v, a->data + i * 2, 2); return dan_f16_to_d(v); }
    }
}

static inline int64_t dan_arr_rik(int32_t kind, const dan_arr *a, int64_t i) {
    switch (kind) {
    case DAN_AI64: { int64_t v; memcpy(&v, a->data + i * 8, 8); return v; }
    case DAN_AI32: { int32_t v; memcpy(&v, a->data + i * 4, 4); return v; }
    case DAN_AI16: { int16_t v; memcpy(&v, a->data + i * 2, 2); return v; }
    case DAN_AI8: return (int8_t)a->data[i];
    default: return a->data[i];
    }
}

static inline void dan_arr_wdk(int32_t kind, dan_arr *a, int64_t i, double v, int line) {
    switch (kind) {
    case DAN_AF64: memcpy(a->data + i * 8, &v, 8); break;
    case DAN_AF32: {
        if (isfinite(v) && fabs(v) > 3.4028234663852886e38) dan_arr_float_range_error(kind, v, line);
        float f = (float)v;
        memcpy(a->data + i * 4, &f, 4);
        break;
    }
    default: {
        if (isfinite(v) && fabs(v) >= 65520.0) dan_arr_float_range_error(kind, v, line);
        uint16_t h = dan_d_to_f16(v);
        memcpy(a->data + i * 2, &h, 2);
    }
    }
}

static inline void dan_arr_wik(int32_t kind, dan_arr *a, int64_t i, int64_t v, int line) {
    switch (kind) {
    case DAN_AI64: memcpy(a->data + i * 8, &v, 8); break;
    case DAN_AI32: {
        if (v < INT32_MIN || v > INT32_MAX) dan_arr_int_range_error(kind, v, line);
        int32_t x = (int32_t)v;
        memcpy(a->data + i * 4, &x, 4);
        break;
    }
    case DAN_AI16: {
        if (v < INT16_MIN || v > INT16_MAX) dan_arr_int_range_error(kind, v, line);
        int16_t x = (int16_t)v;
        memcpy(a->data + i * 2, &x, 2);
        break;
    }
    case DAN_AI8:
        if (v < -128 || v > 127) dan_arr_int_range_error(kind, v, line);
        a->data[i] = (uint8_t)(int8_t)v;
        break;
    default:
        if (v < 0 || v > 255) dan_arr_int_range_error(kind, v, line);
        a->data[i] = (uint8_t)v;
    }
}

/* the same with the kind taken from the array (code that serves every kind) */
static inline double dan_arr_rd(const dan_arr *a, int64_t i) { return dan_arr_rdk(a->kind, a, i); }
static inline int64_t dan_arr_ri(const dan_arr *a, int64_t i) { return dan_arr_rik(a->kind, a, i); }
static inline void dan_arr_wd(dan_arr *a, int64_t i, double v, int line) { dan_arr_wdk(a->kind, a, i, v, line); }
static inline void dan_arr_wi(dan_arr *a, int64_t i, int64_t v, int line) { dan_arr_wik(a->kind, a, i, v, line); }

/* м[i] and м[i] = v in the program: the first argument is the kind, a constant */
static inline double dan_arr_getd(int32_t kind, const dan_arr *a, int64_t i, int line) {
    dan_arr_check(a, i, line);
    return dan_arr_rdk(kind, a, i);
}
static inline int64_t dan_arr_geti(int32_t kind, const dan_arr *a, int64_t i, int line) {
    dan_arr_check(a, i, line);
    return dan_arr_rik(kind, a, i);
}
static inline void dan_arr_setd(int32_t kind, dan_arr *a, int64_t i, double v, int line) {
    dan_arr_check(a, i, line);
    dan_arr_wdk(kind, a, i, v, line);
}
static inline void dan_arr_seti(int32_t kind, dan_arr *a, int64_t i, int64_t v, int line) {
    dan_arr_check(a, i, line);
    dan_arr_wik(kind, a, i, v, line);
}

/* ---- creation ---- */

static dan_arr *dan_arr_filld(int32_t kind, int64_t n, double v, int line) {
    dan_arr *a = dan_arr_new(kind, n, line);
    if (v != 0.0 || signbit(v)) for (int64_t i = 0; i < a->len; i++) dan_arr_wd(a, i, v, line);
    return a;
}

static dan_arr *dan_arr_filli(int32_t kind, int64_t n, int64_t v, int line) {
    dan_arr *a = dan_arr_new(kind, n, line);
    if (v != 0) for (int64_t i = 0; i < a->len; i++) dan_arr_wi(a, i, v, line);
    return a;
}

static dan_arr *dan_arr_fromd(int32_t kind, const double *src, int64_t n, int line) {
    dan_arr *a = dan_arr_new(kind, n, line);
    for (int64_t i = 0; i < n; i++) dan_arr_wd(a, i, src[i], line);
    return a;
}

static dan_arr *dan_arr_fromi(int32_t kind, const int64_t *src, int64_t n, int line) {
    dan_arr *a = dan_arr_new(kind, n, line);
    for (int64_t i = 0; i < n; i++) dan_arr_wi(a, i, src[i], line);
    return a;
}

static void dan_arr_outd(const dan_arr *a, double *dst) { for (int64_t i = 0; i < a->len; i++) dst[i] = dan_arr_rd(a, i); }
static void dan_arr_outi(const dan_arr *a, int64_t *dst) { for (int64_t i = 0; i < a->len; i++) dst[i] = dan_arr_ri(a, i); }

/* ---- places ---- */

static inline dan_arr *dan_arr_share(dan_arr *a) { if (a) dan_hdr_share(&a->h); return a; }
static inline void dan_arr_release(dan_arr *a) { if (a) dan_hdr_release(&a->h, dan_arr_destroy); }
static inline void dan_arr_give(dan_arr *a) { if (a) dan_hdr_give(&a->h, dan_arr_destroy); }

/* a new array with its own copy of the elements */
static dan_arr *dan_arr_copy(const dan_arr *a) {
    dan_arr *r = dan_arr_new(a->kind, a->len, 0);
    if (a->len) memcpy(r->data, a->data, (size_t)a->len * dan_arr_size[a->kind]);
    return r;
}

/* the place *pp is about to change the array: if the array is held elsewhere too,
   or its elements are not its own, the place gets its own copy */
static dan_arr *dan_arr_mut(dan_arr **pp) {
    dan_arr *a = *pp;
    if (a->h.rc > 1 || a->ro) {
        dan_arr *r = dan_arr_copy(a);
        r->h.rc = 1;
        dan_arr_release(a);
        *pp = r;
        return r;
    }
    return a;
}

/* copy for another task */
static dan_arr *dan_arr_clone(dan_arr *a) {
    if (!a) return a;
    dan_arr *r = dan_alloc(sizeof *r);
    *r = (dan_arr){ .len = a->len, .kind = a->kind };
    dan_hdr_detached(&r->h);
    r->data = dan_arr_block(a->kind, a->len, &r->block, 0);
    if (a->len) memcpy(r->data, a->data, (size_t)a->len * dan_arr_size[a->kind]);
    return r;
}

/* м[а..б] - elements а..б inclusive (б == а - 1 gives an empty array); shares the elements */
static dan_arr *dan_arr_slice(dan_arr *a, int64_t from, int64_t to, int line) {
    if (from < 0 || from > a->len) dan_index_error(from, a->len, line);
    if (to < from - 1 || to >= a->len) dan_index_error(to, a->len, line);
    dan_arr *root = a->owner ? a->owner : a;
    dan_arr *r = dan_arr_head(a->kind, to - from + 1);
    dan_hdr_share(&root->h);
    r->owner = root;
    r->data = a->data + from * dan_arr_size[a->kind];
    r->ro = 1;
    return r;
}

static bool dan_arr_eq(const dan_arr *a, const dan_arr *b) {
    if (a->len != b->len) return false;
    bool f = a->kind <= DAN_AF16;
    for (int64_t i = 0; i < a->len; i++) {
        if (f ? dan_arr_rd(a, i) != dan_arr_rd(b, i) : dan_arr_ri(a, i) != dan_arr_ri(b, i)) return false;
    }
    return true;
}

/* ---- output ---- */

/* an element of дроб32 or дроб16 is shown with the fewest digits that give the same element back */
static void dan_arr_format(const dan_arr *a, int64_t i, char *b, size_t n) {
    if (a->kind > DAN_AF16) {
        char t[24];
        snprintf(b, n, "%s", dan_itoa(dan_arr_ri(a, i), t + sizeof t));
        return;
    }
    double v = dan_arr_rd(a, i);
    if (a->kind != DAN_AF64 && isfinite(v) && v != 0.0) {
        for (int p = 1; p <= 17; p++) {
            char t[40];
            snprintf(t, sizeof t, "%.*g", p, v);
            double w = strtod(t, NULL);
            if (a->kind == DAN_AF32 ? (float)w == (float)v : dan_d_to_f16(w) == dan_d_to_f16(v)) { v = w; break; }
        }
    }
    dan_format_float(b, n, v);
}

static void dan_arr_write(const dan_arr *a) {
    putchar('[');
    for (int64_t i = 0; i < a->len; i++) {
        char b[64];
        if (i) fputs(", ", stdout);
        dan_arr_format(a, i, b, sizeof b);
        fputs(b, stdout);
    }
    putchar(']');
}

static dan_str dan_arr_str(const dan_arr *a) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "[");
    for (int64_t i = 0; i < a->len; i++) {
        char t[64];
        if (i) dan_buf_add(&b, ", ");
        dan_arr_format(a, i, t, sizeof t);
        dan_buf_add(&b, t);
    }
    dan_buf_add(&b, "]");
    return dan_buf_str(&b);
}

/* ---- arrays mapped from files ---- */

#ifdef _WIN32
static void dan_arr_unmap(void *block, size_t n) { (void)n; UnmapViewOfFile(block); }
#elif defined(__wasi__)
static void dan_arr_unmap(void *block, size_t n) { (void)n; free(block); }
#else
static void dan_arr_unmap(void *block, size_t n) { munmap(block, n); }
#endif
