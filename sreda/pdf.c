/* ==== PDF: text of pages and creation of documents ====
   Included into a program that uses the functions текст_pdf, шрифт_pdf, ширина_pdf
   or собрать_pdf. Uses inflate/deflate from dannye.c. */

/* ---------- objects ---------- */

enum { PO_NULL, PO_BOOL, PO_NUM, PO_STR, PO_NAME, PO_ARR, PO_DICT, PO_REF, PO_OP };

typedef struct dan_po dan_po;
struct dan_po {
    int t;
    double num;                 /* number, bool, object number of a reference */
    int gen;
    uint8_t *s; size_t n;       /* string, name or operator (bytes, zero-terminated) */
    dan_po **v; char **k;       /* array items; dictionary keys and values */
    size_t cnt;
    const uint8_t *data; size_t dlen;   /* stream data (raw), if the dictionary is a stream */
    int is_stream;
};

/* All parsed data lives in an arena that is freed at once. */
typedef struct dan_pblk { struct dan_pblk *next; size_t used, cap; } dan_pblk;

typedef struct {
    const uint8_t *d; size_t n;
    dan_pblk *arena;
    size_t nobj;
    int64_t *off;       /* offset of object in the file, or -1 */
    int64_t *stm;       /* number of the object stream containing it, or -1 */
    int64_t *idx;       /* index inside that stream */
    dan_po **cache;
    uint8_t *busy;      /* objects being loaded (protection from reference loops) */
    dan_po *trailer;
    int depth;
    int encrypted;
} dan_pdf;

static void *dan_pa(dan_pdf *p, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!p->arena || p->arena->used + n > p->arena->cap) {
        size_t cap = n > 65536 ? n : 65536;
        dan_pblk *b = dan_alloc(sizeof *b + cap);
        b->next = p->arena; b->used = 0; b->cap = cap;
        p->arena = b;
    }
    void *r = (char *)(p->arena + 1) + p->arena->used;
    p->arena->used += n;
    memset(r, 0, n);
    return r;
}

static void dan_pdf_free(dan_pdf *p) {
    while (p->arena) { dan_pblk *b = p->arena; p->arena = b->next; free(b); }
    free(p->off); free(p->stm); free(p->idx); free(p->cache); free(p->busy);
}

static dan_po dan_po_null = { PO_NULL };

static dan_po *dan_pdict_get(const dan_po *d, const char *key) {
    if (!d || d->t != PO_DICT) return &dan_po_null;
    for (size_t i = 0; i < d->cnt; i++) if (!strcmp(d->k[i], key)) return d->v[i];
    return &dan_po_null;
}

/* ---------- lexer ---------- */

typedef struct { const uint8_t *s; size_t pos, end; } dan_plex;

static int dan_pws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int dan_pdelim(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

static void dan_pskip(dan_plex *l) {
    while (l->pos < l->end) {
        int c = l->s[l->pos];
        if (dan_pws(c)) l->pos++;
        else if (c == '%') { while (l->pos < l->end && l->s[l->pos] != '\n' && l->s[l->pos] != '\r') l->pos++; }
        else break;
    }
}

static int dan_phex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static dan_po *dan_pnew(dan_pdf *p, int t) { dan_po *o = dan_pa(p, sizeof *o); o->t = t; return o; }

/* A regular word (number, keyword) at the current position, without moving. */
static size_t dan_pword(const dan_plex *l, size_t pos) {
    size_t e = pos;
    while (e < l->end && !dan_pws(l->s[e]) && !dan_pdelim(l->s[e])) e++;
    return e;
}

static int dan_pnumber_at(const dan_plex *l, size_t pos, size_t e, double *v, int *integer) {
    if (pos >= e || e - pos > 40) return 0;
    char t[48];
    memcpy(t, l->s + pos, e - pos);
    t[e - pos] = 0;
    int digits = 0, dots = 0;
    for (size_t i = 0; i < e - pos; i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') digits++;
        else if (c == '.') dots++;
        else if ((c == '-' || c == '+') && i == 0) {}
        else return 0;
    }
    if (!digits || dots > 1) return 0;
    *v = strtod(t, NULL);
    *integer = dots == 0;
    return 1;
}

static dan_po *dan_pparse(dan_pdf *p, dan_plex *l, int depth);

static dan_po *dan_pparse_string(dan_pdf *p, dan_plex *l) {
    /* (literal) */
    size_t cap = 64, n = 0;
    uint8_t *b = dan_alloc(cap);
    int nest = 1;
    l->pos++;
    while (l->pos < l->end) {
        int c = l->s[l->pos++];
        if (c == '(') nest++;
        else if (c == ')') { if (--nest == 0) break; }
        else if (c == '\\' && l->pos < l->end) {
            c = l->s[l->pos++];
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '\r': if (l->pos < l->end && l->s[l->pos] == '\n') l->pos++; continue;
                case '\n': continue;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && l->pos < l->end && l->s[l->pos] >= '0' && l->s[l->pos] <= '7'; k++)
                            v = v * 8 + (l->s[l->pos++] - '0');
                        c = v & 255;
                    }
            }
        } else if (c == '\r') {     /* an end of line inside a string is \n */
            if (l->pos < l->end && l->s[l->pos] == '\n') l->pos++;
            c = '\n';
        }
        if (n + 1 >= cap) { cap *= 2; b = dan_realloc(b, cap); }
        b[n++] = (uint8_t)c;
    }
    dan_po *o = dan_pnew(p, PO_STR);
    o->s = dan_pa(p, n + 1);
    memcpy(o->s, b, n);
    o->n = n;
    free(b);
    return o;
}

