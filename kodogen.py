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

from dataclasses import fields
import uzly as у
import matematika
import vysshaya  # регистрирует функции высшей и прикладной математики
import dannye
from proverka import это_канал, это_задача, элемент_канала, ДЛИН, КОМПЛ
from proverka import (ЦЕЛ, ДРОБ, СТР, ЛОГ, РАЦ, МН, БАЙТЫ, ПУСТО, НИЧЕГО, это_список, элемент,
                      это_необяз, основа, найти_в_библиотеке, это_словарь, ключ_словаря, значение_словаря,
                      это_результат, ОШИБКА, это_функция, разобрать_тип_функции, ДИАПАЗОН, НЕИЗВ)

БАЗОВЫЕ_C = {ЦЕЛ: "int64_t", ДРОБ: "double", СТР: "dan_str", ЛОГ: "bool", РАЦ: "dan_rat",
             МН: "dan_mn", БАЙТЫ: "dan_bytes", ПУСТО: "void", ДЛИН: "dan_big", КОМПЛ: "dan_cx"}
КОДЫ_ТИПОВ = {ЦЕЛ: "i", ДРОБ: "f", СТР: "s", ЛОГ: "b", РАЦ: "r", МН: "p", БАЙТЫ: "y", ПУСТО: "v", ДЛИН: "g",
              КОМПЛ: "c"}
ЗАПИСЬ = {ЦЕЛ: "dan_write_int", ДРОБ: "dan_write_float", СТР: "dan_write_str", ЛОГ: "dan_write_bool",
          РАЦ: "dan_write_rat", МН: "dan_write_poly", БАЙТЫ: "dan_write_bytes", ДЛИН: "dan_write_big",
          КОМПЛ: "dan_write_cx"}
ОПЕРАЦИИ_КОМПЛ = {"+": "dan_cx_add", "-": "dan_cx_sub", "*": "dan_cx_mul", "/": "dan_cx_div"}
ОПЕРАЦИИ_ДЛИН = {"+": "dan_big_add", "-": "dan_big_sub", "*": "dan_big_mul", "/": "dan_big_div", "%": "dan_big_mod"}
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
    "DAN_MSG_CX_LOG_ZERO": "логарифм нуля не определён",
    "DAN_MSG_EXT_RANGE": "число не помещается в 32-разрядное целое, которое принимает внешняя функция",
    "DAN_MSG_BIG_HUGE": "длинное число слишком велико: больше трёх миллиардов двоичных разрядов",
    "DAN_MSG_BIG_TO_INT": "значение длинного числа не помещается в тип цел",
    "DAN_MSG_NOT_BIG": "«%s» - не целое число",
    "DAN_MSG_DEADLOCK": "Ошибка выполнения: взаимная блокировка - все задачи ждут друг друга, и ни одна не может продолжить:\n",
    "DAN_TXT_MAIN_WAITS": "  главная программа ждёт ",
    "DAN_TXT_TASK_WAITS": "  задача, запущенная в строке %d, ждёт ",
    "DAN_TXT_WAIT_RECV": "значения из канала",
    "DAN_TXT_WAIT_SEND": "места в канале или получателя значения",
    "DAN_TXT_WAIT_TASK": "завершения задачи",
    "DAN_TXT_AT_LINE": "%s в строке %d\n",
    "DAN_TXT_AT_FILE_LINE": "%s в файле %s, строка %d\n",
    "DAN_MSG_SEND_CLOSED": "отправка значения в закрытый канал",
    "DAN_MSG_CLOSE_TWICE": "канал уже закрыт",
    "DAN_MSG_CHAN_CAP": "запас канала не может быть отрицательным",
    "DAN_MSG_THREAD": "не удалось запустить задачу: система не выделила поток",
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

/* with tasks: the first error stops the program, other tasks wait for the end */
static void (*dan_on_error)(void);

static void dan_error(const char *text, int line) {
    if (dan_on_error) dan_on_error();
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
static DAN_TLS uintptr_t dan_stack_base, dan_stack_limit;

static void dan_stack_error(int line) __attribute__((noinline, cold));
static void dan_stack_error(int line) {
    if (line > 0) dan_error(DAN_MSG_DEEP, line);
    if (dan_on_error) dan_on_error();
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

/* ---- memory: reference counting ----
   Every value kept in memory (string, list, dictionary, bytes, variant cell,
   environment of a closure) starts with a header. rc is the number of places
   (variables, fields, elements) that hold the value; a value that is only in
   use as a temporary has rc == 0. A new value is put into the table of
   temporaries (ZCT); at the end of each statement the table is swept, and
   values that no place holds are freed. A value whose last place lets it go
   is freed at once. Freeing is iterative, so long chains do not overflow
   the stack. Values never form cycles (they are values, not shared objects),
   so no garbage collector is needed. */
typedef struct { int32_t rc; uint16_t flags; uint16_t magic; } dan_hdr;
#define DAN_MAGIC 0xDA4E
#define DAN_IN_ZCT 1u
#define DAN_IMMORTAL 2u
#define DAN_HDR_INIT { 0, DAN_IMMORTAL, DAN_MAGIC }
typedef void (*dan_destroy_fn)(dan_hdr *);
typedef struct { dan_hdr *h; dan_destroy_fn d; } dan_zent;
static DAN_TLS dan_zent *dan_zct, *dan_dead;
static DAN_TLS size_t dan_zct_n, dan_zct_cap, dan_dead_n, dan_dead_cap;
static DAN_TLS int dan_draining;

static void dan_zct_push(dan_hdr *h, dan_destroy_fn d) {
    if (h->flags & (DAN_IN_ZCT | DAN_IMMORTAL)) return;
    h->flags |= DAN_IN_ZCT;
    if (dan_zct_n == dan_zct_cap) {
        dan_zct_cap = dan_zct_cap ? dan_zct_cap * 2 : 256;
        dan_zct = dan_realloc(dan_zct, dan_zct_cap * sizeof *dan_zct);
    }
    dan_zct[dan_zct_n].h = h;
    dan_zct[dan_zct_n].d = d;
    dan_zct_n++;
}

static void dan_kill(dan_hdr *h, dan_destroy_fn d) {
    if (dan_dead_n == dan_dead_cap) {
        dan_dead_cap = dan_dead_cap ? dan_dead_cap * 2 : 256;
        dan_dead = dan_realloc(dan_dead, dan_dead_cap * sizeof *dan_dead);
    }
    dan_dead[dan_dead_n].h = h;
    dan_dead[dan_dead_n].d = d;
    dan_dead_n++;
    if (dan_draining) return;
    dan_draining = 1;
    while (dan_dead_n) {
        dan_zent e = dan_dead[--dan_dead_n];
        e.d(e.h);
    }
    dan_draining = 0;
}

static inline size_t dan_mark(void) { return dan_zct_n; }

#define DAN_MAGIC_SHARED 0xDA4F   /* value shared between tasks (channel): counted under a lock */
static void dan_sweep_slow(size_t m) {
    while (dan_zct_n > m) {
        dan_zent e = dan_zct[--dan_zct_n];
        if (e.h->magic == DAN_MAGIC_SHARED) { e.d(e.h); continue; }
        e.h->flags &= (uint16_t)~DAN_IN_ZCT;
        if (e.h->rc == 0) dan_kill(e.h, e.d);
    }
}
static inline void dan_sweep(size_t m) { if (dan_zct_n > m) dan_sweep_slow(m); }

/* a value made for another task: one place (the message) holds it, no table */
static inline void dan_hdr_detached(dan_hdr *h) { h->rc = 1; h->flags = 0; h->magic = DAN_MAGIC; }

static inline void dan_hdr_new(dan_hdr *h, dan_destroy_fn d) {
    h->rc = 0; h->flags = 0; h->magic = DAN_MAGIC;
    dan_zct_push(h, d);
}
static inline void dan_hdr_share(dan_hdr *h) { if (!(h->flags & DAN_IMMORTAL)) h->rc++; }
static inline void dan_hdr_release(dan_hdr *h, dan_destroy_fn d) {
    if (h->flags & DAN_IMMORTAL) return;
    if (--h->rc <= 0) { h->rc = 0; if (!(h->flags & DAN_IN_ZCT)) dan_kill(h, d); }
}
/* the place passes the value on (to the caller): it becomes a temporary */
static inline void dan_hdr_give(dan_hdr *h, dan_destroy_fn d) {
    if (h->flags & DAN_IMMORTAL) return;
    if (--h->rc <= 0) { h->rc = 0; dan_zct_push(h, d); }
}

#define DAN_XSTR_(x) #x
#define DAN_XSTR(x) DAN_XSTR_(x)
#define DAN_SYM(n) DAN_XSTR(__USER_LABEL_PREFIX__) n
#define DAN_ID(x) (x)
#define DAN_NOREL(x) ((void)0)

/* environment of a closure: header, its own way to free the captured values */
typedef struct { dan_hdr h; dan_destroy_fn d; void *(*c)(const void *); } dan_envb;
static inline void dan_env_share(void *e) { if (e) dan_hdr_share(&((dan_envb *)e)->h); }
static inline void dan_env_release(void *e) { if (e) dan_hdr_release(&((dan_envb *)e)->h, ((dan_envb *)e)->d); }
static inline void dan_env_give(void *e) { if (e) dan_hdr_give(&((dan_envb *)e)->h, ((dan_envb *)e)->d); }
static inline void *dan_env_clone(void *e) { return e ? ((dan_envb *)e)->c(e) : NULL; }

/* Strings: the header is right before the characters. Text that did not
   come from dan_str_new (a C literal) has no header and is never freed. */
static void dan_str_destroy(dan_hdr *h) { free(h); }
static char *dan_str_new(size_t n) {
    dan_hdr *h = dan_alloc(sizeof *h + n + 1);
    dan_hdr_new(h, dan_str_destroy);
    char *s = (char *)(h + 1);
    s[n] = 0;
    return s;
}
static inline dan_hdr *dan_str_hdr(dan_str s) {
    uint16_t m;
    if (!s) return NULL;
    memcpy(&m, s - 2, 2);
    return m == DAN_MAGIC ? (dan_hdr *)(void *)(s - sizeof(dan_hdr)) : NULL;
}
static inline dan_str dan_str_share(dan_str s) { dan_hdr *h = dan_str_hdr(s); if (h) dan_hdr_share(h); return s; }
static inline void dan_str_release(dan_str s) { dan_hdr *h = dan_str_hdr(s); if (h) dan_hdr_release(h, dan_str_destroy); }
static inline void dan_str_give(dan_str s) { dan_hdr *h = dan_str_hdr(s); if (h) dan_hdr_give(h, dan_str_destroy); }
/* copy for another task (strings that are never freed are shared as they are) */
static dan_str dan_str_clone(dan_str s) {
    dan_hdr *h = dan_str_hdr(s);
    if (!h || (h->flags & DAN_IMMORTAL)) return s;
    size_t n = strlen(s);
    dan_hdr *r = dan_alloc(sizeof *r + n + 1);
    dan_hdr_detached(r);
    memcpy(r + 1, s, n + 1);
    return (char *)(r + 1);
}
#define DAN_STATIC_STR(name, text) \
    static const struct { dan_hdr h; char s[sizeof(text)]; } name = { DAN_HDR_INIT, text }
DAN_STATIC_STR(dan_true_s, DAN_TRUE);
DAN_STATIC_STR(dan_false_s, DAN_FALSE);
DAN_STATIC_STR(dan_empty_s, "");

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
    char *r = dan_str_new(n);
    memcpy(r, s, n + 1);
    return r;
}

static dan_str dan_concat(dan_str a, dan_str b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = dan_str_new(la + lb);
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
    char *r = dan_str_new(n);
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
    unsigned char *r = (unsigned char *)dan_str_new(n);
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

/* Growing text buffer. Room for a string header is kept before the text:
   dan_buf_str makes the text an ordinary string. Until then the header
   says «never free», so a buffer returned as is stays valid forever. */
typedef struct { char *s; size_t len, cap; } dan_buf;

static void dan_buf_add(dan_buf *b, const char *t) {
    size_t n = strlen(t);
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 16;
        dan_hdr *h = dan_realloc(b->s ? (dan_hdr *)(void *)b->s - 1 : NULL, sizeof(dan_hdr) + b->cap);
        if (!b->s) { h->rc = 0; h->flags = DAN_IMMORTAL; h->magic = DAN_MAGIC; }
        b->s = (char *)(h + 1);
    }
    memcpy(b->s + b->len, t, n + 1);
    b->len += n;
}

static dan_str dan_buf_str(dan_buf *b) {
    if (!b->s) return dan_strdup("");
    dan_hdr *h = (dan_hdr *)(void *)b->s - 1;
    h->flags = 0;
    dan_zct_push(h, dan_str_destroy);
    return b->s;
}

static void dan_buf_free(dan_buf *b) { if (b->s) free((dan_hdr *)(void *)b->s - 1); b->s = NULL; }

static dan_str dan_str_replace(dan_str s, dan_str a, dan_str b, int line) {
    if (!*a) dan_error(DAN_MSG_EMPTY_SEARCH, line);
    dan_buf r = { NULL, 0, 0 };
    dan_buf_add(&r, "");
    size_t la = strlen(a);
    const char *p = s, *f;
    while ((f = strstr(p, a)) != NULL) {
        size_t n = (size_t)(f - p);
        char *part = dan_alloc(n + 1);
        memcpy(part, p, n);
        part[n] = 0;
        dan_buf_add(&r, part);
        free(part);
        dan_buf_add(&r, b);
        p = f + la;
    }
    dan_buf_add(&r, p);
    return dan_buf_str(&r);
}

static dan_str dan_str_repeat(dan_str s, int64_t k, int line) {
    if (k < 0) dan_error(DAN_MSG_NEG_REPEAT, line);
    dan_buf r = { NULL, 0, 0 };
    dan_buf_add(&r, "");
    for (int64_t i = 0; i < k; i++) dan_buf_add(&r, s);
    return dan_buf_str(&r);
}

static dan_str dan_quote_str(dan_str s) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "\"");
    dan_buf_add(&b, s);
    dan_buf_add(&b, "\"");
    return dan_buf_str(&b);
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

