"""Генератор кода языка Дан (версия 0.4).

Переводит проверенное дерево программы в текст на языке C. Полученный
файл самодостаточен: в его начало встраивается небольшая среда
выполнения Дан (вывод значений, строки, списки, ввод, защита от деления
на ноль и выхода за границы списка). Затем файл компилируется обычным
компилятором C (gcc или clang) в исполняемую программу.

Соответствие типов:
    цел    -> int64_t
    дроб   -> double
    лог    -> bool
    стр    -> const char*   (строка в кодировке UTF-8)
    рац    -> dan_rat: несократимая дробь {числитель, знаменатель > 0}
    [тип]  -> указатель на структуру списка; для каждого типа списка,
              встречающегося в программе, создаётся своя структура
              и свои функции (dl_Li для [цел], dl_LLi для [[цел]] и т.д.)

Все имена в получаемом C-коде латинские: имена из программы на Дан
транслитерируются (сумма -> d_summa), а русские тексты записываются
восьмеричными кодами байтов. Поэтому код собирается любым компилятором
C независимо от его настроек кодировки.

Инструкции верхнего уровня помещаются в функцию main, поэтому функции
программы их не видят, как и требует спецификация.
"""

import uzly as у
import matematika
from proverka import ЦЕЛ, ДРОБ, СТР, ЛОГ, РАЦ, ПУСТО, это_список, элемент

БАЗОВЫЕ_C = {ЦЕЛ: "int64_t", ДРОБ: "double", СТР: "dan_str", ЛОГ: "bool", РАЦ: "dan_rat",
             ПУСТО: "void"}
КОДЫ_ТИПОВ = {ЦЕЛ: "i", ДРОБ: "f", СТР: "s", ЛОГ: "b", РАЦ: "r"}
ЗАПИСЬ = {ЦЕЛ: "dan_write_int", ДРОБ: "dan_write_float", СТР: "dan_write_str", ЛОГ: "dan_write_bool",
          РАЦ: "dan_write_rat"}
ОПЕРАЦИИ_РАЦ = {"+": "dan_rat_add", "-": "dan_rat_sub", "*": "dan_rat_mul", "/": "dan_rat_div"}

СООБЩЕНИЯ = {
    "DAN_MSG_RUNTIME": "Ошибка выполнения в строке %d: %s\n",
    "DAN_MSG_DIV_ZERO": "деление на ноль",
    "DAN_MSG_MOD_ZERO": "остаток от деления на ноль",
    "DAN_MSG_NO_MEMORY": "недостаточно памяти",
    "DAN_MSG_TOO_BIG": "результат слишком велик для типа цел",
    "DAN_MSG_INDEX": "индекс %lld за пределами списка длиной %lld",
    "DAN_MSG_NOT_INT": "строку \"%s\" нельзя преобразовать в целое число",
    "DAN_MSG_INT_RANGE": "число \"%s\" слишком велико для типа цел",
    "DAN_MSG_NOT_FLOAT": "строку \"%s\" нельзя преобразовать в число",
    "DAN_MSG_FLOAT_RANGE": "дробное число слишком велико для типа цел или не является числом",
    "DAN_MSG_RAT_BIG": "числитель или знаменатель дроби слишком велик",
    "DAN_MSG_NOT_RAT": "строку \"%s\" нельзя преобразовать в дробь",
    "DAN_MSG_NOT_FINITE": "бесконечность или не-число нельзя преобразовать в дробь",
    "DAN_MSG_NEG_POW": "целое число в отрицательной степени не является целым; "
                       "используйте рац(а) ^ н или дробное число",
    "DAN_TRUE": "истина",
    "DAN_FALSE": "ложь",
}

