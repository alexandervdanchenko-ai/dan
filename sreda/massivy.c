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

/* цел4 and цел2 are packed: two and four elements in a byte, the first one in the low bits */
enum { DAN_AF64, DAN_AF32, DAN_AF16, DAN_AI64, DAN_AI32, DAN_AI16, DAN_AI8, DAN_AU8, DAN_AI4, DAN_AI2 };
static const uint8_t dan_arr_bits[] = { 64, 32, 16, 64, 32, 16, 8, 8, 4, 2 };
static const char *const dan_arr_kind_name[] = {
    "дроб", "дроб32", "дроб16", "цел", "цел32", "цел16", "цел8", "байт", "цел4", "цел2"
};

/* bytes taken by n elements (n is checked by the caller) */
static inline size_t dan_arr_nbytes(int32_t kind, int64_t n) { return ((size_t)n * dan_arr_bits[kind] + 7) / 8; }

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
    if (__builtin_mul_overflow((size_t)n, (size_t)dan_arr_bits[kind], &bytes) || bytes > SIZE_MAX - DAN_ARR_ALIGN - 7)
        dan_error("массив слишком большой", line);
    bytes = (bytes + 7) / 8;
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
                                         "от -128 до 127", "от 0 до 255", "от -8 до 7", "от -2 до 1" };
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
    case DAN_AU8: return a->data[i];
    case DAN_AI4: return (int64_t)((int8_t)(uint8_t)(a->data[i >> 1] >> ((i & 1) * 4) << 4) >> 4);
    default: return (int64_t)((int8_t)(uint8_t)(a->data[i >> 2] >> ((i & 3) * 2) << 6) >> 6);
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
    case DAN_AU8:
        if (v < 0 || v > 255) dan_arr_int_range_error(kind, v, line);
        a->data[i] = (uint8_t)v;
        break;
    case DAN_AI4: {
        if (v < -8 || v > 7) dan_arr_int_range_error(kind, v, line);
        int sh = (int)(i & 1) * 4;
        uint8_t *b = &a->data[i >> 1];
        *b = (uint8_t)((*b & ~(0xFu << sh)) | (((unsigned)v & 0xFu) << sh));
        break;
    }
    default: {
        if (v < -2 || v > 1) dan_arr_int_range_error(kind, v, line);
        int sh = (int)(i & 3) * 2;
        uint8_t *b = &a->data[i >> 2];
        *b = (uint8_t)((*b & ~(0x3u << sh)) | (((unsigned)v & 0x3u) << sh));
    }
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
    if (a->len) memcpy(r->data, a->data, dan_arr_nbytes(a->kind, a->len));
    return r;
}

/* the place *pp is about to change the array: if the array is held elsewhere too,
   or its elements are not its own, the place gets its own copy */
static dan_arr *dan_arr_mut_copy(dan_arr **pp) {
    dan_arr *a = *pp;
    dan_arr *r = dan_arr_copy(a);
    r->h.rc = 1;
    dan_arr_release(a);
    *pp = r;
    return r;
}
/* the check is inlined: a loop that writes the array element by element (м[к] = ...) calls it at
   every element, and the copy is made at most once */
static inline dan_arr *dan_arr_mut(dan_arr **pp) {
    dan_arr *a = *pp;
    if (a->h.rc > 1 || a->ro) return dan_arr_mut_copy(pp);
    return a;
}

/* copy for another task */
static dan_arr *dan_arr_clone(dan_arr *a) {
    if (!a) return a;
    dan_arr *r = dan_alloc(sizeof *r);
    *r = (dan_arr){ .len = a->len, .kind = a->kind };
    dan_hdr_detached(&r->h);
    r->data = dan_arr_block(a->kind, a->len, &r->block, 0);
    if (a->len) memcpy(r->data, a->data, dan_arr_nbytes(a->kind, a->len));
    return r;
}

/* м[а..б] - elements а..б inclusive (б == а - 1 gives an empty array); shares the elements */
static dan_arr *dan_arr_slice(dan_arr *a, int64_t from, int64_t to, int line) {
    if (from < 0 || from > a->len) dan_index_error(from, a->len, line);
    if (to < from - 1 || to >= a->len) dan_index_error(to, a->len, line);
    if ((from * dan_arr_bits[a->kind]) % 8) {
        /* a packed part that starts inside a byte: its own copy */
        dan_arr *r = dan_arr_new(a->kind, to - from + 1, line);
        for (int64_t i = 0; i < r->len; i++) dan_arr_wi(r, i, dan_arr_ri(a, from + i), line);
        return r;
    }
    dan_arr *root = a->owner ? a->owner : a;
    dan_arr *r = dan_arr_head(a->kind, to - from + 1);
    dan_hdr_share(&root->h);
    r->owner = root;
    r->data = a->data + from * dan_arr_bits[a->kind] / 8;
    r->ro = 1;
    return r;
}