static dan_po *dan_pparse_hex(dan_pdf *p, dan_plex *l) {
    l->pos++;
    size_t start = l->pos;
    while (l->pos < l->end && l->s[l->pos] != '>') l->pos++;
    dan_po *o = dan_pnew(p, PO_STR);
    o->s = dan_pa(p, (l->pos - start) / 2 + 2);
    int hi = -1;
    for (size_t i = start; i < l->pos; i++) {
        int v = dan_phex(l->s[i]);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { o->s[o->n++] = (uint8_t)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) o->s[o->n++] = (uint8_t)(hi * 16);
    if (l->pos < l->end) l->pos++;
    return o;
}

static dan_po *dan_pparse_name(dan_pdf *p, dan_plex *l) {
    l->pos++;
    size_t e = dan_pword(l, l->pos);
    dan_po *o = dan_pnew(p, PO_NAME);
    o->s = dan_pa(p, e - l->pos + 1);
    for (size_t i = l->pos; i < e; i++) {
        int c = l->s[i];
        if (c == '#' && i + 2 < e && dan_phex(l->s[i + 1]) >= 0 && dan_phex(l->s[i + 2]) >= 0) {
            c = dan_phex(l->s[i + 1]) * 16 + dan_phex(l->s[i + 2]);
            i += 2;
        }
        o->s[o->n++] = (uint8_t)c;
    }
    l->pos = e;
    return o;
}

static dan_po *dan_pparse(dan_pdf *p, dan_plex *l, int depth) {
    dan_pskip(l);
    if (l->pos >= l->end || depth > 200) return NULL;
    int c = l->s[l->pos];
    if (c == '/') return dan_pparse_name(p, l);
    if (c == '(') return dan_pparse_string(p, l);
    if (c == '<' && l->pos + 1 < l->end && l->s[l->pos + 1] == '<') {
        l->pos += 2;
        size_t cap = 8;
        dan_po *o = dan_pnew(p, PO_DICT);
        char **k = dan_alloc(sizeof(char *) * cap);
        dan_po **v = dan_alloc(sizeof(dan_po *) * cap);
        for (;;) {
            dan_pskip(l);
            if (l->pos + 1 >= l->end) break;
            if (l->s[l->pos] == '>' && l->s[l->pos + 1] == '>') { l->pos += 2; break; }
            if (l->s[l->pos] != '/') {          /* damaged: skip a token */
                dan_po *junk = dan_pparse(p, l, depth + 1);
                if (!junk) break;
                continue;
            }
            dan_po *key = dan_pparse_name(p, l);
            dan_po *val = dan_pparse(p, l, depth + 1);
            if (!val) break;
            if (o->cnt == cap) { cap *= 2; k = dan_realloc(k, sizeof(char *) * cap); v = dan_realloc(v, sizeof(dan_po *) * cap); }
            k[o->cnt] = (char *)key->s;
            v[o->cnt++] = val;
        }
        o->k = dan_pa(p, sizeof(char *) * (o->cnt + 1));
        o->v = dan_pa(p, sizeof(dan_po *) * (o->cnt + 1));
        memcpy(o->k, k, sizeof(char *) * o->cnt);
        memcpy(o->v, v, sizeof(dan_po *) * o->cnt);
        free(k); free(v);
        return o;
    }
    if (c == '<') return dan_pparse_hex(p, l);
    if (c == '[') {
        l->pos++;
        size_t cap = 8;
        dan_po *o = dan_pnew(p, PO_ARR);
        dan_po **v = dan_alloc(sizeof(dan_po *) * cap);
        for (;;) {
            dan_pskip(l);
            if (l->pos >= l->end) break;
            if (l->s[l->pos] == ']') { l->pos++; break; }
            dan_po *it = dan_pparse(p, l, depth + 1);
            if (!it) break;
            if (o->cnt == cap) { cap *= 2; v = dan_realloc(v, sizeof(dan_po *) * cap); }
            v[o->cnt++] = it;
        }
        o->v = dan_pa(p, sizeof(dan_po *) * (o->cnt + 1));
        memcpy(o->v, v, sizeof(dan_po *) * o->cnt);
        free(v);
        return o;
    }
    if (c == ')' || c == '>' || c == ']' || c == '{' || c == '}') {      /* stray delimiter */
        l->pos++;
        dan_po *o = dan_pnew(p, PO_OP);
        o->s = dan_pa(p, 2); o->s[0] = (uint8_t)c; o->n = 1;
        return o;
    }
    size_t e = dan_pword(l, l->pos);
    if (e == l->pos) { l->pos++; return dan_pnew(p, PO_NULL); }
    double v; int integer;
    if (dan_pnumber_at(l, l->pos, e, &v, &integer)) {
        size_t save = e;
        l->pos = e;
        if (integer && v >= 0) {
            /* a reference «12 0 R»? */
            dan_plex t = *l;
            dan_pskip(&t);
            size_t e2 = dan_pword(&t, t.pos);
            double g; int gi;
            if (dan_pnumber_at(&t, t.pos, e2, &g, &gi) && gi) {
                t.pos = e2;
                dan_pskip(&t);
                if (t.pos < t.end && t.s[t.pos] == 'R' && (t.pos + 1 >= t.end || dan_pws(t.s[t.pos + 1]) || dan_pdelim(t.s[t.pos + 1]))) {
                    dan_po *o = dan_pnew(p, PO_REF);
                    o->num = v; o->gen = (int)g;
                    l->pos = t.pos + 1;
                    return o;
                }
            }
        }
        l->pos = save;
        dan_po *o = dan_pnew(p, PO_NUM);
        o->num = v;
        return o;
    }
    dan_po *o;
    if (e - l->pos == 4 && !memcmp(l->s + l->pos, "true", 4)) { o = dan_pnew(p, PO_BOOL); o->num = 1; }
    else if (e - l->pos == 5 && !memcmp(l->s + l->pos, "false", 5)) { o = dan_pnew(p, PO_BOOL); }
    else if (e - l->pos == 4 && !memcmp(l->s + l->pos, "null", 4)) { o = dan_pnew(p, PO_NULL); }
    else {
        o = dan_pnew(p, PO_OP);
        o->s = dan_pa(p, e - l->pos + 1);
        memcpy(o->s, l->s + l->pos, e - l->pos);
        o->n = e - l->pos;
    }
    l->pos = e;
    return o;
}

static int dan_pop_is(const dan_po *o, const char *op) { return o && o->t == PO_OP && !strcmp((const char *)o->s, op); }

/* ---------- objects of the file ---------- */

static dan_po *dan_pget(dan_pdf *p, int64_t num);

static dan_po *dan_pres(dan_pdf *p, dan_po *o) {
    for (int i = 0; o && o->t == PO_REF && i < 32; i++) o = dan_pget(p, (int64_t)o->num);
    return o ? o : &dan_po_null;
}

static dan_po *dan_pkey(dan_pdf *p, const dan_po *d, const char *key) { return dan_pres(p, dan_pdict_get(d, key)); }

static int64_t dan_pint(dan_pdf *p, const dan_po *d, const char *key, int64_t def) {
    dan_po *o = dan_pkey(p, d, key);
    return o->t == PO_NUM ? (int64_t)o->num : def;
}

/* "N G obj <object> [stream ... endstream]" at the position of the lexer */
static dan_po *dan_pparse_indirect(dan_pdf *p, dan_plex *l) {
    dan_po *a = dan_pparse(p, l, 0), *b = dan_pparse(p, l, 0), *c = dan_pparse(p, l, 0);
    if (!a || !b || !c || a->t != PO_NUM || b->t != PO_NUM || !dan_pop_is(c, "obj")) return NULL;
    dan_po *o = dan_pparse(p, l, 0);
    if (!o) return NULL;
    if (o->t == PO_DICT) {
        dan_pskip(l);
        if (l->pos + 6 <= l->end && !memcmp(l->s + l->pos, "stream", 6)) {
            size_t pos = l->pos + 6;
            if (pos < l->end && l->s[pos] == '\r') pos++;
            if (pos < l->end && l->s[pos] == '\n') pos++;
            int64_t len = dan_pint(p, o, "Length", -1);
            size_t e = (len >= 0 && pos + (size_t)len <= l->end) ? pos + (size_t)len : l->end;
            /* the length is checked: after the data must come «endstream» */
            size_t q = e;
            while (q < l->end && dan_pws(l->s[q])) q++;
            if (len < 0 || q + 9 > l->end || memcmp(l->s + q, "endstream", 9)) {
                const uint8_t *f = NULL;
                for (size_t i = pos; i + 9 <= l->end; i++) if (!memcmp(l->s + i, "endstream", 9)) { f = l->s + i; break; }
                e = f ? (size_t)(f - l->s) : l->end;
                while (e > pos && (l->s[e - 1] == '\n' || l->s[e - 1] == '\r')) e--;
            }
            o->is_stream = 1;
            o->data = l->s + pos;
            o->dlen = e - pos;
        }
    }
    return o;
}

static uint8_t *dan_pstream(dan_pdf *p, dan_po *s, size_t *outlen);

static dan_po *dan_pget(dan_pdf *p, int64_t num) {
    if (num < 0 || (size_t)num >= p->nobj) return &dan_po_null;
    if (p->cache[num]) return p->cache[num];
    if (p->busy[num] || p->depth > 64) return &dan_po_null;
    p->busy[num] = 1;
    p->depth++;
    dan_po *o = NULL;
    if (p->stm[num] >= 0) {
        dan_po *st = dan_pget(p, p->stm[num]);
        if (st->is_stream) {
            size_t len;
            uint8_t *d = dan_pstream(p, st, &len);
            int64_t n = dan_pint(p, st, "N", 0), first = dan_pint(p, st, "First", 0);
            dan_plex hl = { d, 0, len };
            for (int64_t i = 0; i < n; i++) {
                dan_po *on = dan_pparse(p, &hl, 0), *oo = dan_pparse(p, &hl, 0);
                if (!on || !oo || on->t != PO_NUM || oo->t != PO_NUM) break;
                if (i == p->idx[num] || (int64_t)on->num == num) {
                    dan_plex ol = { d, (size_t)first + (size_t)oo->num, len };
                    if (ol.pos < len) o = dan_pparse(p, &ol, 0);
                    break;
                }
            }
        }
    } else if (p->off[num] >= 0 && (size_t)p->off[num] < p->n) {
        dan_plex l = { p->d, (size_t)p->off[num], p->n };
        o = dan_pparse_indirect(p, &l);
    }
    p->depth--;
    p->busy[num] = 0;
    if (!o) o = &dan_po_null;
    p->cache[num] = o;
    return o;
}

/* ---------- filters ---------- */

static uint8_t *dan_ppredict(uint8_t *d, size_t *len, int64_t pred, int64_t colors, int64_t bpc, int64_t cols) {
    if (pred < 10) return d;            /* TIFF predictor 2 is not used for text */
    /* the parameters come from the file: anything outside the limits of the format is ignored */
    if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > (1 << 20)) return d;
    size_t bpp = (size_t)((colors * bpc + 7) / 8), row = (size_t)((colors * bpc * cols + 7) / 8);
    if (!bpp) bpp = 1;
    if (!row) return d;
    size_t rows = *len / (row + 1);
    uint8_t *out = dan_alloc(rows * row + 1), *prev = dan_alloc(row);
    memset(prev, 0, row);
    for (size_t r = 0; r < rows; r++) {
        const uint8_t *src = d + r * (row + 1);
        int type = src[0];
        uint8_t *cur = out + r * row;
        for (size_t i = 0; i < row; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = src[1 + i];
            switch (type) {
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
                    x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
            }
            cur[i] = (uint8_t)x;
        }
        memcpy(prev, cur, row);
    }
    free(prev);
    free(d);
    *len = rows * row;
    return out;
}

