"""Стандартная математическая библиотека языка Дан.

Содержит функции по разделу «Основы математики» (числа, разряды,
деление с остатком, делители и кратные, признаки делимости, простые
числа, НОД и НОК). Функции доступны в любой программе без подключения.

Имена библиотеки не зарезервированы: если в программе объявлена своя
функция или переменная с тем же именем, используется она.

Каждая запись описывает функцию:
    варианты  - список пар (типы аргументов, тип результата);
                подходит первый вариант, с которым совпадают аргументы
    c         - функция C для каждого варианта
    строка    - нужно ли передавать в C номер строки (для сообщений об ошибках)
"""

# Названия типов совпадают с proverka.py (там этот модуль импортируется).
ЦЕЛ, ДРОБ, ЛОГ = "цел", "дроб", "лог"

СПИСОК_ЦЕЛ = "[" + ЦЕЛ + "]"

ФУНКЦИИ = {
    "модуль": {
        "варианты": [([ЦЕЛ], ЦЕЛ), ([ДРОБ], ДРОБ)],
        "c": ["dan_abs_int", "dan_abs_float"],
        "строка": [True, False],
        "описание": "модуль числа (расстояние от нуля): модуль(-5) = 5",
    },
    "делится": {
        "варианты": [([ЦЕЛ, ЦЕЛ], ЛОГ)],
        "c": ["dan_divides"],
        "строка": [True],
        "описание": "делится ли первое число на второе без остатка: делится(12, 4) = истина",
    },
    "чётное": {
        "варианты": [([ЦЕЛ], ЛОГ)],
        "c": ["dan_even"],
        "строка": [False],
        "описание": "делится ли число на 2 без остатка",
    },
    "нечётное": {
        "варианты": [([ЦЕЛ], ЛОГ)],
        "c": ["dan_odd"],
        "строка": [False],
        "описание": "даёт ли число остаток 1 при делении на 2",
    },
    "простое": {
        "варианты": [([ЦЕЛ], ЛОГ)],
        "c": ["dan_prime"],
        "строка": [False],
        "описание": "имеет ли число ровно два делителя: 1 и само себя",
    },
    "делители": {
        "варианты": [([ЦЕЛ], СПИСОК_ЦЕЛ)],
        "c": ["dan_divisors"],
        "строка": [True],
        "описание": "все делители натурального числа по возрастанию: делители(12) = [1, 2, 3, 4, 6, 12]",
    },
    "кратные": {
        "варианты": [([ЦЕЛ, ЦЕЛ], СПИСОК_ЦЕЛ)],
        "c": ["dan_multiples"],
        "строка": [True],
        "описание": "первые кратные числа: кратные(5, 4) = [5, 10, 15, 20]",
    },
    "простые_множители": {
        "варианты": [([ЦЕЛ], СПИСОК_ЦЕЛ)],
        "c": ["dan_prime_factors"],
        "строка": [True],
        "описание": "разложение на простые множители: простые_множители(180) = [2, 2, 3, 3, 5]",
    },
    "нод": {
        "варианты": [([ЦЕЛ, ЦЕЛ], ЦЕЛ), ([СПИСОК_ЦЕЛ], ЦЕЛ)],
        "c": ["dan_gcd", "dan_gcd_list"],
        "строка": [True, True],
        "описание": "наибольший общий делитель двух чисел или списка чисел: нод(12, 9) = 3",
    },
    "нок": {
        "варианты": [([ЦЕЛ, ЦЕЛ], ЦЕЛ), ([СПИСОК_ЦЕЛ], ЦЕЛ)],
        "c": ["dan_lcm", "dan_lcm_list"],
        "строка": [True, True],
        "описание": "наименьшее общее кратное двух чисел или списка чисел: нок(9, 12) = 36",
    },
    "цифры": {
        "варианты": [([ЦЕЛ], СПИСОК_ЦЕЛ)],
        "c": ["dan_digits"],
        "строка": [False],
        "описание": "цифры числа от старшего разряда к младшему: цифры(635) = [6, 3, 5]",
    },
    "сумма_цифр": {
        "варианты": [([ЦЕЛ], ЦЕЛ)],
        "c": ["dan_digit_sum"],
        "строка": [False],
        "описание": "сумма цифр числа (для признаков делимости на 3 и 9): сумма_цифр(846) = 18",
    },
    "разряды": {
        "варианты": [([ЦЕЛ], СПИСОК_ЦЕЛ)],
        "c": ["dan_place_values"],
        "строка": [False],
        "описание": "разложение по разрядам без нулевых слагаемых: разряды(635) = [600, 30, 5]",
    },
}

# Написание через «е» вместо «ё» тоже допускается.
СИНОНИМЫ = {"четное": "чётное", "нечетное": "нечётное"}


def найти(имя):
    """Возвращает (каноническое имя, описание) или (None, None)."""
    имя = СИНОНИМЫ.get(имя, имя)
    if имя in ФУНКЦИИ:
        return имя, ФУНКЦИИ[имя]
    return None, None


СООБЩЕНИЯ = {
    "DAN_MSG_NATURAL": "функция \"%s\" определена только для натуральных чисел (1, 2, 3, ...), "
                       "а получено %lld",
    "DAN_MSG_NEG_COUNT": "количество кратных не может быть отрицательным",
    "DAN_MSG_EMPTY_LIST": "функции \"%s\" нужен хотя бы один элемент в списке",
}