static dan_str dan_bool_to_str(bool v) { return v ? dan_true_s.s : dan_false_s.s; }

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
        char *r = dan_str_new((size_t)n);
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
    dan_str s = dan_strdup(r);
    free(r);
    return s;
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

typedef struct { dan_hdr h; int64_t len; uint8_t *data; } dan_bytes_s;
typedef const dan_bytes_s *dan_bytes;

static void dan_bytes_destroy(dan_hdr *h) { dan_bytes_s *b = (dan_bytes_s *)(void *)h; free(b->data); free(b); }
static inline dan_bytes dan_bytes_share(dan_bytes b) { if (b) dan_hdr_share((dan_hdr *)&b->h); return b; }
static inline void dan_bytes_release(dan_bytes b) { if (b) dan_hdr_release((dan_hdr *)&b->h, dan_bytes_destroy); }
static inline void dan_bytes_give(dan_bytes b) { if (b) dan_hdr_give((dan_hdr *)&b->h, dan_bytes_destroy); }
static dan_bytes dan_bytes_clone(dan_bytes b) {
    if (!b || (b->h.flags & DAN_IMMORTAL)) return b;
    dan_bytes_s *r = dan_alloc(sizeof *r);
    dan_hdr_detached(&r->h);
    r->len = b->len;
    r->data = dan_alloc((size_t)b->len + 1);
    memcpy(r->data, b->data, (size_t)b->len + 1);
    return r;
}