static uint8_t *dan_pfilter(dan_pdf *p, const char *name, dan_po *parms, uint8_t *d, size_t *len) {
    if (!strcmp(name, "FlateDecode") || !strcmp(name, "Fl")) {
        size_t skip = (*len >= 2 && (d[0] & 15) == 8 && ((d[0] << 8) | d[1]) % 31 == 0) ? 2 : 0;
        dan_inf s = { d + skip, *len - skip, 0, 0, 0, NULL, 0, 0, 0, 1, 0 };
        dan_inflate_into(&s);
        free(d);
        *len = s.outlen;
        d = s.out ? s.out : dan_alloc(1);
        if (parms->t == PO_DICT)
            d = dan_ppredict(d, len, dan_pint(p, parms, "Predictor", 1), dan_pint(p, parms, "Colors", 1),
                             dan_pint(p, parms, "BitsPerComponent", 8), dan_pint(p, parms, "Columns", 1));
        return d;
    }
    if (!strcmp(name, "ASCIIHexDecode") || !strcmp(name, "AHx")) {
        uint8_t *o = dan_alloc(*len / 2 + 2);
        size_t n = 0;
        int hi = -1;
        for (size_t i = 0; i < *len && d[i] != '>'; i++) {
            int v = dan_phex(d[i]);
            if (v < 0) continue;
            if (hi < 0) hi = v; else { o[n++] = (uint8_t)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) o[n++] = (uint8_t)(hi * 16);
        free(d);
        *len = n;
        return o;
    }
    if (!strcmp(name, "ASCII85Decode") || !strcmp(name, "A85")) {
        uint8_t *o = dan_alloc(*len * 4 + 8);     /* «z» gives four bytes */
        size_t n = 0;
        uint32_t acc = 0;
        int k = 0;
        for (size_t i = 0; i < *len; i++) {
            int c = d[i];
            if (c == '~') break;
            if (c == 'z' && k == 0) { memset(o + n, 0, 4); n += 4; continue; }
            if (c < '!' || c > 'u') continue;
            acc = acc * 85 + (uint32_t)(c - '!');
            if (++k == 5) { o[n++] = (uint8_t)(acc >> 24); o[n++] = (uint8_t)(acc >> 16); o[n++] = (uint8_t)(acc >> 8); o[n++] = (uint8_t)acc; acc = 0; k = 0; }
        }
        if (k > 1) {
            for (int j = k; j < 5; j++) acc = acc * 85 + 84;
            for (int j = 0; j < k - 1; j++) o[n++] = (uint8_t)(acc >> (24 - 8 * j));
        }
        free(d);
        *len = n;
        return o;
    }
    /* LZW, images and others: no text in them */
    free(d);
    *len = 0;
    return dan_alloc(1);
}

/* Decoded data of a stream; the result lives in the arena */
static uint8_t *dan_pstream(dan_pdf *p, dan_po *s, size_t *outlen) {
    *outlen = 0;
    if (!s || !s->is_stream) return dan_pa(p, 1);
    size_t len = s->dlen;
    uint8_t *d = dan_alloc(len + 1);
    memcpy(d, s->data, len);
    dan_po *f = dan_pkey(p, s, "Filter"), *parms = dan_pkey(p, s, "DecodeParms");
    if (f->t == PO_NAME) d = dan_pfilter(p, (const char *)f->s, parms, d, &len);
    else if (f->t == PO_ARR)
        for (size_t i = 0; i < f->cnt; i++) {
            dan_po *fi = dan_pres(p, f->v[i]);
            dan_po *pi = parms->t == PO_ARR && i < parms->cnt ? dan_pres(p, parms->v[i]) : &dan_po_null;
            if (fi->t == PO_NAME) d = dan_pfilter(p, (const char *)fi->s, pi, d, &len);
        }
    uint8_t *r = dan_pa(p, len + 1);
    memcpy(r, d, len);
    free(d);
    *outlen = len;
    return r;
}

/* ---------- cross-reference ---------- */

static void dan_pxref_size(dan_pdf *p, size_t need) {
    if (need <= p->nobj) return;
    if (need > 10000000) need = 10000000;
    size_t old = p->nobj;
    p->off = dan_realloc(p->off, sizeof(int64_t) * need);
    p->stm = dan_realloc(p->stm, sizeof(int64_t) * need);
    p->idx = dan_realloc(p->idx, sizeof(int64_t) * need);
    p->cache = dan_realloc(p->cache, sizeof(dan_po *) * need);
    p->busy = dan_realloc(p->busy, need);
    for (size_t i = old; i < need; i++) { p->off[i] = -1; p->stm[i] = -1; p->idx[i] = 0; p->cache[i] = NULL; p->busy[i] = 0; }
    p->nobj = need;
}

/* entry of the newest section wins: an object already set is not overwritten */
static void dan_pxref_set(dan_pdf *p, int64_t num, int64_t off, int64_t stm, int64_t idx, uint8_t *seen) {
    if (num < 0 || num >= 10000000) return;
    dan_pxref_size(p, (size_t)num + 1);
    if (seen[num / 8] & (1 << (num % 8))) return;
    seen[num / 8] |= (uint8_t)(1 << (num % 8));
    p->off[num] = off;
    p->stm[num] = stm;
    p->idx[num] = idx;
}

static int dan_pread_xref(dan_pdf *p, size_t at, uint8_t *seen, int depth) {
    if (depth > 32 || at >= p->n) return 0;
    dan_plex l = { p->d, at, p->n };
    dan_pskip(&l);
    dan_po *trailer = NULL;
    if (l.pos + 4 <= p->n && !memcmp(p->d + l.pos, "xref", 4)) {
        l.pos += 4;
        for (;;) {
            dan_pskip(&l);
            if (l.pos + 7 <= p->n && !memcmp(p->d + l.pos, "trailer", 7)) { l.pos += 7; break; }
            dan_po *a = dan_pparse(p, &l, 0), *b = dan_pparse(p, &l, 0);
            if (!a || !b || a->t != PO_NUM || b->t != PO_NUM) return 0;
            int64_t start = (int64_t)a->num, count = (int64_t)b->num;
            if (count < 0 || count > 10000000) return 0;
            for (int64_t i = 0; i < count; i++) {
                dan_po *o = dan_pparse(p, &l, 0), *g = dan_pparse(p, &l, 0), *t = dan_pparse(p, &l, 0);
                if (!o || !g || !t) return 0;
                if (dan_pop_is(t, "n")) dan_pxref_set(p, start + i, (int64_t)o->num, -1, 0, seen);
                else dan_pxref_set(p, start + i, -1, -1, 0, seen);
            }
        }
        trailer = dan_pparse(p, &l, 0);
        if (!trailer || trailer->t != PO_DICT) return 0;
        int64_t xs = dan_pint(p, trailer, "XRefStm", -1);       /* hybrid files */
        if (xs > 0) dan_pread_xref(p, (size_t)xs, seen, depth + 1);
    } else {
        dan_po *s = dan_pparse_indirect(p, &l);
        if (!s || !s->is_stream) return 0;
        trailer = s;
        dan_po *w = dan_pkey(p, s, "W"), *index = dan_pkey(p, s, "Index");
        if (w->t != PO_ARR || w->cnt < 3) return 0;
        int ws[3];
        for (int i = 0; i < 3; i++) { dan_po *x = dan_pres(p, w->v[i]); ws[i] = x->t == PO_NUM ? (int)x->num : 0; if (ws[i] < 0 || ws[i] > 8) return 0; }
        size_t len;
        uint8_t *d = dan_pstream(p, s, &len);
        size_t ent = (size_t)(ws[0] + ws[1] + ws[2]), pos = 0;
        if (!ent) return 0;
        int64_t size = dan_pint(p, s, "Size", 0);
        size_t nsec = index->t == PO_ARR ? index->cnt / 2 : 1;
        for (size_t sec = 0; sec < nsec; sec++) {
            int64_t start = 0, count = size;
            if (index->t == PO_ARR) { start = (int64_t)dan_pres(p, index->v[2 * sec])->num; count = (int64_t)dan_pres(p, index->v[2 * sec + 1])->num; }
            for (int64_t i = 0; i < count && pos + ent <= len; i++) {
                uint64_t f[3];
                for (int k = 0; k < 3; k++) {
                    f[k] = 0;
                    for (int b = 0; b < ws[k]; b++) f[k] = f[k] << 8 | d[pos++];
                }
                uint64_t type = ws[0] ? f[0] : 1;
                if (type == 1) dan_pxref_set(p, start + i, (int64_t)f[1], -1, 0, seen);
                else if (type == 2) dan_pxref_set(p, start + i, -1, (int64_t)f[1], (int64_t)f[2], seen);
                else dan_pxref_set(p, start + i, -1, -1, 0, seen);
            }
        }
    }
    if (!p->trailer) p->trailer = trailer;
    else if (dan_pdict_get(p->trailer, "Root")->t == PO_NULL && dan_pdict_get(trailer, "Root")->t != PO_NULL) p->trailer = trailer;
    int64_t prev = dan_pint(p, trailer, "Prev", -1);
    if (prev >= 0 && (size_t)prev != at) dan_pread_xref(p, (size_t)prev, seen, depth + 1);
    return 1;
}

/* Damaged file: objects are found by scanning for «N G obj» */
static void dan_preconstruct(dan_pdf *p) {
    for (size_t i = 0; i < p->nobj; i++) { p->off[i] = -1; p->stm[i] = -1; p->cache[i] = NULL; }
    for (size_t i = 0; i + 5 < p->n; i++) {
        if (memcmp(p->d + i, "obj", 3) || (i + 3 < p->n && !dan_pws(p->d[i + 3]) && !dan_pdelim(p->d[i + 3]))) continue;
        /* go back over «G» and «N» */
        size_t j = i;
        while (j > 0 && dan_pws(p->d[j - 1])) j--;
        size_t ge = j;
        while (j > 0 && isdigit(p->d[j - 1])) j--;
        if (j == ge) continue;
        while (j > 0 && dan_pws(p->d[j - 1])) j--;
        size_t ne = j;
        while (j > 0 && isdigit(p->d[j - 1])) j--;
        if (j == ne || ne - j > 9) continue;
        int64_t num = 0;
        for (size_t k = j; k < ne; k++) num = num * 10 + (p->d[k] - '0');
        if (num >= 10000000) continue;
        dan_pxref_size(p, (size_t)num + 1);
        p->off[num] = (int64_t)j;
        p->stm[num] = -1;
    }
    /* trailer: the last «trailer» dictionary or the catalog among the objects */
    p->trailer = NULL;
    for (size_t i = p->n; i-- > 7;) {
        if (!memcmp(p->d + i - 7, "trailer", 7)) {
            dan_plex l = { p->d, i, p->n };
            dan_po *t = dan_pparse(p, &l, 0);
            if (t && t->t == PO_DICT && dan_pdict_get(t, "Root")->t == PO_REF) { p->trailer = t; break; }
        }
    }
    if (!p->trailer) {
        for (size_t num = 0; num < p->nobj; num++) {
            if (p->off[num] < 0) continue;
            dan_po *o = dan_pget(p, (int64_t)num);
            dan_po *t = dan_pkey(p, o, "Type");
            if (o->t == PO_DICT && t->t == PO_NAME && !strcmp((const char *)t->s, "Catalog")) {
                dan_po *tr = dan_pnew(p, PO_DICT);
                tr->k = dan_pa(p, sizeof(char *));
                tr->v = dan_pa(p, sizeof(dan_po *));
                tr->k[0] = "Root";
                tr->v[0] = dan_pnew(p, PO_REF);
                tr->v[0]->num = (double)num;
                tr->cnt = 1;
                p->trailer = tr;
                break;
            }
            /* object streams found by scanning contain objects too */
            if (o->is_stream && t->t == PO_NAME && !strcmp((const char *)t->s, "ObjStm")) {
                size_t len;
                uint8_t *d = dan_pstream(p, o, &len);
                int64_t n = dan_pint(p, o, "N", 0);
                dan_plex hl = { d, 0, len };
                for (int64_t k = 0; k < n; k++) {
                    dan_po *on = dan_pparse(p, &hl, 0), *oo = dan_pparse(p, &hl, 0);
                    if (!on || !oo || on->t != PO_NUM) break;
                    int64_t m = (int64_t)on->num;
                    if (m < 0 || m >= 10000000) continue;
                    dan_pxref_size(p, (size_t)m + 1);
                    if (p->off[m] < 0) { p->stm[m] = (int64_t)num; p->idx[m] = k; }
                }
            }
        }
    }
}

static const char *dan_popen(dan_pdf *p, const uint8_t *d, size_t n) {
    memset(p, 0, sizeof *p);
    p->d = d;
    p->n = n;
    size_t head = n < 1024 ? n : 1024, i;
    for (i = 0; i + 5 <= head; i++) if (!memcmp(d + i, "%PDF-", 5)) break;
    if (i + 5 > head) return DAN_TXT_PDF_NOT;
    dan_pxref_size(p, 1);
    size_t tail = n > 2048 ? n - 2048 : 0, sx = 0;
    for (size_t k = n; k-- > tail + 9;) if (!memcmp(d + k - 9, "startxref", 9)) { sx = k; break; }
    int ok = 0;
    if (sx) {
        dan_plex l = { d, sx, n };
        dan_po *o = dan_pparse(p, &l, 0);
        if (o && o->t == PO_NUM && o->num >= 0) {
            size_t bits = 1 << 20;
            uint8_t *seen = dan_alloc(bits);
            memset(seen, 0, bits);
            ok = dan_pread_xref(p, (size_t)o->num, seen, 0);
            free(seen);
        }
    }
    if (!ok || !p->trailer || dan_pkey(p, p->trailer, "Root")->t != PO_DICT) dan_preconstruct(p);
    if (!p->trailer || dan_pkey(p, p->trailer, "Root")->t != PO_DICT) return DAN_TXT_PDF_BROKEN;
    if (dan_pdict_get(p->trailer, "Encrypt")->t != PO_NULL) p->encrypted = 1;
    return NULL;
}

/* ---------- fonts of the text ---------- */

typedef struct { uint32_t lo, hi; uint32_t *uni; int nuni; uint32_t base; } dan_pmap;     /* range -> unicode */

typedef struct {
    int two;                    /* codes are two bytes (Type0 fonts) */
    dan_pmap *m; int nm, cap;   /* from ToUnicode */
    uint32_t simple[256];       /* for simple fonts: code -> unicode */
    int has_simple;
    double sw[256];             /* widths of codes of a simple font, thousandths of the size */
    struct { uint32_t lo, hi; double w; } *cw; int ncw, capcw;     /* widths of CID fonts */
    double dw;                  /* width when nothing is known */
} dan_pfont;

/* glyph names that may appear in /Differences: Latin letters are their names */
typedef struct { const char *n; uint16_t u; } dan_pglyph;
static const dan_pglyph dan_pglyphs[] = {
    { "space", 32 }, { "exclam", 33 }, { "quotedbl", 34 }, { "numbersign", 35 }, { "dollar", 36 }, { "percent", 37 },
    { "ampersand", 38 }, { "quotesingle", 39 }, { "quoteright", 0x2019 }, { "parenleft", 40 }, { "parenright", 41 },
    { "asterisk", 42 }, { "plus", 43 }, { "comma", 44 }, { "hyphen", 45 }, { "minus", 0x2212 }, { "period", 46 },
    { "slash", 47 }, { "zero", 48 }, { "one", 49 }, { "two", 50 }, { "three", 51 }, { "four", 52 }, { "five", 53 },
    { "six", 54 }, { "seven", 55 }, { "eight", 56 }, { "nine", 57 }, { "colon", 58 }, { "semicolon", 59 },
    { "less", 60 }, { "equal", 61 }, { "greater", 62 }, { "question", 63 }, { "at", 64 }, { "bracketleft", 91 },
    { "backslash", 92 }, { "bracketright", 93 }, { "asciicircum", 94 }, { "underscore", 95 }, { "grave", 96 },
    { "quoteleft", 0x2018 }, { "braceleft", 123 }, { "bar", 124 }, { "braceright", 125 }, { "asciitilde", 126 },
    { "endash", 0x2013 }, { "emdash", 0x2014 }, { "quotedblleft", 0x201C }, { "quotedblright", 0x201D },
    { "quotedblbase", 0x201E }, { "guillemotleft", 0xAB }, { "guillemotright", 0xBB }, { "bullet", 0x2022 },
    { "ellipsis", 0x2026 }, { "numero", 0x2116 }, { "afii61352", 0x2116 }, { "section", 0xA7 }, { "degree", 0xB0 },
    { "copyright", 0xA9 }, { "registered", 0xAE }, { "trademark", 0x2122 }, { "nbspace", 0xA0 }, { "fi", 0xFB01 },
    { "fl", 0xFB02 }, { "Euro", 0x20AC }, { "periodcentered", 0xB7 }, { "multiply", 0xD7 }, { "divide", 0xF7 },
    { "afii10023", 0x401 }, { "afii10071", 0x451 }, { NULL, 0 }
};

static uint32_t dan_pglyph_unicode(const char *n) {
    if (strlen(n) == 1) return (unsigned char)n[0];
    for (int i = 0; dan_pglyphs[i].n; i++) if (!strcmp(dan_pglyphs[i].n, n)) return dan_pglyphs[i].u;
    /* afii10017..afii10049 - А..Я, afii10065..afii10097 - а..я */
    if (!strncmp(n, "afii100", 7) && strlen(n) == 9) {
        int k = atoi(n + 7);
        if (k >= 17 && k <= 49) return 0x410 + (uint32_t)(k - 17);
        if (k >= 65 && k <= 97) return 0x430 + (uint32_t)(k - 65);
    }
    if (!strncmp(n, "uni", 3) && strlen(n) >= 7) return (uint32_t)strtoul(n + 3, NULL, 16);
    if (n[0] == 'u' && strlen(n) >= 5 && strlen(n) <= 7) return (uint32_t)strtoul(n + 1, NULL, 16);
    return 0;
}

/* WinAnsiEncoding: codes 128-159 differ from Latin-1 */
static const uint16_t dan_pwin[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static void dan_pfont_add(dan_pfont *f, uint32_t lo, uint32_t hi, uint32_t base, uint32_t *uni, int nuni) {
    if (f->nm == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->m = dan_realloc(f->m, sizeof(dan_pmap) * (size_t)f->cap); }
    dan_pmap *m = &f->m[f->nm++];
    m->lo = lo; m->hi = hi; m->base = base; m->uni = uni; m->nuni = nuni;
}

static uint32_t dan_pbytes_int(const dan_po *o) {
    uint32_t v = 0;
    for (size_t i = 0; i < o->n && i < 4; i++) v = v << 8 | o->s[i];
    return v;
}

/* unicode string of a ToUnicode destination (UTF-16BE) */
static int dan_putf16(dan_pdf *p, const dan_po *o, uint32_t **out) {
    int n = 0;
    uint32_t *u = dan_pa(p, sizeof(uint32_t) * (o->n / 2 + 1));
    for (size_t i = 0; i + 1 < o->n; i += 2) {
        uint32_t c = (uint32_t)o->s[i] << 8 | o->s[i + 1];
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < o->n) {
            uint32_t c2 = (uint32_t)o->s[i + 2] << 8 | o->s[i + 3];
            c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
            i += 2;
        }
        u[n++] = c;
    }
    *out = u;
    return n;
}

static void dan_pcmap(dan_pdf *p, dan_pfont *f, const uint8_t *d, size_t len) {
    dan_plex l = { d, 0, len };
    dan_po *prev[3] = { NULL, NULL, NULL };
    for (;;) {
        dan_po *o = dan_pparse(p, &l, 0);
        if (!o) break;
        if (dan_pop_is(o, "begincodespacerange")) {
            dan_po *a = dan_pparse(p, &l, 0);
            if (a && a->t == PO_STR && a->n == 2) f->two = 1;
        } else if (dan_pop_is(o, "beginbfchar")) {
            for (;;) {
                dan_po *a = dan_pparse(p, &l, 0);
                if (!a || dan_pop_is(a, "endbfchar")) break;
                dan_po *b = dan_pparse(p, &l, 0);
                if (!b) break;
                if (a->t != PO_STR) continue;
                if (a->n == 2) f->two = 1;
                uint32_t *u = NULL;
                int nu = 0;
                if (b->t == PO_STR) nu = dan_putf16(p, b, &u);
                else if (b->t == PO_NAME) { u = dan_pa(p, sizeof(uint32_t)); u[0] = dan_pglyph_unicode((const char *)b->s); nu = 1; }
                uint32_t c = dan_pbytes_int(a);
                dan_pfont_add(f, c, c, 0, u, nu);
            }
        } else if (dan_pop_is(o, "beginbfrange")) {
            for (;;) {
                dan_po *a = dan_pparse(p, &l, 0);
                if (!a || dan_pop_is(a, "endbfrange")) break;
                dan_po *b = dan_pparse(p, &l, 0), *c = dan_pparse(p, &l, 0);
                if (!b || !c || a->t != PO_STR || b->t != PO_STR) continue;
                if (a->n == 2) f->two = 1;
                uint32_t lo = dan_pbytes_int(a), hi = dan_pbytes_int(b);
                if (hi < lo || hi - lo > 65535) continue;
                if (c->t == PO_STR) {
                    uint32_t *u;
                    int nu = dan_putf16(p, c, &u);
                    if (nu >= 1) dan_pfont_add(f, lo, hi, u[nu - 1], u, nu);     /* the last character grows */
                } else if (c->t == PO_ARR) {
                    for (uint32_t k = lo; k <= hi && k - lo < c->cnt; k++) {
                        dan_po *s = dan_pres(p, c->v[k - lo]);
                        if (s->t != PO_STR) continue;
                        uint32_t *u;
                        int nu = dan_putf16(p, s, &u);
                        dan_pfont_add(f, k, k, 0, u, nu);
                    }
                }
            }
        }
        prev[0] = prev[1]; prev[1] = prev[2]; prev[2] = o;
    }
    (void)prev;
}

static void dan_pfont_cw(dan_pfont *f, uint32_t lo, uint32_t hi, double w) {
    if (f->ncw == f->capcw) { f->capcw = f->capcw ? f->capcw * 2 : 64; f->cw = dan_realloc(f->cw, sizeof *f->cw * (size_t)f->capcw); }
    f->cw[f->ncw].lo = lo; f->cw[f->ncw].hi = hi; f->cw[f->ncw].w = w;
    f->ncw++;
}

static double dan_pfont_width(const dan_pfont *f, uint32_t code) {
    if (!f->two) return code < 256 ? f->sw[code] : f->dw;
    for (int i = 0; i < f->ncw; i++) if (code >= f->cw[i].lo && code <= f->cw[i].hi) return f->cw[i].w;
    return f->dw;
}

static void dan_pfont_load(dan_pdf *p, dan_pfont *f, dan_po *font) {
    memset(f, 0, sizeof *f);
    f->dw = 500;
    for (int c = 0; c < 256; c++) f->sw[c] = 500;
    dan_po *widths = dan_pkey(p, font, "Widths");
    if (widths->t == PO_ARR) {
        int64_t first = dan_pint(p, font, "FirstChar", 0);
        for (size_t i = 0; i < widths->cnt; i++) {
            dan_po *w = dan_pres(p, widths->v[i]);
            if (w->t == PO_NUM && first + (int64_t)i >= 0 && first + (int64_t)i < 256) f->sw[first + (int64_t)i] = w->num;
        }
    }
    dan_po *desc = dan_pkey(p, font, "DescendantFonts");
    if (desc->t == PO_ARR && desc->cnt > 0) {
        dan_po *cid = dan_pres(p, desc->v[0]);
        f->dw = (double)dan_pint(p, cid, "DW", 1000);
        dan_po *w = dan_pkey(p, cid, "W");
        for (size_t i = 0; w->t == PO_ARR && i < w->cnt;) {
            dan_po *a = dan_pres(p, w->v[i]);
            dan_po *b = i + 1 < w->cnt ? dan_pres(p, w->v[i + 1]) : &dan_po_null;
            if (a->t != PO_NUM) { i++; continue; }
            if (b->t == PO_ARR) {
                for (size_t k = 0; k < b->cnt; k++) {
                    dan_po *x = dan_pres(p, b->v[k]);
                    if (x->t == PO_NUM) dan_pfont_cw(f, (uint32_t)a->num + (uint32_t)k, (uint32_t)a->num + (uint32_t)k, x->num);
                }
                i += 2;
            } else {
                dan_po *c = i + 2 < w->cnt ? dan_pres(p, w->v[i + 2]) : &dan_po_null;
                if (b->t == PO_NUM && c->t == PO_NUM && b->num >= a->num) dan_pfont_cw(f, (uint32_t)a->num, (uint32_t)b->num, c->num);
                i += 3;
            }
        }
    }
    dan_po *sub = dan_pkey(p, font, "Subtype");
    if (sub->t == PO_NAME && !strcmp((const char *)sub->s, "Type0")) f->two = 1;
    dan_po *tu = dan_pkey(p, font, "ToUnicode");
    if (tu->is_stream) {
        size_t len;
        uint8_t *d = dan_pstream(p, tu, &len);
        dan_pcmap(p, f, d, len);
        if (sub->t == PO_NAME && !strcmp((const char *)sub->s, "Type0")) f->two = 1;
    }
    if (!f->two) {
        /* simple font: standard encoding (WinAnsi) and differences */
        f->has_simple = 1;
        for (int c = 0; c < 256; c++) f->simple[c] = (uint32_t)c;
        for (int c = 128; c < 160; c++) f->simple[c] = dan_pwin[c - 128] ? dan_pwin[c - 128] : (uint32_t)c;
        dan_po *enc = dan_pkey(p, font, "Encoding");
        dan_po *diff = enc->t == PO_DICT ? dan_pkey(p, enc, "Differences") : &dan_po_null;
        if (diff->t == PO_ARR) {
            int code = 0;
            for (size_t i = 0; i < diff->cnt; i++) {
                dan_po *x = dan_pres(p, diff->v[i]);
                if (x->t == PO_NUM) code = (int)x->num;
                else if (x->t == PO_NAME && code >= 0 && code < 256) {
                    uint32_t u = dan_pglyph_unicode((const char *)x->s);
                    if (u) f->simple[code] = u;
                    code++;
                }
            }
        }
    }
}

static int dan_pfont_map(const dan_pfont *f, uint32_t code, uint32_t *out) {
    for (int i = f->nm; i-- > 0;) {
        const dan_pmap *m = &f->m[i];
        if (code < m->lo || code > m->hi) continue;
        if (m->lo == m->hi || m->nuni != 1 || !m->uni) {
            int n = 0;
            for (int k = 0; k < m->nuni && k < 8; k++) out[n++] = m->uni[k];
            if (m->lo != m->hi && n) out[n - 1] = m->base + (code - m->lo);
            return n;
        }
        out[0] = m->base + (code - m->lo);
        return 1;
    }
    if (f->has_simple && code < 256) { out[0] = f->simple[code]; return 1; }
    return 0;
}

/* ---------- text of pages ---------- */

typedef struct { char *s; size_t len, cap; } dan_ptext;

static void dan_ptext_add(dan_ptext *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) { t->cap = (t->len + n + 1) * 2; t->s = dan_realloc(t->s, t->cap); }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void dan_ptext_utf8(dan_ptext *t, uint32_t c) {
    char b[4];
    size_t n;
    if (c == 0 || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF) return;
    if (c < 0x20 && c != '\t' && c != '\n') c = ' ';
    if (c == 0xA0) c = ' ';
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); n = 4; }
    dan_ptext_add(t, b, n);
}