СРЕДА = r"""
typedef const char *dan_str;

static void dan_error(const char *text, int line) {
    fflush(stdout);
    fprintf(stderr, DAN_MSG_RUNTIME, line, text);
    exit(1);
}

static void *dan_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) dan_error(DAN_MSG_NO_MEMORY, 0);
    return p;
}

static void *dan_realloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) dan_error(DAN_MSG_NO_MEMORY, 0);
    return p;
}

static void dan_index_error(int64_t i, int64_t len, int line) {
    char buf[200];
    snprintf(buf, sizeof buf, DAN_MSG_INDEX, (long long)i, (long long)len);
    dan_error(buf, line);
}

static void dan_conv_error(const char *fmt, dan_str s, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, fmt, s);
    dan_error(buf, line);
}

/* ---- arithmetic ---- */

/* Division with remainder as defined in school mathematics:
   a = b * q + r, where 0 <= r < |b|. The remainder is never negative,
   so -7 : 2 = -4 (remainder 1), and -7 % 3 == 2. */
static int64_t dan_div(int64_t a, int64_t b, int line) {
    if (b == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    if (a == INT64_MIN && b == -1) dan_error(DAN_MSG_TOO_BIG, line);
    int64_t q = a / b, r = a % b;
    if (r < 0) q += b > 0 ? -1 : 1;
    return q;
}

static int64_t dan_mod(int64_t a, int64_t b, int line) {
    if (b == 0) dan_error(DAN_MSG_MOD_ZERO, line);
    if (b == -1 || b == 1) return 0;
    int64_t r = a % b;
    if (r < 0) r += b > 0 ? b : -b;
    return r;
}

/* ---- strings ---- */

static dan_str dan_strdup(const char *s) {
    size_t n = strlen(s);
    char *r = dan_alloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static dan_str dan_concat(dan_str a, dan_str b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = dan_alloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

static bool dan_str_eq(dan_str a, dan_str b) { return strcmp(a, b) == 0; }

/* Number of characters (not bytes) in a UTF-8 string. */
static int64_t dan_str_len(dan_str s) {
    int64_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

/* ---- output ---- */

static void dan_format_float(char *buf, size_t size, double v) {
    /* Shortest text that reads back as the same number; the integer
       part is never shortened to exponent form (10.0, not 1e+01). */
    double mag = v < 0 ? -v : v;
    int start = 1;
    while (mag >= 10.0 && start < 17) { mag /= 10.0; start++; }
    for (int prec = start; prec <= 17; prec++) {
        snprintf(buf, size, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
}

static void dan_write_int(int64_t v) { printf("%" PRId64, v); }
static void dan_write_float(double v) { char b[64]; dan_format_float(b, sizeof b, v); fputs(b, stdout); }
static void dan_write_bool(bool v) { fputs(v ? DAN_TRUE : DAN_FALSE, stdout); }
static void dan_write_str(dan_str v) { fputs(v, stdout); }
static void dan_write_quoted(dan_str v) { putchar('"'); fputs(v, stdout); putchar('"'); }
static void dan_newline(void) { putchar('\n'); }

/* ---- conversions ---- */

static dan_str dan_int_to_str(int64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%" PRId64, v);
    return dan_strdup(b);
}

static dan_str dan_float_to_str(double v) {
    char b[64];
    dan_format_float(b, sizeof b, v);
    return dan_strdup(b);
}

static dan_str dan_bool_to_str(bool v) { return v ? DAN_TRUE : DAN_FALSE; }

static int64_t dan_str_to_int(dan_str s, int line) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    errno = 0;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) dan_conv_error(DAN_MSG_NOT_INT, s, line);
    while (*end == ' ' || *end == '\t') end++;
    if (*end) dan_conv_error(DAN_MSG_NOT_INT, s, line);
    if (errno == ERANGE) dan_conv_error(DAN_MSG_INT_RANGE, s, line);
    return (int64_t)v;
}

static double dan_str_to_float(dan_str s, int line) {
    /* A comma is accepted as the decimal separator: "3,5" == 3.5 */
    size_t n = strlen(s);
    char *copy = dan_alloc(n + 1);
    for (size_t i = 0; i <= n; i++) copy[i] = s[i] == ',' ? '.' : s[i];
    char *p = copy;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    double v = strtod(p, &end);
    bool bad = end == p;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) bad = true;
    free(copy);
    if (bad) dan_conv_error(DAN_MSG_NOT_FLOAT, s, line);
    return v;
}

static int64_t dan_float_to_int(double v, int line) {
    if (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        dan_error(DAN_MSG_FLOAT_RANGE, line);
    return (int64_t)v;
}

/* ---- input ---- */

static dan_str dan_input(dan_str prompt) {
    if (prompt) fputs(prompt, stdout);
    fflush(stdout);
#ifdef _WIN32
    /* In a Windows console read UTF-16 and convert, so that Cyrillic
       input works regardless of the console code page. */
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        size_t cap = 256, len = 0;
        wchar_t *w = dan_alloc(cap * sizeof(wchar_t));
        for (;;) {
            DWORD got = 0;
            if (len + 1 >= cap) { cap *= 2; w = dan_realloc(w, cap * sizeof(wchar_t)); }
            if (!ReadConsoleW(h, w + len, 1, &got, NULL) || got == 0) break;
            if (w[len] == L'\n') break;
            if (w[len] != L'\r') len++;
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, NULL, 0, NULL, NULL);
        char *r = dan_alloc((size_t)n + 1);
        WideCharToMultiByte(CP_UTF8, 0, w, (int)len, r, n, NULL, NULL);
        r[n] = 0;
        free(w);
        return r;
    }
#endif
    size_t cap = 256, len = 0;
    char *r = dan_alloc(cap);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; r = dan_realloc(r, cap); }
        r[len++] = (char)c;
    }
    if (len && r[len - 1] == '\r') len--;
    r[len] = 0;
    return r;
}

/* ---- integer power ---- */

static int64_t dan_pow_int(int64_t a, int64_t e, int line) {
    if (e < 0) {
        if (a == 1) return 1;
        if (a == -1) return (e % 2 == 0) ? 1 : -1;
        if (a == 0) dan_error(DAN_MSG_DIV_ZERO, line);
        dan_error(DAN_MSG_NEG_POW, line);
    }
    int64_t r = 1;
    while (e > 0) {
        if (e & 1) { if (__builtin_mul_overflow(r, a, &r)) dan_error(DAN_MSG_TOO_BIG, line); }
        e >>= 1;
        if (e > 0 && __builtin_mul_overflow(a, a, &a)) dan_error(DAN_MSG_TOO_BIG, line);
    }
    return r;
}

/* ---- rational numbers (exact fractions) ----
   Always stored reduced, with a positive denominator. Intermediate
   products use 128-bit integers, so overflow is detected, never silent. */

typedef struct { int64_t n, d; } dan_rat;
typedef __int128 dan_i128;
typedef unsigned __int128 dan_u128;

static dan_u128 dan_gcd128(dan_u128 a, dan_u128 b) {
    while (b) { dan_u128 t = a % b; a = b; b = t; }
    return a;
}

static dan_rat dan_rat_make(dan_i128 n, dan_i128 d, int line) {
    if (d == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    if (d < 0) { n = -n; d = -d; }
    dan_u128 g = dan_gcd128(n < 0 ? (dan_u128)(-n) : (dan_u128)n, (dan_u128)d);
    if (g > 1) { n /= (dan_i128)g; d /= (dan_i128)g; }
    if (n > INT64_MAX || n < -INT64_MAX || d > INT64_MAX) dan_error(DAN_MSG_RAT_BIG, line);
    dan_rat r = { (int64_t)n, (int64_t)d };
    return r;
}

static dan_rat dan_rat_from_int(int64_t x, int line) { return dan_rat_make(x, 1, line); }
static dan_rat dan_rat_of(int64_t n, int64_t d, int line) { return dan_rat_make(n, d, line); }

static dan_rat dan_rat_add(dan_rat a, dan_rat b, int line) {
    return dan_rat_make((dan_i128)a.n * b.d + (dan_i128)b.n * a.d, (dan_i128)a.d * b.d, line);
}
static dan_rat dan_rat_sub(dan_rat a, dan_rat b, int line) {
    return dan_rat_make((dan_i128)a.n * b.d - (dan_i128)b.n * a.d, (dan_i128)a.d * b.d, line);
}
static dan_rat dan_rat_mul(dan_rat a, dan_rat b, int line) {
    return dan_rat_make((dan_i128)a.n * b.n, (dan_i128)a.d * b.d, line);
}
static dan_rat dan_rat_div(dan_rat a, dan_rat b, int line) {
    if (b.n == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    return dan_rat_make((dan_i128)a.n * b.d, (dan_i128)a.d * b.n, line);
}
static dan_rat dan_rat_neg(dan_rat a, int line) { return dan_rat_make(-(dan_i128)a.n, a.d, line); }

static int dan_rat_cmp(dan_rat a, dan_rat b) {
    dan_i128 l = (dan_i128)a.n * b.d, r = (dan_i128)b.n * a.d;
    return (l > r) - (l < r);
}

static double dan_rat_to_float(dan_rat a) { return (double)a.n / (double)a.d; }
static int64_t dan_rat_to_int(dan_rat a) { return a.n / a.d; }

static dan_rat dan_rat_pow(dan_rat a, int64_t e, int line) {
    if (e < 0) {
        if (a.n == 0) dan_error(DAN_MSG_DIV_ZERO, line);
        a = dan_rat_make(a.d, a.n, line);
    }
    uint64_t k = e < 0 ? (uint64_t)0 - (uint64_t)e : (uint64_t)e;
    dan_rat r = { 1, 1 };
    while (k > 0) {
        if (k & 1) r = dan_rat_mul(r, a, line);
        k >>= 1;
        if (k > 0) a = dan_rat_mul(a, a, line);
    }
    return r;
}

static void dan_write_rat(dan_rat v) {
    if (v.d == 1) printf("%" PRId64, v.n);
    else printf("%" PRId64 "/%" PRId64, v.n, v.d);
}

static dan_str dan_rat_to_str(dan_rat v) {
    char b[48];
    if (v.d == 1) snprintf(b, sizeof b, "%" PRId64, v.n);
    else snprintf(b, sizeof b, "%" PRId64 "/%" PRId64, v.n, v.d);
    return dan_strdup(b);
}

/* Reads digits into *acc; returns the number of digits read. */
static int dan_read_digits(const char **p, dan_i128 *acc, int *too_big) {
    int count = 0;
    while (**p >= '0' && **p <= '9') {
        if (*acc > ((dan_i128)1 << 100)) *too_big = 1;
        else *acc = *acc * 10 + (**p - '0');
        (*p)++;
        count++;
    }
    return count;
}

/* "3/4", "-3/4", "2 1/3" (mixed number), "0,75" or "0.75", "5" */
static dan_rat dan_rat_from_str(dan_str s, int line) {
    const char *p = s;
    int big = 0, neg = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    dan_i128 whole = 0, num = 0, den = 1;
    if (dan_read_digits(&p, &whole, &big) == 0) dan_conv_error(DAN_MSG_NOT_RAT, s, line);
    if (*p == '/') {
        p++;
        num = whole; whole = 0; den = 0;
        if (dan_read_digits(&p, &den, &big) == 0) dan_conv_error(DAN_MSG_NOT_RAT, s, line);
    } else if (*p == '.' || *p == ',') {
        p++;
        int k = dan_read_digits(&p, &num, &big);
        if (k == 0) dan_conv_error(DAN_MSG_NOT_RAT, s, line);
        if (k > 18) big = 1;
        for (int i = 0; i < k && !big; i++) den *= 10;
    } else if (*p == ' ') {
        const char *q = p;
        while (*q == ' ') q++;
        if (*q >= '0' && *q <= '9') {               /* mixed number: 2 1/3 */
            p = q;
            dan_read_digits(&p, &num, &big);
            if (*p != '/') dan_conv_error(DAN_MSG_NOT_RAT, s, line);
            p++;
            den = 0;
            if (dan_read_digits(&p, &den, &big) == 0) dan_conv_error(DAN_MSG_NOT_RAT, s, line);
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) dan_conv_error(DAN_MSG_NOT_RAT, s, line);
    if (big || whole > INT64_MAX || den > INT64_MAX) dan_error(DAN_MSG_RAT_BIG, line);
    if (den == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    dan_i128 n = whole * den + num;
    return dan_rat_make(neg ? -n : n, den, line);
}

/* Exact fraction from the shortest decimal form: 0.1 -> 1/10, 2.5 -> 5/2 */
static dan_rat dan_rat_from_float(double v, int line) {
    if (v != v || v - v != 0) dan_error(DAN_MSG_NOT_FINITE, line);
    char buf[64];
    dan_format_float(buf, sizeof buf, v);
    const char *p = buf;
    int neg = 0, frac = 0, big = 0;
    if (*p == '-') { neg = 1; p++; }
    dan_i128 m = 0;
    dan_read_digits(&p, &m, &big);
    if (*p == '.') { p++; frac = dan_read_digits(&p, &m, &big); }
    long exp10 = 0;
    if (*p == 'e' || *p == 'E') exp10 = strtol(p + 1, NULL, 10);
    long shift = exp10 - frac;
    dan_i128 n = m, d = 1;
    for (; shift > 0; shift--) { n *= 10; if (n > INT64_MAX) dan_error(DAN_MSG_RAT_BIG, line); }
    for (; shift < 0; shift++) { d *= 10; if (d > INT64_MAX) dan_error(DAN_MSG_RAT_BIG, line); }
    return dan_rat_make(neg ? -n : n, d, line);
}

static void dan_start(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}
"""

