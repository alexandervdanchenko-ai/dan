/* ==== Data library of the Dan language: files, bytes, checksums,
   compression (DEFLATE, gzip), encryption (ChaCha20-Poly1305 with a
   password, PBKDF2-HMAC-SHA256) and zip archives (DEFLATE, optional
   WinZip AES-256 encryption compatible with 7-Zip and WinRAR). ==== */

/* ---- paths: on Windows file names are converted from UTF-8 to UTF-16,
   so that Cyrillic names work regardless of the system code page ---- */

#ifdef _WIN32
static wchar_t *dan_wide(dan_str s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = dan_alloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static dan_str dan_narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = dan_alloc((size_t)(n > 0 ? n : 1));
    if (n <= 0) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
#endif

static FILE *dan_fopen(dan_str path, const char *mode) {
#ifdef _WIN32
    wchar_t wm[8];
    int i = 0;
    for (; mode[i] && i < 7; i++) wm[i] = (wchar_t)mode[i];
    wm[i] = 0;
    return _wfopen(dan_wide(path), wm);
#else
    return fopen(path, mode);
#endif
}

static void dan_path_error(const char *fmt, dan_str path, int line) {
    char buf[700];
    const char *why = errno == ENOENT ? DAN_TXT_ENOENT : errno == EACCES ? DAN_TXT_EACCES : DAN_TXT_EIO;
    snprintf(buf, sizeof buf, fmt, path, why);
    dan_error(buf, line);
}

static dan_bytes_s *dan_bytes_wrap(uint8_t *data, int64_t len) {
    dan_bytes_s *b = dan_alloc(sizeof *b);
    data = dan_realloc(data, (size_t)len + 1);
    data[len] = 0;
    b->data = data;
    b->len = len;
    return b;
}

static dan_bytes dan_read_bytes(dan_str path, int line) {
    FILE *f = dan_fopen(path, "rb");
    if (!f) dan_path_error(DAN_MSG_OPEN, path, line);
    size_t cap = 65536, len = 0;
    uint8_t *d = dan_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; d = dan_realloc(d, cap); }
        size_t n = fread(d + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) dan_path_error(DAN_MSG_READ, path, line);
    return dan_bytes_wrap(d, (int64_t)len);
}

/* Text: the UTF-8 byte order mark is skipped, Windows line ends \r\n become \n. */
static dan_str dan_read_text(dan_str path, int line) {
    dan_bytes b = dan_read_bytes(path, line);
    const uint8_t *s = b->data;
    int64_t n = b->len;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; n -= 3; }
    char *r = dan_alloc((size_t)n + 1);
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {
        if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') continue;
        r[k++] = (char)s[i];
    }
    r[k] = 0;
    return r;
}

static void dan_write_raw(dan_str path, const char *mode, const void *data, size_t len, int line) {
    FILE *f = dan_fopen(path, mode);
    if (!f) dan_path_error(DAN_MSG_OPEN_WRITE, path, line);
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    if (fclose(f) != 0 || w != len) dan_path_error(DAN_MSG_WRITE, path, line);
}

static void dan_write_file_text(dan_str path, dan_str s, int line) { dan_write_raw(path, "wb", s, strlen(s), line); }
static void dan_write_file_bytes(dan_str path, dan_bytes b, int line) { dan_write_raw(path, "wb", b->data, (size_t)b->len, line); }
static void dan_append_file_text(dan_str path, dan_str s, int line) { dan_write_raw(path, "ab", s, strlen(s), line); }

static bool dan_file_exists(dan_str path) {
#ifdef _WIN32
    return GetFileAttributesW(dan_wide(path)) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static int64_t dan_file_size(dan_str path, int line) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(dan_wide(path), GetFileExInfoStandard, &a)) {
        errno = ENOENT;
        dan_path_error(DAN_MSG_OPEN, path, line);
    }
    return ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
#else
    struct stat st;
    if (stat(path, &st) != 0) dan_path_error(DAN_MSG_OPEN, path, line);
    return (int64_t)st.st_size;
#endif
}

static void dan_delete_file(dan_str path, int line) {
#ifdef _WIN32
    if (!DeleteFileW(dan_wide(path))) {
        errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
        dan_path_error(DAN_MSG_DELETE, path, line);
    }
#else
    if (remove(path) != 0) dan_path_error(DAN_MSG_DELETE, path, line);
#endif
}

static bool dan_mkdir_one(const char *path) {
#ifdef _WIN32
    if (CreateDirectoryW(dan_wide(path), NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path, 0777) == 0) return true;
    return errno == EEXIST;
#endif
}

/* Creates a folder together with all missing parent folders. */
static void dan_make_dirs(dan_str path, int line) {
    size_t n = strlen(path);
    char *p = dan_alloc(n + 1);
    memcpy(p, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (i == n || p[i] == '/' || p[i] == '\\') {
            char keep = p[i];
            p[i] = 0;
            /* skip a drive letter such as C: */
            if (!(i == 2 && p[1] == ':') && !dan_mkdir_one(p)) dan_path_error(DAN_MSG_MKDIR, path, line);
            p[i] = keep;
        }
    }
    free(p);
}

static int dan_qcmp_str(const void *a, const void *b) { return strcmp(*(dan_str const *)a, *(dan_str const *)b); }

/* Names in a folder, sorted; folder names end with "/". */
static dl_Ls *dan_list_dir(dan_str path, int line) {
    dl_Ls *r = dl_Ls_from(0, NULL);
#ifdef _WIN32
    dan_buf pat = { NULL, 0, 0 };
    dan_buf_add(&pat, path);
    dan_buf_add(&pat, "\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(dan_wide(pat.s), &fd);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; dan_path_error(DAN_MSG_OPEN_DIR, path, line); }
    do {
        dan_str name = dan_narrow(fd.cFileName);
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) name = dan_concat(name, "/");
        dl_Ls_push(r, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) dan_path_error(DAN_MSG_OPEN_DIR, path, line);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        dan_str full = dan_concat(dan_concat(path, "/"), e->d_name);
        struct stat st;
        dan_str name = dan_strdup(e->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) name = dan_concat(name, "/");
        dl_Ls_push(r, name);
    }
    closedir(d);
#endif
    qsort(r->items, (size_t)r->len, sizeof(dan_str), dan_qcmp_str);
    return r;
}

/* ---- bytes ---- */

static dan_bytes dan_bytes_from_list(const dl_Li *l, int line) {
    uint8_t *d = dan_alloc((size_t)l->len + 1);
    for (int64_t i = 0; i < l->len; i++) {
        if (l->items[i] < 0 || l->items[i] > 255) dan_error(DAN_MSG_BYTE_RANGE, line);
        d[i] = (uint8_t)l->items[i];
    }
    return dan_bytes_wrap(d, l->len);
}

static dl_Li *dan_bytes_to_list(dan_bytes b) {
    dl_Li *r = dl_Li_from(0, NULL);
    for (int64_t i = 0; i < b->len; i++) dl_Li_push(r, b->data[i]);
    return r;
}

static dan_str dan_hex(dan_bytes b) {
    static const char digits[] = "0123456789abcdef";
    char *s = dan_alloc((size_t)b->len * 2 + 1);
    for (int64_t i = 0; i < b->len; i++) {
        s[2 * i] = digits[b->data[i] >> 4];
        s[2 * i + 1] = digits[b->data[i] & 15];
    }
    s[b->len * 2] = 0;
    return s;
}

static int dan_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static dan_bytes dan_unhex(dan_str s, int line) {
    size_t n = strlen(s);
    uint8_t *d = dan_alloc(n / 2 + 1);
    int64_t k = 0;
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r') continue;
        int v = dan_hexval(s[i]);
        if (v < 0) dan_conv_error(DAN_MSG_HEX, s, line);
        if (hi < 0) hi = v; else { d[k++] = (uint8_t)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) dan_conv_error(DAN_MSG_HEX, s, line);
    return dan_bytes_wrap(d, k);
}

/* ---- CRC-32 (as in zip and gzip) ---- */

static uint32_t dan_crc_table[256];
static int dan_crc_ready = 0;

static uint32_t dan_crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!dan_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            dan_crc_table[i] = c;
        }
        dan_crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = dan_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int64_t dan_crc32_bytes(dan_bytes b) { return dan_crc32_update(0, b->data, (size_t)b->len); }
static int64_t dan_crc32_str(dan_str s) { return dan_crc32_update(0, (const uint8_t *)s, strlen(s)); }

/* ---- byte order helpers ---- */

static uint32_t dan_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t dan_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void dan_put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void dan_put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t dan_rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t dan_rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* ---- SHA-256 ---- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } dan_sha256;

static const uint32_t dan_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void dan_sha256_init(dan_sha256 *c) {
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, h0, sizeof h0);
    c->len = 0;
    c->n = 0;
}

static void dan_sha256_block(dan_sha256 *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = dan_rotr32(w[i - 15], 7) ^ dan_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = dan_rotr32(w[i - 2], 17) ^ dan_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = dan_rotr32(e, 6) ^ dan_rotr32(e, 11) ^ dan_rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + dan_sha256_k[i] + w[i];
        uint32_t s0 = dan_rotr32(a, 2) ^ dan_rotr32(a, 13) ^ dan_rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void dan_sha256_update(dan_sha256 *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        if (c->n == 0 && n >= 64) { dan_sha256_block(c, p); p += 64; n -= 64; continue; }
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { dan_sha256_block(c, c->buf); c->n = 0; }
    }
}

static void dan_sha256_final(dan_sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    dan_sha256_update(c, &pad, 1);
    while (c->n != 56) dan_sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    dan_sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* ---- SHA-1 (only for zip AES encryption, as the standard requires) ---- */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } dan_sha1;

static void dan_sha1_init(dan_sha1 *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0;
    c->n = 0;
}

static void dan_sha1_block(dan_sha1 *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = dan_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = dan_rotl32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = dan_rotl32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void dan_sha1_update(dan_sha1 *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        if (c->n == 0 && n >= 64) { dan_sha1_block(c, p); p += 64; n -= 64; continue; }
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { dan_sha1_block(c, c->buf); c->n = 0; }
    }
}