static int dan_ptext_ends_space(const dan_ptext *t) {
    return t->len == 0 || t->s[t->len - 1] == ' ' || t->s[t->len - 1] == '\n';
}

typedef struct { dan_po *name; dan_pfont f; } dan_pfent;

typedef struct {
    dan_pdf *p;
    dan_ptext *out;
    dan_pfent *fonts; int nfonts, capfonts;
    dan_pfont *cur;
    double size, tm[6], lm[6], leading;
    double lasty, lastx;
    int started;
    int forms;                          /* forms drawn in the document: a damaged file must not draw them endlessly */
    dan_po *active[10];                 /* forms being drawn now: a form must not draw itself */
} dan_pctx;

static dan_pfont *dan_pctx_font(dan_pctx *c, dan_po *res, const char *name) {
    dan_po *fd = dan_pkey(c->p, res, "Font");
    dan_po *font = dan_pkey(c->p, fd, name);
    for (int i = 0; i < c->nfonts; i++) if (c->fonts[i].name == font) return &c->fonts[i].f;
    if (c->nfonts == c->capfonts) {
        c->capfonts = c->capfonts ? c->capfonts * 2 : 16;
        c->fonts = dan_realloc(c->fonts, sizeof(dan_pfent) * (size_t)c->capfonts);
    }
    dan_pfent *e = &c->fonts[c->nfonts++];
    e->name = font;
    dan_pfont_load(c->p, &e->f, font);
    return &e->f;
}