/* м[а..б] inside a step of «параллельно для» for a shared array м: the same
   elements as dan_arr_slice, but the part does not hold м (no owner), so the
   counter of м, shared by all steps, is not changed. The check lets such a part
   go only straight into a vector function, which does not keep it; м itself is
   held by the program for the whole loop. */
static dan_arr *dan_arr_borrow(dan_arr *a, int64_t from, int64_t to, int line) {
    if (from < 0 || from > a->len) dan_index_error(from, a->len, line);
    if (to < from - 1 || to >= a->len) dan_index_error(to, a->len, line);
    if ((from * dan_arr_bits[a->kind]) % 8) return dan_arr_slice(a, from, to, line);   /* its own copy */
    dan_arr *r = dan_arr_head(a->kind, to - from + 1);
    r->data = a->data + from * dan_arr_bits[a->kind] / 8;
    r->ro = 1;
    return r;
}

static void dan_arr_part_len_error(int64_t part, int64_t value, int line) {
    char buf[200];
    snprintf(buf, sizeof buf, "в часть из %lld элементов записывается %lld: длины должны совпадать",
             (long long)part, (long long)value);
    dan_error(buf, line);
}

/* м[а..б] = v: a is the array's own (dan_arr_mut was called), v has the same kind.
   The bounds are checked as for a part; the lengths must be equal. v may share
   the elements of a (a part of it, or a itself): memmove copies overlapping
   elements correctly, and a packed copy goes through a copy of v. */
static void dan_arr_put(dan_arr *a, int64_t from, int64_t to, dan_arr *v, int line) {
    if (from < 0 || from > a->len) dan_index_error(from, a->len, line);
    if (to < from - 1 || to >= a->len) dan_index_error(to, a->len, line);
    int64_t n = to - from + 1;
    if (n != v->len) dan_arr_part_len_error(n, v->len, line);
    if (n == 0) return;
    int64_t bits = dan_arr_bits[a->kind];
    if ((from * bits) % 8 == 0 && (n * bits) % 8 == 0) {
        memmove(a->data + from * bits / 8, v->data, (size_t)(n * bits / 8));
        return;
    }
    /* packed elements inside a byte: one by one, from a copy if v lies in the same memory */
    const uint8_t *vs = v->data, *ve = v->data + dan_arr_nbytes(v->kind, v->len);
    const uint8_t *as = a->data, *ae = a->data + dan_arr_nbytes(a->kind, a->len);
    dan_arr *src = v;
    if (vs < ae && as < ve) src = dan_arr_copy(v);
    for (int64_t i = 0; i < n; i++) dan_arr_wi(a, from + i, dan_arr_ri(src, i), line);
}