# Код C библиотеки. Вставляется после определения списка целых чисел dl_Li.
КОД_C = r"""
/* ---- mathematics library (basics of mathematics) ---- */

static void dan_natural_error(const char *name, int64_t n, int line) {
    char buf[300];
    snprintf(buf, sizeof buf, DAN_MSG_NATURAL, name, (long long)n);
    dan_error(buf, line);
}

static uint64_t dan_uabs(int64_t a) { return a < 0 ? (uint64_t)0 - (uint64_t)a : (uint64_t)a; }

static int64_t dan_abs_int(int64_t a, int line) {
    if (a == INT64_MIN) dan_error(DAN_MSG_TOO_BIG, line);
    return a < 0 ? -a : a;
}

static double dan_abs_float(double a) { return a < 0 ? -a : (a == 0 ? 0.0 : a); }

static bool dan_divides(int64_t a, int64_t b, int line) {
    if (b == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    return dan_uabs(a) % dan_uabs(b) == 0;
}

static bool dan_even(int64_t a) { return a % 2 == 0; }
static bool dan_odd(int64_t a) { return a % 2 != 0; }

static bool dan_prime(int64_t n) {
    if (n < 2) return false;
    if (n < 4) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (int64_t d = 5; d <= n / d; d += 6)
        if (n % d == 0 || n % (d + 2) == 0) return false;
    return true;
}

static dl_Li *dan_divisors(int64_t n, int line) {
    if (n < 1) dan_natural_error(DAN_NAME_DIVISORS, n, line);
    dl_Li *small = dl_Li_from(0, NULL), *large = dl_Li_from(0, NULL);
    for (int64_t d = 1; d <= n / d; d++) {
        if (n % d == 0) {
            dl_Li_push(small, d);
            if (d != n / d) dl_Li_push(large, n / d);
        }
    }
    for (int64_t i = large->len - 1; i >= 0; i--) dl_Li_push(small, large->items[i]);
    return small;
}

static dl_Li *dan_multiples(int64_t a, int64_t count, int line) {
    if (count < 0) dan_error(DAN_MSG_NEG_COUNT, line);
    dl_Li *r = dl_Li_from(0, NULL);
    for (int64_t k = 1; k <= count; k++) {
        int64_t v;
        if (__builtin_mul_overflow(a, k, &v)) dan_error(DAN_MSG_TOO_BIG, line);
        dl_Li_push(r, v);
    }
    return r;
}

static dl_Li *dan_prime_factors(int64_t n, int line) {
    if (n < 1) dan_natural_error(DAN_NAME_PRIME_FACTORS, n, line);
    dl_Li *r = dl_Li_from(0, NULL);
    while (n % 2 == 0) { dl_Li_push(r, 2); n /= 2; }
    for (int64_t d = 3; d <= n / d; d += 2)
        while (n % d == 0) { dl_Li_push(r, d); n /= d; }
    if (n > 1) dl_Li_push(r, n);
    return r;
}

static uint64_t dan_gcd_u(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

static int64_t dan_gcd(int64_t a, int64_t b, int line) {
    uint64_t g = dan_gcd_u(dan_uabs(a), dan_uabs(b));
    if (g > (uint64_t)INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    return (int64_t)g;
}

static int64_t dan_lcm(int64_t a, int64_t b, int line) {
    if (a == 0 || b == 0) return 0;
    uint64_t ua = dan_uabs(a), ub = dan_uabs(b);
    uint64_t r;
    if (__builtin_mul_overflow(ua / dan_gcd_u(ua, ub), ub, &r) || r > (uint64_t)INT64_MAX)
        dan_error(DAN_MSG_TOO_BIG, line);
    return (int64_t)r;
}

static int64_t dan_gcd_list(const dl_Li *l, int line) {
    if (l->len == 0) dan_conv_error(DAN_MSG_EMPTY_LIST, DAN_NAME_GCD, line);
    int64_t g = dan_gcd(l->items[0], 0, line);
    for (int64_t i = 1; i < l->len; i++) g = dan_gcd(g, l->items[i], line);
    return g;
}

static int64_t dan_lcm_list(const dl_Li *l, int line) {
    if (l->len == 0) dan_conv_error(DAN_MSG_EMPTY_LIST, DAN_NAME_LCM, line);
    int64_t m = dan_abs_int(l->items[0], line);
    for (int64_t i = 1; i < l->len; i++) m = dan_lcm(m, l->items[i], line);
    return m;
}

static dl_Li *dan_digits(int64_t n) {
    uint64_t u = dan_uabs(n);
    dl_Li *r = dl_Li_from(0, NULL);
    do { dl_Li_push(r, (int64_t)(u % 10)); u /= 10; } while (u);
    for (int64_t i = 0, j = r->len - 1; i < j; i++, j--) {
        int64_t t = r->items[i]; r->items[i] = r->items[j]; r->items[j] = t;
    }
    return r;
}

static int64_t dan_digit_sum(int64_t n) {
    uint64_t u = dan_uabs(n);
    int64_t s = 0;
    do { s += (int64_t)(u % 10); u /= 10; } while (u);
    return s;
}

/* 635 -> [600, 30, 5]; the sign of a negative number is kept: -635 -> [-600, -30, -5] */
static dl_Li *dan_place_values(int64_t n) {
    dl_Li *r = dl_Li_from(0, NULL);
    if (n == 0) { dl_Li_push(r, 0); return r; }
    int64_t place = 1;
    dl_Li *low = dl_Li_from(0, NULL);
    while (n != 0) {
        int64_t digit = n % 10;          /* same sign as n */
        if (digit != 0) dl_Li_push(low, digit * place);
        n /= 10;
        if (n != 0) place *= 10;
    }
    for (int64_t i = low->len - 1; i >= 0; i--) dl_Li_push(r, low->items[i]);
    return r;
}
"""

ИМЕНА_C = {
    "DAN_NAME_DIVISORS": "делители",
    "DAN_NAME_PRIME_FACTORS": "простые_множители",
    "DAN_NAME_GCD": "нод",
    "DAN_NAME_LCM": "нок",
}