/* new line or space between pieces of text, by their position */
static void dan_pctx_move(dan_pctx *c) {
    double y = c->tm[5], x = c->tm[4];
    if (c->started) {
        double h = c->size * (fabs(c->tm[3]) > 0 ? fabs(c->tm[3]) : 1);
        if (h <= 0) h = 10;
        double w = c->size * (fabs(c->tm[0]) > 0 ? fabs(c->tm[0]) : 1);
        if (w <= 0) w = 10;
        if (fabs(y - c->lasty) > h * 0.5) dan_ptext_add(c->out, "\n", 1);
        else if ((x > c->lastx + w * 0.15 || x < c->lastx - w * 2) && !dan_ptext_ends_space(c->out)) dan_ptext_add(c->out, " ", 1);
    }
    c->started = 1;
    c->lasty = y;
}

static void dan_pctx_show(dan_pctx *c, const dan_po *s) {
    if (!c->cur || s->t != PO_STR) return;
    dan_pctx_move(c);
    uint32_t u[8];
    size_t step = c->cur->two ? 2 : 1;
    double adv = 0;
    for (size_t i = 0; i + step <= s->n; i += step) {
        uint32_t code = step == 2 ? ((uint32_t)s->s[i] << 8 | s->s[i + 1]) : s->s[i];
        int n = dan_pfont_map(c->cur, code, u);
        for (int k = 0; k < n; k++) dan_ptext_utf8(c->out, u[k]);
        adv += dan_pfont_width(c->cur, code) / 1000.0;
    }
    c->tm[4] += adv * c->size * c->tm[0];
    c->lastx = c->tm[4];
}

static void dan_pmat_mul(double r[6], const double a[6], const double b[6]) {
    double t[6] = { a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
                    a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5] };
    memcpy(r, t, sizeof t);
}

static void dan_pcontent(dan_pctx *c, dan_po *res, const uint8_t *d, size_t len, int depth) {
    if (depth > 8) return;
    dan_plex l = { d, 0, len };
    dan_po *st[64];
    int n = 0;
    for (;;) {
        dan_pskip(&l);
        if (l.pos >= l.end) break;
        /* inline images: skip their data up to EI */
        if (l.pos + 2 <= l.end && l.s[l.pos] == 'B' && l.s[l.pos + 1] == 'I' && (l.pos + 2 == l.end || dan_pws(l.s[l.pos + 2]))) {
            size_t q = l.pos + 2;
            while (q + 2 < l.end && !(dan_pws(l.s[q]) && l.s[q + 1] == 'E' && l.s[q + 2] == 'I' && (q + 3 >= l.end || dan_pws(l.s[q + 3])))) q++;
            l.pos = q + 3;
            n = 0;
            continue;
        }
        dan_po *o = dan_pparse(c->p, &l, 0);
        if (!o) break;
        if (o->t != PO_OP) { if (n < 64) st[n++] = o; else n = 0; continue; }
        const char *op = (const char *)o->s;
        #define NUM(i) ((i) >= 0 && n > (i) && st[i]->t == PO_NUM ? st[i]->num : 0)
        if (!strcmp(op, "BT")) {
            double id[6] = { 1, 0, 0, 1, 0, 0 };
            memcpy(c->tm, id, sizeof id); memcpy(c->lm, id, sizeof id);
        } else if (!strcmp(op, "Tf") && n >= 2 && st[n - 2]->t == PO_NAME) {
            c->cur = dan_pctx_font(c, res, (const char *)st[n - 2]->s);
            c->size = NUM(n - 1);
        } else if (!strcmp(op, "Td") || !strcmp(op, "TD")) {
            double t[6] = { 1, 0, 0, 1, NUM(n - 2), NUM(n - 1) };
            if (n >= 2) { dan_pmat_mul(c->lm, t, c->lm); memcpy(c->tm, c->lm, sizeof c->lm); }
            if (!strcmp(op, "TD")) c->leading = -NUM(n - 1);
        } else if (!strcmp(op, "Tm") && n >= 6) {
            for (int i = 0; i < 6; i++) c->lm[i] = NUM(n - 6 + i);
            memcpy(c->tm, c->lm, sizeof c->lm);
        } else if (!strcmp(op, "TL")) {
            c->leading = NUM(n - 1);
        } else if (!strcmp(op, "T*") || !strcmp(op, "'") || !strcmp(op, "\"")) {
            double t[6] = { 1, 0, 0, 1, 0, -(c->leading ? c->leading : c->size) };
            dan_pmat_mul(c->lm, t, c->lm);
            memcpy(c->tm, c->lm, sizeof c->lm);
            if (strcmp(op, "T*") && n >= 1) dan_pctx_show(c, st[n - 1]);
        } else if (!strcmp(op, "Tj") && n >= 1) {
            dan_pctx_show(c, st[n - 1]);
        } else if (!strcmp(op, "TJ") && n >= 1 && st[n - 1]->t == PO_ARR) {
            dan_po *a = st[n - 1];
            for (size_t i = 0; i < a->cnt; i++) {
                if (a->v[i]->t == PO_STR) dan_pctx_show(c, a->v[i]);
                else if (a->v[i]->t == PO_NUM) {
                    double k = a->v[i]->num;
                    if (k < -180 && !dan_ptext_ends_space(c->out)) dan_ptext_add(c->out, " ", 1);
                    c->tm[4] -= k / 1000.0 * c->size * c->tm[0];
                    c->lastx = c->tm[4];
                }
            }
        } else if (!strcmp(op, "Do") && n >= 1 && st[n - 1]->t == PO_NAME) {
            dan_po *xo = dan_pkey(c->p, dan_pkey(c->p, res, "XObject"), (const char *)st[n - 1]->s);
            dan_po *sub = dan_pkey(c->p, xo, "Subtype");
            int again = 0;
            for (int i = 0; i < depth && i < 10; i++) again |= c->active[i] == xo;
            if (xo->is_stream && sub->t == PO_NAME && !strcmp((const char *)sub->s, "Form") && !again && depth < 9
                && c->forms < 10000) {
                c->forms++;
                c->active[depth] = xo;
                dan_po *r2 = dan_pkey(c->p, xo, "Resources");
                size_t fl;
                uint8_t *fd = dan_pstream(c->p, xo, &fl);
                dan_pcontent(c, r2->t == PO_DICT ? r2 : res, fd, fl, depth + 1);
                c->active[depth] = NULL;
            }
        }
        #undef NUM
        n = 0;
    }
}

