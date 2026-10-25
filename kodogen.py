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
import vysshaya  # регистрирует функции высшей и прикладной математики
import dannye
from proverka import (ЦЕЛ, ДРОБ, СТР, ЛОГ, РАЦ, МН, БАЙТЫ, ПУСТО, НИЧЕГО, это_список, элемент,
                      это_необяз, основа, найти_в_библиотеке, это_словарь, ключ_словаря, значение_словаря,
                      это_результат, ОШИБКА, это_функция, разобрать_тип_функции)

БАЗОВЫЕ_C = {ЦЕЛ: "int64_t", ДРОБ: "double", СТР: "dan_str", ЛОГ: "bool", РАЦ: "dan_rat",
             МН: "dan_mn", БАЙТЫ: "dan_bytes", ПУСТО: "void"}
КОДЫ_ТИПОВ = {ЦЕЛ: "i", ДРОБ: "f", СТР: "s", ЛОГ: "b", РАЦ: "r", МН: "p", БАЙТЫ: "y", ПУСТО: "v"}
ЗАПИСЬ = {ЦЕЛ: "dan_write_int", ДРОБ: "dan_write_float", СТР: "dan_write_str", ЛОГ: "dan_write_bool",
          РАЦ: "dan_write_rat", МН: "dan_write_poly", БАЙТЫ: "dan_write_bytes"}
ОПЕРАЦИИ_МН = {"+": "dan_poly_add", "-": "dan_poly_sub", "*": "dan_poly_mul", "/": "dan_poly_div",
               "%": "dan_poly_mod"}
ЦЕЛЫЕ_ДЕЙСТВИЯ = {"+": "dan_add", "-": "dan_sub", "*": "dan_mul"}
ОПЕРАЦИИ_РАЦ = {"+": "dan_rat_add", "-": "dan_rat_sub", "*": "dan_rat_mul", "/": "dan_rat_div"}

СООБЩЕНИЯ = {
    "DAN_MSG_RUNTIME": "Ошибка выполнения в строке %d: %s\n",
    "DAN_MSG_RUNTIME_FILE": "Ошибка выполнения в файле %s, строка %d: %s\n",
    "DAN_MSG_DIV_ZERO": "деление на ноль",
    "DAN_MSG_MOD_ZERO": "остаток от деления на ноль",
    "DAN_MSG_NO_MEMORY": "недостаточно памяти",
    "DAN_MSG_RUNTIME_NOLINE": "Ошибка выполнения: %s\n",
    "DAN_MSG_DEEP_DATA": "значение собственного типа вложено слишком глубоко (например, очень длинная "
                         "цепочка звеньев), и его нельзя целиком сравнить, вывести или скопировать",
    "DAN_MSG_DEEP": "слишком глубокая рекурсия: функция из этой строки вызвала себя слишком много раз, "
                    "не завершившись; проверьте условие остановки рекурсии или замените её циклом",
    "DAN_MSG_TOO_BIG": "результат слишком велик для типа цел",
    "DAN_MSG_INDEX": "индекс %lld за пределами (длина %lld)",
    "DAN_MSG_NO_KEY": "в словаре нет ключа ",
    "DAN_MSG_EMPTY_SEARCH": "искомая строка не может быть пустой",
    "DAN_MSG_NEG_REPEAT": "число повторений не может быть отрицательным",
    "DAN_MSG_NOT_INT": "строку \"%s\" нельзя преобразовать в целое число",
    "DAN_MSG_INT_RANGE": "число \"%s\" слишком велико для типа цел",
    "DAN_MSG_NOT_FLOAT": "строку \"%s\" нельзя преобразовать в число",
    "DAN_MSG_FLOAT_RANGE": "дробное число слишком велико для типа цел или не является числом",
    "DAN_MSG_RAT_BIG": "числитель или знаменатель дроби слишком велик",
    "DAN_MSG_NOT_RAT": "строку \"%s\" нельзя преобразовать в дробь",
    "DAN_MSG_NOT_FINITE": "бесконечность или не-число нельзя преобразовать в дробь",
    "DAN_MSG_NEG_POW": "целое число в отрицательной степени не является целым; "
                       "используйте рац(а) ^ н или дробное число",
    "DAN_TXT_BYTES": "байты",
    "DAN_TRUE": "истина",
    "DAN_FALSE": "ложь",
}

def _вызываемые(узел, имена, найдено):
    """Имена функций программы, которые вызываются внутри узла."""
    if isinstance(узел, у.Вызов) and isinstance(узел.функция, у.Имя) and узел.функция.имя in имена:
        найдено.add(узел.функция.имя)
    if isinstance(узел, list):
        for ч in узел:
            _вызываемые(ч, имена, найдено)
    elif isinstance(узел, у.Узел):
        for значение in vars(узел).values():
            if isinstance(значение, (у.Узел, list)):
                _вызываемые(значение, имена, найдено)
    return найдено


def рекурсивные_функции(функции):
    """Функции, которые могут вызвать сами себя (прямо или через другие функции).

    Только их рекурсия может быть неограниченной, поэтому проверка глубины
    стека ставится лишь в них, а остальные функции работают без издержек.
    """
    имена = {ф.имя for ф in функции}
    вызовы = {ф.имя: _вызываемые(ф.тело, имена, set()) for ф in функции}
    рекурсивные = set()
    for начало in имена:
        увиденные, очередь = set(), list(вызовы[начало])
        while очередь:
            имя = очередь.pop()
            if имя == начало:
                рекурсивные.add(начало)
                break
            if имя not in увиденные:
                увиденные.add(имя)
                очередь.extend(вызовы[имя])
    return рекурсивные


СРЕДА = r"""
typedef const char *dan_str;

static void dan_error(const char *text, int line) {
    fflush(stdout);
    if (line >= DAN_FILE_STEP && line / DAN_FILE_STEP < DAN_FILE_COUNT)
        fprintf(stderr, DAN_MSG_RUNTIME_FILE, dan_files[line / DAN_FILE_STEP], line % DAN_FILE_STEP, text);
    else
        fprintf(stderr, DAN_MSG_RUNTIME, line, text);
    exit(1);
}

/* Защита от переполнения стека при глубокой рекурсии. Вместо аварийного
   завершения без сообщения программа останавливается с ошибкой выполнения.
   Стек растёт вниз (x86, ARM); в начале каждой функции программы сравнивается
   глубина стека с пределом, вычисленным в dan_start по размеру стека потока. */
static uintptr_t dan_stack_base, dan_stack_limit;

static void dan_stack_error(int line) __attribute__((noinline, cold));
static void dan_stack_error(int line) {
    if (line > 0) dan_error(DAN_MSG_DEEP, line);
    fflush(stdout);
    fprintf(stderr, DAN_MSG_RUNTIME_NOLINE, DAN_MSG_DEEP_DATA);
    exit(1);
}

#if defined(__GNUC__) || defined(__clang__)
#define DAN_FRAME() ((uintptr_t)__builtin_frame_address(0))
#define DAN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define DAN_FRAME() ((uintptr_t)&dan_probe_)
#define DAN_UNLIKELY(x) (x)
#endif
#define DAN_STACK_CHECK(line) do { char dan_probe_; (void)dan_probe_; \
    if (DAN_UNLIKELY(DAN_FRAME() < dan_stack_base - dan_stack_limit)) dan_stack_error(line); } while (0)

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

/* Integer overflow is never silent: the result is either exact or the
   program stops with a message (no undefined behaviour, no wraparound). */
static inline int64_t dan_add(int64_t a, int64_t b, int line) {
    int64_t r;
    if (__builtin_add_overflow(a, b, &r)) dan_error(DAN_MSG_TOO_BIG, line);
    return r;
}

static inline int64_t dan_sub(int64_t a, int64_t b, int line) {
    int64_t r;
    if (__builtin_sub_overflow(a, b, &r)) dan_error(DAN_MSG_TOO_BIG, line);
    return r;
}

static inline int64_t dan_mul(int64_t a, int64_t b, int line) {
    int64_t r;
    if (__builtin_mul_overflow(a, b, &r)) dan_error(DAN_MSG_TOO_BIG, line);
    return r;
}

static inline int64_t dan_neg(int64_t a, int line) {
    if (a == INT64_MIN) dan_error(DAN_MSG_TOO_BIG, line);
    return -a;
}

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

/* ---- characters of UTF-8 strings ---- */

static size_t dan_utf8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

/* byte offset of character number i (i may equal the length: end of string) */
static const char *dan_utf8_at(dan_str s, int64_t i) {
    const char *p = s;
    while (i > 0 && *p) { p += dan_utf8_len((unsigned char)*p); i--; }
    return i == 0 ? p : NULL;
}

static int64_t dan_str_len(dan_str s);

static dan_str dan_str_sub(const char *a, const char *b) {
    size_t n = (size_t)(b - a);
    char *r = dan_alloc(n + 1);
    memcpy(r, a, n);
    r[n] = 0;
    return r;
}

static dan_str dan_str_char_at(dan_str s, int64_t i, int line) {
    const char *p = i >= 0 ? dan_utf8_at(s, i) : NULL;
    if (!p || !*p) dan_index_error(i, dan_str_len(s), line);
    return dan_str_sub(p, p + dan_utf8_len((unsigned char)*p));
}

/* characters a..b inclusive; b == a - 1 gives an empty string */
static dan_str dan_str_slice(dan_str s, int64_t a, int64_t b, int line) {
    int64_t n = dan_str_len(s);
    if (a < 0 || a > n) dan_index_error(a, n, line);
    if (b < a - 1 || b >= n) dan_index_error(b, n, line);
    const char *pa = dan_utf8_at(s, a), *pb = dan_utf8_at(pa, b - a + 1);
    return dan_str_sub(pa, pb);
}

static int64_t dan_str_find(dan_str s, dan_str t) {
    const char *f = strstr(s, t);
    if (!f) return -1;
    int64_t n = 0;
    for (const char *p = s; p < f; p += dan_utf8_len((unsigned char)*p)) n++;
    return n;
}

static bool dan_str_contains(dan_str s, dan_str t) { return strstr(s, t) != NULL; }
static bool dan_str_starts(dan_str s, dan_str t) { return strncmp(s, t, strlen(t)) == 0; }
static bool dan_str_ends(dan_str s, dan_str t) {
    size_t a = strlen(s), b = strlen(t);
    return b <= a && memcmp(s + a - b, t, b) == 0;
}

static int dan_str_cmp(dan_str a, dan_str b) { return strcmp(a, b); }

static dan_str dan_str_trim(dan_str s) {
    const char *a = s, *b = s + strlen(s);
    while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n' || b[-1] == '\r')) b--;
    return dan_str_sub(a, b);
}

/* upper and lower case for Latin and Cyrillic letters (including ё) */
static dan_str dan_str_case(dan_str s, int upper) {
    size_t n = strlen(s);
    unsigned char *r = dan_alloc(n + 1);
    memcpy(r, s, n + 1);
    for (size_t i = 0; i < n;) {
        unsigned char c = r[i];
        if (c < 0x80) {
            if (upper && c >= 'a' && c <= 'z') r[i] = (unsigned char)(c - 32);
            if (!upper && c >= 'A' && c <= 'Z') r[i] = (unsigned char)(c + 32);
            i++;
            continue;
        }
        size_t k = dan_utf8_len(c);
        if (k == 2 && i + 1 < n) {
            unsigned cp = ((unsigned)(c & 0x1F) << 6) | (r[i + 1] & 0x3F);
            if (upper) {
                if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;
                else if (cp == 0x451) cp = 0x401;
            } else {
                if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;
                else if (cp == 0x401) cp = 0x451;
            }
            r[i] = (unsigned char)(0xC0 | (cp >> 6));
            r[i + 1] = (unsigned char)(0x80 | (cp & 0x3F));
        }
        i += k;
    }
    return (dan_str)r;
}

static dan_str dan_str_upper(dan_str s) { return dan_str_case(s, 1); }
static dan_str dan_str_lower(dan_str s) { return dan_str_case(s, 0); }

/* Growing text buffer. */
typedef struct { char *s; size_t len, cap; } dan_buf;

static void dan_buf_add(dan_buf *b, const char *t) {
    size_t n = strlen(t);
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 16;
        b->s = dan_realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, t, n + 1);
    b->len += n;
}

static dan_str dan_str_replace(dan_str s, dan_str a, dan_str b, int line) {
    if (!*a) dan_error(DAN_MSG_EMPTY_SEARCH, line);
    dan_buf r = { NULL, 0, 0 };
    dan_buf_add(&r, "");
    size_t la = strlen(a);
    const char *p = s, *f;
    while ((f = strstr(p, a)) != NULL) {
        dan_str part = dan_str_sub(p, f);
        dan_buf_add(&r, part);
        dan_buf_add(&r, b);
        p = f + la;
    }
    dan_buf_add(&r, p);
    return r.s;
}

static dan_str dan_str_repeat(dan_str s, int64_t k, int line) {
    if (k < 0) dan_error(DAN_MSG_NEG_REPEAT, line);
    dan_buf r = { NULL, 0, 0 };
    dan_buf_add(&r, "");
    for (int64_t i = 0; i < k; i++) dan_buf_add(&r, s);
    return r.s;
}

static dan_str dan_quote_str(dan_str s) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "\"");
    dan_buf_add(&b, s);
    dan_buf_add(&b, "\"");
    return b.s;
}

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

/* ---- bytes ---- */

typedef struct { int64_t len; uint8_t *data; } dan_bytes_s;
typedef const dan_bytes_s *dan_bytes;

static dan_bytes_s *dan_bytes_alloc(int64_t len) {
    dan_bytes_s *b = dan_alloc(sizeof *b);
    b->len = len;
    b->data = dan_alloc((size_t)len + 1);
    b->data[len] = 0;
    return b;
}

static dan_bytes dan_bytes_from_str(dan_str s) {
    size_t n = strlen(s);
    dan_bytes_s *b = dan_bytes_alloc((int64_t)n);
    memcpy(b->data, s, n);
    return b;
}

/* Text from bytes in UTF-8; the text ends at the first zero byte. */
static dan_str dan_bytes_to_str(dan_bytes b) {
    char *s = dan_alloc((size_t)b->len + 1);
    memcpy(s, b->data, (size_t)b->len);
    s[b->len] = 0;
    return s;
}

static dan_bytes dan_bytes_concat(dan_bytes a, dan_bytes b) {
    dan_bytes_s *r = dan_bytes_alloc(a->len + b->len);
    memcpy(r->data, a->data, (size_t)a->len);
    memcpy(r->data + a->len, b->data, (size_t)b->len);
    return r;
}

static bool dan_bytes_eq(dan_bytes a, dan_bytes b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

static int64_t dan_bytes_get(dan_bytes b, int64_t i, int line) {
    if (i < 0 || i >= b->len) dan_index_error(i, b->len, line);
    return b->data[i];
}

/* байты(5): 68 65 6c 6c 6f */
static dan_str dan_bytes_show(dan_bytes b) {
    dan_buf s = { NULL, 0, 0 };
    char t[32];
    snprintf(t, sizeof t, "(%" PRId64 ")", b->len);
    dan_buf_add(&s, DAN_TXT_BYTES);
    dan_buf_add(&s, t);
    if (b->len) dan_buf_add(&s, ":");
    for (int64_t i = 0; i < b->len && i < 32; i++) { snprintf(t, sizeof t, " %02x", b->data[i]); dan_buf_add(&s, t); }
    if (b->len > 32) dan_buf_add(&s, " ...");
    return s.s;
}

static void dan_write_bytes(dan_bytes b) { fputs(dan_bytes_show(b), stdout); }

/* ---- polynomials: defined after the list types ---- */
typedef struct dan_poly dan_poly;
typedef const dan_poly *dan_mn;
static void dan_write_poly(dan_mn p);
static dan_str dan_poly_to_str(dan_mn p);
static bool dan_poly_eq(dan_mn a, dan_mn b);

static void dan_start(void) {
    char here;
    uintptr_t size = 1024 * 1024;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(&here, &mbi, sizeof mbi))
        size = (uintptr_t)&here - (uintptr_t)mbi.AllocationBase;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        size = (uintptr_t)rl.rlim_cur;
    else
        size = (uintptr_t)256 * 1024 * 1024;
#endif
    dan_stack_base = (uintptr_t)&here;
    if (size > dan_stack_base) size = dan_stack_base;
    /* Запас на функции библиотеки выполнения и самой системы. */
    dan_stack_limit = size / 4 * 3 > 256 * 1024 ? size / 4 * 3 - 256 * 1024 : size / 2;
}
"""