ШАБЛОН_СПИСКА = """
/* ---- list {описание} ---- */
typedef struct {{ int64_t len, cap; {T} *items; }} {S};

static {S} *{S}_from(int64_t n, {T} const *src) {{
    {S} *l = dan_alloc(sizeof *l);
    l->len = n;
    l->cap = n < 4 ? 4 : n;
    l->items = dan_alloc((size_t)l->cap * sizeof({T}));
    if (n) memcpy(l->items, src, (size_t)n * sizeof({T}));
    return l;
}}

static void {S}_push({S} *l, {T} v) {{
    if (l->len == l->cap) {{
        l->cap *= 2;
        l->items = dan_realloc(l->items, (size_t)l->cap * sizeof({T}));
    }}
    l->items[l->len++] = v;
}}

static {T} {S}_get(const {S} *l, int64_t i, int line) {{
    if (i < 0 || i >= l->len) dan_index_error(i, l->len, line);
    return l->items[i];
}}

static void {S}_set({S} *l, int64_t i, {T} v, int line) {{
    if (i < 0 || i >= l->len) dan_index_error(i, l->len, line);
    l->items[i] = v;
}}

static void {S}_write(const {S} *l) {{
    putchar('[');
    for (int64_t i = 0; i < l->len; i++) {{
        if (i) fputs(", ", stdout);
        {запись_элемента}(l->items[i]);
    }}
    putchar(']');
}}
"""