/* Text of every page of a PDF document */
static dr_Ls dan_pdf_text(dan_bytes data, int line) {
    dan_pdf p;
    const char *err = dan_popen(&p, data->data, (size_t)data->len);
    if (err) { dan_pdf_free(&p); return dr_Ls_err(dan_strdup(err), line); }
    if (p.encrypted) {
        dan_pdf_free(&p);
        return dr_Ls_err(dan_strdup(DAN_TXT_PDF_ENCRYPTED), line);
    }
    dl_Ls *pages = dl_Ls_from(0, NULL);
    /* pages are walked without recursion; resources are inherited from parents */
    dan_po *root = dan_pkey(&p, p.trailer, "Root");
    dan_po *stack[64], *rstack[64];
    size_t pos[64];
    int top = 0;
    stack[0] = dan_pkey(&p, root, "Pages");
    rstack[0] = dan_pkey(&p, stack[0], "Resources");
    pos[0] = 0;
    dan_pctx c;
    memset(&c, 0, sizeof c);
    c.p = &p;
    int visited = 0;
    long steps = 0;                     /* nodes of the tree passed: a damaged tree must not be walked endlessly */
    while (top >= 0 && visited < 100000 && steps++ < 1000000) {
        dan_po *node = stack[top];
        dan_po *kids = dan_pkey(&p, node, "Kids");
        dan_po *type = dan_pkey(&p, node, "Type");
        int is_page = (type->t == PO_NAME && !strcmp((const char *)type->s, "Page")) || kids->t != PO_ARR;
        if (is_page) {
            visited++;
            dan_ptext t = { NULL, 0, 0 };
            dan_ptext_add(&t, "", 0);
            c.out = &t;
            c.started = 0;
            c.cur = NULL;
            c.size = 10;
            c.leading = 0;
            dan_po *res = dan_pkey(&p, node, "Resources");
            if (res->t != PO_DICT) res = rstack[top];
            dan_po *cont = dan_pkey(&p, node, "Contents");
            if (cont->is_stream) {
                size_t len;
                uint8_t *d = dan_pstream(&p, cont, &len);
                dan_pcontent(&c, res, d, len, 0);
            } else if (cont->t == PO_ARR) {
                /* parts of one content stream: joined with a space between them */
                dan_ptext all = { NULL, 0, 0 };
                for (size_t i = 0; i < cont->cnt; i++) {
                    size_t len;
                    uint8_t *d = dan_pstream(&p, dan_pres(&p, cont->v[i]), &len);
                    dan_ptext_add(&all, (const char *)d, len);
                    dan_ptext_add(&all, " ", 1);
                }
                if (all.s) dan_pcontent(&c, res, (const uint8_t *)all.s, all.len, 0);
                free(all.s);
            }
            /* trailing spaces of lines are not needed */
            size_t w = 0;
            for (size_t i = 0; i < t.len; i++) {
                if (t.s[i] == '\n') while (w > 0 && t.s[w - 1] == ' ') w--;
                t.s[w++] = t.s[i];
            }
            while (w > 0 && (t.s[w - 1] == ' ' || t.s[w - 1] == '\n')) w--;
            char *s = dan_str_new(w);
            memcpy(s, t.s, w);
            s[w] = 0;
            free(t.s);
            dl_Ls_push(pages, s);
            top--;
            continue;
        }
        if (pos[top] >= kids->cnt || top == 63) { top--; continue; }
        dan_po *kid = dan_pres(&p, kids->v[pos[top]++]);
        if (kid->t != PO_DICT) continue;
        int cycle = 0;
        for (int i = 0; i <= top; i++) cycle |= stack[i] == kid;
        if (cycle) continue;
        dan_po *kr = dan_pkey(&p, kid, "Resources");
        rstack[top + 1] = kr->t == PO_DICT ? kr : rstack[top];
        stack[++top] = kid;
        pos[top] = 0;
    }
    for (int i = 0; i < c.nfonts; i++) { free(c.fonts[i].f.m); free(c.fonts[i].f.cw); }
    free(c.fonts);
    dan_pdf_free(&p);
    return dr_Ls_some(pages);
}

/* ==== creation of PDF documents ====
   Fonts are TrueType files of the system (Times New Roman, Arial, Liberation,
   DejaVu): only the glyphs used are kept (the others are emptied), the font is
   embedded as CIDFontType2 with Identity-H coding, so any letters work and the
   text can be copied and searched (ToUnicode). */

typedef struct {
    char *path;
    uint8_t *d; size_t n;               /* the font file (the chosen font of a collection: offset) */
    size_t base;
    struct { char tag[5]; uint32_t off, len; } t[64]; int nt;
    int units, numglyphs, nhm, loclong;
    int16_t bbox[4], ascent, descent, capheight;
    int italic;
    char psname[64];
    uint32_t cmap_off; int cmap_fmt;
    uint8_t *used;                      /* glyphs used in the document */
    uint32_t *uni;                      /* unicode of a used glyph (for ToUnicode) */
} dan_ttf;

static dan_ttf *dan_ttfs[32];
static int dan_nttf;

static uint32_t dan_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t dan_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static const uint8_t *dan_ttf_table(const dan_ttf *f, const char *tag, uint32_t *len) {
    for (int i = 0; i < f->nt; i++)
        if (!memcmp(f->t[i].tag, tag, 4)) {
            if ((size_t)f->t[i].off + f->t[i].len > f->n) return NULL;
            if (len) *len = f->t[i].len;
            return f->d + f->t[i].off;
        }
    return NULL;
}

static uint16_t dan_ttf_gid(const dan_ttf *f, uint32_t c) {
    const uint8_t *p = f->d + f->cmap_off;
    if (f->cmap_fmt == 4) {
        if (c > 0xFFFF || (size_t)f->cmap_off + 14 > f->n) return 0;
        int segx2 = dan_be16(p + 6);
        if ((size_t)f->cmap_off + 16 + 4 * (size_t)segx2 > f->n) return 0;     /* a damaged font */
        const uint8_t *ends = p + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ranges = deltas + segx2;
        for (int i = 0; i < segx2 / 2; i++) {
            uint16_t end = dan_be16(ends + 2 * i);
            if (c > end) continue;
            uint16_t start = dan_be16(starts + 2 * i);
            if (c < start) return 0;
            uint16_t delta = dan_be16(deltas + 2 * i), ro = dan_be16(ranges + 2 * i);
            if (!ro) return (uint16_t)(c + delta);
            const uint8_t *g = ranges + 2 * i + ro + 2 * (c - start);
            if (g + 2 > f->d + f->n) return 0;
            uint16_t gid = dan_be16(g);
            return gid ? (uint16_t)(gid + delta) : 0;
        }
        return 0;
    }
    if (f->cmap_fmt == 12) {
        if ((size_t)f->cmap_off + 16 > f->n) return 0;
        uint32_t n = dan_be32(p + 12);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *g = p + 16 + 12 * i;
            if (g + 12 > f->d + f->n) break;
            uint32_t s = dan_be32(g), e = dan_be32(g + 4);
            if (c >= s && c <= e) return (uint16_t)(dan_be32(g + 8) + (c - s));
        }
    }
    return 0;
}

static int dan_ttf_advance(const dan_ttf *f, uint16_t gid) {
    uint32_t len;
    const uint8_t *hm = dan_ttf_table(f, "hmtx", &len);
    if (!hm || f->nhm == 0) return f->units / 2;
    int i = gid < f->nhm ? gid : f->nhm - 1;
    if ((uint32_t)(4 * i + 2) > len) return f->units / 2;
    return dan_be16(hm + 4 * i);
}

static void dan_ttf_free(dan_ttf *f) { free(f->d); free(f->path); free(f->used); free(f->uni); free(f); }

/* Font from a file; result: number of the font, or an error */
static dr_i dan_pdf_font(dan_str path, int line) {
    for (int i = 0; i < dan_nttf; i++) if (!strcmp(dan_ttfs[i]->path, path)) return dr_i_some(i);
    if (dan_nttf == 32) return dr_i_err(dan_strdup(DAN_TXT_PDF_FONTS), line);
    FILE *fp = dan_fopen(path, "rb");
    if (!fp) return dr_i_err(dan_concat(dan_concat(DAN_TXT_PDF_NOFONT1, path), DAN_TXT_PDF_NOFONT2), line);
    dan_ttf *f = dan_alloc(sizeof *f);
    memset(f, 0, sizeof *f);
    size_t cap = 1 << 20;
    f->d = dan_alloc(cap);
    size_t r;
    while ((r = fread(f->d + f->n, 1, cap - f->n, fp)) > 0) {
        f->n += r;
        if (f->n == cap) { cap *= 2; f->d = dan_realloc(f->d, cap); }
        if (f->n > ((size_t)1 << 28)) break;
    }
    fclose(fp);
    f->path = (char *)dan_alloc(strlen(path) + 1);
    strcpy(f->path, path);
    const char *why = NULL;
    if (f->n < 12) why = DAN_TXT_PDF_BADFONT;
    else {
        size_t base = 0;
        if (!memcmp(f->d, "ttcf", 4) && f->n >= 16) base = dan_be32(f->d + 12);       /* collection: the first font */
        if (base + 12 > f->n) why = DAN_TXT_PDF_BADFONT;
        else if (!memcmp(f->d + base, "OTTO", 4)) why = DAN_TXT_PDF_OTF;
        else if (dan_be32(f->d + base) != 0x00010000 && memcmp(f->d + base, "true", 4)) why = DAN_TXT_PDF_BADFONT;
        else {
            int nt = dan_be16(f->d + base + 4);
            for (int i = 0; i < nt && f->nt < 64; i++) {
                const uint8_t *e = f->d + base + 12 + 16 * i;
                if (e + 16 > f->d + f->n) break;
                memcpy(f->t[f->nt].tag, e, 4);
                f->t[f->nt].tag[4] = 0;
                f->t[f->nt].off = dan_be32(e + 8);
                f->t[f->nt].len = dan_be32(e + 12);
                f->nt++;
            }
            uint32_t l;
            const uint8_t *head = dan_ttf_table(f, "head", &l), *hhea = dan_ttf_table(f, "hhea", NULL),
                          *maxp = dan_ttf_table(f, "maxp", NULL), *cmap = dan_ttf_table(f, "cmap", &l);
            if (!head || !hhea || !maxp || !cmap || !dan_ttf_table(f, "glyf", NULL) || !dan_ttf_table(f, "loca", NULL) ||
                !dan_ttf_table(f, "hmtx", NULL)) why = DAN_TXT_PDF_BADFONT;
            else {
                f->units = dan_be16(head + 18);
                if (f->units < 16) f->units = 1000;
                for (int k = 0; k < 4; k++) f->bbox[k] = (int16_t)dan_be16(head + 36 + 2 * k);
                f->loclong = dan_be16(head + 50) != 0;
                f->ascent = (int16_t)dan_be16(hhea + 4);
                f->descent = (int16_t)dan_be16(hhea + 6);
                f->nhm = dan_be16(hhea + 34);
                f->numglyphs = dan_be16(maxp + 4);
                f->capheight = (int16_t)(f->ascent * 7 / 10);
                uint32_t ol;
                const uint8_t *os2 = dan_ttf_table(f, "OS/2", &ol);
                if (os2 && ol >= 90 && dan_be16(os2) >= 2) f->capheight = (int16_t)dan_be16(os2 + 88);
                const uint8_t *post = dan_ttf_table(f, "post", NULL);
                if (post) f->italic = (int16_t)dan_be16(post + 4);
                /* character map: Windows Unicode full (3,10) or BMP (3,1), or Unicode (0,x) */
                int best = 0;
                uint32_t cmlen = l;
                for (int i = 0; i < dan_be16(cmap + 2) && 4 + 8 * i + 8 <= (int)cmlen; i++) {
                    int pl = dan_be16(cmap + 4 + 8 * i), en = dan_be16(cmap + 6 + 8 * i);
                    uint32_t off = dan_be32(cmap + 8 + 8 * i);
                    if (off + 4 > cmlen) continue;
                    int fmt = dan_be16(cmap + off);
                    int score = (fmt == 12 && (pl == 3 || pl == 0)) ? 3 : (fmt == 4 && (pl == 3 || pl == 0) && en != 0) ? 2 :
                                (fmt == 4 && pl == 0) ? 1 : 0;
                    if (score > best) { best = score; f->cmap_off = (uint32_t)(cmap - f->d) + off; f->cmap_fmt = fmt; }
                }
                if (!best) why = DAN_TXT_PDF_NOCMAP;
                /* PostScript name of the font */
                uint32_t nl;
                const uint8_t *name = dan_ttf_table(f, "name", &nl);
                strcpy(f->psname, "Font");
                if (name && nl >= 6) {
                    int cnt = dan_be16(name + 2), so = dan_be16(name + 4);
                    for (int i = 0; i < cnt && 6 + 12 * i + 12 <= (int)nl; i++) {
                        const uint8_t *rec = name + 6 + 12 * i;
                        if (dan_be16(rec + 6) != 6) continue;
                        int pl = dan_be16(rec), ln = dan_be16(rec + 8), of = dan_be16(rec + 10);
                        if ((uint32_t)(so + of + ln) > nl) continue;
                        const uint8_t *s = name + so + of;
                        int k = 0;
                        for (int j = 0; j < ln && k < 60; j += (pl == 3 || pl == 0) ? 2 : 1) {
                            int c = (pl == 3 || pl == 0) ? s[j + 1] : s[j];
                            if (c > 32 && c < 127 && c != '[' && c != ']' && c != '(' && c != ')' && c != '/' && c != '<' && c != '>')
                                f->psname[k++] = (char)c;
                        }
                        if (k) { f->psname[k] = 0; break; }
                    }
                }
            }
        }
    }
    if (why) {
        dan_str m = dan_concat(dan_concat(DAN_TXT_PDF_NOFONT1, path), why);
        dan_ttf_free(f);
        return dr_i_err(m, line);
    }
    f->used = dan_alloc((size_t)f->numglyphs + 1);
    memset(f->used, 0, (size_t)f->numglyphs + 1);
    f->uni = dan_alloc(sizeof(uint32_t) * ((size_t)f->numglyphs + 1));
    memset(f->uni, 0, sizeof(uint32_t) * ((size_t)f->numglyphs + 1));
    dan_ttfs[dan_nttf] = f;
    return dr_i_some(dan_nttf++);
}