ШАБЛОН_СПИСКА = """
/* ---- list {описание} ---- */
/* rc - число мест (переменных, полей, элементов), где хранится список.
   Список - значение: если мест несколько, перед изменением делается копия. */
struct {S} {{ int64_t len, cap, rc; {T} *items; }};

static {S} *{S}_from(int64_t n, {T} const *src) {{
    {S} *l = dan_alloc(sizeof *l);
    l->rc = 0;
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

static {T} *{S}_at({S} *l, int64_t i, int line) {{
    if (i < 0 || i >= l->len) dan_index_error(i, l->len, line);
    return &l->items[i];
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

static void {S}_insert({S} *l, int64_t i, {T} v, int line) {{
    if (i < 0 || i > l->len) dan_index_error(i, l->len, line);
    {S}_push(l, v);
    memmove(&l->items[i + 1], &l->items[i], (size_t)(l->len - 1 - i) * sizeof({T}));
    l->items[i] = v;
}}

static void {S}_remove({S} *l, int64_t i, int line) {{
    if (i < 0 || i >= l->len) dan_index_error(i, l->len, line);
    memmove(&l->items[i], &l->items[i + 1], (size_t)(l->len - i - 1) * sizeof({T}));
    l->len--;
}}

/* новый список с теми же элементами; элементы получают ещё одно место хранения */
static {S} *{S}_copy(const {S} *l) {{
    {S} *r = {S}_from(l->len, l->items);
    {поделить_элементы}
    return r;
}}

static {S} *{S}_share({S} *l) {{ l->rc++; return l; }}
static {S} *{S}_release({S} *l) {{ if (l->rc > 0) l->rc--; return l; }}

/* место *pp собирается изменить список: если список хранится и в других
   местах, место получает собственную копию */
static {S} *{S}_mut({S} **pp) {{
    {S} *l = *pp;
    if (l->rc > 1) {{
        {S} *r = {S}_copy(l);
        r->rc = 1;
        l->rc--;
        *pp = r;
        return r;
    }}
    return l;
}}

static {S} *{S}_reversed(const {S} *l) {{
    {S} *r = {S}_copy(l);
    for (int64_t i = 0, j = r->len - 1; i < j; i++, j--) {{ {T} t = r->items[i]; r->items[i] = r->items[j]; r->items[j] = t; }}
    return r;
}}

/* elements a..b inclusive; b == a - 1 gives an empty list */
static {S} *{S}_slice(const {S} *l, int64_t a, int64_t b, int line) {{
    if (a < 0 || a > l->len) dan_index_error(a, l->len, line);
    if (b < a - 1 || b >= l->len) dan_index_error(b, l->len, line);
    {S} *r = {S}_from(b - a + 1, l->items + a);
    {поделить_элементы}
    return r;
}}

static dan_str {S}_str(const {S} *l) {{
    dan_buf b = {{ NULL, 0, 0 }};
    dan_buf_add(&b, "[");
    for (int64_t i = 0; i < l->len; i++) {{
        if (i) dan_buf_add(&b, ", ");
        dan_buf_add(&b, {строка_элемента}(l->items[i]));
    }}
    dan_buf_add(&b, "]");
    return b.s;
}}
"""

# Словари: хеш-таблица с открытой адресацией; записи хранятся в порядке
# добавления, поэтому перебор и вывод предсказуемы. Удалённые записи
# помечаются и убираются при ближайшем расширении таблицы.
КОД_ХЕШЕЙ = r"""
/* ---- dictionaries: hashing ---- */
static uint64_t dan_hash_i(int64_t v) {
    uint64_t z = (uint64_t)v + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint64_t dan_hash_s(dan_str s) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 0x100000001B3ULL; }
    return dan_hash_i((int64_t)h);
}
static uint64_t dan_hash_r(dan_rat r) { return dan_hash_i(r.n) * 31 + dan_hash_i(r.d); }
static void dan_key_error(dan_str key, int line) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, DAN_MSG_NO_KEY);
    dan_buf_add(&b, key);
    dan_error(b.s, line);
}
"""
ХЕШИ = {ЦЕЛ: "dan_hash_i(k)", СТР: "dan_hash_s(k)", ЛОГ: "(uint64_t)(k)", РАЦ: "dan_hash_r(k)"}

ОБЪЯВЛЕНИЯ_СЛОВАРЯ = """
static {D} *{D}_share({D} *d);
static dan_str {D}_str(const {D} *d);
static void {D}_write(const {D} *d);
"""

ШАБЛОН_СЛОВАРЯ = """
/* ---- dictionary {описание} ---- */
struct {D} {{ int64_t len, n, cap, rc, slots; {K} *keys; {V} *vals; uint64_t *hashes; bool *alive; int64_t *index; }};

static {D} *{D}_new(void) {{
    {D} *d = dan_alloc(sizeof *d);
    d->len = d->n = d->cap = d->rc = d->slots = 0;
    d->keys = NULL; d->vals = NULL; d->hashes = NULL; d->alive = NULL; d->index = NULL;
    return d;
}}
static {D} *{D}_share({D} *d) {{ d->rc++; return d; }}
static {D} *{D}_release({D} *d) {{ if (d->rc > 0) d->rc--; return d; }}

static void {D}_reindex({D} *d) {{
    int64_t s = 8;
    while (s < 2 * d->cap) s *= 2;
    d->index = dan_alloc(sizeof(int64_t) * (size_t)s);
    for (int64_t i = 0; i < s; i++) d->index[i] = -1;
    d->slots = s;
    for (int64_t e = 0; e < d->n; e++) {{
        if (!d->alive[e]) continue;
        int64_t p = (int64_t)(d->hashes[e] & (uint64_t)(s - 1));
        while (d->index[p] >= 0) p = (p + 1) & (s - 1);
        d->index[p] = e;
    }}
}}
static int64_t {D}_lookup(const {D} *d, {K} k, uint64_t h) {{
    if (!d->slots) return -1;
    for (int64_t p = (int64_t)(h & (uint64_t)(d->slots - 1));; p = (p + 1) & (d->slots - 1)) {{
        int64_t e = d->index[p];
        if (e < 0) return -1;
        if (d->alive[e] && d->hashes[e] == h) {{ {K} a = d->keys[e], b = k; if ({EQ_K}) return e; }}
    }}
}}
static void {D}_grow({D} *d) {{   /* убирает удалённые записи и увеличивает таблицу */
    int64_t cap = d->len * 2 < 8 ? 8 : d->len * 2, m = 0;
    {K} *keys = dan_alloc(sizeof({K}) * (size_t)cap);
    {V} *vals = dan_alloc(sizeof({V}) * (size_t)cap);
    uint64_t *hashes = dan_alloc(sizeof(uint64_t) * (size_t)cap);
    bool *alive = dan_alloc(sizeof(bool) * (size_t)cap);
    for (int64_t e = 0; e < d->n; e++) {{
        if (!d->alive[e]) continue;
        keys[m] = d->keys[e]; vals[m] = d->vals[e]; hashes[m] = d->hashes[e]; alive[m] = true; m++;
    }}
    d->keys = keys; d->vals = vals; d->hashes = hashes; d->alive = alive; d->n = m; d->cap = cap;
    {D}_reindex(d);
}}
static void {D}_put({D} *d, {K} k, {V} v) {{
    uint64_t h = {HASH};
    int64_t e = {D}_lookup(d, k, h);
    if (e >= 0) {{ d->vals[e] = v; return; }}
    if (d->n == d->cap) {D}_grow(d);
    e = d->n++;
    d->keys[e] = k; d->vals[e] = v; d->hashes[e] = h; d->alive[e] = true; d->len++;
    int64_t p = (int64_t)(h & (uint64_t)(d->slots - 1));
    while (d->index[p] >= 0) p = (p + 1) & (d->slots - 1);
    d->index[p] = e;
}}
static {D} *{D}_from(int64_t n, const {K} *ks, const {V} *vs) {{
    {D} *d = {D}_new();
    for (int64_t i = 0; i < n; i++) {D}_put(d, ks[i], vs[i]);
    return d;
}}
static {V} {D}_get(const {D} *d, {K} k, int line) {{
    int64_t e = {D}_lookup(d, k, {HASH});
    if (e < 0) dan_key_error({СТРОКА_К}(k), line);
    return d->vals[e];
}}
static {V} *{D}_at({D} *d, {K} k, int line) {{
    int64_t e = {D}_lookup(d, k, {HASH});
    if (e < 0) dan_key_error({СТРОКА_К}(k), line);
    return &d->vals[e];
}}
static bool {D}_contains(const {D} *d, {K} k) {{ return {D}_lookup(d, k, {HASH}) >= 0; }}
{НАЙТИ}static {V} {D}_get_or(const {D} *d, {K} k, {V} v) {{
    int64_t e = {D}_lookup(d, k, {HASH});
    return e < 0 ? v : d->vals[e];
}}
static void {D}_remove({D} *d, {K} k) {{
    int64_t e = {D}_lookup(d, k, {HASH});
    if (e >= 0) {{ d->alive[e] = false; d->len--; }}
}}
static {LK} *{D}_keys(const {D} *d) {{
    {LK} *r = {LK}_from(0, NULL);
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {LK}_push(r, d->keys[e]);
    return r;
}}
static {LV} *{D}_values(const {D} *d) {{
    {LV} *r = {LV}_from(0, NULL);
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {LV}_push(r, {ПОДЕЛИТЬ}(d->vals[e]));
    return r;
}}
static {D} *{D}_copy(const {D} *d) {{
    {D} *r = {D}_new();
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {D}_put(r, d->keys[e], {ПОДЕЛИТЬ}(d->vals[e]));
    return r;
}}
static {D} *{D}_mut({D} **pp) {{
    {D} *d = *pp;
    if (d->rc > 1) {{ {D} *r = {D}_copy(d); r->rc = 1; d->rc--; *pp = r; return r; }}
    return d;
}}
static dan_str {D}_str(const {D} *d) {{
    dan_buf b = {{ NULL, 0, 0 }};
    dan_buf_add(&b, "{{");
    bool first = true;
    for (int64_t e = 0; e < d->n; e++) {{
        if (!d->alive[e]) continue;
        if (!first) dan_buf_add(&b, ", ");
        first = false;
        dan_buf_add(&b, {СТРОКА_К}(d->keys[e]));
        dan_buf_add(&b, ": ");
        dan_buf_add(&b, {СТРОКА_З}(d->vals[e]));
    }}
    dan_buf_add(&b, "}}");
    return b.s;
}}
static void {D}_write(const {D} *d) {{ fputs({D}_str(d), stdout); }}
"""