static void dan_sha1_final(dan_sha1 *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    dan_sha1_update(c, &pad, 1);
    while (c->n != 56) dan_sha1_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    dan_sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* ---- HMAC and PBKDF2: the inner and outer hash states are prepared
   once per key, so each PBKDF2 iteration costs two compressions ---- */

typedef struct { dan_sha256 in, out; } dan_hmac256_ctx;
typedef struct { dan_sha1 in, out; } dan_hmac1_ctx;

static void dan_hmac256_init(dan_hmac256_ctx *m, const uint8_t *key, size_t klen) {
    uint8_t k[64] = { 0 }, pad[64];
    if (klen > 64) { dan_sha256 c; dan_sha256_init(&c); dan_sha256_update(&c, key, klen); dan_sha256_final(&c, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    dan_sha256_init(&m->in); dan_sha256_update(&m->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    dan_sha256_init(&m->out); dan_sha256_update(&m->out, pad, 64);
}

static void dan_hmac256(const dan_hmac256_ctx *m, const uint8_t *msg, size_t n, uint8_t out[32]) {
    dan_sha256 c = m->in;
    uint8_t ih[32];
    dan_sha256_update(&c, msg, n);
    dan_sha256_final(&c, ih);
    c = m->out;
    dan_sha256_update(&c, ih, 32);
    dan_sha256_final(&c, out);
}

static void dan_hmac1_init(dan_hmac1_ctx *m, const uint8_t *key, size_t klen) {
    uint8_t k[64] = { 0 }, pad[64];
    if (klen > 64) { dan_sha1 c; dan_sha1_init(&c); dan_sha1_update(&c, key, klen); dan_sha1_final(&c, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    dan_sha1_init(&m->in); dan_sha1_update(&m->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    dan_sha1_init(&m->out); dan_sha1_update(&m->out, pad, 64);
}

static void dan_hmac1(const dan_hmac1_ctx *m, const uint8_t *msg, size_t n, uint8_t out[20]) {
    dan_sha1 c = m->in;
    uint8_t ih[20];
    dan_sha1_update(&c, msg, n);
    dan_sha1_final(&c, ih);
    c = m->out;
    dan_sha1_update(&c, ih, 20);
    dan_sha1_final(&c, out);
}

static void dan_pbkdf2_sha256(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                              uint32_t iter, uint8_t *out, size_t outlen) {
    dan_hmac256_ctx m;
    dan_hmac256_init(&m, pw, pwlen);
    uint8_t *s = dan_alloc(slen + 4);
    memcpy(s, salt, slen);
    for (uint32_t block = 1; outlen > 0; block++) {
        s[slen] = (uint8_t)(block >> 24); s[slen + 1] = (uint8_t)(block >> 16);
        s[slen + 2] = (uint8_t)(block >> 8); s[slen + 3] = (uint8_t)block;
        uint8_t u[32], t[32];
        dan_hmac256(&m, s, slen + 4, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iter; i++) {
            dan_hmac256(&m, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t k = outlen < 32 ? outlen : 32;
        memcpy(out, t, k);
        out += k; outlen -= k;
    }
    free(s);
}

static void dan_pbkdf2_sha1(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                            uint32_t iter, uint8_t *out, size_t outlen) {
    dan_hmac1_ctx m;
    dan_hmac1_init(&m, pw, pwlen);
    uint8_t *s = dan_alloc(slen + 4);
    memcpy(s, salt, slen);
    for (uint32_t block = 1; outlen > 0; block++) {
        s[slen] = (uint8_t)(block >> 24); s[slen + 1] = (uint8_t)(block >> 16);
        s[slen + 2] = (uint8_t)(block >> 8); s[slen + 3] = (uint8_t)block;
        uint8_t u[20], t[20];
        dan_hmac1(&m, s, slen + 4, u);
        memcpy(t, u, 20);
        for (uint32_t i = 1; i < iter; i++) {
            dan_hmac1(&m, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t k = outlen < 20 ? outlen : 20;
        memcpy(out, t, k);
        out += k; outlen -= k;
    }
    free(s);
}

static dan_str dan_sha256_hex(const uint8_t *p, size_t n) {
    dan_sha256 c;
    uint8_t h[32];
    dan_sha256_init(&c);
    dan_sha256_update(&c, p, n);
    dan_sha256_final(&c, h);
    dan_bytes_s b = { 32, h };
    return dan_hex(&b);
}

static dan_str dan_hash_bytes(dan_bytes b) { return dan_sha256_hex(b->data, (size_t)b->len); }
static dan_str dan_hash_str(dan_str s) { return dan_sha256_hex((const uint8_t *)s, strlen(s)); }

/* ---- AES (encryption direction only: CTR mode needs no decryption) ---- */

static const uint8_t dan_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

typedef struct { uint8_t rk[240]; int rounds; } dan_aes;

static void dan_aes_init(dan_aes *a, const uint8_t *key, int keylen) {
    int nk = keylen / 4;
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);
    uint8_t rcon = 1;
    memcpy(a->rk, key, (size_t)keylen);
    for (int i = nk; i < total; i++) {
        uint8_t t[4];
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t x = t[0];
            t[0] = dan_aes_sbox[t[1]] ^ rcon; t[1] = dan_aes_sbox[t[2]];
            t[2] = dan_aes_sbox[t[3]]; t[3] = dan_aes_sbox[x];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = dan_aes_sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) a->rk[4 * i + j] = a->rk[4 * (i - nk) + j] ^ t[j];
    }
}

static uint8_t dan_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void dan_aes_encrypt(const dan_aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= a->rounds; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = dan_aes_sbox[s[i]];
        /* ShiftRows: the state is stored column by column */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) s[4 * c + row] = t[4 * ((c + row) % 4) + row];
        if (r != a->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ dan_xtime(a0 ^ a1);
                col[1] ^= all ^ dan_xtime(a1 ^ a2);
                col[2] ^= all ^ dan_xtime(a2 ^ a3);
                col[3] ^= all ^ dan_xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* WinZip AES: CTR mode with a little-endian counter starting at 1. */
static void dan_aes_ctr_winzip(const dan_aes *a, uint8_t *data, size_t n) {
    uint8_t ctr[16] = { 0 }, ks[16];
    for (size_t off = 0; off < n; off += 16) {
        for (int i = 0; i < 16; i++) if (++ctr[i] != 0) break;
        dan_aes_encrypt(a, ctr, ks);
        size_t k = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < k; i++) data[off + i] ^= ks[i];
    }
}

/* ---- ChaCha20 and Poly1305 (RFC 8439) ---- */

#define DAN_QR(a, b, c, d) \
    a += b; d ^= a; d = dan_rotl32(d, 16); c += d; b ^= c; b = dan_rotl32(b, 12); \
    a += b; d ^= a; d = dan_rotl32(d, 8);  c += d; b ^= c; b = dan_rotl32(b, 7)

static void dan_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = dan_le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = dan_le32(nonce + 4 * i);
    memcpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {
        DAN_QR(x[0], x[4], x[8], x[12]); DAN_QR(x[1], x[5], x[9], x[13]);
        DAN_QR(x[2], x[6], x[10], x[14]); DAN_QR(x[3], x[7], x[11], x[15]);
        DAN_QR(x[0], x[5], x[10], x[15]); DAN_QR(x[1], x[6], x[11], x[12]);
        DAN_QR(x[2], x[7], x[8], x[13]); DAN_QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) dan_put_le32(out + 4 * i, x[i] + s[i]);
}

static void dan_chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t *data, size_t n) {
    uint8_t ks[64];
    for (size_t off = 0; off < n; off += 64, counter++) {
        dan_chacha20_block(key, counter, nonce, ks);
        size_t k = n - off < 64 ? n - off : 64;
        for (size_t i = 0; i < k; i++) data[off + i] ^= ks[i];
    }
}

typedef struct { uint32_t r[5], h[5], pad[4]; size_t left; uint8_t buf[16]; int final; } dan_poly1305;

static void dan_poly1305_init(dan_poly1305 *p, const uint8_t key[32]) {
    p->r[0] = dan_le32(key + 0) & 0x3ffffff;
    p->r[1] = (dan_le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (dan_le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (dan_le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (dan_le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = dan_le32(key + 16 + 4 * i);
    p->left = 0;
    p->final = 0;
}

static void dan_poly1305_blocks(dan_poly1305 *p, const uint8_t *m, size_t n) {
    const uint32_t hibit = p->final ? 0 : (1u << 24);
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (n >= 16) {
        h0 += dan_le32(m + 0) & 0x3ffffff;
        h1 += (dan_le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (dan_le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (dan_le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (dan_le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        n -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void dan_poly1305_update(dan_poly1305 *p, const uint8_t *m, size_t n) {
    if (p->left) {
        size_t k = 16 - p->left < n ? 16 - p->left : n;
        memcpy(p->buf + p->left, m, k);
        p->left += k; m += k; n -= k;
        if (p->left < 16) return;
        dan_poly1305_blocks(p, p->buf, 16);
        p->left = 0;
    }
    size_t full = n & ~(size_t)15;
    if (full) { dan_poly1305_blocks(p, m, full); m += full; n -= full; }
    if (n) { memcpy(p->buf, m, n); p->left = n; }
}

static void dan_poly1305_finish(dan_poly1305 *p, uint8_t mac[16]) {
    if (p->left) {
        p->buf[p->left++] = 1;
        while (p->left < 16) p->buf[p->left++] = 0;
        p->final = 1;
        dan_poly1305_blocks(p, p->buf, 16);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    dan_put_le32(mac, h0); dan_put_le32(mac + 4, h1); dan_put_le32(mac + 8, h2); dan_put_le32(mac + 12, h3);
}

/* AEAD tag over aad and ciphertext (RFC 8439, section 2.8) */
static void dan_aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                         const uint8_t *ct, size_t clen, uint8_t tag[16]) {
    uint8_t otk[64], zeros[16] = { 0 }, lens[16];
    dan_chacha20_block(key, 0, nonce, otk);
    dan_poly1305 p;
    dan_poly1305_init(&p, otk);
    dan_poly1305_update(&p, aad, alen);
    if (alen % 16) dan_poly1305_update(&p, zeros, 16 - alen % 16);
    dan_poly1305_update(&p, ct, clen);
    if (clen % 16) dan_poly1305_update(&p, zeros, 16 - clen % 16);
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)alen >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)clen >> (8 * i)); }
    dan_poly1305_update(&p, lens, 16);
    dan_poly1305_finish(&p, tag);
}

static bool dan_equal_ct(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* ---- random bytes from the operating system ---- */

#ifdef _WIN32
__declspec(dllimport) BOOLEAN __stdcall SystemFunction036(PVOID buffer, ULONG length);
#endif

static void dan_random(uint8_t *out, size_t n, int line) {
#ifdef _WIN32
    if (!SystemFunction036(out, (ULONG)n)) dan_error(DAN_MSG_RANDOM, line);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(out, 1, n, f) != n) dan_error(DAN_MSG_RANDOM, line);
    fclose(f);
#endif
}

/* ---- password encryption: "DANC", version 1, PBKDF2 iterations (4 bytes),
   salt (16), nonce (12), ciphertext, tag (16) ---- */

#define DAN_ENC_HEADER 37
#define DAN_ENC_ITER 600000u

static dan_bytes dan_encrypt_raw(const uint8_t *data, size_t n, dan_str password, int line) {
    uint8_t *out = dan_alloc(DAN_ENC_HEADER + n + 16);
    memcpy(out, "DANC", 4);
    out[4] = 1;
    dan_put_le32(out + 5, DAN_ENC_ITER);
    dan_random(out + 9, 28, line);                         /* salt and nonce */
    uint8_t key[32];
    dan_pbkdf2_sha256((const uint8_t *)password, strlen(password), out + 9, 16, DAN_ENC_ITER, key, 32);
    memcpy(out + DAN_ENC_HEADER, data, n);
    dan_chacha20_xor(key, 1, out + 25, out + DAN_ENC_HEADER, n);
    dan_aead_tag(key, out + 25, out, DAN_ENC_HEADER, out + DAN_ENC_HEADER, n, out + DAN_ENC_HEADER + n);
    return dan_bytes_wrap(out, (int64_t)(DAN_ENC_HEADER + n + 16));
}

static dan_bytes dan_encrypt(dan_bytes data, dan_str password, int line) {
    return dan_encrypt_raw(data->data, (size_t)data->len, password, line);
}

static dan_bytes dan_encrypt_str(dan_str s, dan_str password, int line) {
    return dan_encrypt_raw((const uint8_t *)s, strlen(s), password, line);
}

static dan_bytes dan_decrypt(dan_bytes data, dan_str password, int line) {
    const uint8_t *in = data->data;
    if (data->len < DAN_ENC_HEADER + 16 || memcmp(in, "DANC", 4) != 0) dan_error(DAN_MSG_NOT_ENCRYPTED, line);
    if (in[4] != 1) dan_error(DAN_MSG_ENC_VERSION, line);
    uint32_t iter = dan_le32(in + 5);
    if (iter == 0 || iter > 100000000u) dan_error(DAN_MSG_NOT_ENCRYPTED, line);
    size_t n = (size_t)data->len - DAN_ENC_HEADER - 16;
    uint8_t key[32], tag[16];
    dan_pbkdf2_sha256((const uint8_t *)password, strlen(password), in + 9, 16, iter, key, 32);
    dan_aead_tag(key, in + 25, in, DAN_ENC_HEADER, in + DAN_ENC_HEADER, n, tag);
    if (!dan_equal_ct(tag, in + DAN_ENC_HEADER + n, 16)) dan_error(DAN_MSG_BAD_PASSWORD, line);
    uint8_t *out = dan_alloc(n + 1);
    memcpy(out, in + DAN_ENC_HEADER, n);
    dan_chacha20_xor(key, 1, in + 25, out, n);
    return dan_bytes_wrap(out, (int64_t)n);
}

/* ==== DEFLATE (RFC 1951) ==== */

static const uint16_t dan_len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t dan_len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dan_dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
                                            2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dan_dist_extra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const uint8_t dan_cl_order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

/* ---- output bits, least significant first ---- */

typedef struct { uint8_t *d; size_t len, cap; uint64_t bits; int nbits; } dan_bitw;

static void dan_bitw_byte(dan_bitw *w, uint8_t b) {
    if (w->len == w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->d = dan_realloc(w->d, w->cap); }
    w->d[w->len++] = b;
}

static void dan_bitw_put(dan_bitw *w, uint32_t v, int n) {
    w->bits |= (uint64_t)v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) { dan_bitw_byte(w, (uint8_t)w->bits); w->bits >>= 8; w->nbits -= 8; }
}

static void dan_bitw_align(dan_bitw *w) { if (w->nbits > 0) dan_bitw_put(w, 0, 8 - w->nbits); }

/* ---- Huffman code lengths limited to `limit` bits ---- */

static void dan_huff_lengths(const uint32_t *freq_in, int n, int limit, uint8_t *len) {
    uint32_t freq[288];
    for (int i = 0; i < n; i++) freq[i] = freq_in[i];
    for (;;) {
        /* nodes 0..n-1 are leaves, then internal nodes */
        uint64_t w[576];
        int parent[576], alive[576], count = 0, nodes = n;
        for (int i = 0; i < n; i++) { w[i] = freq[i]; parent[i] = -1; alive[i] = freq[i] > 0; count += alive[i]; }
        for (int i = 0; i < n; i++) len[i] = 0;
        if (count == 0) return;
        if (count == 1) { for (int i = 0; i < n; i++) if (alive[i]) len[i] = 1; return; }
        while (count > 1) {
            int a = -1, b = -1;
            for (int i = 0; i < nodes; i++) {
                if (!alive[i]) continue;
                if (a < 0 || w[i] < w[a]) { b = a; a = i; }
                else if (b < 0 || w[i] < w[b]) b = i;
            }
            w[nodes] = w[a] + w[b];
            parent[nodes] = -1;
            alive[nodes] = 1;
            parent[a] = parent[b] = nodes;
            alive[a] = alive[b] = 0;
            nodes++;
            count--;
        }
        int maxlen = 0;
        for (int i = 0; i < n; i++) {
            if (!freq[i]) continue;
            int d = 0;
            for (int p = i; parent[p] >= 0; p = parent[p]) d++;
            len[i] = (uint8_t)d;
            if (d > maxlen) maxlen = d;
        }
        if (maxlen <= limit) return;
        for (int i = 0; i < n; i++) if (freq[i]) freq[i] = (freq[i] + 1) / 2;   /* flatten and retry */
    }
}

static void dan_huff_codes(const uint8_t *len, int n, uint16_t *code) {
    uint16_t count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint16_t c = 0;
    for (int b = 1; b < 16; b++) { c = (uint16_t)((c + count[b - 1]) << 1); next[b] = c; }
    for (int i = 0; i < n; i++) {
        if (!len[i]) continue;
        uint16_t v = next[len[i]]++, r = 0;
        for (int k = 0; k < len[i]; k++) { r = (uint16_t)((r << 1) | (v & 1)); v >>= 1; }
        code[i] = r;                                   /* reversed for LSB-first output */
    }
}

/* ---- LZ77 matching with hash chains and lazy evaluation ---- */

typedef struct { uint16_t len, dist; } dan_lz;          /* len == 0: literal byte in dist */

#define DAN_WINDOW 32768
#define DAN_HASH_BITS 15
#define DAN_MAX_CHAIN 4096

static int dan_len_code(int len) { int c = 0; while (c < 28 && dan_len_base[c + 1] <= len) c++; return c; }
static int dan_dist_code(int dist) { int c = 0; while (c < 29 && dan_dist_base[c + 1] <= dist) c++; return c; }

typedef struct { const uint8_t *d; size_t n; int32_t *head, *prev; } dan_lz77;

static uint32_t dan_hash3(const uint8_t *p) { return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & ((1u << DAN_HASH_BITS) - 1); }

static void dan_lz_insert(dan_lz77 *z, size_t p) {
    if (p + 3 > z->n) return;
    uint32_t h = dan_hash3(z->d + p);
    z->prev[p] = z->head[h];
    z->head[h] = (int32_t)p;
}

static int dan_lz_search(dan_lz77 *z, size_t p, int *dist) {
    if (p + 3 > z->n) return 0;
    int maxlen = z->n - p < 258 ? (int)(z->n - p) : 258, best = 0;
    int32_t cand = z->head[dan_hash3(z->d + p)];
    int chain = DAN_MAX_CHAIN;
    while (cand >= 0 && p - (size_t)cand <= DAN_WINDOW && chain-- > 0) {
        const uint8_t *a = z->d + cand, *b = z->d + p;
        if (a[best] == b[best] && a[0] == b[0]) {
            int l = 0;
            while (l < maxlen && a[l] == b[l]) l++;
            if (l > best) { best = l; *dist = (int)(p - (size_t)cand); if (l == maxlen) break; }
        }
        cand = z->prev[cand];
    }
    return best >= 3 ? best : 0;
}

static dan_lz *dan_lz77_parse(const uint8_t *d, size_t n, size_t *count) {
    dan_lz77 z = { d, n, dan_alloc(sizeof(int32_t) << DAN_HASH_BITS), dan_alloc(sizeof(int32_t) * (n ? n : 1)) };
    for (size_t i = 0; i < (1u << DAN_HASH_BITS); i++) z.head[i] = -1;
    dan_lz *out = dan_alloc(sizeof(dan_lz) * (n ? n : 1));
    size_t k = 0, i = 0;
    int have_next = 0, next_len = 0, next_dist = 0;
    while (i < n) {
        int len, dist = 0;
        if (have_next) { len = next_len; dist = next_dist; have_next = 0; }
        else len = dan_lz_search(&z, i, &dist);
        dan_lz_insert(&z, i);
        if (len >= 3 && len < 258 && i + 1 < n) {
            next_len = dan_lz_search(&z, i + 1, &next_dist);
            if (next_len > len) {                       /* a longer match starts one byte later */
                out[k].len = 0; out[k].dist = d[i]; k++;
                i++;
                have_next = 1;
                continue;
            }
        }
        if (len >= 3) {
            out[k].len = (uint16_t)len; out[k].dist = (uint16_t)dist; k++;
            for (int j = 1; j < len; j++) dan_lz_insert(&z, i + (size_t)j);
            i += (size_t)len;
        } else {
            out[k].len = 0; out[k].dist = d[i]; k++;
            i++;
        }
    }
    free(z.head);
    free(z.prev);
    *count = k;
    return out;
}

/* ---- block writer: each block is sent as stored, fixed or dynamic Huffman,
   whichever is shortest ---- */

static void dan_fixed_lengths(uint8_t *ll, uint8_t *dl) {
    for (int i = 0; i < 288; i++) ll[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++) dl[i] = 5;
}

static void dan_write_symbols(dan_bitw *w, const dan_lz *s, size_t n, const uint8_t *ll, const uint16_t *lc,
                              const uint8_t *dl, const uint16_t *dc) {
    for (size_t i = 0; i < n; i++) {
        if (s[i].len == 0) { dan_bitw_put(w, lc[s[i].dist], ll[s[i].dist]); continue; }
        int c = dan_len_code(s[i].len);
        dan_bitw_put(w, lc[257 + c], ll[257 + c]);
        if (dan_len_extra[c]) dan_bitw_put(w, (uint32_t)(s[i].len - dan_len_base[c]), dan_len_extra[c]);
        int dcode = dan_dist_code(s[i].dist);
        dan_bitw_put(w, dc[dcode], dl[dcode]);
        if (dan_dist_extra[dcode]) dan_bitw_put(w, (uint32_t)(s[i].dist - dan_dist_base[dcode]), dan_dist_extra[dcode]);
    }
    dan_bitw_put(w, lc[256], ll[256]);
}

static uint64_t dan_symbols_cost(const uint32_t *lf, const uint32_t *df, const uint8_t *ll, const uint8_t *dl) {
    uint64_t bits = 0;
    for (int i = 0; i < 286; i++) bits += (uint64_t)lf[i] * (ll[i] + (i >= 257 ? dan_len_extra[i - 257] : 0));
    for (int i = 0; i < 30; i++) bits += (uint64_t)df[i] * (dl[i] + dan_dist_extra[i]);
    return bits;
}

/* run-length encoding of the code lengths (symbols 0-15, 16, 17, 18) */
typedef struct { uint8_t sym, extra; } dan_clsym;

static int dan_rle_lengths(const uint8_t *lens, int n, dan_clsym *out) {
    int k = 0;
    for (int i = 0; i < n;) {
        int run = 1;
        while (i + run < n && lens[i + run] == lens[i]) run++;
        if (lens[i] == 0) {
            int r = run;
            while (r >= 11) { int t = r > 138 ? 138 : r; out[k].sym = 18; out[k].extra = (uint8_t)(t - 11); k++; r -= t; }
            if (r >= 3) { out[k].sym = 17; out[k].extra = (uint8_t)(r - 3); k++; r = 0; }
            while (r-- > 0) { out[k].sym = 0; out[k].extra = 0; k++; }
        } else {
            out[k].sym = lens[i]; out[k].extra = 0; k++;
            int r = run - 1;
            while (r >= 3) { int t = r > 6 ? 6 : r; out[k].sym = 16; out[k].extra = (uint8_t)(t - 3); k++; r -= t; }
            while (r-- > 0) { out[k].sym = lens[i]; out[k].extra = 0; k++; }
        }
        i += run;
    }
    return k;
}

static void dan_write_stored(dan_bitw *w, const uint8_t *d, size_t n, int last) {
    do {
        size_t k = n > 65535 ? 65535 : n;
        int fin = last && k == n;
        dan_bitw_put(w, (uint32_t)fin, 1);
        dan_bitw_put(w, 0, 2);
        dan_bitw_align(w);
        dan_bitw_byte(w, (uint8_t)k); dan_bitw_byte(w, (uint8_t)(k >> 8));
        dan_bitw_byte(w, (uint8_t)~k); dan_bitw_byte(w, (uint8_t)(~k >> 8));
        for (size_t i = 0; i < k; i++) dan_bitw_byte(w, d[i]);
        d += k;
        n -= k;
    } while (n > 0);
}

static void dan_write_block(dan_bitw *w, const dan_lz *s, size_t ns, const uint8_t *raw, size_t rawlen, int last) {
    uint32_t lf[288] = { 0 }, df[30] = { 0 };
    for (size_t i = 0; i < ns; i++) {
        if (s[i].len == 0) lf[s[i].dist]++;
        else { lf[257 + dan_len_code(s[i].len)]++; df[dan_dist_code(s[i].dist)]++; }
    }
    lf[256] = 1;
    /* complete codes need at least two symbols */
    uint32_t lf2[288], df2[30];
    memcpy(lf2, lf, sizeof lf);
    memcpy(df2, df, sizeof df);
    int used = 0;
    for (int i = 0; i < 286; i++) used += lf2[i] > 0;
    if (used < 2) lf2[lf2[0] ? 1 : 0]++;
    used = 0;
    for (int i = 0; i < 30; i++) used += df2[i] > 0;
    if (used == 0) { df2[0] = 1; df2[1] = 1; } else if (used == 1) df2[df2[0] ? 1 : 0]++;

    uint8_t ll[288] = { 0 }, dl[30] = { 0 }, fll[288], fdl[30];
    dan_huff_lengths(lf2, 286, 15, ll);
    dan_huff_lengths(df2, 30, 15, dl);
    dan_fixed_lengths(fll, fdl);

    int hlit = 286, hdist = 30;
    while (hlit > 257 && ll[hlit - 1] == 0) hlit--;
    while (hdist > 1 && dl[hdist - 1] == 0) hdist--;
    uint8_t all[316];
    memcpy(all, ll, (size_t)hlit);
    memcpy(all + hlit, dl, (size_t)hdist);
    dan_clsym cl[316];
    int ncl = dan_rle_lengths(all, hlit + hdist, cl);
    uint32_t cf[19] = { 0 };
    for (int i = 0; i < ncl; i++) cf[cl[i].sym]++;
    int cl_used = 0;
    for (int i = 0; i < 19; i++) cl_used += cf[i] > 0;
    if (cl_used < 2) cf[cf[0] ? 1 : 0]++;      /* zlib requires a complete code-length code */
    uint8_t clen[19];
    dan_huff_lengths(cf, 19, 7, clen);
    int hclen = 19;
    while (hclen > 4 && clen[dan_cl_order[hclen - 1]] == 0) hclen--;

    uint64_t dyn = 3 + 14 + 3 * (uint64_t)hclen + dan_symbols_cost(lf, df, ll, dl);
    for (int i = 0; i < ncl; i++) dyn += clen[cl[i].sym] + (cl[i].sym == 16 ? 2 : cl[i].sym == 17 ? 3 : cl[i].sym == 18 ? 7 : 0);
    uint64_t fix = 3 + dan_symbols_cost(lf, df, fll, fdl);
    uint64_t sto = (rawlen + 4 * (rawlen / 65535 + 1)) * 8 + 8 + 3;

    if (sto <= dyn && sto <= fix) { dan_write_stored(w, raw, rawlen, last); return; }
    dan_bitw_put(w, (uint32_t)last, 1);
    uint16_t lc[288], dc[30];
    if (fix <= dyn) {
        dan_bitw_put(w, 1, 2);
        dan_huff_codes(fll, 288, lc);
        dan_huff_codes(fdl, 30, dc);
        dan_write_symbols(w, s, ns, fll, lc, fdl, dc);
        return;
    }
    dan_bitw_put(w, 2, 2);
    dan_bitw_put(w, (uint32_t)(hlit - 257), 5);
    dan_bitw_put(w, (uint32_t)(hdist - 1), 5);
    dan_bitw_put(w, (uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) dan_bitw_put(w, clen[dan_cl_order[i]], 3);
    uint16_t cc[19];
    dan_huff_codes(clen, 19, cc);
    for (int i = 0; i < ncl; i++) {
        dan_bitw_put(w, cc[cl[i].sym], clen[cl[i].sym]);
        if (cl[i].sym == 16) dan_bitw_put(w, cl[i].extra, 2);
        else if (cl[i].sym == 17) dan_bitw_put(w, cl[i].extra, 3);
        else if (cl[i].sym == 18) dan_bitw_put(w, cl[i].extra, 7);
    }
    dan_huff_codes(ll, 286, lc);
    dan_huff_codes(dl, 30, dc);
    dan_write_symbols(w, s, ns, ll, lc, dl, dc);
}

#define DAN_BLOCK_SYMBOLS 16384

static uint8_t *dan_deflate_raw(const uint8_t *d, size_t n, size_t *outlen) {
    dan_bitw w = { NULL, 0, 0, 0, 0 };
    size_t ns;
    dan_lz *s = dan_lz77_parse(d, n, &ns);
    if (ns == 0) {                                     /* empty input: one fixed block with end-of-block only */
        dan_bitw_put(&w, 1, 1);
        dan_bitw_put(&w, 1, 2);
        dan_bitw_put(&w, 0, 7);
    }
    size_t pos = 0;
    for (size_t b = 0; b < ns; b += DAN_BLOCK_SYMBOLS) {
        size_t k = ns - b < DAN_BLOCK_SYMBOLS ? ns - b : DAN_BLOCK_SYMBOLS;
        size_t raw = 0;
        for (size_t i = b; i < b + k; i++) raw += s[i].len ? s[i].len : 1;
        dan_write_block(&w, s + b, k, d + pos, raw, b + k == ns);
        pos += raw;
    }
    dan_bitw_align(&w);
    free(s);
    *outlen = w.len;
    return w.d;
}

/* ---- inflate ---- */

typedef struct {
    const uint8_t *in; size_t inlen, pos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outcap;
    int line;
} dan_inf;

typedef struct { int16_t count[16]; int16_t symbol[288]; } dan_huff;

static void dan_inf_fail(dan_inf *s) { dan_error(DAN_MSG_CORRUPT, s->line); }

static int dan_inf_bits(dan_inf *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->inlen) dan_inf_fail(s);
        v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static void dan_inf_put(dan_inf *s, uint8_t b) {
    if (s->outlen == s->outcap) { s->outcap = s->outcap ? s->outcap * 2 : 65536; s->out = dan_realloc(s->out, s->outcap); }
    s->out[s->outlen++] = b;
}

static int dan_huff_build(dan_huff *h, const uint8_t *len, int n) {
    int16_t offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (int16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (int16_t)i;
    return left;
}

static int dan_huff_decode(dan_inf *s, const dan_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= dan_inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    dan_inf_fail(s);
    return -1;
}

static void dan_inf_codes(dan_inf *s, const dan_huff *lc, const dan_huff *dc) {
    for (;;) {
        int sym = dan_huff_decode(s, lc);
        if (sym < 256) { dan_inf_put(s, (uint8_t)sym); continue; }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) dan_inf_fail(s);
        size_t len = dan_len_base[sym] + (size_t)dan_inf_bits(s, dan_len_extra[sym]);
        int ds = dan_huff_decode(s, dc);
        if (ds >= 30) dan_inf_fail(s);
        size_t dist = dan_dist_base[ds] + (size_t)dan_inf_bits(s, dan_dist_extra[ds]);
        if (dist > s->outlen) dan_inf_fail(s);
        while (len--) dan_inf_put(s, s->out[s->outlen - dist]);
    }
}

static void dan_inflate_into(dan_inf *s) {
    int last;
    do {
        last = dan_inf_bits(s, 1);
        int type = dan_inf_bits(s, 2);
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->pos + 4 > s->inlen) dan_inf_fail(s);
            unsigned len = s->in[s->pos] | s->in[s->pos + 1] << 8;
            unsigned nlen = s->in[s->pos + 2] | s->in[s->pos + 3] << 8;
            s->pos += 4;
            if (len != (~nlen & 0xFFFF) || s->pos + len > s->inlen) dan_inf_fail(s);
            for (unsigned i = 0; i < len; i++) dan_inf_put(s, s->in[s->pos + i]);
            s->pos += len;
        } else if (type == 1) {
            static dan_huff fl, fd;
            static int ready = 0;
            if (!ready) {
                uint8_t ll[288], dl[30];
                dan_fixed_lengths(ll, dl);
                dan_huff_build(&fl, ll, 288);
                dan_huff_build(&fd, dl, 30);
                ready = 1;
            }
            dan_inf_codes(s, &fl, &fd);
        } else if (type == 2) {
            int nlen = dan_inf_bits(s, 5) + 257, ndist = dan_inf_bits(s, 5) + 1, ncode = dan_inf_bits(s, 4) + 4;
            if (nlen > 286 || ndist > 30) dan_inf_fail(s);
            uint8_t lengths[320] = { 0 };
            for (int i = 0; i < ncode; i++) lengths[dan_cl_order[i]] = (uint8_t)dan_inf_bits(s, 3);
            dan_huff lencode, lc, dc;
            if (dan_huff_build(&lencode, lengths, 19) != 0) dan_inf_fail(s);
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = dan_huff_decode(s, &lencode);
                if (sym < 16) { lengths[idx++] = (uint8_t)sym; continue; }
                int len = 0, rep;
                if (sym == 16) { if (idx == 0) dan_inf_fail(s); len = lengths[idx - 1]; rep = 3 + dan_inf_bits(s, 2); }
                else if (sym == 17) rep = 3 + dan_inf_bits(s, 3);
                else rep = 11 + dan_inf_bits(s, 7);
                if (idx + rep > nlen + ndist) dan_inf_fail(s);
                while (rep--) lengths[idx++] = (uint8_t)len;
            }
            if (lengths[256] == 0) dan_inf_fail(s);
            if (dan_huff_build(&lc, lengths, nlen) < 0) dan_inf_fail(s);
            if (dan_huff_build(&dc, lengths + nlen, ndist) < 0) dan_inf_fail(s);
            dan_inf_codes(s, &lc, &dc);
        } else dan_inf_fail(s);
    } while (!last);
}

static uint8_t *dan_inflate_raw(const uint8_t *in, size_t n, size_t *outlen, size_t *used, int line) {
    dan_inf s = { in, n, 0, 0, 0, NULL, 0, 0, line };
    dan_inflate_into(&s);
    *outlen = s.outlen;
    if (used) *used = s.pos;
    if (!s.out) s.out = dan_alloc(1);
    return s.out;
}

/* ---- gzip container (RFC 1952): the result opens in 7-Zip, WinRAR, gzip ---- */

static dan_bytes dan_gzip_raw(const uint8_t *d, size_t n) {
    size_t clen;
    uint8_t *c = dan_deflate_raw(d, n, &clen);
    uint8_t *out = dan_alloc(clen + 18);
    static const uint8_t head[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 2, 255 };
    memcpy(out, head, 10);
    memcpy(out + 10, c, clen);
    dan_put_le32(out + 10 + clen, dan_crc32_update(0, d, n));
    dan_put_le32(out + 14 + clen, (uint32_t)n);
    free(c);
    return dan_bytes_wrap(out, (int64_t)(clen + 18));
}

static dan_bytes dan_gzip(dan_bytes b) { return dan_gzip_raw(b->data, (size_t)b->len); }
static dan_bytes dan_gzip_str(dan_str s) { return dan_gzip_raw((const uint8_t *)s, strlen(s)); }

static dan_bytes dan_gunzip(dan_bytes b, int line) {
    const uint8_t *p = b->data;
    size_t n = (size_t)b->len;
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) dan_error(DAN_MSG_NOT_COMPRESSED, line);
    int flags = p[3];
    size_t pos = 10;
    if (flags & 4) { if (pos + 2 > n) dan_error(DAN_MSG_CORRUPT, line); pos += 2 + dan_le16(p + pos); }
    if (flags & 8) { while (pos < n && p[pos]) pos++; pos++; }
    if (flags & 16) { while (pos < n && p[pos]) pos++; pos++; }
    if (flags & 2) pos += 2;
    if (pos >= n) dan_error(DAN_MSG_CORRUPT, line);
    size_t outlen, used;
    uint8_t *out = dan_inflate_raw(p + pos, n - pos, &outlen, &used, line);
    pos += used;
    if (pos + 8 > n) dan_error(DAN_MSG_CORRUPT, line);
    if (dan_le32(p + pos) != dan_crc32_update(0, out, outlen) || dan_le32(p + pos + 4) != (uint32_t)outlen)
        dan_error(DAN_MSG_CORRUPT, line);
    return dan_bytes_wrap(out, (int64_t)outlen);
}

/* ==== zip archives ==== */

typedef struct { uint8_t *d; size_t len, cap; } dan_bbuf;

static void dan_bbuf_add(dan_bbuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 1024; b->d = dan_realloc(b->d, b->cap); }
    memcpy(b->d + b->len, p, n);
    b->len += n;
}
static void dan_bbuf_u16(dan_bbuf *b, uint32_t v) { uint8_t t[2]; dan_put_le16(t, v); dan_bbuf_add(b, t, 2); }
static void dan_bbuf_u32(dan_bbuf *b, uint32_t v) { uint8_t t[4]; dan_put_le32(t, v); dan_bbuf_add(b, t, 4); }

static const char *dan_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void dan_dos_time(uint16_t *t, uint16_t *d) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) { *t = 0; *d = (1 << 5) | 1; return; }
    *t = (uint16_t)((lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2));
    *d = (uint16_t)(((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) | lt->tm_mday);
}

static void dan_create_zip(dan_str zip_path, const dl_Ls *files, dan_str password, int line) {
    dan_bbuf out = { NULL, 0, 0 }, cd = { NULL, 0, 0 };
    uint16_t t, d;
    dan_dos_time(&t, &d);
    int encrypt = password && password[0];
    if (files->len > 65535) dan_error(DAN_MSG_ZIP_BIG, line);
    for (int64_t i = 0; i < files->len; i++) {
        dan_bytes data = dan_read_bytes(files->items[i], line);
        if (data->len >= 0xFFFFFFFFLL) dan_error(DAN_MSG_ZIP_BIG, line);
        const char *name = dan_basename(files->items[i]);
        size_t clen;
        uint8_t *comp = dan_deflate_raw(data->data, (size_t)data->len, &clen);
        uint16_t method = 8;
        if (clen >= (size_t)data->len) { free(comp); comp = data->data; clen = (size_t)data->len; method = 0; }
        uint32_t crc = dan_crc32_update(0, data->data, (size_t)data->len);
        dan_bbuf payload = { NULL, 0, 0 }, extra = { NULL, 0, 0 };
        uint16_t flags = 1 << 11, version = 20, stored_method = method;
        if (encrypt) {
            /* WinZip AES-256, version AE-2 */
            uint8_t salt[16], keys[66], mac[20];
            dan_random(salt, 16, line);
            dan_pbkdf2_sha1((const uint8_t *)password, strlen(password), salt, 16, 1000, keys, 66);
            uint8_t *ct = dan_alloc(clen + 1);
            memcpy(ct, comp, clen);
            dan_aes aes;
            dan_aes_init(&aes, keys, 32);
            dan_aes_ctr_winzip(&aes, ct, clen);
            dan_hmac1_ctx hm;
            dan_hmac1_init(&hm, keys + 32, 32);
            dan_hmac1(&hm, ct, clen, mac);
            dan_bbuf_add(&payload, salt, 16);
            dan_bbuf_add(&payload, keys + 64, 2);
            dan_bbuf_add(&payload, ct, clen);
            dan_bbuf_add(&payload, mac, 10);
            free(ct);
            dan_bbuf_u16(&extra, 0x9901); dan_bbuf_u16(&extra, 7); dan_bbuf_u16(&extra, 2);
            dan_bbuf_add(&extra, "AE", 2);
            uint8_t strength = 3;
            dan_bbuf_add(&extra, &strength, 1);
            dan_bbuf_u16(&extra, method);
            flags |= 1;
            version = 51;
            stored_method = 99;
            crc = 0;
        } else dan_bbuf_add(&payload, comp, clen);
        if (out.len + payload.len + 1024 > 0xFFFFFFFFu) dan_error(DAN_MSG_ZIP_BIG, line);
        uint32_t offset = (uint32_t)out.len;
        size_t nlen = strlen(name);
        /* local file header */
        dan_bbuf_u32(&out, 0x04034b50); dan_bbuf_u16(&out, version); dan_bbuf_u16(&out, flags);
        dan_bbuf_u16(&out, stored_method); dan_bbuf_u16(&out, t); dan_bbuf_u16(&out, d);
        dan_bbuf_u32(&out, crc); dan_bbuf_u32(&out, (uint32_t)payload.len); dan_bbuf_u32(&out, (uint32_t)data->len);
        dan_bbuf_u16(&out, (uint32_t)nlen); dan_bbuf_u16(&out, (uint32_t)extra.len);
        dan_bbuf_add(&out, name, nlen);
        if (extra.len) dan_bbuf_add(&out, extra.d, extra.len);
        dan_bbuf_add(&out, payload.d, payload.len);
        /* central directory entry */
        dan_bbuf_u32(&cd, 0x02014b50); dan_bbuf_u16(&cd, 63); dan_bbuf_u16(&cd, version); dan_bbuf_u16(&cd, flags);
        dan_bbuf_u16(&cd, stored_method); dan_bbuf_u16(&cd, t); dan_bbuf_u16(&cd, d);
        dan_bbuf_u32(&cd, crc); dan_bbuf_u32(&cd, (uint32_t)payload.len); dan_bbuf_u32(&cd, (uint32_t)data->len);
        dan_bbuf_u16(&cd, (uint32_t)nlen); dan_bbuf_u16(&cd, (uint32_t)extra.len); dan_bbuf_u16(&cd, 0);
        dan_bbuf_u16(&cd, 0); dan_bbuf_u16(&cd, 0); dan_bbuf_u32(&cd, 0x20); dan_bbuf_u32(&cd, offset);
        dan_bbuf_add(&cd, name, nlen);
        if (extra.len) dan_bbuf_add(&cd, extra.d, extra.len);
        free(payload.d);
        free(extra.d);
    }
    uint32_t cd_offset = (uint32_t)out.len;
    dan_bbuf_add(&out, cd.d ? cd.d : (uint8_t *)"", cd.len);
    dan_bbuf_u32(&out, 0x06054b50); dan_bbuf_u16(&out, 0); dan_bbuf_u16(&out, 0);
    dan_bbuf_u16(&out, (uint32_t)files->len); dan_bbuf_u16(&out, (uint32_t)files->len);
    dan_bbuf_u32(&out, (uint32_t)cd.len); dan_bbuf_u32(&out, cd_offset); dan_bbuf_u16(&out, 0);
    dan_write_raw(zip_path, "wb", out.d, out.len, line);
    free(out.d);
    free(cd.d);
}

static void dan_create_zip_plain(dan_str zip_path, const dl_Ls *files, int line) { dan_create_zip(zip_path, files, NULL, line); }

typedef struct {
    dan_str name; uint16_t flags, method; uint32_t crc, csize, usize, offset;
    int aes; int aes_strength; uint16_t aes_method; uint16_t aes_version;
} dan_zip_entry;

static dan_zip_entry *dan_zip_entries(dan_bytes z, int64_t *count, int line) {
    const uint8_t *p = z->data;
    size_t n = (size_t)z->len;
    if (n < 22) dan_error(DAN_MSG_NOT_ZIP, line);
    size_t eocd = 0;
    int found = 0;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 65535 + 22;) {
        if (dan_le32(p + i) == 0x06054b50) { eocd = i; found = 1; break; }
    }
    if (!found) dan_error(DAN_MSG_NOT_ZIP, line);
    uint16_t entries = dan_le16(p + eocd + 10);
    uint32_t cd_offset = dan_le32(p + eocd + 16);
    dan_zip_entry *e = dan_alloc(sizeof(dan_zip_entry) * (size_t)(entries ? entries : 1));
    size_t pos = cd_offset;
    for (int i = 0; i < entries; i++) {
        if (pos + 46 > n || dan_le32(p + pos) != 0x02014b50) dan_error(DAN_MSG_CORRUPT, line);
        dan_zip_entry *x = &e[i];
        memset(x, 0, sizeof *x);
        x->flags = dan_le16(p + pos + 8);
        x->method = dan_le16(p + pos + 10);
        x->crc = dan_le32(p + pos + 16);
        x->csize = dan_le32(p + pos + 20);
        x->usize = dan_le32(p + pos + 24);
        uint16_t nlen = dan_le16(p + pos + 28), xlen = dan_le16(p + pos + 30), clen = dan_le16(p + pos + 32);
        x->offset = dan_le32(p + pos + 42);
        if (pos + 46 + nlen + xlen + clen > n) dan_error(DAN_MSG_CORRUPT, line);
        char *name = dan_alloc((size_t)nlen + 1);
        memcpy(name, p + pos + 46, nlen);
        name[nlen] = 0;
        x->name = name;
        for (size_t q = pos + 46 + nlen; q + 4 <= pos + 46 + nlen + xlen;) {
            uint16_t id = dan_le16(p + q), sz = dan_le16(p + q + 2);
            if (id == 0x9901 && sz >= 7) {
                x->aes = 1;
                x->aes_version = dan_le16(p + q + 4);
                x->aes_strength = p[q + 8];
                x->aes_method = dan_le16(p + q + 9);
            }
            q += 4 + (size_t)sz;
        }
        pos += 46 + (size_t)nlen + xlen + clen;
    }
    *count = entries;
    return e;
}

static dl_Ls *dan_zip_list(dan_str zip_path, int line) {
    int64_t k;
    dan_zip_entry *e = dan_zip_entries(dan_read_bytes(zip_path, line), &k, line);
    dl_Ls *r = dl_Ls_from(0, NULL);
    for (int64_t i = 0; i < k; i++) dl_Ls_push(r, e[i].name);
    return r;
}

/* Names that could write outside the target folder are refused (zip slip). */
static void dan_check_entry_name(char *name, int line) {
    for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
    if (name[0] == '/' || strchr(name, ':')) dan_conv_error(DAN_MSG_ZIP_UNSAFE, name, line);
    const char *s = name;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len == 2 && s[0] == '.' && s[1] == '.') dan_conv_error(DAN_MSG_ZIP_UNSAFE, name, line);
        if (!e) break;
        s = e + 1;
    }
}

static dl_Ls *dan_extract_zip(dan_str zip_path, dan_str folder, dan_str password, int line) {
    dan_bytes z = dan_read_bytes(zip_path, line);
    const uint8_t *p = z->data;
    size_t n = (size_t)z->len;
    int64_t k;
    dan_zip_entry *e = dan_zip_entries(z, &k, line);
    dl_Ls *done = dl_Ls_from(0, NULL);
    dan_make_dirs(folder, line);
    for (int64_t i = 0; i < k; i++) {
        dan_zip_entry *x = &e[i];
        char *name = (char *)dan_strdup(x->name);
        dan_check_entry_name(name, line);
        dan_str target = dan_concat(dan_concat(folder, "/"), name);
        size_t tl = strlen(target);
        if (tl && target[tl - 1] == '/') { dan_make_dirs(target, line); continue; }
        const char *slash = strrchr(target, '/');
        if (slash) {
            char *dir = (char *)dan_strdup(target);
            dir[slash - target] = 0;
            dan_make_dirs(dir, line);
        }
        size_t lh = x->offset;
        if (lh + 30 > n || dan_le32(p + lh) != 0x04034b50) dan_error(DAN_MSG_CORRUPT, line);
        size_t start = lh + 30 + dan_le16(p + lh + 26) + dan_le16(p + lh + 28);
        if (start + x->csize > n) dan_error(DAN_MSG_CORRUPT, line);
        uint8_t *data = dan_alloc((size_t)x->csize + 1);
        memcpy(data, p + start, x->csize);
        size_t dlen = x->csize;
        uint16_t method = x->method;
        if (x->flags & 1) {
            if (!x->aes) dan_conv_error(DAN_MSG_ZIPCRYPTO, x->name, line);
            if (!password || !password[0]) dan_conv_error(DAN_MSG_ZIP_NEED_PW, x->name, line);
            int keylen = x->aes_strength == 1 ? 16 : x->aes_strength == 2 ? 24 : 32;
            int saltlen = keylen / 2;
            if (dlen < (size_t)saltlen + 12) dan_error(DAN_MSG_CORRUPT, line);
            uint8_t keys[66], mac[20];
            dan_pbkdf2_sha1((const uint8_t *)password, strlen(password), data, (size_t)saltlen, 1000, keys,
                            (size_t)(2 * keylen + 2));
            if (memcmp(keys + 2 * keylen, data + saltlen, 2) != 0) dan_conv_error(DAN_MSG_ZIP_BAD_PW, x->name, line);
            uint8_t *ct = data + saltlen + 2;
            size_t ctlen = dlen - (size_t)saltlen - 12;
            dan_hmac1_ctx hm;
            dan_hmac1_init(&hm, keys + keylen, (size_t)keylen);
            dan_hmac1(&hm, ct, ctlen, mac);
            if (!dan_equal_ct(mac, ct + ctlen, 10)) dan_conv_error(DAN_MSG_ZIP_BAD_PW, x->name, line);
            dan_aes aes;
            dan_aes_init(&aes, keys, keylen);
            dan_aes_ctr_winzip(&aes, ct, ctlen);
            memmove(data, ct, ctlen);
            dlen = ctlen;
            method = x->aes_method;
        }
        uint8_t *plain;
        size_t plen;
        if (method == 0) { plain = data; plen = dlen; }
        else if (method == 8) plain = dan_inflate_raw(data, dlen, &plen, NULL, line);
        else { char t[32]; snprintf(t, sizeof t, "%u", method); dan_conv_error(DAN_MSG_ZIP_METHOD, t, line); }
        if (plen != x->usize) dan_error(DAN_MSG_CORRUPT, line);
        if (!(x->aes && x->aes_version == 2) && dan_crc32_update(0, plain, plen) != x->crc)
            dan_error(DAN_MSG_CORRUPT, line);
        dan_write_raw(target, "wb", plain, plen, line);
        dl_Ls_push(done, x->name);
    }
    return done;
}

static dl_Ls *dan_extract_zip_plain(dan_str zip_path, dan_str folder, int line) {
    return dan_extract_zip(zip_path, folder, NULL, line);
}


/* ==== LZMA2 in the .xz container (as in 7-Zip and xz): the strongest compression.
   Window up to 64 MB, integrity check CRC-32 of the original data. ==== */

#define DAN_LZ_STATES 12
#define DAN_LZ_POSMAX 16
#define DAN_LZ_CHUNK 60000
#define DAN_LZ_MAXDICT (1u << 26)

typedef uint16_t dan_prob;
typedef struct { dan_prob choice, choice2, low[DAN_LZ_POSMAX][8], mid[DAN_LZ_POSMAX][8], high[256]; } dan_lzlen;
typedef struct {
    dan_prob is_match[DAN_LZ_STATES][DAN_LZ_POSMAX], is_rep[DAN_LZ_STATES], is_rep_g0[DAN_LZ_STATES],
             is_rep_g1[DAN_LZ_STATES], is_rep_g2[DAN_LZ_STATES], is_rep0_long[DAN_LZ_STATES][DAN_LZ_POSMAX];
    dan_prob pos_slot[4][64], pos_special[115], align[16];
    dan_lzlen len, rep_len;
    dan_prob literal[0x300 << 4];              /* LZMA2 limits lc + lp to 4 */
} dan_lzprobs;
typedef struct { dan_lzprobs p; uint32_t reps[4]; int state, lc, lp, pb; } dan_lzm;

static void dan_lzm_reset(dan_lzm *m) {
    dan_prob *a = (dan_prob *)&m->p;
    for (size_t i = 0; i < sizeof m->p / sizeof(dan_prob); i++) a[i] = 1024;
    m->reps[0] = m->reps[1] = m->reps[2] = m->reps[3] = 0;
    m->state = 0;
}

static int dan_lz_state_lit(int s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }

/* ---- range encoder ---- */

typedef struct { uint8_t *d; size_t len, cap; uint64_t low; uint32_t range; uint8_t cache; uint64_t csize; } dan_rce;

static void dan_rce_init(dan_rce *e) {
    e->d = NULL; e->len = e->cap = 0;
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->csize = 1;
}

static void dan_rce_out(dan_rce *e, uint8_t b) {
    if (e->len == e->cap) { e->cap = e->cap ? e->cap * 2 : 65536; e->d = dan_realloc(e->d, e->cap); }
    e->d[e->len++] = b;
}

static void dan_rce_shift(dan_rce *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32), temp = e->cache;
        do { dan_rce_out(e, (uint8_t)(temp + carry)); temp = 0xFF; } while (--e->csize != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->csize++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static void dan_rce_bit(dan_rce *e, dan_prob *p, uint32_t bit) {
    uint32_t bound = (e->range >> 11) * (*p);
    if (!bit) { e->range = bound; *p = (dan_prob)(*p + ((2048 - *p) >> 5)); }
    else { e->low += bound; e->range -= bound; *p = (dan_prob)(*p - (*p >> 5)); }
    while (e->range < (1u << 24)) { e->range <<= 8; dan_rce_shift(e); }
}

static void dan_rce_direct(dan_rce *e, uint32_t v, int n) {
    while (n-- > 0) {
        e->range >>= 1;
        if ((v >> n) & 1) e->low += e->range;
        if (e->range < (1u << 24)) { e->range <<= 8; dan_rce_shift(e); }
    }
}

static void dan_rce_flush(dan_rce *e) { for (int i = 0; i < 5; i++) dan_rce_shift(e); }

static void dan_rce_tree(dan_rce *e, dan_prob *p, int bits, uint32_t sym) {
    uint32_t m = 1;
    for (int i = bits - 1; i >= 0; i--) { uint32_t b = (sym >> i) & 1; dan_rce_bit(e, &p[m], b); m = (m << 1) | b; }
}

static void dan_rce_rtree(dan_rce *e, dan_prob *p, int bits, uint32_t sym) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) { uint32_t b = sym & 1; sym >>= 1; dan_rce_bit(e, &p[m], b); m = (m << 1) | b; }
}

static void dan_lze_len(dan_rce *e, dan_lzlen *l, uint32_t v, uint32_t ps) {
    if (v < 8) { dan_rce_bit(e, &l->choice, 0); dan_rce_tree(e, l->low[ps], 3, v); }
    else if (v < 16) { dan_rce_bit(e, &l->choice, 1); dan_rce_bit(e, &l->choice2, 0); dan_rce_tree(e, l->mid[ps], 3, v - 8); }
    else { dan_rce_bit(e, &l->choice, 1); dan_rce_bit(e, &l->choice2, 1); dan_rce_tree(e, l->high, 8, v - 16); }
}

static uint32_t dan_pos_slot(uint32_t dist) {
    if (dist < 4) return dist;
    int n = 31 - __builtin_clz(dist);
    return ((uint32_t)n << 1) | ((dist >> (n - 1)) & 1);
}

/* ---- match finder: hash chains over 4 bytes ---- */

typedef struct { const uint8_t *d; size_t n; int32_t *head, *prev; uint32_t window; int depth; uint32_t nice; } dan_mf;

#define DAN_MF_HASH_BITS 20
static uint32_t dan_mf_hash(const uint8_t *p) { return (dan_le32(p) * 2654435761u) >> (32 - DAN_MF_HASH_BITS); }

static void dan_mf_insert(dan_mf *f, size_t p) {
    if (p + 4 > f->n) return;
    uint32_t h = dan_mf_hash(f->d + p);
    f->prev[p] = f->head[h];
    f->head[h] = (int32_t)p;
}

static uint32_t dan_mf_find(dan_mf *f, size_t p, size_t limit, uint32_t *dist) {
    if (p + 4 > f->n || limit < 4) return 0;
    int32_t c = f->head[dan_mf_hash(f->d + p)];
    uint32_t best = 0;
    int depth = f->depth;
    while (c >= 0 && depth-- > 0) {
        size_t back = p - (size_t)c;
        if (back > f->window) break;
        const uint8_t *a = f->d + c, *b = f->d + p;
        if (a[best] == b[best]) {
            uint32_t l = 0;
            while (l < limit && a[l] == b[l]) l++;
            if (l > best) {
                best = l;
                *dist = (uint32_t)back;
                if (l >= f->nice || l == limit) break;
            }
        }
        c = f->prev[c];
    }
    return best;
}

static uint32_t dan_rep_len(const uint8_t *d, size_t p, size_t back, size_t limit) {
    if (back > p) return 0;
    uint32_t l = 0;
    while (l < limit && d[p + l] == d[p - back + l]) l++;
    return l;
}

/* ---- LZMA encoder for one LZMA2 chunk [cs, ce) ---- */

static void dan_lze_literal(dan_lzm *m, dan_rce *e, const uint8_t *d, size_t p) {
    uint32_t ps = (uint32_t)p & 3;
    dan_rce_bit(e, &m->p.is_match[m->state][ps], 0);
    uint8_t prev = p ? d[p - 1] : 0;
    dan_prob *lp = m->p.literal + 0x300 * (uint32_t)(prev >> 5);
    uint32_t cur = d[p], sym = 1;
    int i = 7;
    if (m->state >= 7) {
        uint32_t mb = d[p - m->reps[0] - 1];
        for (; i >= 0; i--) {
            uint32_t mbit = (mb >> i) & 1, bit = (cur >> i) & 1;
            dan_rce_bit(e, &lp[((1 + mbit) << 8) + sym], bit);
            sym = (sym << 1) | bit;
            if (mbit != bit) { i--; break; }
        }
    }
    for (; i >= 0; i--) {
        uint32_t bit = (cur >> i) & 1;
        dan_rce_bit(e, &lp[sym], bit);
        sym = (sym << 1) | bit;
    }
    m->state = dan_lz_state_lit(m->state);
}

static void dan_lze_match(dan_lzm *m, dan_rce *e, size_t p, uint32_t len, uint32_t back) {
    uint32_t ps = (uint32_t)p & 3, dist = back - 1;
    dan_rce_bit(e, &m->p.is_match[m->state][ps], 1);
    dan_rce_bit(e, &m->p.is_rep[m->state], 0);
    dan_lze_len(e, &m->p.len, len - 2, ps);
    m->state = m->state < 7 ? 7 : 10;
    uint32_t ls = len - 2 < 3 ? len - 2 : 3, slot = dan_pos_slot(dist);
    dan_rce_tree(e, m->p.pos_slot[ls], 6, slot);
    if (slot >= 4) {
        int nd = (int)(slot >> 1) - 1;
        uint32_t base = (2 | (slot & 1)) << nd, rem = dist - base;
        if (slot < 14) dan_rce_rtree(e, m->p.pos_special + base - slot, nd, rem);
        else { dan_rce_direct(e, rem >> 4, nd - 4); dan_rce_rtree(e, m->p.align, 4, rem & 15); }
    }
    m->reps[3] = m->reps[2]; m->reps[2] = m->reps[1]; m->reps[1] = m->reps[0]; m->reps[0] = dist;
}

static void dan_lze_rep(dan_lzm *m, dan_rce *e, size_t p, int ri, uint32_t len) {
    uint32_t ps = (uint32_t)p & 3;
    dan_rce_bit(e, &m->p.is_match[m->state][ps], 1);
    dan_rce_bit(e, &m->p.is_rep[m->state], 1);
    if (ri == 0) {
        dan_rce_bit(e, &m->p.is_rep_g0[m->state], 0);
        dan_rce_bit(e, &m->p.is_rep0_long[m->state][ps], 1);
    } else {
        dan_rce_bit(e, &m->p.is_rep_g0[m->state], 1);
        if (ri == 1) dan_rce_bit(e, &m->p.is_rep_g1[m->state], 0);
        else { dan_rce_bit(e, &m->p.is_rep_g1[m->state], 1); dan_rce_bit(e, &m->p.is_rep_g2[m->state], ri == 3); }
        uint32_t d = m->reps[ri];
        for (int k = ri; k > 0; k--) m->reps[k] = m->reps[k - 1];
        m->reps[0] = d;
    }
    dan_lze_len(e, &m->p.rep_len, len - 2, ps);
    m->state = m->state < 7 ? 8 : 11;
}

static void dan_lze_shortrep(dan_lzm *m, dan_rce *e, size_t p) {
    uint32_t ps = (uint32_t)p & 3;
    dan_rce_bit(e, &m->p.is_match[m->state][ps], 1);
    dan_rce_bit(e, &m->p.is_rep[m->state], 1);
    dan_rce_bit(e, &m->p.is_rep_g0[m->state], 0);
    dan_rce_bit(e, &m->p.is_rep0_long[m->state][ps], 0);
    m->state = m->state < 7 ? 9 : 11;
}

/* ---- optimal parsing: dynamic programming over up to DAN_OPT_W bytes with
   prices (in 1/16 bit) taken from the current probabilities, as 7-Zip does ---- */

#define DAN_OPT_W 4096
#define DAN_OPT_CUT 96
#define DAN_INF 0x7FFFFFFFu

static uint32_t dan_price_tab[128];
static int dan_price_ready = 0;

static void dan_price_init(void) {
    if (dan_price_ready) return;
    for (int i = 0; i < 128; i++) dan_price_tab[i] = (uint32_t)(-log2((i + 0.5) / 128.0) * 16 + 0.5);
    dan_price_ready = 1;
}

#define DAN_P(prob, bit) dan_price_tab[((bit) ? 2048u - (prob) : (uint32_t)(prob)) >> 4]

static uint32_t dan_tree_price(const dan_prob *p, int bits, uint32_t sym) {
    uint32_t m = 1, pr = 0;
    for (int i = bits - 1; i >= 0; i--) { uint32_t b = (sym >> i) & 1; pr += DAN_P(p[m], b); m = (m << 1) | b; }
    return pr;
}

static uint32_t dan_rtree_price(const dan_prob *p, int bits, uint32_t sym) {
    uint32_t m = 1, pr = 0;
    for (int i = 0; i < bits; i++) { uint32_t b = sym & 1; sym >>= 1; pr += DAN_P(p[m], b); m = (m << 1) | b; }
    return pr;
}

static uint32_t dan_len_price(const dan_lzlen *l, uint32_t v, uint32_t ps) {
    if (v < 8) return DAN_P(l->choice, 0) + dan_tree_price(l->low[ps], 3, v);
    if (v < 16) return DAN_P(l->choice, 1) + DAN_P(l->choice2, 0) + dan_tree_price(l->mid[ps], 3, v - 8);
    return DAN_P(l->choice, 1) + DAN_P(l->choice2, 1) + dan_tree_price(l->high, 8, v - 16);
}

static uint32_t dan_dist_price(const dan_lzm *m, uint32_t dist, uint32_t ls) {
    uint32_t slot = dan_pos_slot(dist), pr = dan_tree_price(m->p.pos_slot[ls], 6, slot);
    if (slot >= 4) {
        int nd = (int)(slot >> 1) - 1;
        uint32_t base = (2 | (slot & 1)) << nd, rem = dist - base;
        if (slot < 14) pr += dan_rtree_price(m->p.pos_special + base - slot, nd, rem);
        else pr += (uint32_t)(nd - 4) * 16 + dan_rtree_price(m->p.align, 4, rem & 15);
    }
    return pr;
}

static uint32_t dan_lit_price(const dan_lzm *m, int state, uint32_t rep0, const uint8_t *d, size_t p) {
    uint8_t prev = p ? d[p - 1] : 0;
    const dan_prob *lp = m->p.literal + 0x300 * (uint32_t)(prev >> 5);
    uint32_t cur = d[p], sym = 1, pr = 0;
    int i = 7;
    if (state >= 7) {
        uint32_t mb = d[p - rep0 - 1];
        for (; i >= 0; i--) {
            uint32_t mbit = (mb >> i) & 1, bit = (cur >> i) & 1;
            pr += DAN_P(lp[((1 + mbit) << 8) + sym], bit);
            sym = (sym << 1) | bit;
            if (mbit != bit) { i--; break; }
        }
    }
    for (; i >= 0; i--) { uint32_t bit = (cur >> i) & 1; pr += DAN_P(lp[sym], bit); sym = (sym << 1) | bit; }
    return pr;
}

typedef struct { uint32_t price, len, dist; int32_t prev; int kind, state; uint32_t reps[4]; } dan_node;
enum { DAN_OP_LIT, DAN_OP_MATCH, DAN_OP_REP, DAN_OP_SHORTREP };

/* all matches at p with increasing lengths: a 3-byte hash slot for close
   short matches, then the 4-byte hash chain */
static int dan_mf_all(dan_mf *f, int32_t *head3, size_t p, size_t limit, uint32_t *lens, uint32_t *dists) {
    int k = 0;
    uint32_t best = 0;
    const uint8_t *b = f->d + p;
    if (p + 3 <= f->n && limit >= 3) {
        uint32_t h3 = ((uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]) * 2654435761u >> 16;
        int32_t c = head3[h3];
        if (c >= 0 && p - (size_t)c <= 65536) {
            const uint8_t *a = f->d + c;
            uint32_t l = 0;
            while (l < limit && a[l] == b[l]) l++;
            if (l >= 3) { lens[k] = l; dists[k] = (uint32_t)(p - (size_t)c); k++; best = l; }
        }
    }
    if (p + 4 <= f->n && limit >= 4 && best < limit) {
        int32_t c = f->head[dan_mf_hash(b)];
        int depth = f->depth;
        while (c >= 0 && depth-- > 0) {
            size_t back = p - (size_t)c;
            if (back > f->window) break;
            const uint8_t *a = f->d + c;
            if (a[best] == b[best]) {
                uint32_t l = 0;
                while (l < limit && a[l] == b[l]) l++;
                if (l > best) {
                    best = l;
                    lens[k] = l; dists[k] = (uint32_t)back; k++;
                    if (l >= f->nice || l == limit || k >= 32) break;
                }
            }
            c = f->prev[c];
        }
    }
    return k;
}

static void dan_mf_insert3(dan_mf *f, int32_t *head3, size_t p) {
    if (p + 3 <= f->n) {
        const uint8_t *b = f->d + p;
        head3[((uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]) * 2654435761u >> 16] = (int32_t)p;
    }
    dan_mf_insert(f, p);
}

static void dan_node_apply(dan_node *to, const dan_node *from, int kind, uint32_t dist) {
    int s = from->state;
    memcpy(to->reps, from->reps, sizeof to->reps);
    if (kind == DAN_OP_LIT) to->state = dan_lz_state_lit(s);
    else if (kind == DAN_OP_SHORTREP) to->state = s < 7 ? 9 : 11;
    else if (kind == DAN_OP_MATCH) {
        to->state = s < 7 ? 7 : 10;
        to->reps[3] = from->reps[2]; to->reps[2] = from->reps[1]; to->reps[1] = from->reps[0]; to->reps[0] = dist - 1;
    } else {
        to->state = s < 7 ? 8 : 11;
        uint32_t r = dist, v = from->reps[r];
        for (uint32_t k = r; k > 0; k--) to->reps[k] = from->reps[k - 1];
        to->reps[0] = v;
    }
}

static void dan_relax(dan_node *nodes, int32_t i, int32_t j, uint32_t price, int kind, uint32_t len, uint32_t dist) {
    if (price >= nodes[j].price) return;
    nodes[j].price = price;
    nodes[j].prev = i;
    nodes[j].kind = kind;
    nodes[j].len = len;
    nodes[j].dist = dist;
    dan_node_apply(&nodes[j], &nodes[i], kind, dist);
}

static void dan_lzma_encode_chunk(dan_lzm *m, dan_mf *f, int32_t *head3, dan_node *nodes, dan_rce *e,
                                  size_t cs, size_t ce) {
    const uint8_t *d = f->d;
    uint32_t lens[32], dists[32], mlp[4][274], rlp[4][274];
    size_t p = cs;
    dan_price_init();
    while (p < ce) {
        int32_t horizon = (int32_t)(ce - p < DAN_OPT_W ? ce - p : DAN_OPT_W);
        for (uint32_t ps = 0; ps < 4; ps++)
            for (uint32_t v = 0; v < 272; v++) {
                mlp[ps][v] = dan_len_price(&m->p.len, v, ps);
                rlp[ps][v] = dan_len_price(&m->p.rep_len, v, ps);
            }
        for (int32_t j = 0; j <= horizon + 273; j++) nodes[j].price = DAN_INF;
        nodes[0].price = 0;
        nodes[0].state = m->state;
        memcpy(nodes[0].reps, m->reps, sizeof m->reps);
        int32_t i = 0, processed = 0;
        for (; i < horizon; i++) {
            size_t pos = p + (size_t)i, limit = ce - pos < 273 ? ce - pos : 273;
            int nm = dan_mf_all(f, head3, pos, limit, lens, dists);
            dan_mf_insert3(f, head3, pos);
            processed = i + 1;
            dan_node *nd = &nodes[i];
            if (nd->price == DAN_INF) continue;
            int s = nd->state;
            uint32_t ps = (uint32_t)pos & 3;
            uint32_t pm0 = DAN_P(m->p.is_match[s][ps], 0), pm1 = DAN_P(m->p.is_match[s][ps], 1);
            uint32_t prep0 = DAN_P(m->p.is_rep[s], 0), prep1 = DAN_P(m->p.is_rep[s], 1);
            /* literal */
            dan_relax(nodes, i, i + 1, nd->price + pm0 + dan_lit_price(m, s, nd->reps[0], d, pos), DAN_OP_LIT, 1, 0);
            /* short rep */
            if ((size_t)nd->reps[0] + 1 <= pos && d[pos] == d[pos - nd->reps[0] - 1])
                dan_relax(nodes, i, i + 1, nd->price + pm1 + prep1 + DAN_P(m->p.is_rep_g0[s], 0)
                          + DAN_P(m->p.is_rep0_long[s][ps], 0), DAN_OP_SHORTREP, 1, 0);
            /* rep matches */
            uint32_t longest = 0;
            for (uint32_t r = 0; r < 4; r++) {
                uint32_t rl = dan_rep_len(d, pos, (size_t)nd->reps[r] + 1, limit);
                if (rl < 2) continue;
                uint32_t base = nd->price + pm1 + prep1;
                if (r == 0) base += DAN_P(m->p.is_rep_g0[s], 0) + DAN_P(m->p.is_rep0_long[s][ps], 1);
                else {
                    base += DAN_P(m->p.is_rep_g0[s], 1);
                    if (r == 1) base += DAN_P(m->p.is_rep_g1[s], 0);
                    else base += DAN_P(m->p.is_rep_g1[s], 1) + DAN_P(m->p.is_rep_g2[s], r == 3);
                }
                for (uint32_t L = 2; L <= rl; L++) dan_relax(nodes, i, i + (int32_t)L, base + rlp[ps][L - 2], DAN_OP_REP, L, r);
                if (rl > longest) longest = rl;
            }
            /* normal matches */
            if (nm > 0) {
                uint32_t base = nd->price + pm1 + prep0;
                int k = 0;
                uint32_t dp[4] = { 0 }, have = 0;
                for (uint32_t L = 3; L <= lens[nm - 1]; L++) {
                    while (lens[k] < L) { k++; have = 0; }
                    uint32_t ls = L - 2 < 3 ? L - 2 : 3;
                    if (!(have & (1u << ls))) { dp[ls] = dan_dist_price(m, dists[k] - 1, ls); have |= 1u << ls; }
                    dan_relax(nodes, i, i + (int32_t)L, base + mlp[ps][L - 2] + dp[ls], DAN_OP_MATCH, L, dists[k]);
                }
                if (lens[nm - 1] > longest) longest = lens[nm - 1];
            }
            if (longest >= DAN_OPT_CUT) {            /* a long match: end the window right after it */
                horizon = i + (int32_t)longest;
                break;
            }
        }
        for (int32_t j = processed; j < horizon; j++) dan_mf_insert3(f, head3, p + (size_t)j);
        /* the cheapest path to the horizon, in forward order */
        int32_t cnt = 0;
        for (int32_t j = horizon; j > 0; j = nodes[j].prev) cnt++;
        int32_t *path = dan_alloc(sizeof(int32_t) * (size_t)(cnt + 1));
        int32_t k = cnt;
        for (int32_t j = horizon; j > 0; j = nodes[j].prev) path[--k] = j;
        size_t at = p;
        for (int32_t t = 0; t < cnt; t++) {
            dan_node *nd = &nodes[path[t]];
            if (nd->kind == DAN_OP_LIT) dan_lze_literal(m, e, d, at);
            else if (nd->kind == DAN_OP_SHORTREP) dan_lze_shortrep(m, e, at);
            else if (nd->kind == DAN_OP_REP) dan_lze_rep(m, e, at, (int)nd->dist, nd->len);
            else dan_lze_match(m, e, at, nd->len, nd->dist);
            at += nd->len;
        }
        free(path);
        p += (size_t)horizon;
    }
}

/* ---- xz container ---- */

static uint64_t dan_crc64_table[256];
static int dan_crc64_ready = 0;

static uint64_t dan_crc64(const uint8_t *p, size_t n) {
    if (!dan_crc64_ready) {
        for (uint64_t i = 0; i < 256; i++) {
            uint64_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xC96C5795D7870F42ull ^ (c >> 1) : c >> 1;
            dan_crc64_table[i] = c;
        }
        dan_crc64_ready = 1;
    }
    uint64_t crc = ~0ull;
    while (n--) crc = dan_crc64_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void dan_bbuf_varint(dan_bbuf *b, uint64_t v) {
    do {
        uint8_t x = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) x |= 0x80;
        dan_bbuf_add(b, &x, 1);
    } while (v);
}

static dan_bytes dan_xz_raw(const uint8_t *d, size_t n) {
    /* dictionary: the smallest size from the xz list that covers the data, up to 64 MB */
    int dict_byte = 0;
    while (dict_byte < 40) {
        uint64_t sz = (uint64_t)(2 | (dict_byte & 1)) << (dict_byte / 2 + 11);
        if (sz >= n || sz >= DAN_LZ_MAXDICT) break;
        dict_byte++;
    }
    uint32_t dict = (uint32_t)((uint64_t)(2 | (dict_byte & 1)) << (dict_byte / 2 + 11));
    dan_mf f = { d, n, dan_alloc(sizeof(int32_t) << DAN_MF_HASH_BITS), dan_alloc(sizeof(int32_t) * (n ? n : 1)),
                 dict, 512, 273 };
    for (size_t i = 0; i < ((size_t)1 << DAN_MF_HASH_BITS); i++) f.head[i] = -1;
    int32_t *head3 = dan_alloc(sizeof(int32_t) << 16);
    for (size_t i = 0; i < ((size_t)1 << 16); i++) head3[i] = -1;
    dan_node *nodes = dan_alloc(sizeof(dan_node) * (DAN_OPT_W + 280));
    dan_lzm *m = dan_alloc(sizeof *m), *snap = dan_alloc(sizeof *snap);
    m->lc = 3; m->lp = 0; m->pb = 2;
    dan_lzm_reset(m);
    dan_bbuf lz2 = { NULL, 0, 0 };
    int first = 1, need_reset = 0, need_props = 1;
    for (size_t cs = 0; cs < n; cs += DAN_LZ_CHUNK) {
        size_t ce = n - cs < DAN_LZ_CHUNK ? n : cs + DAN_LZ_CHUNK, un = ce - cs;
        *snap = *m;
        if (need_reset) dan_lzm_reset(m);
        dan_rce e;
        dan_rce_init(&e);
        dan_lzma_encode_chunk(m, &f, head3, nodes, &e, cs, ce);
        dan_rce_flush(&e);
        uint8_t h[6];
        if (e.len >= un || e.len > 65536) {            /* incompressible: store the chunk as is */
            *m = *snap;
            h[0] = first ? 1 : 2;
            h[1] = (uint8_t)((un - 1) >> 8); h[2] = (uint8_t)(un - 1);
            dan_bbuf_add(&lz2, h, 3);
            dan_bbuf_add(&lz2, d + cs, un);
            need_reset = 1;
            if (first) need_props = 1;
        } else {
            uint8_t control = first ? 0xE0 : need_props ? 0xC0 : need_reset ? 0xA0 : 0x80;
            h[0] = (uint8_t)(control | ((un - 1) >> 16));
            h[1] = (uint8_t)((un - 1) >> 8); h[2] = (uint8_t)(un - 1);
            h[3] = (uint8_t)((e.len - 1) >> 8); h[4] = (uint8_t)(e.len - 1);
            h[5] = 0x5D;                                  /* lc = 3, lp = 0, pb = 2 */
            dan_bbuf_add(&lz2, h, control >= 0xC0 ? 6 : 5);
            dan_bbuf_add(&lz2, e.d, e.len);
            need_reset = 0;
            need_props = 0;
        }
        free(e.d);
        first = 0;
    }
    uint8_t zero = 0;
    dan_bbuf_add(&lz2, &zero, 1);
    free(f.head); free(f.prev); free(m); free(snap); free(head3); free(nodes);

    dan_bbuf x = { NULL, 0, 0 };
    static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    uint8_t flags[2] = { 0, 0x04 };                      /* check: CRC64, as xz does by default */
    dan_bbuf_add(&x, magic, 6);
    dan_bbuf_add(&x, flags, 2);
    dan_bbuf_u32(&x, dan_crc32_update(0, flags, 2));
    /* block header */
    dan_bbuf bh = { NULL, 0, 0 };
    uint8_t b2[2] = { 0, 0xC0 };                         /* size placeholder; both sizes present, 1 filter */
    dan_bbuf_add(&bh, b2, 2);
    dan_bbuf_varint(&bh, lz2.len);
    dan_bbuf_varint(&bh, n);
    uint8_t filt[3] = { 0x21, 0x01, (uint8_t)dict_byte };  /* LZMA2 */
    dan_bbuf_add(&bh, filt, 3);
    while ((bh.len + 4) % 4) dan_bbuf_add(&bh, &zero, 1);
    bh.d[0] = (uint8_t)((bh.len + 4) / 4 - 1);
    dan_bbuf_u32(&bh, dan_crc32_update(0, bh.d, bh.len));
    size_t block_start = x.len;
    dan_bbuf_add(&x, bh.d, bh.len);
    dan_bbuf_add(&x, lz2.d, lz2.len);
    while ((x.len - block_start) % 4) dan_bbuf_add(&x, &zero, 1);
    uint64_t check = dan_crc64(d, n);
    uint8_t c8[8];
    for (int i = 0; i < 8; i++) c8[i] = (uint8_t)(check >> (8 * i));
    dan_bbuf_add(&x, c8, 8);
    uint64_t unpadded = bh.len + lz2.len + 8;
    /* index */
    size_t index_start = x.len;
    dan_bbuf_add(&x, &zero, 1);
    dan_bbuf_varint(&x, 1);
    dan_bbuf_varint(&x, unpadded);
    dan_bbuf_varint(&x, n);
    while ((x.len - index_start) % 4) dan_bbuf_add(&x, &zero, 1);
    dan_bbuf_u32(&x, dan_crc32_update(0, x.d + index_start, x.len - index_start));
    uint32_t backward = (uint32_t)((x.len - index_start) / 4 - 1);
    /* stream footer */
    uint8_t foot[6];
    dan_put_le32(foot, backward);
    foot[4] = flags[0]; foot[5] = flags[1];
    dan_bbuf_u32(&x, dan_crc32_update(0, foot, 6));
    dan_bbuf_add(&x, foot, 6);
    dan_bbuf_add(&x, "YZ", 2);
    free(lz2.d);
    free(bh.d);
    return dan_bytes_wrap(x.d, (int64_t)x.len);
}

/* ---- LZMA2 / xz decoder ---- */

typedef struct { const uint8_t *in; size_t pos, end; uint32_t range, code; int line; } dan_rcd;

static uint8_t dan_rcd_byte(dan_rcd *r) {
    if (r->pos >= r->end) dan_error(DAN_MSG_CORRUPT, r->line);
    return r->in[r->pos++];
}

static void dan_rcd_init(dan_rcd *r, const uint8_t *in, size_t pos, size_t end, int line) {
    r->in = in; r->pos = pos; r->end = end; r->line = line;
    if (dan_rcd_byte(r) != 0) dan_error(DAN_MSG_CORRUPT, line);
    r->code = 0;
    for (int i = 0; i < 4; i++) r->code = (r->code << 8) | dan_rcd_byte(r);
    r->range = 0xFFFFFFFFu;
}

static uint32_t dan_rcd_bit(dan_rcd *r, dan_prob *p) {
    uint32_t bound = (r->range >> 11) * (*p), bit;
    if (r->code < bound) { r->range = bound; *p = (dan_prob)(*p + ((2048 - *p) >> 5)); bit = 0; }
    else { r->range -= bound; r->code -= bound; *p = (dan_prob)(*p - (*p >> 5)); bit = 1; }
    if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | dan_rcd_byte(r); }
    return bit;
}

static uint32_t dan_rcd_direct(dan_rcd *r, int n) {
    uint32_t res = 0;
    while (n-- > 0) {
        r->range >>= 1;
        r->code -= r->range;
        uint32_t t = 0 - (r->code >> 31);
        r->code += r->range & t;
        if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | dan_rcd_byte(r); }
        res = (res << 1) + (t + 1);
    }
    return res;
}

static uint32_t dan_rcd_tree(dan_rcd *r, dan_prob *p, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) | dan_rcd_bit(r, &p[m]);
    return m - (1u << bits);
}