/* склеить(а, б): a new array with the elements of а and then of б */
static dan_arr *dan_arr_concat(const dan_arr *a, const dan_arr *b, int line) {
    if (a->len > INT64_MAX - b->len) dan_error("массив слишком большой", line);
    dan_arr *r = dan_arr_new(a->kind, a->len + b->len, line);
    if (dan_arr_bits[a->kind] >= 8) {
        size_t na = dan_arr_nbytes(a->kind, a->len);
        if (a->len) memcpy(r->data, a->data, na);
        if (b->len) memcpy(r->data + na, b->data, dan_arr_nbytes(b->kind, b->len));
    } else {
        /* packed: the last byte of a part may hold elements after its end */
        for (int64_t i = 0; i < a->len; i++) dan_arr_wi(r, i, dan_arr_ri(a, i), line);
        for (int64_t i = 0; i < b->len; i++) dan_arr_wi(r, a->len + i, dan_arr_ri(b, i), line);
    }
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

/* ---- arrays and bytes ---- */

/* байты(м): the elements as they lie in memory (little-endian on every supported computer) */
static dan_bytes dan_arr_to_bytes(const dan_arr *a) {
    size_t n = dan_arr_nbytes(a->kind, a->len);
    dan_bytes_s *b = dan_bytes_alloc((int64_t)n);
    if (n) memcpy(b->data, a->data, n);
    if (a->len * dan_arr_bits[a->kind] % 8) b->data[n - 1] &= (uint8_t)((1u << (a->len * dan_arr_bits[a->kind] % 8)) - 1);
    return b;
}

/* массив(вид, байты): a copy of the bytes as elements of the kind */
static dan_arr *dan_arr_from_bytes(int32_t kind, dan_bytes b, int line) {
    int64_t size = dan_arr_bits[kind] >= 8 ? dan_arr_bits[kind] / 8 : 1;
    if (b->len % size) {
        char buf[300];
        snprintf(buf, sizeof buf, "в байтах %lld байт, а элемент массива<%s> занимает %lld: число байтов должно делиться на %lld",
                 (long long)b->len, dan_arr_kind_name[kind], (long long)size, (long long)size);
        dan_error(buf, line);
    }
    dan_arr *a = dan_arr_new(kind, b->len * 8 / dan_arr_bits[kind], line);
    if (b->len) memcpy(a->data, b->data, (size_t)b->len);
    return a;
}

/* ---- arrays mapped from files ----

   массив_из_файла(вид, путь, смещение, количество): the elements are read by the system from the file
   when they are first needed, nothing is read in advance; several programs that map the same file share
   one copy of it in memory. The file is mapped for reading: changing the array makes its own copy. */

#ifdef _WIN32
static void dan_arr_unmap(void *block, size_t n) { (void)n; UnmapViewOfFile(block); }
#elif defined(__wasi__)
static void dan_arr_unmap(void *block, size_t n) { (void)n; free(block); }
#else
static void dan_arr_unmap(void *block, size_t n) { munmap(block, n); }
#endif

static dan_str dan_arr_map_error(dan_str path, const char *text) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "файл \"");
    dan_buf_add(&b, path);
    dan_buf_add(&b, "\": ");
    dan_buf_add(&b, text);
    return dan_buf_str(&b);
}

/* the view of bytes [off, off + n) of the file, n > 0; NULL and *err on failure */
static void *dan_arr_map_view(dan_str path, int64_t file_size_check, int64_t off, size_t n, uint8_t **start, int64_t *size,
                              dan_str *err);

static dan_arr *dan_arr_map(int32_t kind, dan_str path, int64_t off, int64_t count, dan_str *err) {
    char buf[300];
    /* a packed element (цел4, цел2) never crosses a byte: such arrays are counted by bytes */
    int64_t esize = dan_arr_bits[kind] >= 8 ? dan_arr_bits[kind] / 8 : 1, per = dan_arr_bits[kind] >= 8 ? 1 : 8 / dan_arr_bits[kind];
    if (off < 0) {
        snprintf(buf, sizeof buf, "смещение не может быть отрицательным: %lld", (long long)off);
        *err = dan_arr_map_error(path, buf);
        return NULL;
    }
    if (count < -1) {
        snprintf(buf, sizeof buf, "количество элементов не может быть отрицательным: %lld", (long long)count);
        *err = dan_arr_map_error(path, buf);
        return NULL;
    }
    uint8_t *start = NULL;
    int64_t size = -1;
    /* first only the size of the file: the number of elements may depend on it */
    if (!dan_arr_map_view(path, 1, 0, 0, NULL, &size, err)) return NULL;
    if (off > size) {
        snprintf(buf, sizeof buf, "смещение %lld за концом файла (в файле %lld байт)", (long long)off, (long long)size);
        *err = dan_arr_map_error(path, buf);
        return NULL;
    }
    if (count == -1) {
        if ((size - off) % esize) {
            snprintf(buf, sizeof buf, "после смещения %lld в файле %lld байт, а элемент массива<%s> занимает %lld: "
                     "укажите количество элементов", (long long)off, (long long)(size - off), dan_arr_kind_name[kind],
                     (long long)esize);
            *err = dan_arr_map_error(path, buf);
            return NULL;
        }
        count = (size - off) / esize * per;
    } else if (count > (size - off) / esize * per) {
        snprintf(buf, sizeof buf, "нужно %lld элементов массива<%s> после смещения %lld, а в файле после него %lld байт",
                 (long long)count, dan_arr_kind_name[kind], (long long)off, (long long)(size - off));
        *err = dan_arr_map_error(path, buf);
        return NULL;
    }
    if (count == 0) return dan_arr_new(kind, 0, 0);
    int64_t bytes = (int64_t)dan_arr_nbytes(kind, count);
    void *view = dan_arr_map_view(path, 0, off, (size_t)bytes, &start, &size, err);
    if (!view) return NULL;
    if (off + bytes > size) {               /* the file became shorter in between */
#ifdef __wasi__
        free(view);
#else
        dan_arr_unmap(view, (size_t)(start - (uint8_t *)view) + (size_t)bytes);
#endif
        *err = dan_arr_map_error(path, "файл изменился во время отображения");
        return NULL;
    }
    dan_arr *a = dan_arr_head(kind, count);
    a->block = view;
    a->data = start;
    a->ro = 1;
#ifdef __wasi__
    a->map_len = 0;                         /* read into memory: freed as an ordinary block */
#else
    a->map_len = (size_t)(start - (uint8_t *)view) + (size_t)bytes;
#endif
    return a;
}