ШАБЛОН_ПОИСКА_В_СЛОВАРЕ = """
static {O} {D}_find(const {D} *d, {K} k) {{
    int64_t e = {D}_lookup(d, k, {HASH});
    return e < 0 ? {O}_none() : {O_SOME}(d->vals[e]);
}}
"""

ШАБЛОН_РАВЕНСТВА_СЛОВАРЯ = """
static bool {D}_eq(const {D} *x, const {D} *y) {{
    if (x->len != y->len) return false;
    for (int64_t e = 0; e < x->n; e++) {{
        if (!x->alive[e]) continue;
        int64_t f = {D}_lookup(y, x->keys[e], x->hashes[e]);
        if (f < 0) return false;
        {V} a = x->vals[e]; {V} b = y->vals[f];
        if (!({EQ_V})) return false;
    }}
    return true;
}}
"""

# Равенство элементов списка (для «содержит» и «найти»).
РАВЕНСТВО = {ЦЕЛ: "(a == b)", ДРОБ: "(a == b)", ЛОГ: "(a == b)", СТР: "dan_str_eq(a, b)",
             РАЦ: "(dan_rat_cmp(a, b) == 0)", МН: "dan_poly_eq(a, b)", БАЙТЫ: "dan_bytes_eq(a, b)"}

ШАБЛОН_ПОИСКА = """
static int64_t {S}_find(const {S} *l, {T} b) {{
    for (int64_t i = 0; i < l->len; i++) {{ {T} a = l->items[i]; if ({EQ}) return i; }}
    return -1;
}}
static bool {S}_contains(const {S} *l, {T} b) {{ return {S}_find(l, b) >= 0; }}
static bool {S}_eq(const {S} *x, const {S} *y) {{
    if (x->len != y->len) return false;
    for (int64_t i = 0; i < x->len; i++) {{ {T} a = x->items[i]; {T} b = y->items[i]; if (!({EQ})) return false; }}
    return true;
}}
"""