static uint32_t dan_rcd_rtree(dan_rcd *r, dan_prob *p, int bits) {
    uint32_t m = 1, s = 0;
    for (int i = 0; i < bits; i++) { uint32_t b = dan_rcd_bit(r, &p[m]); m = (m << 1) | b; s |= b << i; }
    return s;
}

static uint32_t dan_rcd_len(dan_rcd *r, dan_lzlen *l, uint32_t ps) {
    if (!dan_rcd_bit(r, &l->choice)) return dan_rcd_tree(r, l->low[ps], 3);
    if (!dan_rcd_bit(r, &l->choice2)) return 8 + dan_rcd_tree(r, l->mid[ps], 3);
    return 16 + dan_rcd_tree(r, l->high, 8);
}

static void dan_lzma_decode_chunk(dan_lzm *m, dan_rcd *rc, dan_bbuf *out, size_t dict_start, size_t un, int line) {
    if (out->len + un > out->cap) { out->cap = (out->len + un) * 2 + 65536; out->d = dan_realloc(out->d, out->cap); }
    uint8_t *o = out->d;
    size_t n = out->len, end = n + un;
    uint32_t pbmask = (1u << m->pb) - 1, lpmask = (1u << m->lp) - 1;
    while (n < end) {
        size_t pos = n - dict_start;
        uint32_t ps = (uint32_t)pos & pbmask;
        if (!dan_rcd_bit(rc, &m->p.is_match[m->state][ps])) {
            uint8_t prev = pos ? o[n - 1] : 0;
            dan_prob *lp = m->p.literal + 0x300 * (((uint32_t)(pos & lpmask) << m->lc) + (uint32_t)(prev >> (8 - m->lc)));
            uint32_t sym = 1;
            if (m->state >= 7) {
                size_t back = (size_t)m->reps[0] + 1;
                if (back > pos) dan_error(DAN_MSG_CORRUPT, line);
                uint32_t mb = o[n - back];
                do {
                    uint32_t mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    uint32_t bit = dan_rcd_bit(rc, &lp[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (mbit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | dan_rcd_bit(rc, &lp[sym]);
            o[n++] = (uint8_t)sym;
            m->state = dan_lz_state_lit(m->state);
            continue;
        }
        uint32_t len;
        if (!dan_rcd_bit(rc, &m->p.is_rep[m->state])) {
            len = dan_rcd_len(rc, &m->p.len, ps);
            m->state = m->state < 7 ? 7 : 10;
            uint32_t ls = len < 3 ? len : 3, slot = dan_rcd_tree(rc, m->p.pos_slot[ls], 6), dist;
            if (slot < 4) dist = slot;
            else {
                int nd = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << nd;
                if (slot < 14) dist += dan_rcd_rtree(rc, m->p.pos_special + dist - slot, nd);
                else { dist += dan_rcd_direct(rc, nd - 4) << 4; dist += dan_rcd_rtree(rc, m->p.align, 4); }
            }
            if (dist == 0xFFFFFFFFu) dan_error(DAN_MSG_CORRUPT, line);
            m->reps[3] = m->reps[2]; m->reps[2] = m->reps[1]; m->reps[1] = m->reps[0]; m->reps[0] = dist;
        } else {
            if (!dan_rcd_bit(rc, &m->p.is_rep_g0[m->state])) {
                if (!dan_rcd_bit(rc, &m->p.is_rep0_long[m->state][ps])) {
                    size_t back = (size_t)m->reps[0] + 1;
                    if (back > pos) dan_error(DAN_MSG_CORRUPT, line);
                    o[n] = o[n - back];
                    n++;
                    m->state = m->state < 7 ? 9 : 11;
                    continue;
                }
            } else {
                uint32_t dd;
                if (!dan_rcd_bit(rc, &m->p.is_rep_g1[m->state])) dd = m->reps[1];
                else {
                    if (!dan_rcd_bit(rc, &m->p.is_rep_g2[m->state])) dd = m->reps[2];
                    else { dd = m->reps[3]; m->reps[3] = m->reps[2]; }
                    m->reps[2] = m->reps[1];
                }
                m->reps[1] = m->reps[0];
                m->reps[0] = dd;
            }
            len = dan_rcd_len(rc, &m->p.rep_len, ps);
            m->state = m->state < 7 ? 8 : 11;
        }
        len += 2;
        size_t back = (size_t)m->reps[0] + 1;
        if (back > pos || len > end - n) dan_error(DAN_MSG_CORRUPT, line);
        for (uint32_t i = 0; i < len; i++, n++) o[n] = o[n - back];
    }
    out->len = n;
}

static size_t dan_lzma2_decode(const uint8_t *p, size_t pos, size_t end, dan_bbuf *out, int line) {
    dan_lzm *m = dan_alloc(sizeof *m);
    int have_props = 0, need_dict = 1;
    size_t dict_start = out->len;
    for (;;) {
        if (pos >= end) dan_error(DAN_MSG_CORRUPT, line);
        uint8_t c = p[pos++];
        if (c == 0) break;
        if (c == 1 || c >= 0xE0) { dict_start = out->len; need_dict = 0; }
        else if (need_dict) dan_error(DAN_MSG_CORRUPT, line);
        if (c < 0x80) {
            if (c > 2 || pos + 2 > end) dan_error(DAN_MSG_CORRUPT, line);
            size_t sz = ((size_t)p[pos] << 8 | p[pos + 1]) + 1;
            pos += 2;
            if (pos + sz > end) dan_error(DAN_MSG_CORRUPT, line);
            dan_bbuf_add(out, p + pos, sz);
            pos += sz;
            if (c == 1) have_props = 0;
            continue;
        }
        if (pos + 4 > end) dan_error(DAN_MSG_CORRUPT, line);
        size_t un = ((size_t)(c & 0x1F) << 16) + ((size_t)p[pos] << 8 | p[pos + 1]) + 1;
        size_t pk = ((size_t)p[pos + 2] << 8 | p[pos + 3]) + 1;
        pos += 4;
        int reset = (c >> 5) & 3;
        if (reset >= 2) {
            if (pos >= end) dan_error(DAN_MSG_CORRUPT, line);
            uint32_t props = p[pos++];
            if (props >= 225) dan_error(DAN_MSG_CORRUPT, line);
            m->lc = (int)(props % 9); props /= 9;
            m->lp = (int)(props % 5);
            m->pb = (int)(props / 5);
            if (m->lc + m->lp > 4) dan_error(DAN_MSG_CORRUPT, line);
            have_props = 1;
        } else if (!have_props) dan_error(DAN_MSG_CORRUPT, line);
        if (reset >= 1) dan_lzm_reset(m);
        if (pos + pk > end) dan_error(DAN_MSG_CORRUPT, line);
        dan_rcd rc;
        dan_rcd_init(&rc, p, pos, pos + pk, line);
        dan_lzma_decode_chunk(m, &rc, out, dict_start, un, line);
        pos += pk;
    }
    free(m);
    return pos;
}

static uint64_t dan_read_varint(const uint8_t *p, size_t *pos, size_t end, int line) {
    uint64_t v = 0;
    for (int i = 0; i < 9; i++) {
        if (*pos >= end) dan_error(DAN_MSG_CORRUPT, line);
        uint8_t b = p[(*pos)++];
        v |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return v;
    }
    dan_error(DAN_MSG_CORRUPT, line);
    return 0;
}

static dan_bytes dan_unxz(const uint8_t *p, size_t n, int line) {
    static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    if (n < 24 || memcmp(p, magic, 6) != 0) dan_error(DAN_MSG_NOT_COMPRESSED, line);
    if (p[6] != 0 || dan_le32(p + 8) != dan_crc32_update(0, p + 6, 2)) dan_error(DAN_MSG_CORRUPT, line);
    int check = p[7] & 0x0F;
    static const int check_sizes[16] = { 0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64 };
    size_t csize = (size_t)check_sizes[check];
    dan_bbuf out = { NULL, 0, 0 };
    size_t pos = 12;
    for (;;) {
        if (pos >= n) dan_error(DAN_MSG_CORRUPT, line);
        if (p[pos] == 0) break;                          /* index: no more blocks */
        size_t block_start = pos, hsize = ((size_t)p[pos] + 1) * 4;
        if (pos + hsize > n || dan_le32(p + pos + hsize - 4) != dan_crc32_update(0, p + pos, hsize - 4))
            dan_error(DAN_MSG_CORRUPT, line);
        uint8_t bflags = p[pos + 1];
        size_t q = pos + 2, hend = pos + hsize - 4;
        if ((bflags & 3) != 0) dan_error(DAN_MSG_XZ_FILTER, line);
        if (bflags & 0x40) dan_read_varint(p, &q, hend, line);
        if (bflags & 0x80) dan_read_varint(p, &q, hend, line);
        if (dan_read_varint(p, &q, hend, line) != 0x21) dan_error(DAN_MSG_XZ_FILTER, line);
        if (dan_read_varint(p, &q, hend, line) != 1) dan_error(DAN_MSG_CORRUPT, line);
        size_t out_start = out.len;
        pos = dan_lzma2_decode(p, pos + hsize, n, &out, line);
        while ((pos - block_start) % 4) { if (pos >= n || p[pos] != 0) dan_error(DAN_MSG_CORRUPT, line); pos++; }
        if (pos + csize > n) dan_error(DAN_MSG_CORRUPT, line);
        const uint8_t *data = out.d ? out.d + out_start : (const uint8_t *)"";
        size_t dlen = out.len - out_start;
        if (check == 1 && dan_le32(p + pos) != dan_crc32_update(0, data, dlen)) dan_error(DAN_MSG_CORRUPT, line);
        if (check == 4) {
            uint64_t c64 = dan_crc64(data, dlen), stored = 0;
            for (int i = 0; i < 8; i++) stored |= (uint64_t)p[pos + i] << (8 * i);
            if (c64 != stored) dan_error(DAN_MSG_CORRUPT, line);
        }
        if (check == 10) {
            dan_sha256 c;
            uint8_t h[32];
            dan_sha256_init(&c);
            dan_sha256_update(&c, data, dlen);
            dan_sha256_final(&c, h);
            if (memcmp(h, p + pos, 32) != 0) dan_error(DAN_MSG_CORRUPT, line);
        }
        pos += csize;
    }
    if (!out.d) out.d = dan_alloc(1);
    return dan_bytes_wrap(out.d, (int64_t)out.len);
}

/* ---- public functions: compress (xz), compress_gzip, decompress (any) ---- */

static dan_bytes dan_compress(dan_bytes b) { return dan_xz_raw(b->data, (size_t)b->len); }
static dan_bytes dan_compress_str(dan_str s) { return dan_xz_raw((const uint8_t *)s, strlen(s)); }

static dan_bytes dan_decompress(dan_bytes b, int line) {
    const uint8_t *p = b->data;
    size_t n = (size_t)b->len;
    if (n >= 4 && memcmp(p, "DANC", 4) == 0) dan_error(DAN_MSG_NEED_PASSWORD, line);
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return dan_gunzip(b, line);
    return dan_unxz(p, n, line);
}

/* compression with a password: compress first, then encrypt (as 7z and rar do) */
static dan_bytes dan_compress_pw(dan_bytes b, dan_str pw, int line) { return dan_encrypt(dan_compress(b), pw, line); }
static dan_bytes dan_compress_str_pw(dan_str s, dan_str pw, int line) { return dan_encrypt(dan_compress_str(s), pw, line); }
static dan_bytes dan_decompress_pw(dan_bytes b, dan_str pw, int line) { return dan_decompress(dan_decrypt(b, pw, line), line); }