static uint32_t dan_utf8_next(const unsigned char **s) {
    const unsigned char *p = *s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = p;
    return c;
}

/* Width of a text in points */
static double dan_pdf_width(int64_t font, dan_str text, double size, int line) {
    if (font < 0 || font >= dan_nttf) dan_error(DAN_MSG_PDF_FONTNUM, line);
    dan_ttf *f = dan_ttfs[font];
    double w = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t c = dan_utf8_next(&p);
        w += dan_ttf_advance(f, dan_ttf_gid(f, c));
    }
    return w * size / f->units;
}

/* --- the subset of a font --- */

static void dan_ttf_mark(dan_ttf *f, uint16_t gid, const uint8_t *loca, const uint8_t *glyf, uint32_t glen, int depth) {
    if (gid >= f->numglyphs || depth > 8) return;
    f->used[gid] = 1;
    uint32_t a = f->loclong ? dan_be32(loca + 4 * gid) : 2u * dan_be16(loca + 2 * gid);
    uint32_t b = f->loclong ? dan_be32(loca + 4 * gid + 4) : 2u * dan_be16(loca + 2 * gid + 2);
    if (b <= a || b > glen || b - a < 10) return;
    const uint8_t *g = glyf + a;
    if ((int16_t)dan_be16(g) >= 0) return;
    /* a composite glyph: its parts are needed too */
    const uint8_t *p = g + 10, *e = glyf + b;
    for (;;) {
        if (p + 4 > e) break;
        uint16_t fl = dan_be16(p), comp = dan_be16(p + 2);
        dan_ttf_mark(f, comp, loca, glyf, glen, depth + 1);
        p += 4 + ((fl & 1) ? 4 : 2);
        if (fl & 8) p += 2; else if (fl & 0x40) p += 4; else if (fl & 0x80) p += 8;
        if (!(fl & 0x20)) break;
    }
}

static void dan_bb_u16(dan_bbuf *b, uint32_t v) { uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v }; dan_bbuf_add(b, t, 2); }
static void dan_bb_u32(dan_bbuf *b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; dan_bbuf_add(b, t, 4); }

static uint8_t *dan_ttf_subset(dan_ttf *f, size_t *outlen) {
    uint32_t glen, llen;
    const uint8_t *glyf = dan_ttf_table(f, "glyf", &glen), *loca = dan_ttf_table(f, "loca", &llen);
    size_t need = (size_t)(f->numglyphs + 1) * (f->loclong ? 4 : 2);
    if (llen < need) { f->numglyphs = (int)(llen / (f->loclong ? 4 : 2)) - 1; if (f->numglyphs < 1) f->numglyphs = 1; }
    f->used[0] = 1;
    for (int g = 0; g < f->numglyphs; g++) if (f->used[g]) dan_ttf_mark(f, (uint16_t)g, loca, glyf, glen, 0);
    dan_bbuf ng = { NULL, 0, 0 }, nl = { NULL, 0, 0 };
    for (int g = 0; g < f->numglyphs; g++) {
        dan_bb_u32(&nl, (uint32_t)ng.len);
        if (!f->used[g]) continue;
        uint32_t a = f->loclong ? dan_be32(loca + 4 * g) : 2u * dan_be16(loca + 2 * g);
        uint32_t b = f->loclong ? dan_be32(loca + 4 * g + 4) : 2u * dan_be16(loca + 2 * g + 2);
        if (b > a && b <= glen) {
            dan_bbuf_add(&ng, glyf + a, b - a);
            while (ng.len % 4) dan_bbuf_add(&ng, "", 1);
        }
    }
    dan_bb_u32(&nl, (uint32_t)ng.len);
    if (!ng.d) dan_bbuf_add(&ng, "\0\0\0\0", 4);
    /* tables of the new font, in alphabetical order of tags */
    const char *keep[] = { "cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "prep" };
    const uint8_t *data[9];
    uint32_t lens[9];
    uint8_t *head_copy = NULL;
    int n = 0;
    for (int i = 0; i < 9; i++) {
        uint32_t l = 0;
        const uint8_t *d = NULL;
        if (!strcmp(keep[i], "glyf")) { d = ng.d; l = (uint32_t)ng.len; }
        else if (!strcmp(keep[i], "loca")) { d = nl.d; l = (uint32_t)nl.len; }
        else d = dan_ttf_table(f, keep[i], &l);
        if (!d) continue;
        if (!strcmp(keep[i], "head") && l >= 54) {
            head_copy = dan_alloc(l);
            memcpy(head_copy, d, l);
            memset(head_copy + 8, 0, 4);            /* checkSumAdjustment */
            head_copy[50] = 0; head_copy[51] = 1;   /* long offsets in loca */
            d = head_copy;
        }
        keep[n] = keep[i]; data[n] = d; lens[n] = l; n++;
    }
    dan_bbuf out = { NULL, 0, 0 };
    int es = 0;
    while ((2 << es) <= n) es++;
    dan_bb_u32(&out, 0x00010000);
    dan_bb_u16(&out, (uint32_t)n);
    dan_bb_u16(&out, (uint32_t)(16 << es));
    dan_bb_u16(&out, (uint32_t)es);
    dan_bb_u16(&out, (uint32_t)(n * 16 - (16 << es)));
    uint32_t off = 12 + 16 * (uint32_t)n;
    for (int i = 0; i < n; i++) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < lens[i]; k += 4) {
            uint8_t q[4] = { 0, 0, 0, 0 };
            for (uint32_t j = 0; j < 4 && k + j < lens[i]; j++) q[j] = data[i][k + j];
            sum += dan_be32(q);
        }
        dan_bbuf_add(&out, keep[i], 4);
        dan_bb_u32(&out, sum);
        dan_bb_u32(&out, off);
        dan_bb_u32(&out, lens[i]);
        off += (lens[i] + 3) & ~3u;
    }
    for (int i = 0; i < n; i++) {
        dan_bbuf_add(&out, data[i], lens[i]);
        while (out.len % 4) dan_bbuf_add(&out, "", 1);
    }
    free(ng.d); free(nl.d); free(head_copy);
    *outlen = out.len;
    return out.d;
}

/* --- the document --- */

typedef struct { dan_bbuf b; size_t *off; int nobj, cap; } dan_pw;

static int dan_pw_begin(dan_pw *w) {
    if (w->nobj + 1 >= w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->off = dan_realloc(w->off, sizeof(size_t) * (size_t)w->cap); }
    w->off[++w->nobj] = w->b.len;
    char t[32];
    snprintf(t, sizeof t, "%d 0 obj\n", w->nobj);
    dan_bbuf_add(&w->b, t, strlen(t));
    return w->nobj;
}

static void dan_pw_s(dan_pw *w, const char *s) { dan_bbuf_add(&w->b, s, strlen(s)); }

static void dan_pw_f(dan_pw *w, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    dan_pw_s(w, t);
}

static void dan_pw_stream(dan_pw *w, const char *dict_extra, const uint8_t *d, size_t n, int compress) {
    size_t clen = n;
    uint8_t *c = NULL;
    if (compress && n > 64) {
        size_t raw;
        uint8_t *def = dan_deflate_raw(d, n, &raw);
        /* zlib wrapper: header, data, Adler-32 */
        c = dan_alloc(raw + 6);
        c[0] = 0x78; c[1] = 0x9C;
        memcpy(c + 2, def, raw);
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
        uint32_t ad = b << 16 | a;
        c[raw + 2] = (uint8_t)(ad >> 24); c[raw + 3] = (uint8_t)(ad >> 16); c[raw + 4] = (uint8_t)(ad >> 8); c[raw + 5] = (uint8_t)ad;
        clen = raw + 6;
        free(def);
    }
    dan_pw_f(w, "<< /Length %zu%s%s >>\nstream\n", clen, c ? " /Filter /FlateDecode" : "", dict_extra);
    dan_bbuf_add(&w->b, c ? c : d, clen);
    dan_pw_s(w, "\nendstream\nendobj\n");
    free(c);
}