# Функции строк, которые возвращают списки строк: идут после определения dl_Ls.
КОД_СТРОК_СО_СПИСКАМИ = r"""
static dl_Ls *dan_str_chars(dan_str s) {
    dl_Ls *r = dl_Ls_from(0, NULL);
    for (const char *p = s; *p;) {
        size_t k = dan_utf8_len((unsigned char)*p);
        dl_Ls_push(r, dan_str_sub(p, p + k));
        p += k;
    }
    return r;
}

static dl_Ls *dan_str_split(dan_str s, dan_str sep, int line) {
    if (!*sep) dan_error(DAN_MSG_EMPTY_SEARCH, line);
    dl_Ls *r = dl_Ls_from(0, NULL);
    size_t ls = strlen(sep);
    const char *p = s, *f;
    while ((f = strstr(p, sep)) != NULL) { dl_Ls_push(r, dan_str_sub(p, f)); p = f + ls; }
    dl_Ls_push(r, dan_strdup(p));
    return r;
}

/* words separated by any spaces, tabs and line breaks */
static dl_Ls *dan_str_split_ws(dan_str s) {
    dl_Ls *r = dl_Ls_from(0, NULL);
    const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *a = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        dl_Ls_push(r, dan_str_sub(a, p));
    }
    return r;
}

static dan_str dl_Ls_join(const dl_Ls *l, dan_str sep) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    for (int64_t i = 0; i < l->len; i++) {
        if (i) dan_buf_add(&b, sep);
        dan_buf_add(&b, l->items[i]);
    }
    return b.s;
}
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


class Генератор:
    def __init__(self):
        self.имена = {}            # имя в Дан -> имя в C
        self.занятые = set()
        self.строки = []
        self.отступ = 0
        self.счётчик = 0
        self.списки = {}           # тип списка -> имя структуры C (вложенные раньше внешних)
        self.нужна_математика = False
        self.нужны_данные = False
        self.собственные = {}      # собственные типы: имя -> [(поле, тип, значение по умолчанию)]
        self.имена_типов = {}      # собственный тип -> часть имени в C
        self.поля_c = {}           # (тип, поле) -> имя поля в C
        self.значимые = {}         # собственные и необязательные типы -> имя типа C
        self.расширения = {}       # (тип списка, более широкий тип) -> функция C
        self.словари = {}          # тип словаря -> имя структуры C
        self.функциональные = {}   # тип функции -> номер (df_N)
        self.лямбды_код = []       # код безымянных функций и обёрток (ставится перед функциями программы)
        self.обёртки = {}          # функция программы, используемая как значение -> имя обёртки C
        self.помощники = {}        # (метод, тип списка, тип функции) -> функция C для методов с функцией
        self.перечисления = {}     # перечисление -> [(вариант, [(поле, тип)])]
        self.перечисления_c = {}   # перечисление, используемое в программе -> имя типа C
        self.написания = {}

    def код_типа(self, тип):
        """Короткий код типа для имён в C: [цел] -> Li, стр? -> Os, [Сотрудник] -> LTSotrudnik."""
        if это_список(тип):
            return "L" + self.код_типа(элемент(тип))
        if это_словарь(тип):
            return "D" + self.код_типа(ключ_словаря(тип)) + self.код_типа(значение_словаря(тип))
        if это_функция(тип):
            self.значимый(тип)
            return "F" + str(self.функциональные[тип])
        if тип in self.перечисления:
            return "V" + self.имя_типа(тип)
        if это_результат(тип):
            return "R" + self.код_типа(основа(тип))
        if это_необяз(тип):
            return "O" + self.код_типа(основа(тип))
        if тип in self.собственные:
            return "T" + self.имя_типа(тип)
        return КОДЫ_ТИПОВ[тип]

    def имя_типа(self, тип):
        if тип not in self.имена_типов:
            основа_имени, номер = _латиницей(тип), 2
            кандидат = основа_имени
            while кандидат in self.имена_типов.values():
                кандидат = f"{основа_имени}_{номер}"
                номер += 1
            self.имена_типов[тип] = кандидат
        return self.имена_типов[тип]

    def поле_c(self, тип, поле):
        if (тип, поле) not in self.поля_c:
            занятые = {c for (т, _), c in self.поля_c.items() if т == тип}
            основа_имени, номер = "f_" + _латиницей(поле), 2
            кандидат = основа_имени
            while кандидат in занятые:
                кандидат = f"{основа_имени}_{номер}"
                номер += 1
            self.поля_c[(тип, поле)] = кандидат
        return self.поля_c[(тип, поле)]

    def значимый(self, тип):
        """Имя типа C для собственного или необязательного типа (значения, а не ссылки)."""
        if тип not in self.значимые:
            if это_функция(тип):
                self.функциональные[тип] = len(self.функциональные) + 1
                self.значимые[тип] = f"df_{self.функциональные[тип]}"
                параметры, результат = разобрать_тип_функции(тип)
                for т in параметры + [результат]:
                    if т not in (ПУСТО,):
                        self.тип_c(т)
            elif это_результат(тип):
                self.значимые[тип] = "dr_" + self.код_типа(основа(тип))
                if основа(тип) != ПУСТО:
                    self.тип_c(основа(тип))
            elif это_необяз(тип):
                self.значимые[тип] = "do_" + self.код_типа(основа(тип))
                self.тип_c(основа(тип))
            else:
                self.значимые[тип] = "dt_" + self.имя_типа(тип)
                for поле, тип_поля, _ in self.собственные[тип]:
                    self.поле_c(тип, поле)
                    self.тип_c(тип_поля)
        return self.значимые[тип]

    def рекурсивный(self, тип, искомый=None, посещённые=None):
        """Может ли значение собственного типа содержать (через поля) значение того же типа."""
        искомый = искомый or тип
        посещённые = посещённые if посещённые is not None else set()
        for _, т, _ in self.собственные.get(тип, []):
            while это_необяз(т):
                т = основа(т)
            if т == искомый:
                return True
            if т in self.собственные and т not in посещённые:
                посещённые.add(т)
                if self.рекурсивный(т, искомый, посещённые):
                    return True
        return False

    def по_ссылке(self, тип):
        """Необязательное значение рекурсивного типа хранится по ссылке (NULL - ничего)."""
        return это_необяз(тип) and основа(тип) in self.собственные and self.рекурсивный(основа(тип))

    def расширение(self, из_типа, в_тип):
        """Функция C, создающая из списка чисел список более широкого типа."""
        ключ = (из_типа, в_тип)
        if ключ not in self.расширения:
            имя = f"dan_widen_{self.код_типа(из_типа)}_{self.код_типа(в_тип)}"
            self.расширения[ключ] = имя
            self.структура(из_типа)
            self.структура(в_тип)
            if это_список(элемент(в_тип)):
                self.расширение(элемент(из_типа), элемент(в_тип))
        return self.расширения[ключ]

    def _функции_расширения(self):
        строки = [f"static {self.структура(в)} *{имя}(const {self.структура(из)} *l, int line);"
                  for (из, в), имя in self.расширения.items()]
        for (из_типа, в_тип), имя in self.расширения.items():
            S_из, S_в = self.структура(из_типа), self.структура(в_тип)
            эл = self.привести("l->items[i]", элемент(из_типа), элемент(в_тип), "line")
            if это_список(элемент(в_тип)):
                эл = f"{self.структура(элемент(в_тип))}_share({эл})"
            строки.append(f"static {S_в} *{имя}(const {S_из} *l, int line) {{\n"
                          f"    (void)line;\n    {S_в} *r = {S_в}_from(0, NULL);\n"
                          f"    for (int64_t i = 0; i < l->len; i++) {S_в}_push(r, {эл});\n"
                          f"    return r;\n}}")
        return "\n".join(строки) + "\n"

    def словарь_c(self, тип):
        """Имя структуры C словаря; регистрирует нужные ему типы."""
        if тип not in self.словари:
            self.словари[тип] = "dm_" + self.код_типа(тип)
            к, з = ключ_словаря(тип), значение_словаря(тип)
            self.структура("[" + к + "]")
            self.структура("[" + з + "]")
            if not это_функция(з):
                self.тип_c(з if это_необяз(з) else з + "?")
            self.тип_c(з)
        return self.словари[тип]

    def содержит_списки(self, тип, в_работе=None):
        """Хранит ли значение типа списки, за местами хранения которых нужно следить.
        Необязательное значение рекурсивного типа неизменяемо и не считается."""
        в_работе = в_работе if в_работе is not None else set()
        if это_список(тип) or это_словарь(тип):
            return True
        if self.по_ссылке(тип) or это_функция(тип) or тип in self.перечисления:
            return False
        if это_результат(тип):
            return основа(тип) != ПУСТО and self.содержит_списки(основа(тип), в_работе)
        if это_необяз(тип):
            return self.содержит_списки(основа(тип), в_работе)
        if тип in self.собственные and тип not in в_работе:
            в_работе.add(тип)
            return any(self.содержит_списки(т, в_работе) for _, т, _ in self.собственные[тип])
        return False

    def поделить(self, тип):
        """Функция C, которая отмечает новое место хранения значения (или None)."""
        if not self.содержит_списки(тип):
            return None
        if это_список(тип):
            return self.структура(тип) + "_share"
        if это_словарь(тип):
            return self.словарь_c(тип) + "_share"
        return self.значимый(тип) + "_share"

    @staticmethod
    def владеющее(узел):
        """Значение только что создано (вызовом функции, конструктором): места
        хранения вложенных в него списков уже учтены, копировать учёт не нужно."""
        return isinstance(узел, (у.Вызов, у.Ничего, у.Список))

    def сохранить(self, узел, код, тип):
        """Код значения, которое кладётся в новое место хранения (переменную,
        поле, элемент списка)."""
        функция = self.поделить(тип)
        if функция is None or (not (это_список(тип) or это_словарь(тип)) and self.владеющее(узел)):
            return код
        return f"{функция}({код})"

    def равенство(self, тип):
        """Выражение C «a равно b» для значений типа или None, если сравнивать нельзя."""
        if тип in РАВЕНСТВО:
            return РАВЕНСТВО[тип]
        if это_список(тип):
            return f"{self.структура(тип)}_eq(a, b)" if self.сравнимый(тип) else None
        if это_словарь(тип):
            return f"{self.словарь_c(тип)}_eq(a, b)" if self.сравнимый(тип) else None
        if тип in self.перечисления:
            return f"{self.перечисление_c(тип)}_eq(a, b)" if self.сравнимый(тип) else None
        if это_необяз(тип) or тип in self.собственные:
            if self.сравнимый(тип):
                return f"{self.значимый(тип)}_eq(a, b)"
        return None

    def сравнимый(self, тип, в_работе=None):
        в_работе = в_работе if в_работе is not None else set()
        if тип in РАВЕНСТВО:
            return True
        if это_список(тип):
            return self.сравнимый(элемент(тип), в_работе)
        if это_словарь(тип):
            return self.сравнимый(значение_словаря(тип), в_работе)
        if тип in self.перечисления:
            if тип in в_работе:
                return True
            в_работе.add(тип)
            return all(self.сравнимый(т, в_работе) for _, поля in self.перечисления[тип] for _, т in поля)
        if это_необяз(тип):
            return self.сравнимый(основа(тип), в_работе)
        if тип in self.собственные:
            if тип in в_работе:
                return True
            в_работе.add(тип)
            return all(self.сравнимый(т, в_работе) for _, т, _ in self.собственные[тип])
        return False

    def в_строку(self, тип, код):
        """Код C, превращающий значение в строку так, как его печатает «вывод»."""
        if тип == СТР:
            return код
        if это_список(тип):
            return f"{self.структура(тип)}_str({код})"
        if это_словарь(тип):
            return f"{self.словарь_c(тип)}_str({код})"
        if это_необяз(тип) or это_функция(тип) or тип in self.собственные:
            return f"{self.значимый(тип)}_str({код})"
        if тип in self.перечисления:
            return f"{self.перечисление_c(тип)}_str({код})"
        return self._преобразование(СТР, тип, код, 0)
    # ---------- типы ----------

    def перечисление_c(self, тип):
        if тип not in self.перечисления_c:
            self.перечисления_c[тип] = "dv_" + self.имя_типа(тип)
            for _, поля in self.перечисления[тип]:
                for _, т in поля:
                    self.тип_c(т)
        return self.перечисления_c[тип]

    def тип_c(self, тип):
        if тип in self.перечисления:
            return self.перечисление_c(тип)
        if это_список(тип):
            return self.структура(тип) + "*"
        if это_словарь(тип):
            return self.словарь_c(тип) + "*"
        if это_необяз(тип) or это_результат(тип) or это_функция(тип) or тип in self.собственные:
            return self.значимый(тип)
        if тип == МН:
            self.включить_математику()
        return БАЗОВЫЕ_C[тип]

    def включить_данные(self):
        if self.нужны_данные:
            return
        self.нужны_данные = True
        for тип_списка in ("[цел]", "[стр]"):
            self.структура(тип_списка)

    def включить_математику(self):
        """Библиотека и многочлены нужны: регистрируются все используемые ими типы списков."""
        if self.нужна_математика:
            return
        self.нужна_математика = True
        for тип_списка in ("[цел]", "[рац]", "[дроб]", "[стр]", "[мн]", "[[рац]]"):
            self.структура(тип_списка)
        for тип in vysshaya.НУЖНЫЕ_ТИПЫ:
            self.тип_c(тип)

    def структура(self, тип):
        if тип not in self.списки:
            тип_эл = элемент(тип)
            if это_список(тип_эл):
                self.структура(тип_эл)
            self.списки[тип] = "dl_" + self.код_типа(тип)
        return self.списки[тип]

    def запись(self, тип, в_списке=False):
        """Функция C, которая печатает значение этого типа без перевода строки."""
        if это_список(тип):
            return self.структура(тип) + "_write"
        if это_словарь(тип):
            return self.словарь_c(тип) + "_write"
        if это_функция(тип):
            return self.значимый(тип) + "_write"
        if тип in self.перечисления:
            return self.перечисление_c(тип) + "_write"
        if это_необяз(тип):
            return self.значимый(тип) + ("_qwrite" if в_списке else "_write")
        if тип in self.собственные:
            return self.значимый(тип) + "_write"
        if тип == СТР and в_списке:
            return "dan_write_quoted"
        return ЗАПИСЬ[тип]

    def строка_элемента(self, тип):
        """Функция C, которая превращает элемент списка в строку (как при выводе)."""
        if это_список(тип):
            return self.структура(тип) + "_str"
        if это_словарь(тип):
            return self.словарь_c(тип) + "_str"
        if это_функция(тип):
            return self.значимый(тип) + "_str"
        if тип in self.перечисления:
            return self.перечисление_c(тип) + "_str"
        if это_необяз(тип):
            return self.значимый(тип) + "_qstr"
        if тип in self.собственные:
            return self.значимый(тип) + "_str"
        return {ЦЕЛ: "dan_int_to_str", ДРОБ: "dan_float_to_str", РАЦ: "dan_rat_to_str",
                ЛОГ: "dan_bool_to_str", СТР: "dan_quote_str", МН: "dan_poly_to_str",
                БАЙТЫ: "dan_bytes_show"}[тип]

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
        self.собственные = getattr(программа, "типы", {}) or {}
        self.перечисления = getattr(программа, "перечисления", {}) or {}
        self.написания = getattr(программа, "написания", {}) or {}
        функции = [и for и in программа.инструкции if isinstance(и, у.Функция)]
        self.рекурсивные = рекурсивные_функции(функции)
        прочие = [и for и in программа.инструкции if not isinstance(и, у.Функция)]

        # Сначала генерируется тело программы: при этом выясняется,
        # какие типы списков нужны, и их определения ставятся выше.
        if функции:
            self.пиши("/* ---- program functions ---- */")
            for ф in функции:
                self.пиши(self._заголовок(ф) + ";")
            self.пиши()
        место_лямбд = len(self.строки)
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
        if self.лямбды_код:
            self.строки[место_лямбд:место_лямбд] = ["/* ---- anonymous functions ---- */"] + self.лямбды_код

        шапка = [
            "/* Создано компилятором языка Дан. Редактировать не нужно. */",
            "#include <stdint.h>", "#include <inttypes.h>", "#include <stdbool.h>",
            "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
            "#include <errno.h>", "#include <math.h>", "#include <time.h>", "#include <sys/stat.h>",
            "#ifndef _WIN32", "#include <dirent.h>", "#include <sys/resource.h>", "#endif",
            "#ifdef _WIN32", "#include <windows.h>", "#endif", "",
        ]
        шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in СООБЩЕНИЯ.items()]
        if self.нужны_данные:
            шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in dannye.СООБЩЕНИЯ.items()]
        if self.нужна_математика:
            шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in matematika.СООБЩЕНИЯ.items()]
            шапка += [f"#define {имя} {строка_c(текст)}" for имя, текст in matematika.ИМЕНА_C.items()]
        файлы = getattr(программа, "файлы", None) or ["программа.dan"]
        шапка += ["", f"#define DAN_FILE_STEP {у.ШАГ_ФАЙЛА}", f"#define DAN_FILE_COUNT {len(файлы)}",
                  "static const char *const dan_files[] = {" + ", ".join(строка_c(ф) for ф in файлы) + "};"]
        части = ["\n".join(шапка), СРЕДА]
        self._собрать_все_типы()
        части.append(self._определения_типов())
        for имя in self.словари.values():
            части.append(ОБЪЯВЛЕНИЯ_СЛОВАРЯ.format(D=имя))
            for тип, и in self.словари.items():
                if и == имя and self.сравнимый(тип):
                    части.append(f"static bool {имя}_eq(const {имя} *x, const {имя} *y);")
        for тип, имя in self.списки.items():
            тип_эл = элемент(тип)
            поделить = self.поделить(тип_эл)
            поделить_элементы = (f"for (int64_t i = 0; i < r->len; i++) r->items[i] = {поделить}(r->items[i]);"
                                 if поделить else "")
            части.append(ШАБЛОН_СПИСКА.format(
                описание=тип, S=имя, T=self.тип_c(тип_эл),
                запись_элемента=self.запись(тип_эл, в_списке=True),
                строка_элемента=self.строка_элемента(тип_эл),
                поделить_элементы=поделить_элементы))
            равенство = self.равенство(тип_эл)
            if равенство:
                части.append(ШАБЛОН_ПОИСКА.format(S=имя, T=self.тип_c(тип_эл), EQ=равенство))
            if тип_эл == СТР:
                части.append(КОД_СТРОК_СО_СПИСКАМИ)
        if self.словари:
            части.append(КОД_ХЕШЕЙ)
        for тип, имя in self.словари.items():
            к, з = ключ_словаря(тип), значение_словаря(тип)
            необяз = з if это_необяз(з) else з + "?"
            поделить = self.поделить(з)
            найти = "" if это_функция(з) else ШАБЛОН_ПОИСКА_В_СЛОВАРЕ.format(
                D=имя, K=self.тип_c(к), HASH=ХЕШИ[к], O=self.тип_c(необяз),
                O_SOME=(self.тип_c(необяз) + "_some") if not это_необяз(з) else "")
            части.append(ШАБЛОН_СЛОВАРЯ.format(
                описание=тип, D=имя, K=self.тип_c(к), V=self.тип_c(з),
                EQ_K=self.равенство(к), HASH=ХЕШИ[к], СТРОКА_К=self.строка_элемента(к),
                СТРОКА_З=self.строка_элемента(з), НАЙТИ=найти,
                LK=self.структура("[" + к + "]"), LV=self.структура("[" + з + "]"),
                ПОДЕЛИТЬ=поделить or ""))
            if self.сравнимый(тип):
                части.append(ШАБЛОН_РАВЕНСТВА_СЛОВАРЯ.format(D=имя, V=self.тип_c(з), EQ_V=self.равенство(з)))
        части.append(self._функции_типов())
        части.append(self._функции_перечислений())
        части.append(self._функции_расширения())
        части.append("\n".join(self.помощники.values()))
        if self.нужны_данные:
            части.append(dannye.код_c())
        if self.нужна_математика:
            части.append(matematika.КОД_C)
            части.append(matematika.код_c_дополнения())
            части.append(matematika._код_многочленов_строки())
            части.append(matematika.КОД_МНОГОЧЛЕНОВ)
            части.append(vysshaya.КОД_C)
        части.append("\n".join(self.строки))
        return "\n".join(части) + "\n"

    # ---------- собственные и необязательные типы ----------

    def _собрать_все_типы(self):
        """Регистрирует все типы, которые понадобятся определениям (до неподвижной точки)."""
        while True:
            размер = (len(self.списки), len(self.значимые), len(self.словари), len(self.перечисления_c))
            for тип in list(self.перечисления_c):
                for _, поля in self.перечисления[тип]:
                    for _, т in поля:
                        self.тип_c(т)
                        self.строка_элемента(т)
                        self.равенство(т)
                        self.поделить(т)
            for тип in list(self.словари):
                for т in (ключ_словаря(тип), значение_словаря(тип)):
                    self.тип_c(т)
                    self.строка_элемента(т)
                    self.равенство(т)
                    self.поделить(т)
            for тип in list(self.списки):
                тип_эл = элемент(тип)
                self.тип_c(тип_эл)
                self.строка_элемента(тип_эл)
                self.равенство(тип_эл)
                self.поделить(тип_эл)
            for тип in list(self.значимые):
                if это_функция(тип):
                    continue
                if это_результат(тип):
                    if основа(тип) != ПУСТО:
                        self.тип_c(основа(тип))
                        self.поделить(основа(тип))
                    continue
                поля = [основа(тип)] if это_необяз(тип) else [т for _, т, _ in self.собственные[тип]]
                for т in поля:
                    self.тип_c(т)
                    self.строка_элемента(т)
                    self.в_строку(т, "x")
                    self.равенство(т)
                    self.поделить(т)
            if (len(self.списки), len(self.значимые), len(self.словари), len(self.перечисления_c)) == размер:
                return

    def _определения_типов(self):
        строки = ["/* ---- types of the program ---- */"]
        for имя in list(self.списки.values()) + list(self.словари.values()):
            строки.append(f"typedef struct {имя} {имя};")
        for имя in self.перечисления_c.values():
            строки.append(f"typedef struct {имя}_s {имя}_s;")
            строки.append(f"typedef const {имя}_s *{имя};")
        готовые = set()
        for тип, имя in self.значимые.items():
            if self.по_ссылке(тип):
                строки.append(f"typedef const struct {self.значимый(основа(тип))} *{имя};")
                готовые.add(тип)

        def определить(тип):
            if тип in готовые:
                return
            готовые.add(тип)
            имя = self.значимые[тип]
            if это_функция(тип):
                параметры, результат = разобрать_тип_функции(тип)
                for т in параметры + [результат]:
                    if т in self.значимые:
                        определить(т)
                р = self.тип_c(результат)
                п = "".join(f", {self.тип_c(т)}" for т in параметры)
                строки.append(f"typedef struct {{ {р} (*f)(void *{п}); void *env; }} {имя};")
                аргументы = "".join(f", {self.тип_c(т)} a{и}" for и, т in enumerate(параметры))
                передать = "".join(f", a{и}" for и in range(len(параметры)))
                возврат = "" if р == "void" else "return "
                строки.append(f"static inline {р} {имя}_call({имя} v{аргументы}) {{ {возврат}v.f(v.env{передать}); }}")
                строки.append(f"static inline dan_str {имя}_str({имя} v) {{ (void)v; return {строка_c('<функция>')}; }}")
                строки.append(f"static inline void {имя}_write({имя} v) {{ fputs({имя}_str(v), stdout); }}")
                return
            if это_необяз(тип) or это_результат(тип):
                зависимости = [основа(тип)]
            else:
                зависимости = [т for _, т, _ in self.собственные[тип]]
            for т in зависимости:
                if т in self.значимые:
                    определить(т)
            if это_результат(тип):
                значение = "" if основа(тип) == ПУСТО else f" {self.тип_c(основа(тип))} value;"
                строки.append(f"typedef struct {{ bool ok;{значение} dan_str err; int line; }} {имя};")
            elif это_необяз(тип):
                строки.append(f"typedef struct {{ bool has; {self.тип_c(основа(тип))} value; }} {имя};")
            else:
                поля = " ".join(f"{self.тип_c(т)} {self.поле_c(тип, п)};" for п, т, _ in self.собственные[тип])
                строки.append(f"typedef struct {имя} {{ {поля} }} {имя};")

        for тип in list(self.значимые):
            определить(тип)
        # перечисления: значения - указатели на неизменяемые ячейки (деревья любой глубины)
        for тип, имя in self.перечисления_c.items():
            варианты = []
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                if поля:
                    содержимое = " ".join(f"{self.тип_c(т)} {self.поле_c((тип, вариант), п)};" for п, т in поля)
                    варианты.append(f"struct {{ {содержимое} }} v{номер};")
            объединение = f" union {{ {' '.join(варианты)} }} u;" if варианты else ""
            строки.append(f"struct {имя}_s {{ int tag;{объединение} }};")
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                параметры = ", ".join(f"{self.тип_c(т)} a{и}" for и, (_, т) in enumerate(поля)) or "void"
                присвоить = " ".join(f"c->u.v{номер}.{self.поле_c((тип, вариант), п)} = a{и};"
                                     for и, (п, _) in enumerate(поля))
                строки.append(f"static {имя} {имя}_mk{номер}({параметры}) {{ {имя}_s *c = dan_alloc(sizeof *c); "
                              f"c->tag = {номер}; {присвоить} return c; }}")
            строки.append(f"static dan_str {имя}_str({имя} v);")
            строки.append(f"static void {имя}_write({имя} v);")
            if self.сравнимый(тип):
                строки.append(f"static bool {имя}_eq({имя} a, {имя} b);")
        for тип, имя in self.значимые.items():
            if это_функция(тип):
                continue
            if это_результат(тип):
                if основа(тип) == ПУСТО:
                    строки += [
                        f"static inline bool {имя}_has({имя} o) {{ return o.ok; }}",
                        f"static inline {имя} {имя}_some(void) {{ return ({имя}){{ .ok = true, .err = NULL, .line = 0 }}; }}",
                    ]
                else:
                    т_c = self.тип_c(основа(тип))
                    строки += [
                        f"static inline bool {имя}_has({имя} o) {{ return o.ok; }}",
                        f"static inline {т_c} {имя}_val({имя} o) {{ return o.value; }}",
                        f"static inline {имя} {имя}_some({т_c} v) {{ return ({имя}){{ .ok = true, .value = v, .err = NULL, .line = 0 }}; }}",
                    ]
                строки.append(f"static inline {имя} {имя}_err(dan_str e, int line) {{ {имя} r; memset(&r, 0, sizeof r); "
                              f"r.ok = false; r.err = e; r.line = line; return r; }}")
                if self.поделить(тип):
                    строки.append(f"static {имя} {имя}_share({имя} v);")
                continue
            if это_необяз(тип):
                т_c = self.тип_c(основа(тип))
                if self.по_ссылке(тип):
                    строки += [
                        f"static inline bool {имя}_has({имя} o) {{ return o != NULL; }}",
                        f"static inline {т_c} {имя}_val({имя} o) {{ return *o; }}",
                        f"static inline {имя} {имя}_none(void) {{ return NULL; }}",
                        f"static {имя} {имя}_some({т_c} v) {{ {т_c} *p = dan_alloc(sizeof *p); *p = v; return p; }}",
                    ]
                else:
                    строки += [
                        f"static inline bool {имя}_has({имя} o) {{ return o.has; }}",
                        f"static inline {т_c} {имя}_val({имя} o) {{ return o.value; }}",
                        f"static inline {имя} {имя}_none(void) {{ return ({имя}){{ .has = false }}; }}",
                        f"static inline {имя} {имя}_some({т_c} v) {{ return ({имя}){{ .has = true, .value = v }}; }}",
                    ]
            строки.append(f"static dan_str {имя}_str({имя} v);")
            if not это_необяз(тип):
                строки.append(f"static void {имя}_strb({имя} v, dan_buf *b);")
            строки.append(f"static void {имя}_write({имя} v);")
            if это_необяз(тип):
                строки.append(f"static dan_str {имя}_qstr({имя} v);")
                строки.append(f"static void {имя}_qwrite({имя} v);")
            if self.сравнимый(тип):
                строки.append(f"static bool {имя}_eq({имя} a, {имя} b);")
            if self.поделить(тип):
                строки.append(f"static {имя} {имя}_share({имя} v);")
        return "\n".join(строки) + "\n"

    def _рекурсивные_типы(self):
        """Собственные типы, значения которых могут содержать значения того же типа."""
        def прямые(тип):
            итог = set()
            for _, т, _ in self.собственные[тип]:
                while это_список(т) or это_необяз(т):
                    т = элемент(т) if это_список(т) else основа(т)
                if т in self.собственные:
                    итог.add(т)
            return итог
        рекурсивные = set()
        for начало in self.собственные:
            увиденные, очередь = set(), list(прямые(начало))
            while очередь:
                т = очередь.pop()
                if т == начало:
                    рекурсивные.add(начало)
                    break
                if т not in увиденные:
                    увиденные.add(т)
                    очередь.extend(прямые(т))
        return рекурсивные

    def _функции_перечислений(self):
        строки = []
        for тип, имя in self.перечисления_c.items():
            строки.append(f"static dan_str {имя}_str({имя} v) {{")
            строки.append("    DAN_STACK_CHECK(0);")
            строки.append("    dan_buf b = { NULL, 0, 0 };")
            строки.append("    switch (v->tag) {")
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                надпись = self.написания.get((тип, вариант), вариант)
                строки.append(f"    case {номер}:")
                if not поля:
                    строки.append(f"        dan_buf_add(&b, {строка_c(надпись)});")
                else:
                    строки.append(f"        dan_buf_add(&b, {строка_c(надпись + '(')});")
                    for и, (п, т) in enumerate(поля):
                        подпись = (", " if и else "") + self.написания.get((тип, вариант, п), п) + " = "
                        строки.append(f"        dan_buf_add(&b, {строка_c(подпись)});")
                        строки.append(f"        dan_buf_add(&b, {self.строка_элемента(т)}"
                                      f"(v->u.v{номер}.{self.поле_c((тип, вариант), п)}));")
                    строки.append('        dan_buf_add(&b, ")");')
                строки.append("        break;")
            строки.append("    }")
            строки.append("    return b.s;")
            строки.append("}")
            строки.append(f"static void {имя}_write({имя} v) {{ fputs({имя}_str(v), stdout); }}")
            if self.сравнимый(тип):
                строки.append(f"static bool {имя}_eq({имя} a, {имя} b) {{")
                строки.append("    DAN_STACK_CHECK(0);")
                строки.append("    if (a == b) return true;")
                строки.append("    if (a->tag != b->tag) return false;")
                строки.append("    switch (a->tag) {")
                for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                    части = [self._подставить(self.равенство(т), f"a->u.v{номер}.{self.поле_c((тип, вариант), п)}",
                                              f"b->u.v{номер}.{self.поле_c((тип, вариант), п)}") for п, т in поля]
                    строки.append(f"    case {номер}: return {' && '.join(части) or 'true'};")
                строки.append("    }")
                строки.append("    return true;")
                строки.append("}")
        return "\n".join(строки) + "\n"

    def _функции_типов(self):
        строки = []
        рекурсивные = self._рекурсивные_типы()
        ничего = строка_c("ничего")
        for тип, имя in self.значимые.items():
            if это_функция(тип):
                continue
            if это_результат(тип):
                if self.поделить(тип):
                    строки.append(f"static {имя} {имя}_share({имя} v) {{ if (v.ok) v.value = "
                                  f"{self.поделить(основа(тип))}(v.value); return v; }}")
                continue
            строки.append(f"/* ---- {тип} ---- */")
            if это_необяз(тип):
                т = основа(тип)
                знач = f"{имя}_val(v)"
                строки.append(f"static dan_str {имя}_str({имя} v) {{ return {имя}_has(v) ? {self.в_строку(т, знач)} : {ничего}; }}")
                строки.append(f"static dan_str {имя}_qstr({имя} v) {{ return {имя}_has(v) ? {self.строка_элемента(т)}({знач}) : {ничего}; }}")
                строки.append(f"static void {имя}_qwrite({имя} v) {{ fputs({имя}_qstr(v), stdout); }}")
                if self.сравнимый(тип):
                    равно = self._подставить(self.равенство(т), f"{имя}_val(a)", f"{имя}_val(b)")
                    строки.append(f"static bool {имя}_eq({имя} a, {имя} b) {{ return {имя}_has(a) == {имя}_has(b) "
                                  f"&& (!{имя}_has(a) || {равно}); }}")
            else:
                поля = self.собственные[тип]
                проверка = "DAN_STACK_CHECK(0); " if тип in рекурсивные else ""
                # Вложенные значения собственных типов дописываются в общий буфер,
                # поэтому вывод длинной цепочки занимает линейное время.
                строки.append(f"static void {имя}_strb({имя} v, dan_buf *b) {{")
                if проверка:
                    строки.append("    DAN_STACK_CHECK(0);")
                строки.append(f"    dan_buf_add(b, {строка_c(self.написания.get(тип, тип) + '(')});")
                for номер, (поле, т, _) in enumerate(поля):
                    подпись = (", " if номер else "") + self.написания.get((тип, поле), поле) + " = "
                    строки.append(f"    dan_buf_add(b, {строка_c(подпись)});")
                    знач = f"v.{self.поле_c(тип, поле)}"
                    if т in self.собственные:
                        строки.append(f"    {self.значимый(т)}_strb({знач}, b);")
                    elif это_необяз(т) and основа(т) in self.собственные:
                        о = self.значимый(т)
                        строки.append(f"    if ({о}_has({знач})) {self.значимый(основа(т))}_strb({о}_val({знач}), b); "
                                      f"else dan_buf_add(b, {ничего});")
                    else:
                        строки.append(f"    dan_buf_add(b, {self.строка_элемента(т)}({знач}));")
                строки.append('    dan_buf_add(b, ")");')
                строки.append("}")
                строки.append(f"static dan_str {имя}_str({имя} v) {{ dan_buf b = {{ NULL, 0, 0 }}; "
                              f"{имя}_strb(v, &b); return b.s; }}")
                if self.сравнимый(тип):
                    части = []
                    for поле, т, _ in поля:
                        ф = self.поле_c(тип, поле)
                        части.append(self._подставить(self.равенство(т), f"a.{ф}", f"b.{ф}"))
                    строки.append(f"static bool {имя}_eq({имя} a, {имя} b) {{ {проверка}return {' && '.join(части)}; }}")
            строки.append(f"static void {имя}_write({имя} v) {{ fputs({имя}_str(v), stdout); }}")
            if self.поделить(тип):
                if это_необяз(тип):
                    строки.append(f"static {имя} {имя}_share({имя} v) {{ if (v.has) v.value = "
                                  f"{self.поделить(основа(тип))}(v.value); return v; }}")
                else:
                    поля = [(self.поле_c(тип, п), self.поделить(т)) for п, т, _ in self.собственные[тип]]
                    тело = " ".join(f"v.{ф} = {д}(v.{ф});" for ф, д in поля if д)
                    проверка = "DAN_STACK_CHECK(0); " if тип in рекурсивные else ""
                    строки.append(f"static {имя} {имя}_share({имя} v) {{ {проверка}{тело} return v; }}")
        return "\n".join(строки) + "\n"

    @staticmethod
    def _подставить(шаблон, а, б):
        """Подставляет операнды в шаблон равенства вида «f(a, b)» или «(a == b)»."""
        if шаблон.endswith("(a, b)"):
            return шаблон[:-len("(a, b)")] + f"({а}, {б})"
        if шаблон == "(a == b)":
            return f"({а} == {б})"
        if шаблон == "(dan_rat_cmp(a, b) == 0)":
            return f"(dan_rat_cmp({а}, {б}) == 0)"
        raise ValueError(шаблон)

    def _заголовок(self, ф):
        результат = self.тип_c(ф.тип_результата) if ф.тип_результата else "void"
        параметры = ", ".join(f"{self.тип_c(п.тип)} const {self.имя(п.имя)}" for п in ф.параметры)
        return f"static {результат} {self.имя(ф.имя)}({параметры or 'void'})"

    def функция(self, ф):
        self.пиши(f"/* line {ф.строка} */")
        self.пиши(self._заголовок(ф) + " {")
        self.отступ += 1
        if ф.имя in self.рекурсивные:
            self.пиши(f"DAN_STACK_CHECK({ф.строка});")
        self.текущий_результат = ф.тип_результата
        for и in ф.тело.инструкции:
            self.инструкция(и)
        if ф.тип_результата == ПУСТО + "!":
            self.пиши(f"return {self.тип_c(ф.тип_результата)}_some();")
        self.текущий_результат = None
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
        значение = self.сохранить(и.значение, self.выр(и.значение), и.тип_итог)
        self.пиши(f"{self.тип_c(и.тип_итог)}{модификатор} {self.имя(и.имя)} = {значение};")

    def и_Присваивание(self, и):
        цель = self.имя(и.имя)
        значение = self.выр(и.значение)
        if и.оператор == "=":
            значение = self.сохранить(и.значение, значение, и.тип_цели)
            if это_список(и.тип_цели):
                # прежний список больше не хранится в этой переменной
                прежний = self.служебное_имя("old")
                self.пиши(f"{{ {self.тип_c(и.тип_цели)} {прежний} = {цель}; {цель} = {значение}; "
                          f"{self.структура(и.тип_цели)}_release({прежний}); }}")
            else:
                self.пиши(f"{цель} = {значение};")
            return
        результат = self._арифметика(и.оператор[0], цель, значение, и.тип_результата, и.строка)
        self.пиши(f"{цель} = {результат};")

    def и_ПрисваиваниеЭлемента(self, и):
        if getattr(и, "словарь", False):
            return self._присвоить_в_словарь(и)
        тип_списка = и.объект.тип_выр
        структура = self.структура(тип_списка)
        объект = self.служебное_имя("list")
        индекс = self.служебное_имя("i")
        # Список и индекс вычисляются один раз, даже при «с[ф()] += 1».
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"{структура}* const {объект} = {структура}_mut(&{self.место(и.объект)});")
        self.пиши(f"const int64_t {индекс} = {self.выр(и.индекс)};")
        значение = self.сохранить(и.значение, self.выр(и.значение), и.тип_элемента)
        if и.оператор != "=":
            текущее = f"{структура}_get({объект}, {индекс}, {и.строка})"
            значение = self._арифметика(и.оператор[0], текущее, значение, и.тип_результата, и.строка)
        self.пиши(f"{структура}_set({объект}, {индекс}, {значение}, {и.строка});")
        self.отступ -= 1
        self.пиши("}")

    def _присвоить_в_словарь(self, и):
        тип = и.объект.тип_выр
        D = self.словарь_c(тип)
        словарь = self.служебное_имя("dict")
        ключ = self.служебное_имя("key")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"{D}* const {словарь} = {D}_mut(&{self.место(и.объект)});")
        self.пиши(f"const {self.тип_c(ключ_словаря(тип))} {ключ} = {self.выр(и.индекс)};")
        значение = self.сохранить(и.значение, self.выр(и.значение), и.тип_элемента)
        if и.оператор == "=":
            self.пиши(f"{D}_put({словарь}, {ключ}, {значение});")
        else:
            место = self.служебное_имя("p")
            self.пиши(f"{self.тип_c(и.тип_элемента)} *const {место} = {D}_at({словарь}, {ключ}, {и.строка});")
            результат = self._арифметика(и.оператор[0], f"*{место}", значение, и.тип_результата, и.строка)
            self.пиши(f"*{место} = {результат};")
        self.отступ -= 1
        self.пиши("}")

    def и_ОбъявлениеТипа(self, и):
        pass                       # определение типа выводится в начале файла C

    def и_ОбъявлениеВариантов(self, и):
        pass

    def и_Выбор(self, и):
        тип = и.значение.тип_выр
        значение = self.служебное_имя("sel")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"{self.тип_c(тип)} const {значение} = {self.выр(и.значение)};")
        первая = True
        for ветвь in и.ветви:
            условия, привязки = [], []
            for образец in ветвь.образцы:
                if тип in self.перечисления:
                    варианты = self.перечисления[тип]
                    номер = [к for к, (н, _) in enumerate(варианты) if н == образец.имя][0]
                    условия.append(f"{значение}->tag == {номер}")
                    for имя, (п, т) in zip(образец.привязки, варианты[номер][1]):
                        if имя != "_":
                            привязки.append(f"{self.тип_c(т)} const {self.имя(имя)} = "
                                            f"{значение}->u.v{номер}.{self.поле_c((тип, образец.имя), п)};")
                elif isinstance(образец, у.Диапазон):
                    условия.append(f"({значение} >= {self.выр(образец.начало)} && {значение} <= {self.выр(образец.конец)})")
                else:
                    код = self.выр(образец)
                    if тип == СТР:
                        условия.append(f"dan_str_eq({значение}, {код})")
                    elif тип == РАЦ:
                        условия.append(f"dan_rat_cmp({значение}, {код}) == 0")
                    else:
                        условия.append(f"{значение} == {код}")
            начало = "if" if первая else "} else if"
            первая = False
            self.пиши(f"{начало} ({' || '.join(условия)}) {{")
            self.отступ += 1
            for строка in привязки:
                self.пиши(строка)
            self.отступ -= 1
            self.блок(ветвь.тело)
        if и.иначе is not None:
            self.пиши("} else {" if not первая else "{")
            self.блок(и.иначе)
        self.пиши("}")
        self.отступ -= 1
        self.пиши("}")

    def место(self, в):
        """Выражение C, обозначающее место в памяти (для изменения поля)."""
        if isinstance(в, у.Имя):
            return self.имя(в.имя)
        if isinstance(в, у.Поле):
            return f"{self.место(в.объект)}.{self.поле_c(в.объект.тип_выр, в.имя)}"
        if это_словарь(в.объект.тип_выр):
            D = self.словарь_c(в.объект.тип_выр)
            return f"(*{D}_at({D}_mut(&{self.место(в.объект)}), {self.выр(в.индекс)}, {в.строка}))"
        структура = self.структура(в.объект.тип_выр)
        return (f"(*{структура}_at({структура}_mut(&{self.место(в.объект)}), "
                f"{self.выр(в.индекс)}, {в.строка}))")

    def и_ПрисваиваниеПоля(self, и):
        значение = self.служебное_имя("v")
        место = self.служебное_имя("p")
        self.пиши("{")
        self.отступ += 1
        # Сначала вычисляется новое значение, затем находится поле: вычисление
        # значения может, например, добавить элементы в список с этим полем.
        self.пиши(f"__auto_type const {значение} = "
                  f"{self.сохранить(и.значение, self.выр(и.значение), и.тип_поля)};")
        поле = у.Поле(и.объект, и.имя, строка=и.строка, столбец=и.столбец)
        self.пиши(f"{self.тип_c(и.тип_поля)} *const {место} = &{self.место(поле)};")
        if и.оператор == "=":
            self.пиши(f"*{место} = {значение};")
        else:
            результат = self._арифметика(и.оператор[0], f"*{место}", значение, и.тип_результата, и.строка)
            self.пиши(f"*{место} = {результат};")
        self.отступ -= 1
        self.пиши("}")

    def и_ЕслиЕсть(self, и):
        тип = и.значение.тип_выр
        временное = self.служебное_имя("opt")
        self.пиши("{")
        self.отступ += 1
        тип_c = self.тип_c(тип)
        self.пиши(f"const {тип_c} {временное} = {self.выр(и.значение)};")
        self.пиши(f"if ({тип_c}_has({временное})) {{")
        self.отступ += 1
        self.пиши(f"{self.тип_c(основа(тип))} const {self.имя(и.имя)} = {тип_c}_val({временное});")
        self.отступ -= 1
        self.блок(и.тогда)
        if getattr(и, "имя_ошибки", None):
            self.пиши("} else {")
            self.отступ += 1
            self.пиши(f"dan_str const {self.имя(и.имя_ошибки)} = {временное}.err;")
            self.отступ -= 1
            self.блок(и.иначе)
            self.пиши("}")
        else:
            self._иначе_ветвь(и)
        self.отступ -= 1
        self.пиши("}")

    def и_ЕслиОшибка(self, и):
        тип = и.значение.тип_выр
        временное = self.служебное_имя("res")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"const {self.тип_c(тип)} {временное} = {self.выр(и.значение)};")
        self.пиши(f"if (!{временное}.ok) {{")
        self.отступ += 1
        self.пиши(f"dan_str const {self.имя(и.имя)} = {временное}.err;")
        self.отступ -= 1
        self.блок(и.тогда)
        self._иначе_ветвь(и)
        self.отступ -= 1
        self.пиши("}")

    def _иначе_ветвь(self, и):
        """Ветвь «иначе» после «если пусть»: блок или вложенное условие."""
        if и.иначе is None:
            self.пиши("}")
            return
        self.пиши("} else {")
        if isinstance(и.иначе, (у.Если, у.ЕслиЕсть, у.ЕслиОшибка)):
            self.отступ += 1
            self.инструкция(и.иначе)
            self.отступ -= 1
        else:
            self.блок(и.иначе)
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
        if isinstance(и.иначе, (у.ЕслиЕсть, у.ЕслиОшибка)):
            self._иначе_ветвь(и)
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
            первое = self.служебное_имя("from")
            число = self.служебное_имя("count")
            шаг = self.служебное_имя("k")
            начало = self.выр(и.источник.начало)
            граница = self.выр(и.источник.конец)
            # Границы вычисляются один раз до начала цикла. Счёт идёт по
            # беззнаковому номеру шага, поэтому цикл до самого большого
            # целого числа завершается, а не переполняется.
            self.пиши("{")
            self.отступ += 1
            self.пиши(f"const int64_t {первое} = {начало}, {число}_last = {граница};")
            self.пиши(f"const uint64_t {число} = {число}_last < {первое} ? 0 : "
                      f"(uint64_t){число}_last - (uint64_t){первое} + 1;")
            self.пиши(f"for (uint64_t {шаг} = 0; {шаг} < {число}; {шаг}++) {{")
            self.отступ += 1
            self.пиши(f"const int64_t {перем} = (int64_t)((uint64_t){первое} + {шаг});")
            self.отступ -= 1
            self.блок(и.тело)
            self.пиши("}")
            self.отступ -= 1
            self.пиши("}")
            return
        if getattr(и, "по_строке", False):
            структура = self.структура("[стр]")
            источник = f"dan_str_chars({self.выр(и.источник)})"
        elif getattr(и, "по_словарю", False):
            тип = и.источник.тип_выр
            структура = self.структура("[" + ключ_словаря(тип) + "]")
            источник = f"{self.словарь_c(тип)}_keys({self.выр(и.источник)})"
        else:
            структура = self.структура(и.источник.тип_выр)
            источник = self.выр(и.источник)
        список = self.служебное_имя("list")
        номер = self.служебное_имя("i")
        self.пиши("{")
        self.отступ += 1
        # Цикл перебирает значение списка на момент начала: если список
        # изменится в теле цикла, изменение получит собственную копию.
        self.пиши(f"{структура}* const {список} = {структура}_share({источник});")
        self.пиши(f"for (int64_t {номер} = 0; {номер} < {список}->len; {номер}++) {{")
        self.отступ += 1
        self.пиши(f"{self.тип_c(и.тип_элемента)} const {перем} = {список}->items[{номер}];")
        self.отступ -= 1
        self.блок(и.тело)
        self.пиши("}")
        self.пиши(f"{структура}_release({список});")
        self.отступ -= 1
        self.пиши("}")

    def и_Вернуть(self, и):
        if и.значение is None:
            if getattr(self, "текущий_результат", None) == ПУСТО + "!":
                self.пиши(f"return {self.тип_c(ПУСТО + '!')}_some();")
            else:
                self.пиши("return;")
            return
        код = self.выр(и.значение)
        тип = getattr(и.значение, "привести_к", None) or и.значение.тип_выр
        if getattr(и.значение, "в_необяз", None):
            тип = и.значение.в_необяз
        if getattr(и.значение, "в_результат", None):
            тип = и.значение.в_результат
        if тип == ОШИБКА:
            pass
        elif self.содержит_списки(тип):
            if getattr(и, "перемещение", False) and not getattr(и.значение, "в_необяз", None) \
                    and not getattr(и.значение, "в_результат", None):
                # переменная функции перестаёт существовать: её место хранения освобождается
                if это_список(тип):
                    код = f"{self.структура(тип)}_release({код})"
            elif not это_список(тип) and not self.владеющее(и.значение):
                код = f"{self.поделить(тип)}({код})"
        self.пиши(f"return {код};")

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
        необяз = getattr(в, "в_необяз", None)
        if необяз:
            тип_c = self.тип_c(необяз)
            if в.тип_выр == НИЧЕГО:
                код = f"{тип_c}_none()"
            else:
                if self.по_ссылке(необяз):   # значение помещается в неизменяемую ячейку
                    код = self.сохранить(в, код, основа(необяз))
                код = f"{тип_c}_some({код})"
        результат = getattr(в, "в_результат", None)
        if результат:
            тип_c = self.тип_c(результат)
            if в.тип_выр == ОШИБКА:
                код = f"{тип_c}_err({код}, {в.строка})"
            else:
                код = f"{тип_c}_some({код})"
        return код

    def в_Передать(self, в):
        тип = в.значение.тип_выр
        тип_c = self.тип_c(тип)
        временное = self.служебное_имя("res")
        функция = getattr(в, "функция_результат", None)
        if функция:
            при_ошибке = f"return {self.тип_c(функция)}_err({временное}.err, {временное}.line);"
        else:
            при_ошибке = f"dan_error({временное}.err, {временное}.line);"
        значение = "(void)0" if основа(тип) == ПУСТО else f"{временное}.value"
        return (f"({{ const {тип_c} {временное} = {self.выр(в.значение)}; "
                f"if (!{временное}.ok) {{ {при_ошибке} }} {значение}; }})")

    def в_Лямбда(self, в):
        тип = в.тип_выр
        df = self.тип_c(тип)
        параметры, результат = разобрать_тип_функции(тип)
        self.счётчик += 1
        номер = self.счётчик
        имя_c = f"dan_lambda_{номер}"
        захваты = list(getattr(в, "захваты", {}).items())
        сохр = (self.строки, self.отступ, getattr(self, "текущий_результат", None))
        self.строки, self.отступ, self.текущий_результат = [], 1, результат
        if захваты:
            self.пиши(f"const dan_env_{номер} *env = envp;")
            for имя, т in захваты:
                self.пиши(f"{self.тип_c(т)} const {self.имя(имя)} = env->{self.имя(имя)};")
        else:
            self.пиши("(void)envp;")
        if в.выражение is not None:
            код = self.выр(в.выражение)
            if результат == ПУСТО:
                self.пиши(f"{код};")
            else:
                тип_з = getattr(в.выражение, "в_результат", None) or getattr(в.выражение, "в_необяз", None) \
                    or getattr(в.выражение, "привести_к", None) or в.выражение.тип_выр
                if not (это_список(тип_з) or это_словарь(тип_з)) and self.поделить(тип_з) \
                        and not self.владеющее(в.выражение):
                    код = f"{self.поделить(тип_з)}({код})"
                self.пиши(f"return {код};")
        else:
            for и in в.тело.инструкции:
                self.инструкция(и)
            if результат == ПУСТО + "!":
                self.пиши(f"return {self.тип_c(результат)}_some();")
        тело = self.строки
        self.строки, self.отступ, self.текущий_результат = сохр
        параметры_c = "".join(f", {self.тип_c(т)} const {self.имя(п.имя)}" for п, т in zip(в.параметры, параметры))
        код = []
        if захваты:
            поля = " ".join(f"{self.тип_c(т)} {self.имя(имя)};" for имя, т in захваты)
            код.append(f"typedef struct {{ {поля} }} dan_env_{номер};")
        код.append(f"static {self.тип_c(результат)} {имя_c}(void *envp{параметры_c}) {{")
        код += тело
        код.append("}")
        self.лямбды_код += код
        if not захваты:
            return f"(({df}){{ {имя_c}, NULL }})"
        окр = self.служебное_имя("env")
        присвоения = " ".join(f"{окр}->{self.имя(имя)} = {self.сохранить(у.Имя(имя), self.имя(имя), т)};"
                              for имя, т in захваты)
        return (f"({{ dan_env_{номер} *{окр} = dan_alloc(sizeof *{окр}); {присвоения} "
                f"({df}){{ {имя_c}, {окр} }}; }})")

    def обёртка(self, имя_дан, тип):
        """Функция программы как значение: обёртка с общим видом вызова."""
        if имя_дан not in self.обёртки:
            параметры, результат = разобрать_тип_функции(тип)
            имя_c = f"dan_wrap_{self.имя(имя_дан)}"
            аргументы = "".join(f", {self.тип_c(т)} a{и}" for и, т in enumerate(параметры))
            передать = ", ".join(f"a{и}" for и in range(len(параметры)))
            р = self.тип_c(результат)
            возврат = "" if р == "void" else "return "
            self.лямбды_код.append(f"static {р} {имя_c}(void *env{аргументы}) {{ (void)env; "
                                   f"{возврат}{self.имя(имя_дан)}({передать}); }}")
            self.обёртки[имя_дан] = имя_c
        return self.обёртки[имя_дан]

    def в_Ничего(self, в):
        return "0"                 # «ничего» всегда заворачивается в необязательный тип (см. выр)

    def в_Иначе(self, в):
        тип = в.значение.тип_выр
        временное = self.служебное_имя("opt")
        запасное = self.выр(в.запасное)
        тип_c = self.тип_c(тип)
        результат = временное if в.тип_выр == тип else f"{тип_c}_val({временное})"
        return (f"({{ const {тип_c} {временное} = {self.выр(в.значение)}; "
                f"{тип_c}_has({временное}) ? {результат} : ({запасное}); }})")

    def привести(self, код, из_типа, в_тип, строка):
        """Числовое преобразование цел -> рац -> дроб, а также цел, рац -> мн
        и такое же расширение элементов списков: [цел] -> [рац]."""
        if это_список(в_тип):
            return f"{self.расширение(из_типа, в_тип)}({код}, {строка})"
        if в_тип == МН:
            self.включить_математику()
            if из_типа == ЦЕЛ:
                return f"dan_poly_from_int({код}, {строка})"
            return f"dan_poly_const({код})"
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
        if getattr(в, "вариант", None):
            тип, вариант = в.вариант
            номер = [и for и, (н, _) in enumerate(self.перечисления[тип]) if н == вариант][0]
            return f"{self.перечисление_c(тип)}_mk{номер}()"
        if getattr(в, "функция_значение", None):
            return f"(({self.тип_c(в.тип_выр)}){{ {self.обёртка(в.функция_значение, в.тип_выр)}, NULL }})"
        if getattr(в, "константа", None):
            self.тип_c(в.тип_выр)
            return matematika.КОНСТАНТЫ[в.константа][1]
        return self.имя(в.имя)

    def в_Унарная(self, в):
        if в.оператор == "-" and в.тип_выр == РАЦ:
            return f"dan_rat_neg({self.выр(в.операнд)}, {в.строка})"
        if в.оператор == "-" and в.тип_выр == МН:
            return f"dan_poly_neg({self.выр(в.операнд)}, {в.строка})"
        if в.оператор == "-" and в.тип_выр == ЦЕЛ:
            return f"dan_neg({self.выр(в.операнд)}, {в.строка})"
        оп = "-" if в.оператор == "-" else "!"
        return f"({оп}{self.выр(в.операнд)})"

    def в_Бинарная(self, в):
        оп = в.оператор
        if getattr(в, "сравнение_с_ничего", None) is not None:
            узел = в.сравнение_с_ничего
            есть = f"{self.тип_c(узел.тип_выр)}_has({self.выр(узел)})"
            return f"(!{есть})" if оп == "==" else f"({есть})"
        л, п = self.выр(в.левый), self.выр(в.правый)
        if оп in ("+", "-", "*", "/", "%"):
            return self._арифметика(оп, л, п, в.тип_выр, в.строка)
        if оп == "^":
            if в.тип_выр == ЦЕЛ:
                return f"dan_pow_int({л}, {п}, {в.строка})"
            if в.тип_выр == РАЦ:
                return f"dan_rat_pow({л}, {п}, {в.строка})"
            if в.тип_выр == МН:
                return f"dan_poly_pow({л}, {п}, {в.строка})"
            return f"pow({л}, {п})"
        if оп == "и":
            return f"({л} && {п})"
        if оп == "или":
            return f"({л} || {п})"
        if в.левый.тип_выр == СТР and оп not in ("==", "!="):
            return f"(dan_str_cmp({л}, {п}) {оп} 0)"
        if в.левый.тип_выр in (СТР, БАЙТЫ):
            функция = "dan_str_eq" if в.левый.тип_выр == СТР else "dan_bytes_eq"
            равно = f"{функция}({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if это_словарь(getattr(в, "тип_операндов", None) or ""):
            равно = f"{self.словарь_c(в.тип_операндов)}_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if это_список(getattr(в, "тип_операндов", None) or ""):
            равно = f"{self.структура(в.тип_операндов)}_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if getattr(в, "тип_операндов", None) in self.перечисления:
            равно = f"{self.перечисление_c(в.тип_операндов)}_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if getattr(в, "тип_операндов", None) in self.собственные:
            равно = f"{self.значимый(в.тип_операндов)}_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        if getattr(в, "тип_операндов", None) == РАЦ:
            return f"(dan_rat_cmp({л}, {п}) {оп} 0)"
        if getattr(в, "тип_операндов", None) == МН:
            равно = f"dan_poly_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        return f"({л} {оп} {п})"

    def _арифметика(self, оп, л, п, тип, строка):
        """Код «л оп п», где оба операнда уже приведены к типу результата."""
        if тип == СТР:
            return f"dan_concat({л}, {п})"
        if тип == БАЙТЫ:
            return f"dan_bytes_concat({л}, {п})"
        if тип == РАЦ:
            return f"{ОПЕРАЦИИ_РАЦ[оп]}({л}, {п}, {строка})"
        if тип == МН:
            return f"{ОПЕРАЦИИ_МН[оп]}({л}, {п}, {строка})"
        if тип == ЦЕЛ:
            if оп == "/":
                return f"dan_div({л}, {п}, {строка})"
            if оп == "%":
                return f"dan_mod({л}, {п}, {строка})"
            return f"{ЦЕЛЫЕ_ДЕЙСТВИЯ[оп]}({л}, {п}, {строка})"
        return f"({л} {оп} {п})"

    def в_Список(self, в):
        тип = в.тип_выр
        структура = self.структура(тип)
        if not в.элементы:
            return f"{структура}_from(0, NULL)"
        тип_эл = элемент(тип)
        элементы = ", ".join(self.сохранить(э, self.выр(э), тип_эл) for э in в.элементы)
        return f"{структура}_from({len(в.элементы)}, ({self.тип_c(элемент(тип))}[]){{{элементы}}})"

    def в_Словарь(self, в):
        тип = в.тип_выр
        D = self.словарь_c(тип)
        if not в.ключи:
            return f"{D}_new()"
        к, з = ключ_словаря(тип), значение_словаря(тип)
        ключи = ", ".join(self.выр(у_) for у_ in в.ключи)
        значения = ", ".join(self.сохранить(у_, self.выр(у_), з) for у_ in в.значения)
        return (f"{D}_from({len(в.ключи)}, ({self.тип_c(к)}[]){{{ключи}}}, "
                f"({self.тип_c(з)}[]){{{значения}}})")

    def в_Индекс(self, в):
        тип = в.объект.тип_выр
        if это_словарь(тип):
            return f"{self.словарь_c(тип)}_get({self.выр(в.объект)}, {self.выр(в.индекс)}, {в.строка})"
        if getattr(в, "срез", False):
            объект = self.выр(в.объект)
            а, б = self.выр(в.индекс.начало), self.выр(в.индекс.конец)
            if тип == СТР:
                return f"dan_str_slice({объект}, {а}, {б}, {в.строка})"
            if тип == БАЙТЫ:
                self.включить_данные()
                return f"dan_bytes_slice({объект}, {а}, {б}, {в.строка})"
            return f"{self.структура(тип)}_slice({объект}, {а}, {б}, {в.строка})"
        if тип == СТР:
            return f"dan_str_char_at({self.выр(в.объект)}, {self.выр(в.индекс)}, {в.строка})"
        if в.объект.тип_выр == БАЙТЫ:
            return f"dan_bytes_get({self.выр(в.объект)}, {self.выр(в.индекс)}, {в.строка})"
        структура = self.структура(в.объект.тип_выр)
        return f"{структура}_get({self.выр(в.объект)}, {self.выр(в.индекс)}, {в.строка})"

    def в_Поле(self, в):
        тип = в.объект.тип_выр
        if тип in self.собственные:
            return f"({self.выр(в.объект)}).{self.поле_c(тип, в.имя)}"
        if тип == РАЦ:
            return f"({self.выр(в.объект)}).{'n' if в.имя == 'числитель' else 'd'}"
        if тип == МН:
            if в.имя == "степень":
                return f"({self.выр(в.объект)})->deg"
            return f"dan_poly_coeffs({self.выр(в.объект)})"
        if тип == СТР:
            return f"dan_str_len({self.выр(в.объект)})"
        if тип == БАЙТЫ:
            return f"({self.выр(в.объект)})->len"
        if это_словарь(тип):
            return f"({self.выр(в.объект)})->len"
        if not это_список(тип):           # литерал «[]»
            return "INT64_C(0)"
        return f"({self.выр(в.объект)})->len"

    def в_Вызов(self, в):
        if getattr(в, "вызов_значения", None):
            аргументы = "".join(", " + self.выр(а) for а in в.аргументы)
            return f"{self.тип_c(в.вызов_значения)}_call({self.выр(в.функция)}{аргументы})"
        if getattr(в, "метод_функции", None):
            return self._функциональный_метод(в)
        if isinstance(в.функция, у.Поле):    # метод: список.добавить(х), строка.найти(т)
            объект = в.функция.объект
            аргументы = [self.выр(объект)] + [self.выр(а) for а in в.аргументы]
            if getattr(в, "метод_словаря", None):
                тип = объект.тип_выр
                D = self.словарь_c(тип)
                суффикс = в.метод_словаря
                if суффикс == "remove":
                    аргументы[0] = f"{D}_mut(&{self.место(объект)})"
                return f"{D}_{суффикс}({', '.join(аргументы)})"
            if getattr(в, "метод_c", None):
                функция, нужна_строка = в.метод_c
                self.структура("[стр]")       # некоторые методы строк возвращают списки строк
                if нужна_строка:
                    аргументы.append(str(в.строка))
                return f"{функция}({', '.join(аргументы)})"
            суффикс, нужна_строка = в.метод_списка
            структура = self.структура(объект.тип_выр)
            if в.метод in ("добавить", "вставить", "удалить"):
                аргументы[0] = f"{структура}_mut(&{self.место(объект)})"
                if в.метод != "удалить":
                    аргументы[-1] = self.сохранить(в.аргументы[-1], аргументы[-1], элемент(объект.тип_выр))
            if нужна_строка:
                аргументы.append(str(в.строка))
            return f"{структура}_{суффикс}({', '.join(аргументы)})"

        имя = в.функция.имя
        if getattr(в, "вариант", None):
            тип, вариант = в.вариант
            варианты = self.перечисления[тип]
            номер = [и for и, (н, _) in enumerate(варианты) if н == вариант][0]
            значения = ", ".join(self.сохранить(узел, self.выр(узел), т)
                                 for (_, т), узел in zip(варианты[номер][1], в.значения_полей))
            return f"{self.перечисление_c(тип)}_mk{номер}({значения})"
        if getattr(в, "конструктор", None):
            тип = в.конструктор
            значения = ", ".join(f".{self.поле_c(тип, поле)} = {self.сохранить(узел, self.выр(узел), тип_поля)}"
                                 for (поле, тип_поля, _), узел in zip(self.собственные[тип], в.значения_полей))
            return f"(({self.тип_c(тип)}){{ {значения} }})"
        аргументы = [self.выр(а) for а in в.аргументы]

        if getattr(в, "библиотека", None):
            каноническое, вариант = в.библиотека
            _, описание = найти_в_библиотеке(каноническое)
            if описание.get("модуль") == "данные":
                self.включить_данные()
            else:
                self.включить_математику()
            if описание["строка"][вариант]:
                аргументы.append(str(в.строка))
            return f"{описание['c'][вариант]}({', '.join(аргументы)})"

        if имя == "вывод":
            тип = в.аргументы[0].тип_выр
            return f"({self.запись(тип)}({аргументы[0]}), dan_newline())"
        if имя == "ошибка":
            return аргументы[0]
        if имя == "ввод":
            return f"dan_input({аргументы[0] if аргументы else 'NULL'})"
        if имя == БАЙТЫ and getattr(в, "из_списка", False):
            self.включить_данные()
            return f"dan_bytes_from_list({аргументы[0]}, {в.строка})"
        if имя == РАЦ and len(аргументы) == 2:
            return f"dan_rat_of({аргументы[0]}, {аргументы[1]}, {в.строка})"
        if имя in (СТР, ЦЕЛ, ДРОБ, РАЦ, БАЙТЫ):
            return self._преобразование(имя, в.аргументы[0].тип_выр, аргументы[0], в.строка)
        return f"{self.имя(имя)}({', '.join(аргументы)})"

    def _функциональный_метод(self, в):
        вид, тип_ф, результат = в.метод_функции
        объект = в.функция.объект
        тип_списка = объект.тип_выр
        т = элемент(тип_списка)
        S = self.структура(тип_списка)
        F = self.тип_c(тип_ф)
        ключ = (вид, тип_списка, тип_ф)
        if ключ not in self.помощники:
            имя = f"dan_{вид}_{len(self.помощники) + 1}"
            T = self.тип_c(т)
            поделить_т = self.поделить(т)
            эл = (lambda код: f"{поделить_т}({код})") if поделить_т else (lambda код: код)
            if вид == "преобразовать":
                R = self.структура("[" + результат + "]")
                поделить_р = self.поделить(результат) if (это_список(результат) or это_словарь(результат)) else None
                знач = f"{поделить_р}({F}_call(f, l->items[i]))" if поделить_р else f"{F}_call(f, l->items[i])"
                код = (f"static {R} *{имя}(const {S} *l, {F} f) {{\n    {R} *r = {R}_from(0, NULL);\n"
                       f"    for (int64_t i = 0; i < l->len; i++) {R}_push(r, {знач});\n    return r;\n}}")
            elif вид == "отобрать":
                код = (f"static {S} *{имя}(const {S} *l, {F} f) {{\n    {S} *r = {S}_from(0, NULL);\n"
                       f"    for (int64_t i = 0; i < l->len; i++) if ({F}_call(f, l->items[i])) "
                       f"{S}_push(r, {эл('l->items[i]')});\n    return r;\n}}")
            elif вид in ("любой", "все"):
                искомое = "true" if вид == "любой" else "false"
                проверка = f"{F}_call(f, l->items[i])" if вид == "любой" else f"!{F}_call(f, l->items[i])"
                код = (f"static bool {имя}(const {S} *l, {F} f) {{\n    for (int64_t i = 0; i < l->len; i++) "
                       f"if ({проверка}) return {искомое};\n    return !{искомое};\n}}")
            elif вид == "найти_первый":
                О = self.тип_c(т if это_необяз(т) else т + "?")
                some = f"{О}_some({эл('l->items[i]')})" if not это_необяз(т) else эл("l->items[i]")
                код = (f"static {О} {имя}(const {S} *l, {F} f) {{\n    for (int64_t i = 0; i < l->len; i++) "
                       f"if ({F}_call(f, l->items[i])) return {some};\n    return {О}_none();\n}}")
            elif вид == "свернуть":
                A = self.тип_c(результат)
                поделить_а = self.поделить(результат)
                начало = f"{поделить_а}(acc)" if поделить_а else "acc"
                код = (f"static {A} {имя}(const {S} *l, {A} acc, {F} f) {{\n    {A} r = {начало};\n"
                       f"    for (int64_t i = 0; i < l->len; i++) r = {F}_call(f, r, l->items[i]);\n    return r;\n}}")
            else:  # упорядочить_по: устойчивая сортировка слиянием по ключу
                K = self.тип_c(результат)
                сравнить = {ЦЕЛ: "(ka < kb ? -1 : ka > kb)", ДРОБ: "(ka < kb ? -1 : ka > kb)",
                            РАЦ: "dan_rat_cmp(ka, kb)", СТР: "dan_str_cmp(ka, kb)"}[результат]
                код = (f"static {S} *{имя}(const {S} *l, {F} f) {{\n"
                       f"    int64_t n = l->len;\n    {S} *r = {S}_copy(l);\n"
                       f"    {K} *k = dan_alloc(sizeof({K}) * (size_t)(n ? n : 1));\n"
                       f"    int64_t *ix = dan_alloc(sizeof(int64_t) * (size_t)(n ? n : 1)), *tmp = dan_alloc(sizeof(int64_t) * (size_t)(n ? n : 1));\n"
                       f"    for (int64_t i = 0; i < n; i++) {{ k[i] = {F}_call(f, l->items[i]); ix[i] = i; }}\n"
                       f"    for (int64_t w = 1; w < n; w *= 2)\n"
                       f"        for (int64_t lo = 0; lo < n; lo += 2 * w) {{\n"
                       f"            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, a = lo, b = mid, o = lo;\n"
                       f"            while (a < mid || b < hi) {{\n"
                       f"                if (b >= hi) {{ tmp[o++] = ix[a++]; continue; }}\n"
                       f"                if (a >= mid) {{ tmp[o++] = ix[b++]; continue; }}\n"
                       f"                {K} ka = k[ix[a]], kb = k[ix[b]];\n"
                       f"                tmp[o++] = ({сравнить}) <= 0 ? ix[a++] : ix[b++];\n"
                       f"            }}\n"
                       f"            for (int64_t i = lo; i < hi; i++) ix[i] = tmp[i];\n"
                       f"        }}\n"
                       f"    for (int64_t i = 0; i < n; i++) r->items[i] = l->items[ix[i]];\n"
                       f"    return r;\n}}")
            self.помощники[ключ] = код
        имя = self.помощники[ключ].split("(", 1)[0].split()[-1].lstrip("*")
        аргументы = [self.выр(объект)] + [self.выр(а) for а in в.аргументы]
        return f"{имя}({', '.join(аргументы)})"

    def _преобразование(self, куда, откуда, значение, строка):
        if куда == откуда:
            return значение
        if куда == СТР and (это_необяз(откуда) or откуда in self.собственные):
            return self.в_строку(откуда, значение)
        if куда == СТР and откуда in self.перечисления:
            return f"{self.перечисление_c(откуда)}_str({значение})"
        if куда == СТР and это_словарь(откуда):
            return f"{self.словарь_c(откуда)}_str({значение})"
        if куда == СТР and это_список(откуда):
            return f"{self.структура(откуда)}_str({значение})"
        if куда == БАЙТЫ:
            return f"dan_bytes_from_str({значение})"
        if откуда == БАЙТЫ:
            return f"dan_bytes_to_str({значение})"
        if куда == СТР:
            return {ЦЕЛ: "dan_int_to_str", ДРОБ: "dan_float_to_str", РАЦ: "dan_rat_to_str",
                    ЛОГ: "dan_bool_to_str", МН: "dan_poly_to_str"}[откуда] + f"({значение})"
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