# Транслитерация русских букв для имён в C.
ТРАНСЛИТ = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "yo",
    "ж": "zh", "з": "z", "и": "i", "й": "j", "к": "k", "л": "l", "м": "m",
    "н": "n", "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u",
    "ф": "f", "х": "h", "ц": "c", "ч": "ch", "ш": "sh", "щ": "shh",
    "ъ": "jj", "ы": "y", "ь": "q", "э": "eh", "ю": "yu", "я": "ya",
}


def _латиницей(имя):
    части = []
    for с in имя:
        н = с.lower()
        if н in ТРАНСЛИТ:
            т = ТРАНСЛИТ[н]
            части.append(т.capitalize() if с != н else т)
        elif с.isascii() and (с.isalnum() or с == "_"):
            части.append(с)
        else:
            части.append(f"u{ord(с):x}")
    return "".join(части)


def строка_c(текст):
    """Строковый литерал C. Не-ASCII байты записываются восьмеричными кодами."""
    части = ['"']
    for байт in текст.encode("utf-8"):
        с = chr(байт)
        if с == '"':
            части.append('\\"')
        elif с == "\\":
            части.append("\\\\")
        elif с == "\n":
            части.append("\\n")
        elif с == "\t":
            части.append("\\t")
        elif 32 <= байт < 127:
            части.append(с)
        else:
            части.append(f"\\{байт:03o}")
    части.append('"')
    return "".join(части)