static void dan_pw_num(char *t, double v) {
    /* a number without exponent and needless zeros */
    snprintf(t, 32, "%.3f", v);
    char *e = t + strlen(t) - 1;
    while (*e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    if (!strcmp(t, "-0")) strcpy(t, "0");
}

/* Commands of a page:
     T \t font \t size \t bold(0/1) \t x \t y \t text
     L \t x1 \t y1 \t x2 \t y2 \t width
     R \t x \t y \t w \t h \t width
   Coordinates are points from the lower left corner of the page. */
static dan_bytes dan_pdf_build(const dl_Ls *pages, double pw, double ph, int line) {
    dan_pw w;
    memset(&w, 0, sizeof w);
    w.cap = 64;
    w.off = dan_alloc(sizeof(size_t) * (size_t)w.cap);
    w.nobj = 2;             /* 1 - catalog, 2 - page tree: written at the end */
    dan_pw_s(&w, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    for (int i = 0; i < dan_nttf; i++) {
        memset(dan_ttfs[i]->used, 0, (size_t)dan_ttfs[i]->numglyphs + 1);
        memset(dan_ttfs[i]->uni, 0, sizeof(uint32_t) * ((size_t)dan_ttfs[i]->numglyphs + 1));
    }
    int fonts_used[32] = { 0 };
    int npages = (int)pages->len;
    int *page_obj = dan_alloc(sizeof(int) * (size_t)(npages + 1));
    /* object numbers: 1 catalog, 2 page tree, then for each page: page and content,
       then the fonts (5 objects each) */
    int first_font = 3 + 2 * npages;
    char t[32], t2[32], t3[32], t4[32];
    dan_bbuf content = { NULL, 0, 0 };
    for (int pg = 0; pg < npages; pg++) {
        content.len = 0;
        const char *s = pages->items[pg];
        while (*s) {
            const char *e = strchr(s, '\n');
            size_t ln = e ? (size_t)(e - s) : strlen(s);
            /* fields */
            const char *f[7];
            size_t fl[7];
            int nf = 0;
            const char *q = s;
            while (nf < 7 && q <= s + ln) {
                const char *tab = nf < 6 ? memchr(q, '\t', (size_t)(s + ln - q)) : NULL;
                f[nf] = q;
                fl[nf] = tab ? (size_t)(tab - q) : (size_t)(s + ln - q);
                nf++;
                if (!tab) break;
                q = tab + 1;
            }
            if (nf >= 7 && f[0][0] == 'T') {
                int font = atoi(f[1]);
                if (font >= 0 && font < dan_nttf) {
                    dan_ttf *ft = dan_ttfs[font];
                    fonts_used[font] = 1;
                    double size = strtod(f[2], NULL);
                    int bold = f[3][0] == '1';
                    dan_pw_num(t, strtod(f[4], NULL));
                    dan_pw_num(t2, strtod(f[5], NULL));
                    dan_pw_num(t3, size);
                    char head[160];
                    if (bold) {
                        dan_pw_num(t4, size * 0.03);
                        snprintf(head, sizeof head, "BT 2 Tr %s w /F%d %s Tf %s %s Td <", t4, font, t3, t, t2);
                    } else snprintf(head, sizeof head, "BT /F%d %s Tf %s %s Td <", font, t3, t, t2);
                    dan_bbuf_add(&content, head, strlen(head));
                    char *txt = dan_alloc(fl[6] + 1);
                    memcpy(txt, f[6], fl[6]);
                    txt[fl[6]] = 0;
                    for (const unsigned char *p = (const unsigned char *)txt; *p;) {
                        uint32_t c = dan_utf8_next(&p);
                        uint16_t gid = dan_ttf_gid(ft, c);
                        if (gid < ft->numglyphs) { ft->used[gid] = 1; if (!ft->uni[gid]) ft->uni[gid] = c; }
                        char hx[8];
                        snprintf(hx, sizeof hx, "%04X", gid);
                        dan_bbuf_add(&content, hx, 4);
                    }
                    free(txt);
                    const char *tail = bold ? "> Tj 0 Tr ET\n" : "> Tj ET\n";
                    dan_bbuf_add(&content, tail, strlen(tail));
                }
            } else if (nf >= 6 && (f[0][0] == 'L' || f[0][0] == 'R')) {
                double a = strtod(f[1], NULL), b = strtod(f[2], NULL), c = strtod(f[3], NULL), d = strtod(f[4], NULL);
                char lw[32];
                dan_pw_num(lw, strtod(f[5], NULL));
                dan_pw_num(t, a); dan_pw_num(t2, b); dan_pw_num(t3, c); dan_pw_num(t4, d);
                char cmd[200];
                if (f[0][0] == 'L') snprintf(cmd, sizeof cmd, "%s w %s %s m %s %s l S\n", lw, t, t2, t3, t4);
                else snprintf(cmd, sizeof cmd, "%s w %s %s %s %s re S\n", lw, t, t2, t3, t4);
                dan_bbuf_add(&content, cmd, strlen(cmd));
            }
            if (!e) break;
            s = e + 1;
        }
        /* page and its content */
        page_obj[pg] = dan_pw_begin(&w);
        dan_pw_num(t, pw); dan_pw_num(t2, ph);
        dan_pw_f(&w, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %s %s] /Contents %d 0 R /Resources << /Font <<", t, t2, page_obj[pg] + 1);
        for (int i = 0; i < dan_nttf; i++) dan_pw_f(&w, " /F%d %d 0 R", i, first_font + 5 * i);
        dan_pw_s(&w, " >> >> >>\nendobj\n");
        dan_pw_begin(&w);
        dan_pw_stream(&w, "", content.d ? content.d : (const uint8_t *)"", content.len, 1);
    }
    free(content.d);
    /* fonts (all loaded fonts get objects, so numbers are known in advance) */
    for (int i = 0; i < dan_nttf; i++) {
        dan_ttf *f = dan_ttfs[i];
        char tag[8];
        /* subset tag: six letters derived from the font number and its use */
        uint32_t h = 2166136261u + (uint32_t)i;
        for (int g = 0; g < f->numglyphs; g++) if (f->used[g]) h = (h ^ (uint32_t)g) * 16777619u;
        for (int k = 0; k < 6; k++) { tag[k] = (char)('A' + h % 26); h /= 26; }
        tag[6] = 0;
        int base = dan_pw_begin(&w);       /* Type0 */
        dan_pw_f(&w, "<< /Type /Font /Subtype /Type0 /BaseFont /%s+%s /Encoding /Identity-H /DescendantFonts [%d 0 R] /ToUnicode %d 0 R >>\nendobj\n",
                 tag, f->psname, base + 1, base + 4);
        dan_pw_begin(&w);                  /* CIDFontType2 */
        double k = 1000.0 / f->units;
        dan_pw_f(&w, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s+%s /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor %d 0 R /CIDToGIDMap /Identity /DW %d /W [",
                 tag, f->psname, base + 2, (int)lround(dan_ttf_advance(f, 0) * k));
        for (int g = 0; g < f->numglyphs; g++)
            if (f->used[g] && fonts_used[i]) dan_pw_f(&w, " %d [%d]", g, (int)lround(dan_ttf_advance(f, (uint16_t)g) * k));
        dan_pw_s(&w, " ] >>\nendobj\n");
        dan_pw_begin(&w);                  /* descriptor */
        dan_pw_f(&w, "<< /Type /FontDescriptor /FontName /%s+%s /Flags %d /FontBBox [%d %d %d %d] /ItalicAngle %d /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>\nendobj\n",
                 tag, f->psname, f->italic ? 96 : 32, (int)lround(f->bbox[0] * k), (int)lround(f->bbox[1] * k),
                 (int)lround(f->bbox[2] * k), (int)lround(f->bbox[3] * k), f->italic, (int)lround(f->ascent * k),
                 (int)lround(f->descent * k), (int)lround(f->capheight * k), base + 3);
        dan_pw_begin(&w);                  /* the font file */
        size_t sl;
        if (!fonts_used[i]) memset(f->used, 0, (size_t)f->numglyphs + 1);
        uint8_t *sub = dan_ttf_subset(f, &sl);
        char extra[48];
        snprintf(extra, sizeof extra, " /Length1 %zu", sl);
        dan_pw_stream(&w, extra, sub, sl, 1);
        free(sub);
        dan_pw_begin(&w);                  /* ToUnicode */
        dan_bbuf cm = { NULL, 0, 0 };
        const char *cmh = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        dan_bbuf_add(&cm, cmh, strlen(cmh));
        int cnt = 0;
        for (int g = 0; g < f->numglyphs; g++) if (f->uni[g]) cnt++;
        int done = 0;
        for (int g = 0; g < f->numglyphs && done < cnt;) {
            int chunk = cnt - done > 100 ? 100 : cnt - done;
            char hd[32];
            snprintf(hd, sizeof hd, "%d beginbfchar\n", chunk);
            dan_bbuf_add(&cm, hd, strlen(hd));
            for (int k2 = 0; k2 < chunk && g < f->numglyphs; g++) {
                if (!f->uni[g]) continue;
                char e[48];
                uint32_t u = f->uni[g];
                if (u >= 0x10000) {
                    uint32_t v = u - 0x10000;
                    snprintf(e, sizeof e, "<%04X> <%04X%04X>\n", g, 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else snprintf(e, sizeof e, "<%04X> <%04X>\n", g, u);
                dan_bbuf_add(&cm, e, strlen(e));
                k2++;
                done++;
            }
            dan_bbuf_add(&cm, "endbfchar\n", 10);
        }
        const char *cmt = "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        dan_bbuf_add(&cm, cmt, strlen(cmt));
        dan_pw_stream(&w, "", cm.d, cm.len, 1);
        free(cm.d);
    }
    /* catalog and page tree */
    w.off[1] = w.b.len;
    dan_pw_s(&w, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    w.off[2] = w.b.len;
    dan_pw_s(&w, "2 0 obj\n<< /Type /Pages /Kids [");
    for (int pg = 0; pg < npages; pg++) dan_pw_f(&w, "%d 0 R ", page_obj[pg]);
    dan_pw_f(&w, "] /Count %d >>\nendobj\n", npages);
    size_t xref = w.b.len;
    int total = w.nobj;
    dan_pw_f(&w, "xref\n0 %d\n0000000000 65535 f \n", total + 1);
    for (int i = 1; i <= total; i++) {
        char e[24];
        snprintf(e, sizeof e, "%010zu 00000 n \n", w.off[i]);
        dan_bbuf_add(&w.b, e, 20);
    }
    dan_pw_f(&w, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", total + 1, xref);
    free(w.off);
    free(page_obj);
    (void)line;
    return dan_bytes_wrap(w.b.d, (int64_t)w.b.len);
}