#ifdef _WIN32
static void *dan_arr_map_view(dan_str path, int64_t only_size, int64_t off, size_t n, uint8_t **start, int64_t *size,
                              dan_str *err) {
    wchar_t *w = dan_wide(path);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD e = GetLastError();
    dan_free(w);
    if (f == INVALID_HANDLE_VALUE) {
        *err = dan_arr_map_error(path, e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                                 ? "файла нет" : "файл нельзя открыть для чтения");
        return NULL;
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f, &li)) {
        CloseHandle(f);
        *err = dan_arr_map_error(path, "нельзя узнать размер файла");
        return NULL;
    }
    *size = li.QuadPart;
    if (only_size) {
        CloseHandle(f);
        return (void *)1;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int64_t gran = si.dwAllocationGranularity, base = off / gran * gran;
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    if (!m) {
        *err = dan_arr_map_error(path, "файл нельзя отобразить в память");
        return NULL;
    }
    void *v = MapViewOfFile(m, FILE_MAP_READ, (DWORD)((uint64_t)base >> 32), (DWORD)((uint64_t)base & 0xFFFFFFFFu),
                            (SIZE_T)(off - base) + n);
    CloseHandle(m);                         /* the view keeps the mapping */
    if (!v) {
        *err = dan_arr_map_error(path, "файл нельзя отобразить в память");
        return NULL;
    }
    *start = (uint8_t *)v + (off - base);
    return v;
}
#elif defined(__wasi__)
/* WebAssembly has no mapping of files: the part of the file is read into memory */
static void *dan_arr_map_view(dan_str path, int64_t only_size, int64_t off, size_t n, uint8_t **start, int64_t *size,
                              dan_str *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = dan_arr_map_error(path, "файла нет или его нельзя открыть для чтения");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (*size = ftell(f)) < 0) {
        fclose(f);
        *err = dan_arr_map_error(path, "нельзя узнать размер файла");
        return NULL;
    }
    if (only_size) {
        fclose(f);
        return (void *)1;
    }
    void *block = NULL;
    uint8_t *p = dan_arr_block(DAN_AU8, (int64_t)n, &block, 0);
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(p, 1, n, f) != n) {
        fclose(f);
        free(block);
        *err = dan_arr_map_error(path, "ошибка чтения");
        return NULL;
    }
    fclose(f);
    *start = p;
    return block;
}
#else
static void *dan_arr_map_view(dan_str path, int64_t only_size, int64_t off, size_t n, uint8_t **start, int64_t *size,
                              dan_str *err) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *err = dan_arr_map_error(path, errno == ENOENT ? "файла нет" : "файл нельзя открыть для чтения");
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *err = dan_arr_map_error(path, "это не обычный файл");
        return NULL;
    }
    *size = (int64_t)st.st_size;
    if (only_size) {
        close(fd);
        return (void *)1;
    }
    int64_t page = (int64_t)sysconf(_SC_PAGESIZE), base = off / page * page;
    void *v = mmap(NULL, (size_t)(off - base) + n, PROT_READ, MAP_PRIVATE, fd, (off_t)base);
    close(fd);                              /* the mapping keeps the file */
    if (v == MAP_FAILED) {
        *err = dan_arr_map_error(path, "файл нельзя отобразить в память");
        return NULL;
    }
    *start = (uint8_t *)v + (off - base);
    return v;
}
#endif