def дробь_c(значение):
    т = repr(float(значение))
    if "inf" in т:
        return "(1.0/0.0)" if значение > 0 else "(-1.0/0.0)"
    if "." not in т and "e" not in т:
        т += ".0"
    return т


def код_типа(тип):
    if это_список(тип):
        return "L" + код_типа(элемент(тип))
    return КОДЫ_ТИПОВ[тип]


class Генератор:
    def __init__(self):
        self.имена = {}            # имя в Дан -> имя в C
        self.занятые = set()
        self.строки = []
        self.отступ = 0
        self.счётчик = 0
        self.списки = {}           # тип списка -> имя структуры C (вложенные раньше внешних)
        self.нужна_математика = False

    # ---------- типы ----------

    def тип_c(self, тип):
        if это_список(тип):
            return self.структура(тип) + "*"
        return БАЗОВЫЕ_C[тип]

    def структура(self, тип):
        if тип not in self.списки:
            тип_эл = элемент(тип)
            if это_список(тип_эл):
                self.структура(тип_эл)
            self.списки[тип] = "dl_" + код_типа(тип)
        return self.списки[тип]

    def запись(self, тип, в_списке=False):
        """Функция C, которая печатает значение этого типа без перевода строки."""
        if это_список(тип):
            return self.структура(тип) + "_write"
        if тип == СТР and в_списке:
            return "dan_write_quoted"
        return ЗАПИСЬ[тип]

    # ---------- имена и вывод ----------

    def имя(self, имя_дан):
        if имя_дан not in self.имена:
            основа = "d_" + _латиницей(имя_дан)
            кандидат, номер = основа, 2
            while кандидат in self.занятые:
                кандидат = f"{основа}_{номер}"
                номер += 1
            self.занятые.add(кандидат)
            self.имена[имя_дан] = кандидат
        return self.имена[имя_дан]

    def служебное_имя(self, основа):
        self.счётчик += 1
        return f"_{основа}{self.счётчик}"

    def пиши(self, текст=""):
        self.строки.append("    " * self.отступ + текст if текст else "")

    # ---------- программа ----------

    def программа(self, программа):
        функции = [и for и in программа.инструкции if isinstance(и, у.Функция)]
        прочие = [и for и in программа.инструкции if not isinstance(и, у.Функция)]

        # Сначала генерируется тело программы: при этом выясняется,
        # какие типы списков нужны, и их определения ставятся выше.
        if функции:
            self.пиши("/* ---- program functions ---- */")
            for ф in функции:
                self.пиши(self._заголовок(ф) + ";")
            self.пиши()
        for ф in функции:
            self.функция(ф)
            self.пиши()

        self.пиши("int main(void) {")
        self.отступ += 1
        self.пиши("dan_start();")
        for и in прочие:
            self.инструкция(и)
        self.пиши("return 0;")
        self.отступ -= 1
        self.пиши("}")

        шапка = [
            "/* Создано компилятором языка Дан. Редактировать не нужно. */",
            "#include <stdint.h>", "#include <inttypes.h>", "#include <stdbool.h>",
            "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
            "#include <errno.h>", "#include <math.h>",
            "#ifdef _WIN32", "#include <windows.h>", "#endif", "",
        ]
        шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in СООБЩЕНИЯ.items()]
        if self.нужна_математика:
            шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in matematika.СООБЩЕНИЯ.items()]
            шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in matematika.ИМЕНА_C.items()]
        части = ["\n".join(шапка), СРЕДА]
        for тип, имя in self.списки.items():
            тип_эл = элемент(тип)
            части.append(ШАБЛОН_СПИСКА.format(
                описание=тип, S=имя, T=self.тип_c(тип_эл),
                запись_элемента=self.запись(тип_эл, в_списке=True)))
        if self.нужна_математика:
            части.append(matematika.КОД_C)
            части.append(matematika.код_c_дополнения())
        части.append("\n".join(self.строки))
        return "\n".join(части) + "\n"

    def _заголовок(self, ф):
        результат = self.тип_c(ф.тип_результата) if ф.тип_результата else "void"
        параметры = ", ".join(f"{self.тип_c(п.тип)} const {self.имя(п.имя)}" for п in ф.параметры)
        return f"static {результат} {self.имя(ф.имя)}({параметры or 'void'})"

    def функция(self, ф):
        self.пиши(f"/* line {ф.строка} */")
        self.пиши(self._заголовок(ф) + " {")
        self.отступ += 1
        for и in ф.тело.инструкции:
            self.инструкция(и)
        self.отступ -= 1
        self.пиши("}")

    # ---------- инструкции ----------

    def блок(self, блок):
        self.отступ += 1
        for и in блок.инструкции:
            self.инструкция(и)
        self.отступ -= 1

    def инструкция(self, и):
        getattr(self, "и_" + type(и).__name__)(и)

    def и_ОбъявлениеПеременной(self, и):
        модификатор = "" if и.изменяемая else " const"
        self.пиши(f"{self.тип_c(и.тип_итог)}{модификатор} {self.имя(и.имя)} = {self.выр(и.значение)};")

    def и_Присваивание(self, и):
        цель = self.имя(и.имя)
        значение = self.выр(и.значение)
        if и.оператор == "=":
            self.пиши(f"{цель} = {значение};")
            return
        результат = self._арифметика(и.оператор[0], цель, значение, и.тип_результата, и.строка)
        self.пиши(f"{цель} = {результат};")

    def и_ПрисваиваниеЭлемента(self, и):
        тип_списка = и.объект.тип_выр
        структура = self.структура(тип_списка)
        объект = self.служебное_имя("list")
        индекс = self.служебное_имя("i")
        # Список и индекс вычисляются один раз, даже при «с[ф()] += 1».
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"{структура}* const {объект} = {self.выр(и.объект)};")
        self.пиши(f"const int64_t {индекс} = {self.выр(и.индекс)};")
        значение = self.выр(и.значение)
        if и.оператор != "=":
            текущее = f"{структура}_get({объект}, {индекс}, {и.строка})"
            значение = self._арифметика(и.оператор[0], текущее, значение, и.тип_результата, и.строка)
        self.пиши(f"{структура}_set({объект}, {индекс}, {значение}, {и.строка});")
        self.отступ -= 1
        self.пиши("}")

    def и_Выражение(self, и):
        self.пиши(f"{self.выр(и.выражение)};")

    def и_Если(self, и, продолжение=False):
        начало = "} else if" if продолжение else "if"
        self.пиши(f"{начало} ({self.выр(и.условие)}) {{")
        self.блок(и.тогда)
        if isinstance(и.иначе, у.Если):
            self.и_Если(и.иначе, продолжение=True)
            return
        if и.иначе is not None:
            self.пиши("} else {")
            self.блок(и.иначе)
        self.пиши("}")

    def и_Пока(self, и):
        self.пиши(f"while ({self.выр(и.условие)}) {{")
        self.блок(и.тело)
        self.пиши("}")

    def и_Для(self, и):
        перем = self.имя(и.переменная)
        if и.тип_элемента is None:
            конец = self.служебное_имя("end")
            начало = self.выр(и.источник.начало)
            граница = self.выр(и.источник.конец)
            # Граница вычисляется один раз до начала цикла.
            self.пиши(f"for (int64_t {перем} = {начало}, {конец} = {граница}; "
                      f"{перем} <= {конец}; {перем}++) {{")
            self.блок(и.тело)
            self.пиши("}")
            return
        структура = self.структура(и.источник.тип_выр)
        список = self.служебное_имя("list")
        номер = self.служебное_имя("i")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"{структура}* const {список} = {self.выр(и.источник)};")
        self.пиши(f"for (int64_t {номер} = 0; {номер} < {список}->len; {номер}++) {{")
        self.отступ += 1
        self.пиши(f"{self.тип_c(и.тип_элемента)} const {перем} = {список}->items[{номер}];")
        self.отступ -= 1
        self.блок(и.тело)
        self.пиши("}")
        self.отступ -= 1
        self.пиши("}")

    def и_Вернуть(self, и):
        if и.значение is None:
            self.пиши("return;")
        else:
            self.пиши(f"return {self.выр(и.значение)};")

    def и_Прервать(self, и):
        self.пиши("break;")

    def и_Продолжить(self, и):
        self.пиши("continue;")

    # ---------- выражения ----------

    def выр(self, в):
        код = getattr(self, "в_" + type(в).__name__)(в)
        к_типу = getattr(в, "привести_к", None)
        if к_типу and к_типу != в.тип_выр:
            код = self.привести(код, в.тип_выр, к_типу, в.строка)
        return код

    @staticmethod
    def привести(код, из_типа, в_тип, строка):
        """Числовое преобразование цел -> рац -> дроб."""
        if в_тип == ДРОБ:
            return f"dan_rat_to_float({код})" if из_типа == РАЦ else f"((double)({код}))"
        if в_тип == РАЦ and из_типа == ЦЕЛ:
            return f"dan_rat_from_int({код}, {строка})"
        return код

    def в_ЦелоеЧисло(self, в):
        return f"INT64_C({в.значение})"

    def в_ДробноеЧисло(self, в):
        return дробь_c(в.значение)

    def в_Строка(self, в):
        return строка_c(в.значение)

    def в_Логическое(self, в):
        return "true" if в.значение else "false"

    def в_Имя(self, в):
        if getattr(в, "константа", None):
            return matematika.КОНСТАНТЫ[в.константа][1]
        return self.имя(в.имя)

    def в_Унарная(self, в):
        if в.оператор == "-" and в.тип_выр == РАЦ:
            return f"dan_rat_neg({self.выр(в.операнд)}, {в.строка})"
        оп = "-" if в.оператор == "-" else "!"
        return f"({оп}{self.выр(в.операнд)})"

    def в_Бинарная(self, в):
        л, п = self.выр(в.левый), self.выр(в.правый)
        оп = в.оператор
        if оп in ("+", "-", "*", "/", "%"):
            return self._арифметика(оп, л, п, в.тип_выр, в.строка)
        if оп == "^":
            if в.тип_выр == ЦЕЛ:
                return f"dan_pow_int({л}, {п}, {в.строка})"
            if в.тип_выр == РАЦ:
                return f"dan_rat_pow({л}, {п}, {в.строка})"
            return f"pow({л}, {п})"
        if оп == "и":
            return f"({л} && {п})"
        if оп == "или":
            return f"({л} || {п})"
        if в.левый.тип_выр == СТР:
            равно = f"dan_str_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if getattr(в, "тип_операндов", None) == РАЦ:
            return f"(dan_rat_cmp({л}, {п}) {оп} 0)"
        return f"({л} {оп} {п})"

    def _арифметика(self, оп, л, п, тип, строка):
        """Код «л оп п», где оба операнда уже приведены к типу результата."""
        if тип == СТР:
            return f"dan_concat({л}, {п})"
        if тип == РАЦ:
            return f"{ОПЕРАЦИИ_РАЦ[оп]}({л}, {п}, {строка})"
        if тип == ЦЕЛ:
            if оп == "/":
                return f"dan_div({л}, {п}, {строка})"
            if оп == "%":
                return f"dan_mod({л}, {п}, {строка})"
        return f"({л} {оп} {п})"

    def в_Список(self, в):
        тип = в.тип_выр
        структура = self.структура(тип)
        if not в.элементы:
            return f"{структура}_from(0, NULL)"
        элементы = ", ".join(self.выр(э) for э in в.элементы)
        return f"{структура}_from({len(в.элементы)}, ({self.тип_c(элемент(тип))}[]){{{элементы}}})"

    def в_Индекс(self, в):
        структура = self.структура(в.объект.тип_выр)
        return f"{структура}_get({self.выр(в.объект)}, {self.выр(в.индекс)}, {в.строка})"

    def в_Поле(self, в):
        тип = в.объект.тип_выр
        if тип == РАЦ:
            return f"({self.выр(в.объект)}).{'n' if в.имя == 'числитель' else 'd'}"
        if тип == СТР:
            return f"dan_str_len({self.выр(в.объект)})"
        if not это_список(тип):           # литерал «[]»
            return "INT64_C(0)"
        return f"({self.выр(в.объект)})->len"

    def в_Вызов(self, в):
        if isinstance(в.функция, у.Поле):    # метод: список.добавить(х)
            объект = в.функция.объект
            структура = self.структура(объект.тип_выр)
            return f"{структура}_push({self.выр(объект)}, {self.выр(в.аргументы[0])})"

        имя = в.функция.имя
        аргументы = [self.выр(а) for а in в.аргументы]

        if getattr(в, "библиотека", None):
            каноническое, вариант = в.библиотека
            описание = matematika.ФУНКЦИИ[каноническое]
            self.нужна_математика = True
            for тип_списка in ("[цел]", "[рац]", "[дроб]", "[стр]"):
                self.структура(тип_списка)   # библиотека работает со списками этих типов
            if описание["строка"][вариант]:
                аргументы.append(str(в.строка))
            return f"{описание['c'][вариант]}({', '.join(аргументы)})"

        if имя == "вывод":
            тип = в.аргументы[0].тип_выр
            return f"({self.запись(тип)}({аргументы[0]}), dan_newline())"
        if имя == "ввод":
            return f"dan_input({аргументы[0] if аргументы else 'NULL'})"
        if имя == РАЦ and len(аргументы) == 2:
            return f"dan_rat_of({аргументы[0]}, {аргументы[1]}, {в.строка})"
        if имя in (СТР, ЦЕЛ, ДРОБ, РАЦ):
            return self._преобразование(имя, в.аргументы[0].тип_выр, аргументы[0], в.строка)
        return f"{self.имя(имя)}({', '.join(аргументы)})"

    @staticmethod
    def _преобразование(куда, откуда, значение, строка):
        if куда == откуда:
            return значение
        if куда == СТР:
            return {ЦЕЛ: "dan_int_to_str", ДРОБ: "dan_float_to_str", РАЦ: "dan_rat_to_str",
                    ЛОГ: "dan_bool_to_str"}[откуда] + f"({значение})"
        if куда == ЦЕЛ:
            if откуда == РАЦ:
                return f"dan_rat_to_int({значение})"
            функция = "dan_float_to_int" if откуда == ДРОБ else "dan_str_to_int"
            return f"{функция}({значение}, {строка})"
        if куда == РАЦ:
            функция = {ЦЕЛ: "dan_rat_from_int", ДРОБ: "dan_rat_from_float",
                       СТР: "dan_rat_from_str"}[откуда]
            return f"{функция}({значение}, {строка})"
        # дроб
        if откуда == ЦЕЛ:
            return f"((double)({значение}))"
        if откуда == РАЦ:
            return f"dan_rat_to_float({значение})"
        return f"dan_str_to_float({значение}, {строка})"


def сгенерировать(программа):
    """Возвращает текст программы на C для проверенного дерева."""
    return Генератор().программа(программа)