static dan_bytes_s *dan_bytes_alloc(int64_t len) {
    dan_bytes_s *b = dan_alloc(sizeof *b);
    dan_hdr_new(&b->h, dan_bytes_destroy);
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
    char *s = dan_str_new((size_t)b->len);
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
    return dan_buf_str(&s);
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

ДЛИННЫЕ_C = r"""
/* ---- long integers (длинное): sign and magnitude in base 2^32 ---- */
typedef struct { dan_hdr h; int32_t sign, n; uint32_t d[]; } dan_big_s;
typedef const dan_big_s *dan_big;

static void dan_big_destroy(dan_hdr *h) { free(h); }
static dan_big_s *dan_big_alloc(int64_t n) {
    if (n > 100000000) dan_error(DAN_MSG_BIG_HUGE, 0);
    dan_big_s *b = dan_alloc(sizeof *b + sizeof(uint32_t) * (size_t)(n ? n : 1));
    dan_hdr_new(&b->h, dan_big_destroy);
    b->sign = 0;
    b->n = (int32_t)n;
    return b;
}
static dan_big dan_big_norm(dan_big_s *b) {
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
    if (b->n == 0) b->sign = 0;
    return b;
}
static inline dan_big dan_big_share(dan_big b) { if (b) dan_hdr_share((dan_hdr *)&b->h); return b; }
static inline void dan_big_release(dan_big b) { if (b) dan_hdr_release((dan_hdr *)&b->h, dan_big_destroy); }
static inline void dan_big_give(dan_big b) { if (b) dan_hdr_give((dan_hdr *)&b->h, dan_big_destroy); }
static dan_big dan_big_clone(dan_big b) {
    dan_big_s *r = dan_alloc(sizeof *r + sizeof(uint32_t) * (size_t)(b->n ? b->n : 1));
    dan_hdr_detached(&r->h);
    r->sign = b->sign; r->n = b->n;
    memcpy(r->d, b->d, sizeof(uint32_t) * (size_t)b->n);
    return r;
}

static dan_big dan_big_from_int(int64_t v) {
    dan_big_s *b = dan_big_alloc(2);
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    b->d[0] = (uint32_t)m;
    b->d[1] = (uint32_t)(m >> 32);
    b->sign = v < 0 ? -1 : v > 0;
    return dan_big_norm(b);
}

static int dan_mag_cmp(const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (int32_t i = an - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* |a| + |b| or |a| - |b| (|a| >= |b|) with the given sign */
static dan_big dan_big_addmag(dan_big a, dan_big b, int32_t sign) {
    if (a->n < b->n) { dan_big t = a; a = b; b = t; }
    dan_big_s *r = dan_big_alloc((int64_t)a->n + 1);
    uint64_t c = 0;
    for (int32_t i = 0; i < a->n; i++) {
        c += (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c;
        c >>= 32;
    }
    r->d[a->n] = (uint32_t)c;
    r->sign = sign;
    return dan_big_norm(r);
}
static dan_big dan_big_submag(dan_big a, dan_big b, int32_t sign) {
    dan_big_s *r = dan_big_alloc(a->n);
    int64_t br = 0;
    for (int32_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - br;
        br = t < 0;
        r->d[i] = (uint32_t)(t + (br ? ((int64_t)1 << 32) : 0));
    }
    r->sign = sign;
    return dan_big_norm(r);
}
static dan_big dan_big_add_signed(dan_big a, dan_big b, int32_t bsign) {
    if (b->sign == 0) return a->sign ? a : dan_big_from_int(0);
    if (a->sign == 0) {
        if (bsign == b->sign) return b;
        dan_big_s *r = (dan_big_s *)dan_big_clone(b);
        dan_hdr_new(&r->h, dan_big_destroy);
        r->sign = bsign;
        return r;
    }
    if (a->sign == bsign) return dan_big_addmag(a, b, a->sign);
    int c = dan_mag_cmp(a->d, a->n, b->d, b->n);
    if (c == 0) return dan_big_from_int(0);
    return c > 0 ? dan_big_submag(a, b, a->sign) : dan_big_submag(b, a, bsign);
}
static dan_big dan_big_add(dan_big a, dan_big b, int line) { (void)line; return dan_big_add_signed(a, b, b->sign); }
static dan_big dan_big_sub(dan_big a, dan_big b, int line) { (void)line; return dan_big_add_signed(a, b, -b->sign); }
static dan_big dan_big_neg(dan_big a, int line) { return dan_big_sub(dan_big_from_int(0), a, line); }

static dan_big dan_big_mul(dan_big a, dan_big b, int line) {
    (void)line;
    if (!a->sign || !b->sign) return dan_big_from_int(0);
    dan_big_s *r = dan_big_alloc((int64_t)a->n + b->n);
    memset(r->d, 0, sizeof(uint32_t) * (size_t)r->n);
    for (int32_t i = 0; i < a->n; i++) {
        uint64_t c = 0, x = a->d[i];
        if (!x) continue;
        for (int32_t j = 0; j < b->n; j++) {
            c += x * b->d[j] + r->d[i + j];
            r->d[i + j] = (uint32_t)c;
            c >>= 32;
        }
        for (int32_t k = i + b->n; c; k++) { c += r->d[k]; r->d[k] = (uint32_t)c; c >>= 32; }
    }
    r->sign = a->sign * b->sign;
    return dan_big_norm(r);
}

/* Division of magnitudes (Knuth, algorithm D): q = u / v, r = u % v. */
static void dan_mag_divmod(const uint32_t *u, int32_t m, const uint32_t *v, int32_t n, uint32_t *q, uint32_t *r) {
    const uint64_t B = (uint64_t)1 << 32;
    if (n == 1) {
        uint64_t k = 0;
        for (int32_t j = m - 1; j >= 0; j--) {
            uint64_t cur = (k << 32) | u[j];
            q[j] = (uint32_t)(cur / v[0]);
            k = cur % v[0];
        }
        r[0] = (uint32_t)k;
        return;
    }
    int s = __builtin_clz(v[n - 1]);
    uint32_t *vn = dan_alloc(sizeof(uint32_t) * (size_t)n), *un = dan_alloc(sizeof(uint32_t) * (size_t)(m + 1));
    for (int32_t i = n - 1; i > 0; i--) vn[i] = (uint32_t)(((uint64_t)v[i] << s) | ((uint64_t)v[i - 1] >> (32 - s)));
    vn[0] = (uint32_t)((uint64_t)v[0] << s);
    un[m] = (uint32_t)((uint64_t)u[m - 1] >> (32 - s));
    for (int32_t i = m - 1; i > 0; i--) un[i] = (uint32_t)(((uint64_t)u[i] << s) | ((uint64_t)u[i - 1] >> (32 - s)));
    un[0] = (uint32_t)((uint64_t)u[0] << s);
    for (int32_t j = m - n; j >= 0; j--) {
        uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
        while (qhat >= B || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= B) break;
        }
        int64_t k = 0, t;
        for (int32_t i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + n] - k;
        un[j + n] = (uint32_t)t;
        q[j] = (uint32_t)qhat;
        if (t < 0) {
            q[j]--;
            k = 0;
            for (int32_t i = 0; i < n; i++) {
                t = (int64_t)un[i + j] + vn[i] + k;
                un[i + j] = (uint32_t)t;
                k = t >> 32;
            }
            un[j + n] = (uint32_t)((int64_t)un[j + n] + k);
        }
    }
    for (int32_t i = 0; i < n - 1; i++) r[i] = (uint32_t)(((uint64_t)un[i] >> s) | ((uint64_t)un[i + 1] << (32 - s)));
    r[n - 1] = un[n - 1] >> s;
    free(vn);
    free(un);
}

/* School division with remainder: a = b * q + r, 0 <= r < |b| (as for цел). */
static void dan_big_divmod(dan_big a, dan_big b, dan_big *q, dan_big *r, int line) {
    if (!b->sign) dan_error(DAN_MSG_DIV_ZERO, line);
    if (dan_mag_cmp(a->d, a->n, b->d, b->n) < 0) {
        dan_big_s *q0 = dan_big_alloc(0);
        if (a->sign >= 0) { *q = q0; *r = a; return; }
        /* -|a| = b * (-sign b) + (|b| - |a|) */
        *q = dan_big_from_int(-b->sign);
        *r = dan_big_submag(b, a, 1);
        return;
    }
    dan_big_s *qq = dan_big_alloc((int64_t)a->n - b->n + 1), *rr = dan_big_alloc(b->n);
    dan_mag_divmod(a->d, a->n, b->d, b->n, qq->d, rr->d);
    qq->sign = 1; rr->sign = 1;
    dan_big_norm(qq); dan_big_norm(rr);
    if (a->sign > 0) {
        qq->sign = qq->n ? b->sign : 0;
        *q = qq; *r = rr;
        return;
    }
    if (rr->n == 0) {
        qq->sign = qq->n ? -b->sign : 0;
        *q = qq; *r = rr;
        return;
    }
    dan_big one = dan_big_from_int(1);
    dan_big q1 = dan_big_add(qq, one, line);
    dan_big_s *qs = (dan_big_s *)q1;
    qs->sign = -b->sign;
    *q = q1;
    *r = dan_big_submag(b, rr, 1);
}
static dan_big dan_big_div(dan_big a, dan_big b, int line) { dan_big q, r; dan_big_divmod(a, b, &q, &r, line); return q; }
static dan_big dan_big_mod(dan_big a, dan_big b, int line) { dan_big q, r; dan_big_divmod(a, b, &q, &r, line); return r; }

static int dan_big_cmp(dan_big a, dan_big b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    int c = dan_mag_cmp(a->d, a->n, b->d, b->n);
    return a->sign < 0 ? -c : c;
}
static bool dan_big_eq(dan_big a, dan_big b) { return dan_big_cmp(a, b) == 0; }

static dan_big dan_big_pow(dan_big a, int64_t e, int line) {
    if (e < 0) {
        if (a->n == 1 && a->d[0] == 1) return (a->sign > 0 || e % 2 == 0) ? dan_big_from_int(1) : a;
        if (!a->sign) dan_error(DAN_MSG_DIV_ZERO, line);
        dan_error(DAN_MSG_NEG_POW, line);
    }
    dan_big r = dan_big_from_int(1), x = a;
    while (e > 0) {
        if (e & 1) r = dan_big_mul(r, x, line);
        e >>= 1;
        if (e) x = dan_big_mul(x, x, line);
    }
    return r;
}

static int64_t dan_big_to_int(dan_big a, int line) {
    if (a->n > 2) dan_error(DAN_MSG_BIG_TO_INT, line);
    uint64_t m = a->n == 0 ? 0 : a->n == 1 ? a->d[0] : ((uint64_t)a->d[1] << 32) | a->d[0];
    if (a->sign >= 0 ? m > (uint64_t)INT64_MAX : m > (uint64_t)INT64_MAX + 1) dan_error(DAN_MSG_BIG_TO_INT, line);
    return a->sign < 0 ? (int64_t)((uint64_t)0 - m) : (int64_t)m;
}
static double dan_big_to_float(dan_big a) {
    double r = 0;
    for (int32_t i = a->n - 1; i >= 0; i--) r = r * 4294967296.0 + a->d[i];
    return a->sign < 0 ? -r : r;
}

static dan_str dan_big_to_str(dan_big a) {
    if (!a->sign) return dan_strdup("0");
    int32_t n = a->n;
    uint32_t *t = dan_alloc(sizeof(uint32_t) * (size_t)n);
    memcpy(t, a->d, sizeof(uint32_t) * (size_t)n);
    size_t cap = (size_t)n * 10 + 2, len = 0;
    char *buf = dan_alloc(cap);
    while (n > 0) {
        uint64_t k = 0;
        for (int32_t j = n - 1; j >= 0; j--) {
            uint64_t cur = (k << 32) | t[j];
            t[j] = (uint32_t)(cur / 1000000000u);
            k = cur % 1000000000u;
        }
        while (n > 0 && t[n - 1] == 0) n--;
        for (int i = 0; i < 9 && (n > 0 || k); i++) { buf[len++] = (char)('0' + k % 10); k /= 10; }
    }
    if (a->sign < 0) buf[len++] = '-';
    char *s = dan_str_new(len);
    for (size_t i = 0; i < len; i++) s[i] = buf[len - 1 - i];
    free(t);
    free(buf);
    return s;
}
static void dan_write_big(dan_big a) { fputs(dan_big_to_str(a), stdout); }

static dan_big dan_big_from_str(dan_str s, int line) {
    const char *p = s;
    while (*p == ' ' || *p == '\\t') p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    size_t digits = 0;
    for (const char *q = p; *q; q++) if (*q >= '0' && *q <= '9') digits++;
    dan_big_s *r = dan_big_alloc((int64_t)(digits / 9 + 2));
    memset(r->d, 0, sizeof(uint32_t) * (size_t)r->n);
    int32_t used = 0, any = 0;
    uint64_t chunk = 0, mult = 1;
    for (;; p++) {
        if (*p == '_' || *p == ' ') continue;
        if (*p && (*p < '0' || *p > '9')) dan_conv_error(DAN_MSG_NOT_BIG, s, line);
        if (*p) { any = 1; chunk = chunk * 10 + (uint64_t)(*p - '0'); mult *= 10; }
        if (mult == 1000000000u || (!*p && mult > 1)) {
            uint64_t c = chunk;
            for (int32_t i = 0; i < used; i++) { c += (uint64_t)r->d[i] * mult; r->d[i] = (uint32_t)c; c >>= 32; }
            if (c) r->d[used++] = (uint32_t)c;
            chunk = 0; mult = 1;
        }
        if (!*p) break;
    }
    if (!any) dan_conv_error(DAN_MSG_NOT_BIG, s, line);
    r->n = used;
    r->sign = neg ? -1 : 1;
    return dan_big_norm(r);
}
"""

КОМПЛЕКСНЫЕ_C = r"""
/* ---- complex numbers (компл) ---- */
typedef struct { double re, im; } dan_cx;
static inline dan_cx dan_cx_make(double re, double im) { dan_cx z; z.re = re; z.im = im; return z; }
static inline dan_cx dan_cx_add(dan_cx a, dan_cx b, int line) { (void)line; return dan_cx_make(a.re + b.re, a.im + b.im); }
static inline dan_cx dan_cx_sub(dan_cx a, dan_cx b, int line) { (void)line; return dan_cx_make(a.re - b.re, a.im - b.im); }
static inline dan_cx dan_cx_mul(dan_cx a, dan_cx b, int line) {
    (void)line;
    return dan_cx_make(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}
/* division without overflow of intermediate products (Smith's method) */
static dan_cx dan_cx_div(dan_cx a, dan_cx b, int line) {
    if (b.re == 0 && b.im == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    if (fabs(b.re) >= fabs(b.im)) {
        double r = b.im / b.re, d = b.re + b.im * r;
        return dan_cx_make((a.re + a.im * r) / d, (a.im - a.re * r) / d);
    }
    double r = b.re / b.im, d = b.re * r + b.im;
    return dan_cx_make((a.re * r + a.im) / d, (a.im * r - a.re) / d);
}
static inline dan_cx dan_cx_neg(dan_cx a) { return dan_cx_make(-a.re, -a.im); }
static inline bool dan_cx_eq(dan_cx a, dan_cx b) { return a.re == b.re && a.im == b.im; }
static inline double dan_cx_abs(dan_cx a) { return hypot(a.re, a.im); }
static inline double dan_cx_arg(dan_cx a) { return atan2(a.im, a.re); }
static inline dan_cx dan_cx_conj(dan_cx a) { return dan_cx_make(a.re, -a.im); }
static dan_cx dan_cx_exp(dan_cx a) { double e = exp(a.re); return dan_cx_make(e * cos(a.im), e * sin(a.im)); }
static dan_cx dan_cx_ln(dan_cx a, int line) {
    if (a.re == 0 && a.im == 0) dan_error(DAN_MSG_CX_LOG_ZERO, line);
    return dan_cx_make(log(dan_cx_abs(a)), dan_cx_arg(a));
}
/* principal square root: the real part is never negative */
static dan_cx dan_cx_sqrt(dan_cx a) {
    if (a.re == 0 && a.im == 0) return a;
    double m = dan_cx_abs(a);
    double re = sqrt((m + fabs(a.re)) / 2);
    if (a.re >= 0) return dan_cx_make(re, a.im / (2 * re));
    double im = a.im < 0 ? -re : re;
    return dan_cx_make(fabs(a.im) / (2 * re), im);
}
/* whole powers are computed by multiplication, so (1 + i)^2 is exactly 2i */
static dan_cx dan_cx_pow_int(dan_cx a, int64_t e, int line) {
    if (e < 0) {
        if (a.re == 0 && a.im == 0) dan_error(DAN_MSG_DIV_ZERO, line);
        return dan_cx_div(dan_cx_make(1, 0), dan_cx_pow_int(a, e == INT64_MIN ? INT64_MAX : -e, line), line);
    }
    dan_cx r = dan_cx_make(1, 0);
    while (e > 0) {
        if (e & 1) r = dan_cx_mul(r, a, line);
        e >>= 1;
        if (e) a = dan_cx_mul(a, a, line);
    }
    return r;
}
static dan_cx dan_cx_pow(dan_cx a, dan_cx b, int line) {
    if (b.im == 0 && b.re == floor(b.re) && fabs(b.re) < 9e18) return dan_cx_pow_int(a, (int64_t)b.re, line);
    if (a.re == 0 && a.im == 0) {
        if (b.re > 0) return a;
        dan_error(DAN_MSG_DIV_ZERO, line);
    }
    return dan_cx_exp(dan_cx_mul(b, dan_cx_ln(a, line), line));
}

static void dan_cx_part(char *buf, size_t size, double v) {
    dan_format_float(buf, size, v);
    size_t n = strlen(buf);
    if (n > 2 && buf[n - 2] == '.' && buf[n - 1] == '0') buf[n - 2] = 0;    /* 3.0 -> 3 */
}
/* written as in mathematics: 3 + 4i, 2 - i, 5i, -1 */
static dan_str dan_cx_to_str(dan_cx a) {
    char re[64], im[64], out[160];
    double ai = a.im;
    dan_cx_part(re, sizeof re, a.re);
    dan_cx_part(im, sizeof im, fabs(ai));
    const char *imp = strcmp(im, "1") == 0 ? "" : im;
    if (ai == 0) snprintf(out, sizeof out, "%s", re);
    else if (a.re == 0) snprintf(out, sizeof out, "%s%si", ai < 0 ? "-" : "", imp);
    else snprintf(out, sizeof out, "%s %s %si", re, ai < 0 ? "-" : "+", imp);
    return dan_strdup(out);
}
static void dan_write_cx(dan_cx a) { fputs(dan_cx_to_str(a), stdout); }
static dan_cx dan_cx_from_str(dan_str s, int line) {
    char *end;
    double v = strtod(s, &end);
    if (end == s) dan_conv_error(DAN_MSG_NOT_FLOAT, s, line);
    return dan_cx_make(v, 0);
}
"""

# Комплексные корни квадратного уравнения: после определения списка [компл].
КОМПЛЕКСНЫЕ_КОРНИ_C = r"""
static dl_Lc *dan_cx_quadratic(double a, double b, double c) {
    dl_Lc *r = dl_Lc_from(0, NULL);
    if (a == 0) {
        if (b != 0) dl_Lc_push(r, dan_cx_make(-c / b, 0));
        return r;
    }
    double d = b * b - 4 * a * c;
    if (d >= 0) {
        double s = sqrt(d);
        dl_Lc_push(r, dan_cx_make((-b + s) / (2 * a), 0));
        if (d > 0) dl_Lc_push(r, dan_cx_make((-b - s) / (2 * a), 0));
    } else {
        double s = sqrt(-d);
        dl_Lc_push(r, dan_cx_make(-b / (2 * a), s / (2 * a)));
        dl_Lc_push(r, dan_cx_make(-b / (2 * a), -s / (2 * a)));
    }
    return r;
}
"""

# Задачи и каналы: потоки операционной системы. Значения, переданные задаче
# или в канал, копируются, поэтому общих изменяемых данных у задач нет и
# подсчёт мест хранения остаётся обычным (без атомарных операций). Общий
# между задачами только канал: его счётчик меняется под замком.
ЗАДАЧИ_C = r"""
/* ---- tasks and channels ---- */
#define DAN_TASK_STACK ((size_t)64 * 1024 * 1024)
#ifdef _WIN32
typedef HANDLE dan_thread_t;
typedef CRITICAL_SECTION dan_mutex_t;
typedef CONDITION_VARIABLE dan_cond_t;
#define DAN_THREAD_FN DWORD WINAPI
#define DAN_THREAD_RETURN 0
static void dan_mutex_init(dan_mutex_t *m) { InitializeCriticalSection(m); }
static void dan_lock(dan_mutex_t *m) { EnterCriticalSection(m); }
static void dan_unlock(dan_mutex_t *m) { LeaveCriticalSection(m); }
static void dan_cond_init(dan_cond_t *c) { InitializeConditionVariable(c); }
static void dan_cond_wait(dan_cond_t *c, dan_mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void dan_cond_signal(dan_cond_t *c) { WakeConditionVariable(c); }
static void dan_cond_free(dan_cond_t *c) { (void)c; }
static int dan_thread_create(dan_thread_t *t, DWORD (WINAPI *f)(void *), void *a) {
    *t = CreateThread(NULL, DAN_TASK_STACK, f, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return *t != NULL;
}
static void dan_thread_join(dan_thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
typedef pthread_t dan_thread_t;
typedef pthread_mutex_t dan_mutex_t;
typedef pthread_cond_t dan_cond_t;
#define DAN_THREAD_FN void *
#define DAN_THREAD_RETURN NULL
static void dan_mutex_init(dan_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void dan_lock(dan_mutex_t *m) { pthread_mutex_lock(m); }
static void dan_unlock(dan_mutex_t *m) { pthread_mutex_unlock(m); }
static void dan_cond_init(dan_cond_t *c) { pthread_cond_init(c, NULL); }
static void dan_cond_wait(dan_cond_t *c, dan_mutex_t *m) { pthread_cond_wait(c, m); }
static void dan_cond_signal(dan_cond_t *c) { pthread_cond_signal(c); }
static void dan_cond_free(dan_cond_t *c) { pthread_cond_destroy(c); }
static int dan_thread_create(dan_thread_t *t, void *(*f)(void *), void *a) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, DAN_TASK_STACK);
    int ok = pthread_create(t, &at, f, a) == 0;
    pthread_attr_destroy(&at);
    return ok;
}
static void dan_thread_join(dan_thread_t t) { pthread_join(t, NULL); }
#endif

/* Every task (and the main program) has a record. All waiting happens under
   one lock, and the number of tasks that are not waiting is counted: when it
   drops to zero, no task can ever wake the others, and the program stops
   with a list of where each task waits (instead of hanging silently). */
typedef struct dan_trec {
    dan_cond_t cond;
    int blocked, line, start_line, is_main;
    const char *what;
    struct dan_trec *next_all, *prev_all, *next_wait;
} dan_trec;
static dan_mutex_t dan_sched, dan_out_mx;
static dan_trec *dan_all, dan_main_rec;
static int dan_active;
static DAN_TLS dan_trec *dan_self;

static dan_mutex_t dan_fail_mx;
static void dan_fail_once(void) { dan_lock(&dan_fail_mx); }   /* never released: the program is ending */
static void dan_out_lock(void) { dan_lock(&dan_out_mx); }
static void dan_out_unlock(void) { dan_unlock(&dan_out_mx); }

static void dan_trec_add(dan_trec *t) {
    t->prev_all = NULL; t->next_all = dan_all;
    if (dan_all) dan_all->prev_all = t;
    dan_all = t;
}
static void dan_trec_remove(dan_trec *t) {
    if (t->prev_all) t->prev_all->next_all = t->next_all; else dan_all = t->next_all;
    if (t->next_all) t->next_all->prev_all = t->prev_all;
}

static void dan_print_place(const char *what, int line) {
    if (line >= DAN_FILE_STEP && line / DAN_FILE_STEP < DAN_FILE_COUNT)
        fprintf(stderr, DAN_TXT_AT_FILE_LINE, what, dan_files[line / DAN_FILE_STEP], line % DAN_FILE_STEP);
    else
        fprintf(stderr, DAN_TXT_AT_LINE, what, line);
}

static void dan_deadlock(void) {
    dan_fail_once();
    fflush(stdout);
    fputs(DAN_MSG_DEADLOCK, stderr);
    for (dan_trec *t = dan_all; t; t = t->next_all) {
        if (!t->blocked) continue;
        if (t->is_main) fputs(DAN_TXT_MAIN_WAITS, stderr);
        else fprintf(stderr, DAN_TXT_TASK_WAITS, t->start_line % DAN_FILE_STEP);
        dan_print_place(t->what, t->line);
    }
    exit(1);
}

/* called under dan_sched: wait until a waker clears «blocked» */
static void dan_block(dan_trec **list, int line, const char *what) {
    dan_trec *me = dan_self;
    me->blocked = 1; me->line = line; me->what = what;
    me->next_wait = *list; *list = me;
    if (--dan_active == 0) dan_deadlock();
    while (me->blocked) dan_cond_wait(&me->cond, &dan_sched);
}
static void dan_wake_all(dan_trec **list) {
    for (dan_trec *t = *list; t; t = t->next_wait) {
        if (t->blocked) { t->blocked = 0; dan_active++; dan_cond_signal(&t->cond); }
    }
    *list = NULL;
}

static void dan_tasks_init(void) {
    dan_mutex_init(&dan_fail_mx);
    dan_on_error = dan_fail_once;
    dan_mutex_init(&dan_sched);
    dan_mutex_init(&dan_out_mx);
    dan_cond_init(&dan_main_rec.cond);
    dan_main_rec.is_main = 1;
    dan_trec_add(&dan_main_rec);
    dan_self = &dan_main_rec;
    dan_active = 1;
}

typedef struct dan_task {
    dan_thread_t th;
    int done, joined, line;
    dan_trec *joiners;
    dan_trec rec;
    struct dan_task *next;
} dan_task;

static void dan_task_start(dan_task *t, DAN_THREAD_FN (*f)(void *), void *arg, int line) {
    t->done = 0; t->joined = 0; t->line = line; t->joiners = NULL; t->next = NULL;
    memset(&t->rec, 0, sizeof t->rec);
    dan_cond_init(&t->rec.cond);
    t->rec.start_line = line;
    dan_lock(&dan_sched);
    dan_trec_add(&t->rec);
    dan_active++;
    dan_unlock(&dan_sched);
    if (!dan_thread_create(&t->th, f, arg)) dan_error(DAN_MSG_THREAD, line);
}

/* the beginning of the task's own thread */
static void dan_task_begin(dan_task *t) {
    char here;
    dan_self = &t->rec;
    dan_stack_base = (uintptr_t)&here;
    dan_stack_limit = DAN_TASK_STACK / 4 * 3;
}

static void dan_task_end(dan_task *t) {
    free(dan_zct); free(dan_dead);
    dan_zct = dan_dead = NULL;
    dan_zct_n = dan_zct_cap = dan_dead_n = dan_dead_cap = 0;
    dan_lock(&dan_sched);
    dan_trec_remove(&t->rec);
    t->done = 1;
    dan_wake_all(&t->joiners);
    if (--dan_active == 0 && dan_all) dan_deadlock();
    dan_unlock(&dan_sched);
}

static void dan_task_join(dan_task *t, int line) {
    dan_lock(&dan_sched);
    while (!t->done) dan_block(&t->joiners, line, DAN_TXT_WAIT_TASK);
    dan_unlock(&dan_sched);
    if (!t->joined) {
        dan_thread_join(t->th);
        dan_cond_free(&t->rec.cond);
        t->joined = 1;
    }
}

static bool dan_task_ready(dan_task *t) {
    dan_lock(&dan_sched);
    bool r = t->done != 0;
    dan_unlock(&dan_sched);
    return r;
}

/* «параллельно { ... }»: tasks without a name belong to the block */
typedef struct { dan_task *head; } dan_tgroup;
static void dan_tgroup_add(dan_tgroup *g, dan_task *t) { t->next = g->head; g->head = t; }
static void dan_tgroup_join(dan_tgroup *g, int line) {
    while (g->head) {
        dan_task *t = g->head;
        g->head = t->next;
        dan_task_join(t, line);
        free(t);
    }
}

/* Channels. Values in a channel are copies made for the receiver. An
   unbuffered channel (capacity 0) makes the sender wait until the value is
   taken. Receiving from a closed channel gives the remaining values, then
   «ничего»; sending to a closed channel is an error. */
typedef struct dan_chan {
    dan_hdr h;
    size_t esize;
    int64_t slots, head, count, sent, taken;
    int closed, unbuffered;
    unsigned char *buf;
    void (*drop)(void *);
    dan_trec *waiters;
} dan_chan;

static void dan_chan_destroy(dan_hdr *h) {
    dan_chan *c = (dan_chan *)(void *)h;
    for (int64_t i = 0; i < c->count; i++) c->drop(c->buf + (size_t)((c->head + i) % c->slots) * c->esize);
    free(c->buf);
    free(c);
}
/* the table of temporaries reached a channel: decide under the lock */
static void dan_chan_zct_done(dan_hdr *h) {
    dan_lock(&dan_sched);
    h->flags &= (uint16_t)~DAN_IN_ZCT;
    int dead = h->rc == 0;
    dan_unlock(&dan_sched);
    if (dead) dan_kill(h, dan_chan_destroy);
}
static dan_chan *dan_chan_new(size_t esize, int64_t cap, void (*drop)(void *), int line) {
    if (cap < 0) dan_error(DAN_MSG_CHAN_CAP, line);
    dan_chan *c = dan_alloc(sizeof *c);
    c->h.rc = 0; c->h.flags = 0; c->h.magic = DAN_MAGIC_SHARED;
    dan_zct_push(&c->h, dan_chan_zct_done);
    c->esize = esize;
    c->unbuffered = cap == 0;
    c->slots = cap == 0 ? 1 : cap;
    c->head = c->count = c->sent = c->taken = 0;
    c->closed = 0;
    c->buf = dan_alloc((size_t)c->slots * esize);
    c->drop = drop;
    c->waiters = NULL;
    return c;
}
static dan_chan *dan_chan_share(dan_chan *c) {
    if (c) { dan_lock(&dan_sched); c->h.rc++; dan_unlock(&dan_sched); }
    return c;
}
static void dan_chan_release(dan_chan *c) {
    if (!c) return;
    dan_lock(&dan_sched);
    int dead = --c->h.rc <= 0 && !(c->h.flags & DAN_IN_ZCT);
    dan_unlock(&dan_sched);
    if (dead) dan_kill(&c->h, dan_chan_destroy);
}
static void dan_chan_give(dan_chan *c) {
    if (!c) return;
    dan_lock(&dan_sched);
    if (--c->h.rc <= 0 && !(c->h.flags & DAN_IN_ZCT)) {
        c->h.rc = 0;
        c->h.flags |= DAN_IN_ZCT;
        if (dan_zct_n == dan_zct_cap) {
            dan_zct_cap = dan_zct_cap ? dan_zct_cap * 2 : 256;
            dan_zct = dan_realloc(dan_zct, dan_zct_cap * sizeof *dan_zct);
        }
        dan_zct[dan_zct_n].h = &c->h;
        dan_zct[dan_zct_n].d = dan_chan_zct_done;
        dan_zct_n++;
    }
    dan_unlock(&dan_sched);
}
static dan_chan *dan_chan_clone(dan_chan *c) { return dan_chan_share(c); }

static void dan_chan_send(dan_chan *c, const void *v, int line) {
    dan_lock(&dan_sched);
    while (!c->closed && c->count == c->slots) dan_block(&c->waiters, line, DAN_TXT_WAIT_SEND);
    if (c->closed) { dan_unlock(&dan_sched); dan_error(DAN_MSG_SEND_CLOSED, line); }
    memcpy(c->buf + (size_t)((c->head + c->count) % c->slots) * c->esize, v, c->esize);
    c->count++;
    int64_t ticket = ++c->sent;
    dan_wake_all(&c->waiters);
    if (c->unbuffered)
        while (c->taken < ticket) dan_block(&c->waiters, line, DAN_TXT_WAIT_SEND);
    dan_unlock(&dan_sched);
}
static int dan_chan_recv(dan_chan *c, void *out, int line) {
    dan_lock(&dan_sched);
    while (c->count == 0 && !c->closed) dan_block(&c->waiters, line, DAN_TXT_WAIT_RECV);
    if (c->count == 0) { dan_unlock(&dan_sched); return 0; }
    memcpy(out, c->buf + (size_t)c->head * c->esize, c->esize);
    c->head = (c->head + 1) % c->slots;
    c->count--;
    c->taken++;
    dan_wake_all(&c->waiters);
    dan_unlock(&dan_sched);
    return 1;
}
static void dan_chan_close(dan_chan *c, int line) {
    dan_lock(&dan_sched);
    if (c->closed) { dan_unlock(&dan_sched); dan_error(DAN_MSG_CLOSE_TWICE, line); }
    c->closed = 1;
    dan_wake_all(&c->waiters);
    dan_unlock(&dan_sched);
}
"""

ШАБЛОН_СПИСКА = """
/* ---- list {описание} ---- */
/* h.rc - число мест (переменных, полей, элементов), где хранится список.
   Список - значение: если мест несколько, перед изменением делается копия.
   Список сам учитывает свои элементы: при добавлении элемент получает ещё
   одно место хранения, при удалении - теряет его. */
struct {S} {{ dan_hdr h; int64_t len, cap; {T} *items; }};

static void {S}_destroy(dan_hdr *h) {{
    {S} *l = ({S} *)(void *)h;
    for (int64_t i = 0; i < l->len; i++) {RL}(l->items[i]);
    free(l->items);
    free(l);
}}

static {S} *{S}_from(int64_t n, {T} const *src) {{
    {S} *l = dan_alloc(sizeof *l);
    dan_hdr_new(&l->h, {S}_destroy);
    l->len = n;
    l->cap = n < 4 ? 4 : n;
    l->items = dan_alloc((size_t)l->cap * sizeof({T}));
    for (int64_t i = 0; i < n; i++) l->items[i] = {SH}(src[i]);
    return l;
}}

static void {S}_push({S} *l, {T} v) {{
    if (l->len == l->cap) {{
        l->cap *= 2;
        l->items = dan_realloc(l->items, (size_t)l->cap * sizeof({T}));
    }}
    l->items[l->len++] = {SH}(v);
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
    {T} old = l->items[i];
    l->items[i] = {SH}(v);
    {RL}(old);
    (void)old;
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
    {T} last = l->items[l->len - 1];
    memmove(&l->items[i + 1], &l->items[i], (size_t)(l->len - 1 - i) * sizeof({T}));
    l->items[i] = last;
}}

static void {S}_remove({S} *l, int64_t i, int line) {{
    if (i < 0 || i >= l->len) dan_index_error(i, l->len, line);
    {T} old = l->items[i];
    memmove(&l->items[i], &l->items[i + 1], (size_t)(l->len - i - 1) * sizeof({T}));
    l->len--;
    {RL}(old);
    (void)old;
}}

/* новый список с теми же элементами; элементы получают ещё одно место хранения */
static {S} *{S}_copy(const {S} *l) {{
    return {S}_from(l->len, l->items);
}}

static inline {S} *{S}_share({S} *l) {{ if (l) dan_hdr_share(&l->h); return l; }}
static inline void {S}_release({S} *l) {{ if (l) dan_hdr_release(&l->h, {S}_destroy); }}
static inline void {S}_give({S} *l) {{ if (l) dan_hdr_give(&l->h, {S}_destroy); }}

/* место *pp собирается изменить список: если список хранится и в других
   местах, место получает собственную копию */
static {S} *{S}_mut({S} **pp) {{
    {S} *l = *pp;
    if (l->h.rc > 1) {{
        {S} *r = {S}_copy(l);
        r->h.rc = 1;
        l->h.rc--;
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
    return {S}_from(b - a + 1, l->items + a);
}}

static dan_str {S}_str(const {S} *l) {{
    dan_buf b = {{ NULL, 0, 0 }};
    dan_buf_add(&b, "[");
    for (int64_t i = 0; i < l->len; i++) {{
        if (i) dan_buf_add(&b, ", ");
        dan_buf_add(&b, {строка_элемента}(l->items[i]));
    }}
    dan_buf_add(&b, "]");
    return dan_buf_str(&b);
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
static void {D}_release({D} *d);
static void {D}_give({D} *d);
static dan_str {D}_str(const {D} *d);
static void {D}_write(const {D} *d);
"""

ШАБЛОН_СЛОВАРЯ = """
/* ---- dictionary {описание} ---- */
struct {D} {{ dan_hdr h; int64_t len, n, cap, slots; {K} *keys; {V} *vals; uint64_t *hashes; bool *alive; int64_t *index; }};

static void {D}_destroy(dan_hdr *h) {{
    {D} *d = ({D} *)(void *)h;
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {{ {RK}(d->keys[e]); {RV}(d->vals[e]); }}
    free(d->keys); free(d->vals); free(d->hashes); free(d->alive); free(d->index);
    free(d);
}}
static {D} *{D}_new(void) {{
    {D} *d = dan_alloc(sizeof *d);
    dan_hdr_new(&d->h, {D}_destroy);
    d->len = d->n = d->cap = d->slots = 0;
    d->keys = NULL; d->vals = NULL; d->hashes = NULL; d->alive = NULL; d->index = NULL;
    return d;
}}
static {D} *{D}_new_detached(void) {{
    {D} *d = dan_alloc(sizeof *d);
    dan_hdr_detached(&d->h);
    d->len = d->n = d->cap = d->slots = 0;
    d->keys = NULL; d->vals = NULL; d->hashes = NULL; d->alive = NULL; d->index = NULL;
    return d;
}}
static {D} *{D}_share({D} *d) {{ if (d) dan_hdr_share(&d->h); return d; }}
static void {D}_release({D} *d) {{ if (d) dan_hdr_release(&d->h, {D}_destroy); }}
static void {D}_give({D} *d) {{ if (d) dan_hdr_give(&d->h, {D}_destroy); }}

static void {D}_reindex({D} *d) {{
    int64_t s = 8;
    while (s < 2 * d->cap) s *= 2;
    free(d->index);
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
    free(d->keys); free(d->vals); free(d->hashes); free(d->alive);
    d->keys = keys; d->vals = vals; d->hashes = hashes; d->alive = alive; d->n = m; d->cap = cap;
    {D}_reindex(d);
}}
static void {D}_put({D} *d, {K} k, {V} v) {{
    uint64_t h = {HASH};
    int64_t e = {D}_lookup(d, k, h);
    if (e >= 0) {{ {V} old = d->vals[e]; d->vals[e] = {SV}(v); {RV}(old); (void)old; return; }}
    if (d->n == d->cap) {D}_grow(d);
    e = d->n++;
    d->keys[e] = {SK}(k); d->vals[e] = {SV}(v); d->hashes[e] = h; d->alive[e] = true; d->len++;
    int64_t p = (int64_t)(h & (uint64_t)(d->slots - 1));
    while (d->index[p] >= 0) p = (p + 1) & (d->slots - 1);
    d->index[p] = e;
}}
/* запись в новый словарь значений, которые уже учтены (копия для задачи) */
static void {D}_put_raw({D} *d, {K} k, {V} v) {{
    uint64_t h = {HASH};
    if (d->n == d->cap) {D}_grow(d);
    int64_t e = d->n++;
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
    if (e >= 0) {{ d->alive[e] = false; d->len--; {RK}(d->keys[e]); {RV}(d->vals[e]); }}
}}
static {LK} *{D}_keys(const {D} *d) {{
    {LK} *r = {LK}_from(0, NULL);
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {LK}_push(r, d->keys[e]);
    return r;
}}
static {LV} *{D}_values(const {D} *d) {{
    {LV} *r = {LV}_from(0, NULL);
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {LV}_push(r, d->vals[e]);
    return r;
}}
static {D} *{D}_copy(const {D} *d) {{
    {D} *r = {D}_new();
    for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) {D}_put(r, d->keys[e], d->vals[e]);
    return r;
}}
static {D} *{D}_mut({D} **pp) {{
    {D} *d = *pp;
    if (d->h.rc > 1) {{ {D} *r = {D}_copy(d); r->h.rc = 1; d->h.rc--; *pp = r; return r; }}
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
    return dan_buf_str(&b);
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
РАВЕНСТВО = {КОМПЛ: "dan_cx_eq(a, b)", ДЛИН: "dan_big_eq(a, b)", ЦЕЛ: "(a == b)", ДРОБ: "(a == b)", ЛОГ: "(a == b)", СТР: "dan_str_eq(a, b)",
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
    return dan_buf_str(&b);
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
        self.литералы = {}         # текст строковой постоянной -> имя в C
        self.области = []          # места хранения (переменные), которые нужно освободить при выходе
        self.метка_функции = None  # отметка таблицы временных значений в начале функции
        self.метки_циклов = []     # отметки в начале шага каждого вложенного цикла
        self.нужны_задачи = False  # программа запускает задачи или пользуется каналами
        self.нужны_длинные = False # программа пользуется длинными целыми
        self.нужны_компл = False   # программа пользуется комплексными числами
        self.клоны = {}            # тип -> функция C, копирующая значение для другой задачи
        self.каналы = {}           # тип элементов канала -> часть имени функций C
        self.задачи_c = {}         # тип результата задачи -> структура C
        self.группы = []           # переменные C блоков «параллельно» текущей функции

    def код_типа(self, тип):
        """Короткий код типа для имён в C: [цел] -> Li, стр? -> Os, [Сотрудник] -> LTSotrudnik."""
        if это_канал(тип):
            return "K" + self.код_типа(элемент_канала(тип))
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

    def куча(self, тип, в_работе=None):
        """Хранит ли значение типа ссылки на значения в памяти, у которых
        учитываются места хранения (строки, списки, словари, ячейки и т.п.)."""
        в_работе = в_работе if в_работе is not None else set()
        if тип in (СТР, БАЙТЫ, ДЛИН) or это_список(тип) or это_словарь(тип) or это_функция(тип) \
                or тип in self.перечисления or это_результат(тип) or self.по_ссылке(тип) or это_канал(тип):
            return True
        if это_необяз(тип):
            return self.куча(основа(тип), в_работе)
        if тип in self.собственные:
            if тип in в_работе:
                return False
            в_работе.add(тип)
            return any(self.куча(т, в_работе) for _, т, _ in self.собственные[тип])
        return False

    # прежнее название проверки
    содержит_списки = куча

    def _учёт(self, тип, действие):
        if not self.куча(тип):
            return None
        if тип == СТР:
            return "dan_str_" + действие
        if тип == БАЙТЫ:
            return "dan_bytes_" + действие
        if тип == ДЛИН:
            self.нужны_длинные = True
            return "dan_big_" + действие
        if это_канал(тип):
            self.канал_c(тип)
            return "dan_chan_" + действие
        if это_список(тип):
            return self.структура(тип) + "_" + действие
        if это_словарь(тип):
            return self.словарь_c(тип) + "_" + действие
        if тип in self.перечисления:
            return self.перечисление_c(тип) + "_" + действие
        return self.значимый(тип) + "_" + действие

    def удержать(self, тип):
        """Функция C: значение получает ещё одно место хранения (или None)."""
        return self._учёт(тип, "share")

    def отпустить(self, тип):
        """Функция C: значение теряет место хранения и освобождается, если мест не осталось."""
        return self._учёт(тип, "release")

    def отдать(self, тип):
        """Функция C: место передаёт значение дальше (например, вызывающей функции)."""
        return self._учёт(тип, "give")

    поделить = удержать

    def сохранить(self, узел, код, тип):
        """Код значения, которое кладётся в переменную: значение получает место хранения."""
        функция = self.удержать(тип)
        return f"{функция}({код})" if функция else код

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
        if тип == ДЛИН:
            self.нужны_длинные = True
        if тип == КОМПЛ:
            self.нужны_компл = True
        if это_канал(тип):
            self.канал_c(тип)
            return "dan_chan *"
        if это_задача(тип):
            return self.задача_c(тип)
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
                БАЙТЫ: "dan_bytes_show", ДЛИН: "dan_big_to_str", КОМПЛ: "dan_cx_to_str"}[тип]

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

        внешние = self._внешние_функции(программа)
        # Сначала генерируется тело программы: при этом выясняется,
        # какие типы списков нужны, и их определения ставятся выше.
        if внешние:
            self.строки += внешние
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
        место_запуска = len(self.строки) - 1
        self.метка_функции, self.области, self.метки_циклов = "dan_main_mark", [[]], []
        self.текущий_результат = None
        self.пиши(f"const size_t {self.метка_функции} = dan_mark();")
        for и in прочие:
            self.инструкция(и)
        self._выход_из_функции()
        self.пиши("return 0;")
        if self.нужны_задачи:
            self.строки[место_запуска] += " dan_tasks_init();"
        self.отступ -= 1
        self.пиши("}")
        if self.лямбды_код:
            self.строки[место_лямбд:место_лямбд] = ["/* ---- anonymous functions ---- */"] + self.лямбды_код

        шапка = [
            "/* Создано компилятором языка Дан. Редактировать не нужно. */",
            "#include <stdint.h>", "#include <inttypes.h>", "#include <stdbool.h>",
            "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", "#include <stddef.h>",
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
        if self.нужны_задачи:
            шапка += ["#define DAN_TASKS 1", "#define DAN_TLS _Thread_local",
                      "#ifndef _WIN32", "#include <pthread.h>", "#endif",
                      "#define DAN_OUT_LOCK() dan_out_lock()", "#define DAN_OUT_UNLOCK() dan_out_unlock()"]
        else:
            шапка += ["#define DAN_TLS", "#define DAN_OUT_LOCK() ((void)0)", "#define DAN_OUT_UNLOCK() ((void)0)"]
        файлы = getattr(программа, "файлы", None) or ["программа.dan"]
        шапка += ["", f"#define DAN_FILE_STEP {у.ШАГ_ФАЙЛА}", f"#define DAN_FILE_COUNT {len(файлы)}",
                  "static const char *const dan_files[] = {" + ", ".join(строка_c(ф) for ф in файлы) + "};"]
        части = ["\n".join(шапка), СРЕДА]
        if self.нужны_задачи:
            части.append(ЗАДАЧИ_C)
        if self.нужны_длинные:
            части.append(ДЛИННЫЕ_C)
        if self.нужны_компл:
            части.append(КОМПЛЕКСНЫЕ_C)
        self._собрать_все_типы()
        части.append(self._определения_типов())
        части.append(self._определения_задач())
        for имя in self.словари.values():
            части.append(ОБЪЯВЛЕНИЯ_СЛОВАРЯ.format(D=имя))
            for тип, и in self.словари.items():
                if и == имя and self.сравнимый(тип):
                    части.append(f"static bool {имя}_eq(const {имя} *x, const {имя} *y);")
        for тип, имя in self.списки.items():
            тип_эл = элемент(тип)
            части.append(ШАБЛОН_СПИСКА.format(
                описание=тип, S=имя, T=self.тип_c(тип_эл),
                запись_элемента=self.запись(тип_эл, в_списке=True),
                строка_элемента=self.строка_элемента(тип_эл),
                SH=self.удержать(тип_эл) or "DAN_ID", RL=self.отпустить(тип_эл) or "DAN_NOREL"))
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
            найти = "" if это_функция(з) else ШАБЛОН_ПОИСКА_В_СЛОВАРЕ.format(
                D=имя, K=self.тип_c(к), HASH=ХЕШИ[к], O=self.тип_c(необяз),
                O_SOME=(self.тип_c(необяз) + "_some") if not это_необяз(з) else "")
            части.append(ШАБЛОН_СЛОВАРЯ.format(
                описание=тип, D=имя, K=self.тип_c(к), V=self.тип_c(з),
                EQ_K=self.равенство(к), HASH=ХЕШИ[к], СТРОКА_К=self.строка_элемента(к),
                СТРОКА_З=self.строка_элемента(з), НАЙТИ=найти,
                LK=self.структура("[" + к + "]"), LV=self.структура("[" + з + "]"),
                SK=self.удержать(к) or "DAN_ID", RK=self.отпустить(к) or "DAN_NOREL",
                SV=self.удержать(з) or "DAN_ID", RV=self.отпустить(з) or "DAN_NOREL"))
            if self.сравнимый(тип):
                части.append(ШАБЛОН_РАВЕНСТВА_СЛОВАРЯ.format(D=имя, V=self.тип_c(з), EQ_V=self.равенство(з)))
        части.append(self._функции_типов())
        части.append(self._функции_перечислений())
        части.append(self._функции_расширения())
        части.append("\n".join(self.помощники.values()))
        if "[компл]" in self.списки:
            части.append(КОМПЛЕКСНЫЕ_КОРНИ_C)
        части.append(self._функции_клонов())
        части.append(self._функции_каналов())
        части.append(self._функции_задач())
        if self.нужны_данные:
            части.append(dannye.код_c())
        if self.нужна_математика:
            части.append(matematika.КОД_C)
            части.append(matematika.код_c_дополнения())
            части.append(matematika._код_многочленов_строки())
            части.append(matematika.КОД_МНОГОЧЛЕНОВ)
            части.append(vysshaya.КОД_C)
        части.append("\n".join(self.строки))
        постоянные = [f"DAN_STATIC_STR({имя}, {строка_c(текст)});" for текст, имя in self.литералы.items()]
        части.insert(2, "\n".join(["/* ---- string constants ---- */"] + постоянные) + "\n")
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
                строки.append(f"static inline dan_str {имя}_str({имя} v) {{ (void)v; return {self.литерал('<функция>')}; }}")
                строки.append(f"static inline void {имя}_write({имя} v) {{ fputs({имя}_str(v), stdout); }}")
                строки.append(f"static inline {имя} {имя}_share({имя} v) {{ dan_env_share(v.env); return v; }}")
                строки.append(f"static inline void {имя}_release({имя} v) {{ dan_env_release(v.env); }}")
                строки.append(f"static inline void {имя}_give({имя} v) {{ dan_env_give(v.env); }}")
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
            строки.append(f"struct {имя}_s {{ dan_hdr h; int tag;{объединение} }};")
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                параметры = ", ".join(f"{self.тип_c(т)} a{и}" for и, (_, т) in enumerate(поля)) or "void"
                строки.append(f"static {имя} {имя}_mk{номер}({параметры});")
            строки.append(f"static void {имя}_destroy(dan_hdr *h);")
            строки.append(f"static inline {имя} {имя}_share({имя} v) {{ if (v) dan_hdr_share((dan_hdr *)&v->h); return v; }}")
            строки.append(f"static inline void {имя}_release({имя} v) {{ if (v) dan_hdr_release((dan_hdr *)&v->h, {имя}_destroy); }}")
            строки.append(f"static inline void {имя}_give({имя} v) {{ if (v) dan_hdr_give((dan_hdr *)&v->h, {имя}_destroy); }}")
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
                строки += [f"static {имя} {имя}_share({имя} v);", f"static void {имя}_release({имя} v);",
                           f"static void {имя}_give({имя} v);"]
                continue
            if это_необяз(тип):
                т_c = self.тип_c(основа(тип))
                if self.по_ссылке(тип):
                    строки += [
                        f"typedef struct {{ dan_hdr h; {т_c} v; }} {имя}_cell;",
                        f"static inline bool {имя}_has({имя} o) {{ return o != NULL; }}",
                        f"static inline {т_c} {имя}_val({имя} o) {{ return *o; }}",
                        f"static inline {имя} {имя}_none(void) {{ return NULL; }}",
                        f"static inline dan_hdr *{имя}_hdr({имя} o) {{ return (dan_hdr *)(void *)((char *)(void *)o - offsetof({имя}_cell, v)); }}",
                        f"static void {имя}_destroy(dan_hdr *h);",
                        f"static {имя} {имя}_some({т_c} v);",
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
            if self.куча(тип):
                строки += [f"static {имя} {имя}_share({имя} v);", f"static void {имя}_release({имя} v);",
                           f"static void {имя}_give({имя} v);"]
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
            # Ячейка варианта учитывает свои поля: при создании они получают
            # место хранения, при освобождении ячейки - теряют его.
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                параметры = ", ".join(f"{self.тип_c(т)} a{и}" for и, (_, т) in enumerate(поля)) or "void"
                присвоить = " ".join(f"c->u.v{номер}.{self.поле_c((тип, вариант), п)} = "
                                     f"{self._учесть(т, f'a{и}')};" for и, (п, т) in enumerate(поля))
                строки.append(f"static {имя} {имя}_mk{номер}({параметры}) {{ {имя}_s *c = dan_alloc(sizeof *c); "
                              f"dan_hdr_new(&c->h, {имя}_destroy); c->tag = {номер}; {присвоить} return c; }}")
            строки.append(f"static void {имя}_destroy(dan_hdr *h) {{")
            строки.append(f"    {имя}_s *c = ({имя}_s *)(void *)h;")
            строки.append("    switch (c->tag) {")
            for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                освободить = " ".join(f"{self.отпустить(т)}(c->u.v{номер}.{self.поле_c((тип, вариант), п)});"
                                      for п, т in поля if self.отпустить(т))
                if освободить:
                    строки.append(f"    case {номер}: {освободить} break;")
            строки.append("    default: break;")
            строки.append("    }")
            строки.append("    free(c);")
            строки.append("}")
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
            строки.append("    return dan_buf_str(&b);")
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
        ничего_значение = self.литерал("ничего")
        for тип, имя in self.значимые.items():
            if это_функция(тип):
                continue
            if это_результат(тип):
                т = основа(тип)
                for действие in ("share", "release", "give"):
                    ф = self._учёт(т, действие) if т != ПУСТО else None
                    знач = (f"v.value = {ф}(v.value);" if действие == "share" else f"{ф}(v.value);") if ф else ""
                    ошибка = (f"v.err = dan_str_share(v.err);" if действие == "share"
                              else f"dan_str_{действие}(v.err);")
                    возврат = f"{имя}" if действие == "share" else "void"
                    конец = " return v;" if действие == "share" else ""
                    строки.append(f"static {возврат} {имя}_{действие}({имя} v) {{ if (v.ok) {{ {знач} }} "
                                  f"else {{ {ошибка} }}{конец} }}")
                continue
            строки.append(f"/* ---- {тип} ---- */")
            if это_необяз(тип):
                т = основа(тип)
                знач = f"{имя}_val(v)"
                строки.append(f"static dan_str {имя}_str({имя} v) {{ return {имя}_has(v) ? {self.в_строку(т, знач)} : {ничего_значение}; }}")
                строки.append(f"static dan_str {имя}_qstr({имя} v) {{ return {имя}_has(v) ? {self.строка_элемента(т)}({знач}) : {ничего_значение}; }}")
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
                              f"{имя}_strb(v, &b); return dan_buf_str(&b); }}")
                if self.сравнимый(тип):
                    части = []
                    for поле, т, _ in поля:
                        ф = self.поле_c(тип, поле)
                        части.append(self._подставить(self.равенство(т), f"a.{ф}", f"b.{ф}"))
                    строки.append(f"static bool {имя}_eq({имя} a, {имя} b) {{ {проверка}return {' && '.join(части)}; }}")
            строки.append(f"static void {имя}_write({имя} v) {{ fputs({имя}_str(v), stdout); }}")
            if not self.куча(тип):
                continue
            if self.по_ссылке(тип):
                # необязательное значение рекурсивного типа: неизменяемая ячейка
                т = основа(тип)
                т_c = self.тип_c(т)
                освободить = f"{self.отпустить(т)}(c->v);" if self.отпустить(т) else ""
                строки += [
                    f"static void {имя}_destroy(dan_hdr *h) {{ {имя}_cell *c = ({имя}_cell *)(void *)h; {освободить} free(c); }}",
                    f"static {имя} {имя}_some({т_c} v) {{ {имя}_cell *c = dan_alloc(sizeof *c); "
                    f"dan_hdr_new(&c->h, {имя}_destroy); c->v = {self._учесть(т, 'v')}; return &c->v; }}",
                    f"static {имя} {имя}_share({имя} v) {{ if (v) dan_hdr_share({имя}_hdr(v)); return v; }}",
                    f"static void {имя}_release({имя} v) {{ if (v) dan_hdr_release({имя}_hdr(v), {имя}_destroy); }}",
                    f"static void {имя}_give({имя} v) {{ if (v) dan_hdr_give({имя}_hdr(v), {имя}_destroy); }}",
                ]
                continue
            if это_необяз(тип):
                т = основа(тип)
                for действие in ("share", "release", "give"):
                    ф = self._учёт(т, действие)
                    if действие == "share":
                        строки.append(f"static {имя} {имя}_share({имя} v) {{ if (v.has) v.value = {ф}(v.value); return v; }}")
                    else:
                        строки.append(f"static void {имя}_{действие}({имя} v) {{ if (v.has) {ф}(v.value); }}")
                continue
            проверка = "DAN_STACK_CHECK(0); " if тип in рекурсивные else ""
            for действие in ("share", "release", "give"):
                поля = [(self.поле_c(тип, п), self._учёт(т, действие)) for п, т, _ in self.собственные[тип]]
                if действие == "share":
                    тело = " ".join(f"v.{ф} = {д}(v.{ф});" for ф, д in поля if д)
                    строки.append(f"static {имя} {имя}_share({имя} v) {{ {проверка}{тело} return v; }}")
                else:
                    тело = " ".join(f"{д}(v.{ф});" for ф, д in поля if д)
                    строки.append(f"static void {имя}_{действие}({имя} v) {{ {проверка}{тело} }}")
        return "\n".join(строки) + "\n"

    def клон(self, тип):
        """Функция C, копирующая значение для другой задачи (или None, если копия не нужна)."""
        if not self.куча(тип):
            return None
        if тип == СТР:
            return "dan_str_clone"
        if тип == БАЙТЫ:
            return "dan_bytes_clone"
        if тип == ДЛИН:
            return "dan_big_clone"
        if это_канал(тип):
            self.канал_c(тип)
            return "dan_chan_clone"
        if тип not in self.клоны:
            if это_список(тип):
                имя = self.структура(тип) + "_clone"
            elif это_словарь(тип):
                имя = self.словарь_c(тип) + "_clone"
            elif тип in self.перечисления:
                имя = self.перечисление_c(тип) + "_clone"
            else:
                имя = self.значимый(тип) + "_clone"
            self.клоны[тип] = имя
            # вложенные типы тоже понадобятся
            if это_список(тип):
                self.клон(элемент(тип))
            elif это_словарь(тип):
                self.клон(ключ_словаря(тип)); self.клон(значение_словаря(тип))
            elif тип in self.перечисления:
                for _, поля in self.перечисления[тип]:
                    for _, т in поля:
                        self.клон(т)
            elif это_необяз(тип) or это_результат(тип):
                if основа(тип) != ПУСТО:
                    self.клон(основа(тип))
            elif тип in self.собственные:
                for _, т, _ in self.собственные[тип]:
                    self.клон(т)
        return self.клоны[тип]

    def _скопировать(self, тип, код):
        ф = self.клон(тип)
        return f"{ф}({код})" if ф else код

    def _функции_клонов(self):
        """Копирование значений для задач: новые значения с одним местом хранения, без таблицы временных."""
        объявления, определения = [], []
        обработанные = set()
        while len(обработанные) < len(self.клоны):
            for тип, имя in list(self.клоны.items()):
                if тип in обработанные:
                    continue
                обработанные.add(тип)
                т_c = self.тип_c(тип)
                if это_список(тип):
                    S = self.структура(тип)
                    объявления.append(f"static {S} *{имя}(const {S} *l);")
                    определения.append(
                        f"static {S} *{имя}(const {S} *l) {{ {S} *r = dan_alloc(sizeof *r); dan_hdr_detached(&r->h); "
                        f"r->len = l->len; r->cap = l->len < 4 ? 4 : l->len; "
                        f"r->items = dan_alloc((size_t)r->cap * sizeof *r->items); "
                        f"for (int64_t i = 0; i < l->len; i++) r->items[i] = {self._скопировать(элемент(тип), 'l->items[i]')}; "
                        f"return r; }}")
                elif это_словарь(тип):
                    D = self.словарь_c(тип)
                    к, з = ключ_словаря(тип), значение_словаря(тип)
                    объявления.append(f"static {D} *{имя}(const {D} *d);")
                    определения.append(
                        f"static {D} *{имя}(const {D} *d) {{ {D} *r = {D}_new_detached(); "
                        f"for (int64_t e = 0; e < d->n; e++) if (d->alive[e]) "
                        f"{D}_put_raw(r, {self._скопировать(к, 'd->keys[e]')}, {self._скопировать(з, 'd->vals[e]')}); "
                        f"return r; }}")
                elif тип in self.перечисления:
                    V = self.перечисление_c(тип)
                    объявления.append(f"static {V} {имя}({V} v);")
                    ветви = []
                    for номер, (вариант, поля) in enumerate(self.перечисления[тип]):
                        копии = " ".join(f"c->u.v{номер}.{self.поле_c((тип, вариант), п)} = "
                                         f"{self._скопировать(т, f'v->u.v{номер}.' + self.поле_c((тип, вариант), п))};"
                                         for п, т in поля)
                        if копии:
                            ветви.append(f"case {номер}: {копии} break;")
                    определения.append(
                        f"static {V} {имя}({V} v) {{ if (!v) return v; DAN_STACK_CHECK(0); {V}_s *c = dan_alloc(sizeof *c); "
                        f"*c = *v; dan_hdr_detached(&c->h); switch (v->tag) {{ {' '.join(ветви)} default: break; }} return c; }}")
                elif это_функция(тип):
                    объявления.append(f"static {т_c} {имя}({т_c} v);")
                    определения.append(f"static {т_c} {имя}({т_c} v) {{ v.env = dan_env_clone(v.env); return v; }}")
                elif self.по_ссылке(тип):
                    т = основа(тип)
                    объявления.append(f"static {т_c} {имя}({т_c} v);")
                    определения.append(
                        f"static {т_c} {имя}({т_c} v) {{ if (!v) return v; DAN_STACK_CHECK(0); {т_c}_cell *c = dan_alloc(sizeof *c); "
                        f"dan_hdr_detached(&c->h); c->v = {self._скопировать(т, '*v')}; return &c->v; }}")
                elif это_результат(тип):
                    т = основа(тип)
                    значение = f"v.value = {self._скопировать(т, 'v.value')};" if т != ПУСТО and self.клон(т) else ""
                    объявления.append(f"static {т_c} {имя}({т_c} v);")
                    определения.append(f"static {т_c} {имя}({т_c} v) {{ if (v.ok) {{ {значение} }} else "
                                       f"v.err = dan_str_clone(v.err); return v; }}")
                elif это_необяз(тип):
                    объявления.append(f"static {т_c} {имя}({т_c} v);")
                    определения.append(f"static {т_c} {имя}({т_c} v) {{ if (v.has) v.value = "
                                       f"{self._скопировать(основа(тип), 'v.value')}; return v; }}")
                else:
                    объявления.append(f"static {т_c} {имя}({т_c} v);")
                    поля = " ".join(f"v.{self.поле_c(тип, п)} = {self._скопировать(т, 'v.' + self.поле_c(тип, п))};"
                                    for п, т, _ in self.собственные[тип] if self.клон(т))
                    определения.append(f"static {т_c} {имя}({т_c} v) {{ DAN_STACK_CHECK(0); {поля} return v; }}")
        return "\n".join(объявления + определения) + "\n"

    def канал_c(self, тип):
        """Регистрирует функции канала для типа элементов; возвращает часть имени."""
        т = элемент_канала(тип)
        if т not in self.каналы:
            self.нужны_задачи = True
            self.каналы[т] = "dk_" + self.код_типа(т)
            self.тип_c(т)
            self.тип_c(т + "?")
            self.клон(т)
        return self.каналы[т]

    def _функции_каналов(self):
        строки = []
        for т, имя in self.каналы.items():
            т_c, о = self.тип_c(т), self.тип_c(т + "?")
            освободить = f"{self.отпустить(т)}(*({т_c} *)p);" if self.отпустить(т) else "(void)p;"
            отдать = f"{self.отдать(т)}(x);" if self.отдать(т) else ""
            строки += [
                f"static void {имя}_drop(void *p) {{ {освободить} }}",
                f"static dan_chan *{имя}_new(int64_t cap, int line) {{ return dan_chan_new(sizeof({т_c}), cap, {имя}_drop, line); }}",
                f"static void {имя}_send(dan_chan *c, {т_c} v, int line) {{ {т_c} x = {self._скопировать(т, 'v')}; "
                f"dan_chan_send(c, &x, line); }}",
                f"static {о} {имя}_recv(dan_chan *c, int line) {{ {т_c} x; if (!dan_chan_recv(c, &x, line)) "
                f"return {о}_none(); {отдать} return {о}_some(x); }}",
            ]
        return "\n".join(строки) + "\n"

    def задача_c(self, тип):
        """Структура C задачи с местом для её результата."""
        результат = тип[len("задача<"):-1]
        if результат not in self.задачи_c:
            self.нужны_задачи = True
            имя = "dz_" + (self.код_типа(результат) if результат != ПУСТО else "v")
            self.задачи_c[результат] = имя
            if результат != ПУСТО:
                self.тип_c(результат)
                self.клон(результат)
        return self.задачи_c[результат]

    def _определения_задач(self):
        строки = []
        for результат, имя in self.задачи_c.items():
            if результат == ПУСТО:
                строки.append(f"typedef struct {{ dan_task t; }} {имя};")
            else:
                строки.append(f"typedef struct {{ dan_task t; {self.тип_c(результат)} result; }} {имя};")
        return "\n".join(строки) + "\n"

    def _функции_задач(self):
        строки = []
        for результат, имя in self.задачи_c.items():
            освободить = f"{self.отпустить(результат)}(z->result);" if результат != ПУСТО and self.отпустить(результат) else ""
            строки.append(f"static void {имя}_end({имя} *z, int line) {{ dan_task_join(&z->t, line); {освободить} }}")
        return "\n".join(строки) + "\n"

    def _итоговый_тип(self, узел):
        return (getattr(узел, "в_результат", None) or getattr(узел, "в_необяз", None)
                or getattr(узел, "привести_к", None) or узел.тип_выр)

    def _запустить(self, запуск, задача, результат):
        """Код запуска задачи: значения для вызова вычисляются здесь, копии получает задача."""
        self.нужны_задачи = True
        вызов = запуск.вызов
        значения = list(вызов.аргументы)
        if isinstance(вызов.функция, у.Поле):
            значения.insert(0, вызов.функция.объект)
        elif getattr(вызов, "вызов_значения", None):
            значения.insert(0, вызов.функция)
        self.счётчик += 1
        номер = self.счётчик
        А = f"dan_targs_{номер}"
        типы = [self._итоговый_тип(з) for з in значения]
        тип_результата = запуск.тип_результата
        поля = " ".join(f"{self.тип_c(т)} a{и};" for и, т in enumerate(типы))
        # функция задачи: вызов с полученными копиями
        сохр = (self.строки, self.отступ, getattr(self, "текущий_результат", None),
                self.области, self.метка_функции, self.метки_циклов, self.группы)
        self.строки, self.отступ, self.текущий_результат = [], 1, None
        self.области, self.метка_функции, self.метки_циклов, self.группы = [[]], "dan_fmark", [], []
        for и, з in enumerate(значения):
            з.готовый_код = f"a->a{и}"
        self.пиши(f"{А} *a = p;")
        self.пиши("dan_task_begin(a->task);")
        self.пиши("{")
        self.пиши(f"    const size_t {self.метка_функции} = dan_mark();")
        код = self.выр(вызов)
        if тип_результата in (ПУСТО, None):
            self.пиши(f"    {код};")
        else:
            self.пиши(f"    {self.тип_c(тип_результата)} const r = {код};")
            self.пиши(f"    if (a->out) *a->out = {self._скопировать(тип_результата, 'r')};")
        self.пиши(f"    dan_sweep({self.метка_функции});")
        self.пиши("}")
        for и, т in enumerate(типы):
            if self.отпустить(т):
                self.пиши(f"{self.отпустить(т)}(a->a{и});")
        self.пиши("dan_task *t = a->task;")
        self.пиши("free(a);")
        self.пиши("dan_task_end(t);")
        self.пиши("return DAN_THREAD_RETURN;")
        for з in значения:
            del з.готовый_код
        тело = self.строки
        (self.строки, self.отступ, self.текущий_результат,
         self.области, self.метка_функции, self.метки_циклов, self.группы) = сохр
        выход = "" if тип_результата in (ПУСТО, None) else f" {self.тип_c(тип_результата)} *out;"
        self.лямбды_код.append(f"typedef struct {{ dan_task *task;{выход} {поля} }} {А};")
        self.лямбды_код.append(f"static DAN_THREAD_FN dan_task_entry_{номер}(void *p) {{")
        self.лямбды_код += тело
        self.лямбды_код.append("}")
        # запуск в текущей функции
        присвоения = " ".join(f"a->a{и} = {self._скопировать(т, self.выр(з))};" for и, (з, т) in enumerate(zip(значения, типы)))
        выход = "" if тип_результата in (ПУСТО, None) else f" a->out = {результат or 'NULL'};"
        self.пиши(f"{{ {А} *a = dan_alloc(sizeof *a); a->task = {задача};{выход} {присвоения} "
                  f"dan_task_start(a->task, dan_task_entry_{номер}, a, {запуск.строка}); }}")

    def _учесть(self, тип, код):
        """Код значения, которое получает место хранения (удержанное значение)."""
        ф = self.удержать(тип)
        return f"{ф}({код})" if ф else код

    def литерал(self, текст):
        """Строковая постоянная программы: строка с заголовком, которая не освобождается."""
        if текст not in self.литералы:
            self.литералы[текст] = f"dan_lit_{len(self.литералы) + 1}"
        return f"{self.литералы[текст]}.s"

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

    ВНЕШНИЕ_C = {"цел": "int64_t", "цел32": "int32_t", "дроб": "double", "дроб32": "float", "лог": "int",
                 "стр": "const char *", "стр?": "const char *"}

    def _внешние_функции(self, программа):
        """Функции библиотек C: объявление под служебным именем и обёртка с типами Дана."""
        строки = []
        for блок in программа.инструкции:
            if not isinstance(блок, у.ВнешнийБлок):
                continue
            if блок.библиотека:
                строки.append(f"/* DAN_LINK: {блок.библиотека} */")
            for ф in блок.функции:
                self.счётчик += 1
                служебное = f"dan_ext_{self.счётчик}"
                рез = ф.тип_результата
                рез_c = self.ВНЕШНИЕ_C.get(рез, "void") if рез else "void"
                параметры_c = ", ".join(self.ВНЕШНИЕ_C[п.тип] for п in ф.параметры) or "void"
                строки.append(f"extern {рез_c} {служебное}({параметры_c}) __asm__(DAN_SYM({строка_c(ф.имя_c)}));")
                тип_дана = {"цел32": ЦЕЛ, "дроб32": ДРОБ}
                параметры = ", ".join(f"{self.тип_c(тип_дана.get(п.тип, п.тип))} a{и}" for и, п in enumerate(ф.параметры))
                проверки, аргументы = [], []
                for и, п in enumerate(ф.параметры):
                    if п.тип == "цел32":
                        проверки.append(f"if (a{и} < INT32_MIN || a{и} > INT32_MAX) dan_error(DAN_MSG_EXT_RANGE, {ф.строка});")
                        аргументы.append(f"(int32_t)a{и}")
                    elif п.тип == "дроб32":
                        аргументы.append(f"(float)a{и}")
                    elif п.тип == "лог":
                        аргументы.append(f"(int)a{и}")
                    else:
                        аргументы.append(f"a{и}")
                вызов = f"{служебное}({', '.join(аргументы)})"
                if not рез:
                    возврат, тип_р = f"{вызов};", "void"
                elif рез == "стр":
                    возврат, тип_р = f"const char *r = {вызов}; return r ? dan_strdup(r) : dan_empty_s.s;", "dan_str"
                elif рез == "стр?":
                    о = self.тип_c("стр?")
                    возврат, тип_р = f"const char *r = {вызов}; return r ? {о}_some(dan_strdup(r)) : {о}_none();", о
                elif рез == "лог":
                    возврат, тип_р = f"return {вызов} != 0;", "bool"
                else:
                    тип_р = self.тип_c(тип_дана.get(рез, рез))
                    возврат = f"return ({тип_р}){вызов};"
                строки.append(f"static {тип_р} {self.имя(ф.имя)}({параметры or 'void'}) {{ {' '.join(проверки)} {возврат} }}")
        return строки

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
        self.метка_функции, self.области, self.метки_циклов = "dan_fmark", [[]], []
        if self._куча_в(ф.тело, глубоко=True) or self._куча_тип(ф.тип_результата or ПУСТО):
            self.пиши(f"const size_t {self.метка_функции} = dan_mark();")
        else:
            self.метка_функции = None      # функция не создаёт значений в памяти
        for и in ф.тело.инструкции:
            self.инструкция(и)
        if ф.тип_результата in (None, ПУСТО, ПУСТО + "!") and not self._завершает(ф.тело):
            self._выход_из_функции()
        if ф.тип_результата == ПУСТО + "!":
            self.пиши(f"return {self.тип_c(ф.тип_результата)}_some();")
        self.текущий_результат = None
        self.отступ -= 1
        self.пиши("}")

    # ---------- инструкции ----------

    def блок(self, блок):
        self.отступ += 1
        self.области.append([])
        for и in блок.инструкции:
            self.инструкция(и)
        область = self.области.pop()
        if not self._завершает(блок):
            self._освободить(область)
        self.отступ -= 1

    @staticmethod
    def _завершает(блок):
        """Кончается ли блок переходом (вернуть, прервать, продолжить)."""
        return bool(блок.инструкции) and isinstance(блок.инструкции[-1], (у.Вернуть, у.Прервать, у.Продолжить))

    # ---------- память: места хранения и временные значения ----------

    def _код_отпустить(self, имя_c, тип):
        if тип.startswith("<группа:"):
            return f"dan_tgroup_join(&{имя_c}, {тип[len('<группа:'):-1]});"     # блок ждёт свои задачи
        if это_задача(тип):
            return f"{self.задача_c(тип)}_end(&{имя_c}, {имя_c}.t.line);"       # задача не переживает блок
        return f"{self.отпустить(тип)}({имя_c});"

    def _освободить(self, область):
        for имя_c, тип in reversed(область):
            self.пиши(self._код_отпустить(имя_c, тип))

    def _код_освобождения(self, от=0):
        """Код C, освобождающий переменные областей начиная с номера «от»."""
        return " ".join(self._код_отпустить(имя_c, тип)
                        for область in reversed(self.области[от:]) for имя_c, тип in reversed(область))

    def _выход_из_функции(self):
        код = self._код_освобождения()
        if код:
            self.пиши(код)
        if self.метка_функции:
            self.пиши(f"dan_sweep({self.метка_функции});")

    def _код_возврата(self, код, тип):
        """Код C возврата значения: значение переживает освобождение переменных функции."""
        уборка = f"dan_sweep({self.метка_функции});" if self.метка_функции else ""
        освободить = self._код_освобождения()
        if тип in (None, ПУСТО):
            return f"{освободить} {уборка} return;".strip()
        if not освободить and not уборка:
            return f"return {код};"
        результат = self.служебное_имя("ret")
        удержать, отдать = self.удержать(тип), self.отдать(тип)
        if not удержать:
            return (f"{{ {self.тип_c(тип)} const {результат} = {код}; {освободить} "
                    f"{уборка} return {результат}; }}")
        return (f"{{ {self.тип_c(тип)} const {результат} = {удержать}({код}); {освободить} "
                f"{уборка} {отдать}({результат}); return {результат}; }}")

    def _куча_в(self, узел, глубоко=False):
        """Есть ли в выражении значения, которые могут оказаться временными в памяти;
        «глубоко» - и во вложенных блоках инструкций."""
        if isinstance(узел, list):
            return any(self._куча_в(э, глубоко) for э in узел)
        if not isinstance(узел, у.Узел):
            return False
        for признак in ("тип_выр", "привести_к", "в_необяз", "в_результат"):
            т = getattr(узел, признак, None)
            if isinstance(т, str) and т not in (ОШИБКА, НИЧЕГО, "функция", ДИАПАЗОН, ПУСТО) \
                    and self._куча_тип(т):
                return True
        if isinstance(узел, у.Лямбда):
            return True
        return any(self._куча_в(getattr(узел, п.name), глубоко) for п in fields(узел)
                   if (глубоко or п.name not in ("тело",)) and isinstance(getattr(узел, п.name), (у.Узел, list)))

    def _куча_тип(self, тип):
        try:
            return self.куча(тип)
        except (KeyError, ValueError, IndexError):
            return True

    def _выражения(self, и):
        """Выражения, которые вычисляет сама инструкция (без вложенных блоков)."""
        if isinstance(и, у.ОбъявлениеПеременной) or isinstance(и, у.Присваивание):
            return [и.значение]
        if isinstance(и, у.ПрисваиваниеЭлемента):
            return [и.объект, и.индекс, и.значение]
        if isinstance(и, у.ПрисваиваниеПоля):
            return [и.объект, и.значение]
        if isinstance(и, у.Выражение):
            return [и.выражение]
        if isinstance(и, у.Если):
            итог = [и.условие]
            while isinstance(и.иначе, у.Если):
                и = и.иначе
                итог.append(и.условие)
            return итог
        if isinstance(и, (у.ЕслиЕсть, у.ЕслиОшибка, у.Выбор)):
            return [и.значение]
        if isinstance(и, у.Для):
            return [и.источник]
        return []

    def инструкция(self, и):
        метка = None
        if self._куча_в(self._выражения(и)):
            метка = self.служебное_имя("m")
            self.пиши(f"const size_t {метка} = dan_mark();")
        getattr(self, "и_" + type(и).__name__)(и)
        if метка:
            self.пиши(f"dan_sweep({метка});")

    def и_ОбъявлениеПеременной(self, и):
        if isinstance(и.значение, у.Запуск):
            имя = self.имя(и.имя)
            тип = и.тип_итог
            self.пиши(f"{self.задача_c(тип)} {имя};")
            результат = None if и.значение.тип_результата == ПУСТО else f"&{имя}.result"
            self._запустить(и.значение, f"&{имя}.t", результат)
            self.области[-1].append((имя, тип))
            return
        модификатор = "" if и.изменяемая else " const"
        значение = self.сохранить(и.значение, self.выр(и.значение), и.тип_итог)
        self.пиши(f"{self.тип_c(и.тип_итог)}{модификатор} {self.имя(и.имя)} = {значение};")
        if self.куча(и.тип_итог):
            self.области[-1].append((self.имя(и.имя), и.тип_итог))

    def _заменить(self, место, значение, тип):
        """Новое значение в месте хранения: сначала оно получает место, затем прежнее его теряет."""
        if not self.куча(тип):
            self.пиши(f"{место} = {значение};")
            return
        прежнее = self.служебное_имя("old")
        self.пиши(f"{{ {self.тип_c(тип)} {прежнее} = {место}; {место} = {self.удержать(тип)}({значение}); "
                  f"{self.отпустить(тип)}({прежнее}); }}")

    def и_Присваивание(self, и):
        цель = self.имя(и.имя)
        значение = self.выр(и.значение)
        if и.оператор == "=":
            self._заменить(цель, значение, и.тип_цели)
            return
        результат = self._арифметика(и.оператор[0], цель, значение, и.тип_результата, и.строка)
        self._заменить(цель, результат, и.тип_цели)

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
        значение = self.выр(и.значение)
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
        значение = self.выр(и.значение)
        if и.оператор == "=":
            self.пиши(f"{D}_put({словарь}, {ключ}, {значение});")
        else:
            место = self.служебное_имя("p")
            self.пиши(f"{self.тип_c(и.тип_элемента)} *const {место} = {D}_at({словарь}, {ключ}, {и.строка});")
            результат = self._арифметика(и.оператор[0], f"*{место}", значение, и.тип_результата, и.строка)
            self._заменить(f"*{место}", результат, и.тип_элемента)
        self.отступ -= 1
        self.пиши("}")

    def и_ОбъявлениеТипа(self, и):
        pass                       # определение типа выводится в начале файла C

    def и_ОбъявлениеВариантов(self, и):
        pass

    def и_ВнешнийБлок(self, и):
        pass                       # объявления функций C стоят перед функциями программы

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
        self.пиши(f"__auto_type const {значение} = {self.выр(и.значение)};")
        поле = у.Поле(и.объект, и.имя, строка=и.строка, столбец=и.столбец)
        self.пиши(f"{self.тип_c(и.тип_поля)} *const {место} = &{self.место(поле)};")
        if и.оператор == "=":
            self._заменить(f"*{место}", значение, и.тип_поля)
        else:
            результат = self._арифметика(и.оператор[0], f"*{место}", значение, и.тип_результата, и.строка)
            self._заменить(f"*{место}", результат, и.тип_поля)
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
        if isinstance(и.выражение, у.Запуск):
            запуск = и.выражение
            тип = f"задача<{запуск.тип_результата}>"
            dz, h = self.задача_c(тип), self.служебное_имя("task")
            self.пиши(f"{{ {dz} *{h} = dan_alloc(sizeof *{h});")
            self._запустить(запуск, f"&{h}->t", None)
            self.пиши(f"dan_tgroup_add(&{self.группы[-1]}, &{h}->t); }}")
            return
        self.пиши(f"{self.выр(и.выражение)};")

    def и_Параллельно(self, и):
        self.нужны_задачи = True
        группа = self.служебное_имя("group")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"dan_tgroup {группа} = {{ NULL }};")
        self.области.append([(группа, f"<группа:{и.строка}>")])
        self.группы.append(группа)
        self.пиши("{")
        self.блок(и.тело)
        self.пиши("}")
        self.группы.pop()
        self._освободить(self.области.pop())
        self.отступ -= 1
        self.пиши("}")

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
        if self._куча_в(и.условие):
            # значения, созданные при проверке условия, освобождаются на каждом шаге
            self.пиши("for (;;) {")
            метка, условие = self.служебное_имя("mc"), self.служебное_имя("c")
            self.пиши(f"    const size_t {метка} = dan_mark();")
            self.пиши(f"    const bool {условие} = {self.выр(и.условие)};")
            self.пиши(f"    dan_sweep({метка});")
            self.пиши(f"    if (!{условие}) break;")
        else:
            self.пиши(f"while ({self.выр(и.условие)}) {{")
        self._тело_цикла(и.тело)
        self.пиши("}")

    def _тело_цикла(self, тело):
        метка = None
        if self._есть_выход_из_шага(тело.инструкции):
            метка = self.служебное_имя("ml")
            self.пиши(f"    const size_t {метка} = dan_mark();")
        self.метки_циклов.append((метка, len(self.области)))
        self.блок(тело)
        self.метки_циклов.pop()

    def _есть_выход_из_шага(self, узел):
        """Есть ли в теле цикла «прервать» или «продолжить» (не во вложенных циклах)."""
        if isinstance(узел, list):
            return any(self._есть_выход_из_шага(э) for э in узел)
        if isinstance(узел, (у.Прервать, у.Продолжить)):
            return True
        if not isinstance(узел, у.Узел) or isinstance(узел, (у.Пока, у.Для, у.Лямбда)):
            return False
        return any(self._есть_выход_из_шага(getattr(узел, п.name)) for п in fields(узел)
                   if isinstance(getattr(узел, п.name), (у.Узел, list)))

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
            self._тело_цикла(и.тело)
            self.пиши("}")
            self.отступ -= 1
            self.пиши("}")
            return
        if getattr(и, "по_каналу", False):
            return self._цикл_по_каналу(и, перем)
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
        тип_списка = "[" + и.тип_элемента + "]"
        self.области.append([(список, тип_списка)])
        self.пиши(f"for (int64_t {номер} = 0; {номер} < {список}->len; {номер}++) {{")
        self.отступ += 1
        self.пиши(f"{self.тип_c(и.тип_элемента)} const {перем} = {список}->items[{номер}];")
        self.отступ -= 1
        self._тело_цикла(и.тело)
        self.пиши("}")
        self._освободить(self.области.pop())
        self.отступ -= 1
        self.пиши("}")

    def _цикл_по_каналу(self, и, перем):
        тип = и.источник.тип_выр
        dk = self.канал_c(тип)
        т = элемент_канала(тип)
        о = self.тип_c(т + "?")
        канал, полученное, метка = self.служебное_имя("chan"), self.служебное_имя("got"), self.служебное_имя("ml")
        self.пиши("{")
        self.отступ += 1
        self.пиши(f"dan_chan *const {канал} = dan_chan_share({self.выр(и.источник)});")
        self.области.append([(канал, тип)])
        self.пиши("for (;;) {")
        self.пиши(f"    const size_t {метка} = dan_mark();")
        self.пиши(f"    const {о} {полученное} = {dk}_recv({канал}, {и.строка});")
        self.пиши(f"    if (!{о}_has({полученное})) {{ dan_sweep({метка}); break; }}")
        self.пиши(f"    {self.тип_c(т)} const {перем} = {о}_val({полученное});")
        # полученное значение живёт до конца шага цикла
        self.метки_циклов.append((метка, len(self.области)))
        self.блок(и.тело)
        self.метки_циклов.pop()
        if not self._завершает(и.тело):
            self.пиши(f"    dan_sweep({метка});")
        self.пиши("}")
        self._освободить(self.области.pop())
        self.отступ -= 1
        self.пиши("}")

    def и_Вернуть(self, и):
        результат = getattr(self, "текущий_результат", None)
        if и.значение is None:
            if результат == ПУСТО + "!":
                self.пиши(self._код_возврата(f"{self.тип_c(ПУСТО + '!')}_some()", результат))
            else:
                self.пиши(self._код_возврата(None, None))
            return
        # Значение возврата удерживается, пока освобождаются переменные функции,
        # и передаётся вызывающей стороне как временное.
        self.пиши(self._код_возврата(self.выр(и.значение), результат))

    def _выход_из_шага(self):
        метка, от = self.метки_циклов[-1]
        код = self._код_освобождения(от)
        if код:
            self.пиши(код)
        if метка:
            self.пиши(f"dan_sweep({метка});")

    def и_Прервать(self, и):
        self._выход_из_шага()
        self.пиши("break;")

    def и_Продолжить(self, и):
        self._выход_из_шага()
        self.пиши("continue;")

    # ---------- выражения ----------

    def выр(self, в):
        готовый = getattr(в, "готовый_код", None)
        if готовый is not None:      # значение уже вычислено (передано задаче)
            return готовый
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
            при_ошибке = self._код_возврата(f"{self.тип_c(функция)}_err({временное}.err, {временное}.line)", функция)
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
        сохр = (self.строки, self.отступ, getattr(self, "текущий_результат", None),
                self.области, self.метка_функции, self.метки_циклов)
        self.строки, self.отступ, self.текущий_результат = [], 1, результат
        self.области, self.метка_функции, self.метки_циклов = [[]], "dan_fmark", []
        if захваты:
            self.пиши(f"const dan_env_{номер} *env = envp;")
            for имя, т in захваты:
                self.пиши(f"{self.тип_c(т)} const {self.имя(имя)} = env->{self.имя(имя)};")
        else:
            self.пиши("(void)envp;")
        if self._куча_в(в.тело if в.выражение is None else в.выражение, глубоко=True) or self._куча_тип(результат):
            self.пиши(f"const size_t {self.метка_функции} = dan_mark();")
        else:
            self.метка_функции = None
        if в.выражение is not None:
            код = self.выр(в.выражение)
            if результат == ПУСТО:
                self.пиши(f"{код};")
                self._выход_из_функции()
            else:
                self.пиши(self._код_возврата(код, результат))
        else:
            for и in в.тело.инструкции:
                self.инструкция(и)
            if результат in (ПУСТО, ПУСТО + "!") and not self._завершает(в.тело):
                self._выход_из_функции()
            if результат == ПУСТО + "!":
                self.пиши(f"return {self.тип_c(результат)}_some();")
        тело = self.строки
        (self.строки, self.отступ, self.текущий_результат,
         self.области, self.метка_функции, self.метки_циклов) = сохр
        параметры_c = "".join(f", {self.тип_c(т)} const {self.имя(п.имя)}" for п, т in zip(в.параметры, параметры))
        код = []
        if захваты:
            поля = " ".join(f"{self.тип_c(т)} {self.имя(имя)};" for имя, т in захваты)
            код.append(f"typedef struct {{ dan_hdr h; dan_destroy_fn d; void *(*c)(const void *); {поля} }} dan_env_{номер};")
            освободить = " ".join(f"{self.отпустить(т)}(e->{self.имя(имя)});" for имя, т in захваты if self.отпустить(т))
            код.append(f"static void dan_env_{номер}_destroy(dan_hdr *h) {{ dan_env_{номер} *e = (dan_env_{номер} *)(void *)h; "
                       f"{освободить} free(e); }}")
            копии = " ".join(f"r->{self.имя(имя)} = {self._скопировать(т, 'e->' + self.имя(имя))};" for имя, т in захваты)
            код.append(f"static void *dan_env_{номер}_clone(const void *p) {{ const dan_env_{номер} *e = p; "
                       f"dan_env_{номер} *r = dan_alloc(sizeof *r); *r = *e; dan_hdr_detached(&r->h); {копии} return r; }}")
        код.append(f"static {self.тип_c(результат)} {имя_c}(void *envp{параметры_c}) {{")
        код += тело
        код.append("}")
        self.лямбды_код += код
        if not захваты:
            return f"(({df}){{ {имя_c}, NULL }})"
        окр = self.служебное_имя("env")
        присвоения = " ".join(f"{окр}->{self.имя(имя)} = {self.сохранить(у.Имя(имя), self.имя(имя), т)};"
                              for имя, т in захваты)
        return (f"({{ dan_env_{номер} *{окр} = dan_alloc(sizeof *{окр}); "
                f"dan_hdr_new(&{окр}->h, dan_env_{номер}_destroy); {окр}->d = dan_env_{номер}_destroy; "
                f"{окр}->c = dan_env_{номер}_clone; {присвоения} "
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
        if в_тип == ДЛИН and из_типа == ЦЕЛ:
            self.нужны_длинные = True
            return f"dan_big_from_int({код})"
        if в_тип == КОМПЛ:
            self.нужны_компл = True
            if из_типа == РАЦ:
                return f"dan_cx_make(dan_rat_to_float({код}), 0)"
            return f"dan_cx_make((double)({код}), 0)"
        return код

    def в_ЦелоеЧисло(self, в):
        return f"INT64_C({в.значение})"

    def в_ДробноеЧисло(self, в):
        return дробь_c(в.значение)

    def в_Строка(self, в):
        return self.литерал(в.значение)

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
        if в.оператор == "-" and в.тип_выр == ДЛИН:
            return f"dan_big_neg({self.выр(в.операнд)}, {в.строка})"
        if в.оператор == "-" and в.тип_выр == КОМПЛ:
            return f"dan_cx_neg({self.выр(в.операнд)})"
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
            if в.тип_выр == ДЛИН:
                return f"dan_big_pow({л}, {п}, {в.строка})"
            if в.тип_выр == КОМПЛ:
                if (getattr(в.правый, "привести_к", None) or в.правый.тип_выр) == ЦЕЛ:
                    return f"dan_cx_pow_int({л}, {п}, {в.строка})"
                return f"dan_cx_pow({л}, {п}, {в.строка})"
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
        if getattr(в, "тип_операндов", None) == ДЛИН:
            return f"(dan_big_cmp({л}, {п}) {оп} 0)"
        if getattr(в, "тип_операндов", None) == КОМПЛ:
            равно = f"dan_cx_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
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
        if тип == ДЛИН:
            return f"{ОПЕРАЦИИ_ДЛИН[оп]}({л}, {п}, {строка})"
        if тип == КОМПЛ:
            return f"{ОПЕРАЦИИ_КОМПЛ[оп]}({л}, {п}, {строка})"
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
        элементы = ", ".join(self.выр(э) for э in в.элементы)
        return f"{структура}_from({len(в.элементы)}, ({self.тип_c(элемент(тип))}[]){{{элементы}}})"

    def в_Словарь(self, в):
        тип = в.тип_выр
        D = self.словарь_c(тип)
        if not в.ключи:
            return f"{D}_new()"
        к, з = ключ_словаря(тип), значение_словаря(тип)
        ключи = ", ".join(self.выр(у_) for у_ in в.ключи)
        значения = ", ".join(self.выр(у_) for у_ in в.значения)
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
        if getattr(в, "свойство_задачи", False):
            return f"dan_task_ready(&{self.выр(в.объект)}.t)"
        if тип == КОМПЛ:
            return {"вещ": "({}).re", "мним": "({}).im", "модуль": "dan_cx_abs({})", "аргумент": "dan_cx_arg({})",
                    "сопряженное": "dan_cx_conj({})"}[в.имя].format(self.выр(в.объект))
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
        if getattr(в, "метод_задачи", None):
            задача = self.выр(в.функция.объект)
            if в.тип_выр == ПУСТО:
                return f"dan_task_join(&{задача}.t, {в.строка})"
            return f"(dan_task_join(&{задача}.t, {в.строка}), {задача}.result)"
        if getattr(в, "метод_канала", None):
            канал = self.выр(в.функция.объект)
            dk = self.канал_c(в.функция.объект.тип_выр)
            if в.метод_канала == "отправить":
                return f"{dk}_send({канал}, {self.выр(в.аргументы[0])}, {в.строка})"
            if в.метод_канала == "получить":
                return f"{dk}_recv({канал}, {в.строка})"
            return f"dan_chan_close({канал}, {в.строка})"
        if isinstance(в.функция, у.Имя) and в.функция.имя == "канал" and это_канал(в.тип_выр):
            запас = self.выр(в.аргументы[0]) if в.аргументы else "0"
            return f"{self.канал_c(в.тип_выр)}_new({запас}, {в.строка})"
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
            if нужна_строка:
                аргументы.append(str(в.строка))
            return f"{структура}_{суффикс}({', '.join(аргументы)})"

        имя = в.функция.имя
        if getattr(в, "вариант", None):
            тип, вариант = в.вариант
            варианты = self.перечисления[тип]
            номер = [и for и, (н, _) in enumerate(варианты) if н == вариант][0]
            значения = ", ".join(self.выр(узел)
                                 for (_, т), узел in zip(варианты[номер][1], в.значения_полей))
            return f"{self.перечисление_c(тип)}_mk{номер}({значения})"
        if getattr(в, "конструктор", None):
            тип = в.конструктор
            значения = ", ".join(f".{self.поле_c(тип, поле)} = {self.выр(узел)}"
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
            return f"(DAN_OUT_LOCK(), {self.запись(тип)}({аргументы[0]}), dan_newline(), DAN_OUT_UNLOCK())"
        if имя == "ошибка":
            return аргументы[0]
        if имя == "ввод":
            return f"dan_input({аргументы[0] if аргументы else 'NULL'})"
        if имя == БАЙТЫ and getattr(в, "из_списка", False):
            self.включить_данные()
            return f"dan_bytes_from_list({аргументы[0]}, {в.строка})"
        if имя == РАЦ and len(аргументы) == 2:
            return f"dan_rat_of({аргументы[0]}, {аргументы[1]}, {в.строка})"
        if имя == КОМПЛ and len(аргументы) == 2:
            self.нужны_компл = True
            return f"dan_cx_make({аргументы[0]}, {аргументы[1]})"
        if имя in (СТР, ЦЕЛ, ДРОБ, РАЦ, БАЙТЫ, ДЛИН, КОМПЛ):
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
            эл = (lambda код: код)    # список сам учитывает добавленные элементы
            if вид == "преобразовать":
                R = self.структура("[" + результат + "]")
                знач = f"{F}_call(f, l->items[i])"
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
                код = (f"static {A} {имя}(const {S} *l, {A} acc, {F} f) {{\n    {A} r = acc;\n"
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
                       f"    free(k); free(ix); free(tmp);\n"
                       f"    return r;\n}}")
            self.помощники[ключ] = код
        имя = self.помощники[ключ].split("(", 1)[0].split()[-1].lstrip("*")
        аргументы = [self.выр(объект)] + [self.выр(а) for а in в.аргументы]
        return f"{имя}({', '.join(аргументы)})"

    def _преобразование(self, куда, откуда, значение, строка):
        if куда == откуда:
            return значение
        if КОМПЛ in (куда, откуда):
            self.нужны_компл = True
            if куда == СТР:
                return f"dan_cx_to_str({значение})"
            return self.привести(значение, откуда, КОМПЛ, строка)
        if ДЛИН in (куда, откуда):
            self.нужны_длинные = True
            if куда == ДЛИН:
                return f"dan_big_from_int({значение})" if откуда == ЦЕЛ else f"dan_big_from_str({значение}, {строка})"
            if куда == ЦЕЛ:
                return f"dan_big_to_int({значение}, {строка})"
            if куда == ДРОБ:
                return f"dan_big_to_float({значение})"
            return f"dan_big_to_str({значение})"
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
