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

# Константы библиотеки: имя -> (тип, запись в C).
КОНСТАНТЫ = {
    "пи": ("дроб", "3.141592653589793"),
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


# =====================================================================
# Шаги 12-57: дроби, округление, единицы, пропорции, проценты,
# множества, статистика, геометрия.
# =====================================================================

РАЦ, СТР = "рац", "стр"
СПИСОК_РАЦ, СПИСОК_ДРОБ, СПИСОК_СТР = "[рац]", "[дроб]", "[стр]"
ЧИСЛОВЫЕ = (ЦЕЛ, РАЦ, ДРОБ)
СПИСОК = {ЦЕЛ: СПИСОК_ЦЕЛ, РАЦ: СПИСОК_РАЦ, ДРОБ: СПИСОК_ДРОБ, СТР: СПИСОК_СТР}
ТИП_C = {ЦЕЛ: "int64_t", РАЦ: "dan_rat", ДРОБ: "double", СТР: "dan_str"}
СТРУКТУРА_C = {ЦЕЛ: "dl_Li", РАЦ: "dl_Lr", ДРОБ: "dl_Lf", СТР: "dl_Ls"}
СУФФИКС = {ЦЕЛ: "i", РАЦ: "r", ДРОБ: "f", СТР: "s"}


def _строка_c(текст):
    части = ['"']
    for байт in текст.encode("utf-8"):
        с = chr(байт)
        if с in '"\\':
            части.append("\\" + с)
        elif 32 <= байт < 127:
            части.append(с)
        else:
            части.append(f"\\{байт:03o}")
    части.append('"')
    return "".join(части)


def _добавить(имя, варианты, описание):
    """варианты: список (типы аргументов, тип результата, функция C, нужна ли строка)."""
    ФУНКЦИИ[имя] = {
        "варианты": [(а, р) for а, р, _, _ in варианты],
        "c": [ф for _, _, ф, _ in варианты],
        "строка": [с for _, _, _, с in варианты],
        "описание": описание,
    }


ДОП_КОД_C = []


# ---------- формулы: один раз описываются, код C создаётся для каждого типа ----------

def _c_формула(выражение, тип, аргументы):
    """Переводит формулу из кортежей ("+", а, б) / ("арг", 0) / ("число", 2) в C для типа."""
    вид = выражение[0]
    if вид == "арг":
        return аргументы[выражение[1]]
    if вид == "число":
        к = выражение[1]
        return {ЦЕЛ: f"INT64_C({к})", РАЦ: f"dan_rat_from_int({к}, line)",
                ДРОБ: f"{float(к)!r}"}[тип]
    if вид == "пи":
        return "3.141592653589793"
    л = _c_формула(выражение[1], тип, аргументы)
    п = _c_формула(выражение[2], тип, аргументы)
    if тип == РАЦ:
        функция = {"+": "dan_rat_add", "-": "dan_rat_sub", "*": "dan_rat_mul", "/": "dan_rat_div"}[вид]
        return f"{функция}({л}, {п}, line)"
    return f"({л} {вид} {п})"


def _формула(имя, параметры, выражение, типы, описание, результат=None):
    варианты = []
    for тип in типы:
        имя_c = f"dan_f_{len(ДОП_КОД_C)}_{СУФФИКС[тип]}"
        аргументы = [f"a{и}" for и in range(len(параметры))]
        список = ", ".join(f"{ТИП_C[тип]} {а}" for а in аргументы)
        тип_результата = результат or тип
        ДОП_КОД_C.append(
            f"static {ТИП_C[тип_результата]} {имя_c}({список}, int line) {{ (void)line; "
            f"return {_c_формула(выражение, тип, аргументы)}; }}")
        варианты.append(([тип] * len(параметры), тип_результата, имя_c, True))
    _добавить(имя, варианты, описание)


def А(н):
    return ("арг", н)


def Ч(к):
    return ("число", к)


def сл(а, б):
    return ("+", а, б)


def умн(а, б):
    return ("*", а, б)


def дел(а, б):
    return ("/", а, б)


ВСЕ = (ЦЕЛ, РАЦ, ДРОБ)
ТОЧНЫЕ = (РАЦ, ДРОБ)     # результат может быть нецелым

_формула("пропорция", "абв", дел(умн(А(1), А(2)), А(0)), ТОЧНЫЕ,
         "четвёртый член пропорции а : б = в : х, то есть х = б × в : а")
_формула("процент", "чп", дел(умн(А(0), А(1)), Ч(100)), ТОЧНЫЕ,
         "п процентов от числа: процент(200, 15) = 30")
_формула("сколько_процентов", "чц", дел(умн(А(0), Ч(100)), А(1)), ТОЧНЫЕ,
         "сколько процентов часть составляет от целого: сколько_процентов(30, 200) = 15")
_формула("число_по_проценту", "чп", дел(умн(А(0), Ч(100)), А(1)), ТОЧНЫЕ,
         "число по его части и проценту: число_по_проценту(30, 15) = 200")
_формула("периметр_прямоугольника", "аб", умн(Ч(2), сл(А(0), А(1))), ВСЕ, "P = 2 × (а + б)")
_формула("площадь_прямоугольника", "аб", умн(А(0), А(1)), ВСЕ, "S = а × б")
_формула("периметр_квадрата", "а", умн(Ч(4), А(0)), ВСЕ, "P = 4 × а")
_формула("площадь_квадрата", "а", умн(А(0), А(0)), ВСЕ, "S = а²")
_формула("площадь_треугольника", "ah", дел(умн(А(0), А(1)), Ч(2)), ТОЧНЫЕ,
         "S = основание × высота : 2")
_формула("объём_куба", "а", умн(умн(А(0), А(0)), А(0)), ВСЕ, "V = а³")
_формула("объём_параллелепипеда", "абв", умн(умн(А(0), А(1)), А(2)), ВСЕ, "V = а × б × в")
_формула("площадь_круга", "р", умн(("пи",), умн(А(0), А(0))), (ДРОБ,), "S = π × р²")
_формула("длина_окружности", "р", умн(умн(Ч(2), ("пи",)), А(0)), (ДРОБ,), "C = 2 × π × р")
ФУНКЦИИ["объем_куба"] = ФУНКЦИИ["объём_куба"]
ФУНКЦИИ["объем_параллелепипеда"] = ФУНКЦИИ["объём_параллелепипеда"]

# модуль для дробей
ФУНКЦИИ["модуль"]["варианты"].insert(1, ([РАЦ], РАЦ))
ФУНКЦИИ["модуль"]["c"].insert(1, "dan_rat_abs")
ФУНКЦИИ["модуль"]["строка"].insert(1, True)

# ---------- дроби и числа ----------

_добавить("смешанное", [
    ([РАЦ], СТР, "dan_mixed_str", False),
    ([ЦЕЛ, ЦЕЛ, ЦЕЛ], РАЦ, "dan_mixed_make", True),
], "смешанное число: смешанное(рац(7, 3)) = \"2 1/3\", смешанное(2, 1, 3) = 7/3")
_добавить("периодическая", [([РАЦ], СТР, "dan_periodic", True)],
          "десятичная запись дроби с периодом: периодическая(рац(1, 6)) = \"0,1(6)\"")
_добавить("округлить", [
    ([РАЦ], ЦЕЛ, "dan_round_rat", True),
    ([ДРОБ], ЦЕЛ, "dan_round_float", True),
    ([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_round_int_places", True),
    ([РАЦ, ЦЕЛ], ДРОБ, "dan_round_rat_places", True),
    ([ДРОБ, ЦЕЛ], ДРОБ, "dan_round_float_places", True),
], "округление по школьному правилу (5 и больше - вверх): округлить(2.675, 2) = 2.68")
_добавить("корень", [([ДРОБ], ДРОБ, "dan_sqrt", True)], "квадратный корень: корень(2) = 1.4142135623730951")
_добавить("целый_корень", [([ЦЕЛ], ЦЕЛ, "dan_isqrt", True)],
          "наибольшее целое, квадрат которого не больше числа: целый_корень(50) = 7")
_добавить("перевести", [([ДРОБ, СТР, СТР], ДРОБ, "dan_convert_units", True)],
          "перевод единиц измерения: перевести(1.5, \"км\", \"м\") = 1500")

# ---------- множества, статистика, упорядочивание ----------


def _списочные(имя, типы, результат, функция_c, описание, строка=True, аргументы=1):
    варианты = []
    for т in типы:
        р = результат(т)
        варианты.append(([СПИСОК[т]] * аргументы, р, f"{функция_c}_{СУФФИКС[т]}", строка))
    _добавить(имя, варианты, описание)


_списочные("упорядочить", (ЦЕЛ, РАЦ, ДРОБ, СТР), lambda т: СПИСОК[т], "dan_sorted",
           "новый список, упорядоченный по возрастанию", строка=False)
_списочные("множество", (ЦЕЛ, СТР), lambda т: СПИСОК[т], "dan_set",
           "элементы без повторов, по возрастанию: множество([3, 1, 3]) = [1, 3]", строка=False)
for _имя, _c, _описание in (
        ("объединение", "dan_union", "элементы, входящие хотя бы в одно множество"),
        ("пересечение", "dan_intersection", "элементы, входящие в оба множества"),
        ("разность", "dan_difference", "элементы первого множества, которых нет во втором")):
    _списочные(_имя, (ЦЕЛ, СТР), lambda т: СПИСОК[т], _c, _описание, строка=False, аргументы=2)
_списочные("подмножество", (ЦЕЛ, СТР), lambda т: ЛОГ, "dan_subset",
           "входит ли каждый элемент первого множества во второе", строка=False, аргументы=2)
_добавить("принадлежит", [
    ([ЦЕЛ, СПИСОК_ЦЕЛ], ЛОГ, "dan_contains_i", False),
    ([СТР, СПИСОК_СТР], ЛОГ, "dan_contains_s", False),
], "есть ли элемент в списке (множестве): принадлежит(3, [1, 2, 3]) = истина")

_списочные("сумма", ВСЕ, lambda т: т, "dan_sum", "сумма элементов списка")
_списочные("среднее", ВСЕ, lambda т: РАЦ if т == РАЦ else ДРОБ, "dan_mean",
           "среднее арифметическое: среднее([5, 4, 3, 5]) = 4.25")
_списочные("медиана", ВСЕ, lambda т: РАЦ if т == РАЦ else ДРОБ, "dan_median",
           "медиана: середина упорядоченного ряда")
_списочные("мода", (ЦЕЛ,), lambda т: т, "dan_mode",
           "мода: значение, которое встречается чаще всего (наименьшее из таких)")
_списочные("размах", ВСЕ, lambda т: т, "dan_range", "размах: разность наибольшего и наименьшего")
for _имя, _c in (("мин", "dan_min"), ("макс", "dan_max")):
    _варианты = [([СПИСОК[т]], т, f"{_c}_{СУФФИКС[т]}", True) for т in ВСЕ]
    _варианты += [([т, т], т, f"{_c}2_{СУФФИКС[т]}", False) for т in ВСЕ]
    _добавить(_имя, _варианты, ("наименьшее" if _имя == "мин" else "наибольшее")
              + " из двух чисел или из списка")

СООБЩЕНИЯ.update({
    "DAN_MSG_PERIOD_LONG": "период дроби слишком длинный (больше 100000 цифр)",
    "DAN_MSG_UNIT_UNKNOWN": "неизвестная единица измерения \"%s\"",
    "DAN_MSG_UNIT_KIND": "единицы \"%s\" и \"%s\" измеряют разные величины",
    "DAN_MSG_SQRT_NEG": "квадратный корень из отрицательного числа не существует",
    "DAN_MSG_EMPTY_STAT": "у пустого списка нет такой характеристики",
    "DAN_MSG_MIXED": "в смешанном числе числитель и знаменатель дробной части должны быть "
                     "неотрицательными, а знаменатель больше нуля",
})

# Единицы измерения: имя, вид величины, множитель к основной единице (числитель, знаменатель).
ЕДИНИЦЫ = [
    # длина, основная единица - метр
    ("мм", 1, 1, 1000), ("см", 1, 1, 100), ("дм", 1, 1, 10), ("м", 1, 1, 1), ("км", 1, 1000, 1),
    # масса, грамм
    ("мг", 2, 1, 1000), ("г", 2, 1, 1), ("кг", 2, 1000, 1), ("ц", 2, 100000, 1), ("т", 2, 1000000, 1),
    # время, секунда
    ("с", 3, 1, 1), ("сек", 3, 1, 1), ("мин", 3, 60, 1), ("ч", 3, 3600, 1), ("час", 3, 3600, 1),
    ("сут", 3, 86400, 1), ("сутки", 3, 86400, 1), ("нед", 3, 604800, 1),
    # площадь, квадратный метр
    ("мм²", 4, 1, 1000000), ("мм2", 4, 1, 1000000), ("см²", 4, 1, 10000), ("см2", 4, 1, 10000),
    ("дм²", 4, 1, 100), ("дм2", 4, 1, 100), ("м²", 4, 1, 1), ("м2", 4, 1, 1),
    ("а", 4, 100, 1), ("ар", 4, 100, 1), ("сотка", 4, 100, 1), ("га", 4, 10000, 1),
    ("км²", 4, 1000000, 1), ("км2", 4, 1000000, 1),
    # объём, кубический метр
    ("мм³", 5, 1, 1000000000), ("мм3", 5, 1, 1000000000), ("см³", 5, 1, 1000000),
    ("см3", 5, 1, 1000000), ("мл", 5, 1, 1000000), ("дм³", 5, 1, 1000), ("дм3", 5, 1, 1000),
    ("л", 5, 1, 1000), ("м³", 5, 1, 1), ("м3", 5, 1, 1),
    # скорость, метр в секунду
    ("м/с", 6, 1, 1), ("м/мин", 6, 1, 60), ("км/ч", 6, 1000, 3600), ("км/мин", 6, 1000, 60),
]


def _код_единиц():
    строки = ["typedef struct { const char *name; int kind; double num, den; } dan_unit;",
              "static const dan_unit dan_units[] = {"]
    for имя, вид, ч, з in ЕДИНИЦЫ:
        строки.append(f"    {{ {_строка_c(имя)}, {вид}, {ч}.0, {з}.0 }},")
    строки.append("};")
    return "\n".join(строки)


def _код_списков():
    """Код функций со списками для каждого типа элементов."""
    куски = []
    for т in (ЦЕЛ, РАЦ, ДРОБ, СТР):
        T, S, с = ТИП_C[т], СТРУКТУРА_C[т], СУФФИКС[т]
        сравнить = {ЦЕЛ: "(x > y) - (x < y)", РАЦ: "dan_rat_cmp(x, y)",
                    ДРОБ: "(x > y) - (x < y)", СТР: "strcmp(x, y)"}[т]
        куски.append(f"""
static int dan_cmp_{с}({T} x, {T} y) {{ return {сравнить}; }}
static int dan_qcmp_{с}(const void *a, const void *b) {{ return dan_cmp_{с}(*(const {T} *)a, *(const {T} *)b); }}
static {S} *dan_sorted_{с}(const {S} *l) {{
    {S} *r = {S}_from(l->len, l->items);
    qsort(r->items, (size_t)r->len, sizeof({T}), dan_qcmp_{с});
    return r;
}}""")
        if т in (ЦЕЛ, СТР):
            куски.append(f"""
static {S} *dan_set_{с}(const {S} *l) {{
    {S} *s = dan_sorted_{с}(l);
    int64_t k = 0;
    for (int64_t i = 0; i < s->len; i++)
        if (k == 0 || dan_cmp_{с}(s->items[k - 1], s->items[i]) != 0) s->items[k++] = s->items[i];
    s->len = k;
    return s;
}}
static bool dan_contains_{с}({T} x, const {S} *l) {{
    for (int64_t i = 0; i < l->len; i++) if (dan_cmp_{с}(l->items[i], x) == 0) return true;
    return false;
}}
static {S} *dan_union_{с}(const {S} *a, const {S} *b) {{
    {S} *r = {S}_from(a->len, a->items);
    for (int64_t i = 0; i < b->len; i++) {S}_push(r, b->items[i]);
    return dan_set_{с}(r);
}}
static {S} *dan_intersection_{с}(const {S} *a, const {S} *b) {{
    {S} *r = {S}_from(0, NULL);
    for (int64_t i = 0; i < a->len; i++) if (dan_contains_{с}(a->items[i], b)) {S}_push(r, a->items[i]);
    return dan_set_{с}(r);
}}
static {S} *dan_difference_{с}(const {S} *a, const {S} *b) {{
    {S} *r = {S}_from(0, NULL);
    for (int64_t i = 0; i < a->len; i++) if (!dan_contains_{с}(a->items[i], b)) {S}_push(r, a->items[i]);
    return dan_set_{с}(r);
}}
static bool dan_subset_{с}(const {S} *a, const {S} *b) {{
    for (int64_t i = 0; i < a->len; i++) if (!dan_contains_{с}(a->items[i], b)) return false;
    return true;
}}""")
        if т in ЧИСЛОВЫЕ:
            сложить = {ЦЕЛ: "s + l->items[i]", РАЦ: "dan_rat_add(s, l->items[i], line)",
                       ДРОБ: "s + l->items[i]"}[т]
            ноль = {ЦЕЛ: "0", РАЦ: "dan_rat_from_int(0, line)", ДРОБ: "0.0"}[т]
            тип_ср = "dan_rat" if т == РАЦ else "double"
            среднее = {ЦЕЛ: "(double)dan_sum_i(l, line) / (double)l->len",
                       РАЦ: "dan_rat_div(dan_sum_r(l, line), dan_rat_from_int(l->len, line), line)",
                       ДРОБ: "dan_sum_f(l, line) / (double)l->len"}[т]
            if т == ЦЕЛ:
                половина = "((double)s->items[m - 1] + (double)s->items[m]) / 2.0"
                середина = "(double)s->items[m]"
            elif т == РАЦ:
                половина = ("dan_rat_div(dan_rat_add(s->items[m - 1], s->items[m], line), "
                            "dan_rat_from_int(2, line), line)")
                середина = "s->items[m]"
            else:
                половина = "(s->items[m - 1] + s->items[m]) / 2.0"
                середина = "s->items[m]"
            вычесть = {ЦЕЛ: "b - a", РАЦ: "dan_rat_sub(b, a, line)", ДРОБ: "b - a"}[т]
            куски.append(f"""
static void dan_need_items_{с}(const {S} *l, int line) {{ if (l->len == 0) dan_error(DAN_MSG_EMPTY_STAT, line); }}
static {T} dan_sum_{с}(const {S} *l, int line) {{
    (void)line;
    {T} s = {ноль};
    for (int64_t i = 0; i < l->len; i++) s = {сложить};
    return s;
}}
static {тип_ср} dan_mean_{с}(const {S} *l, int line) {{ dan_need_items_{с}(l, line); return {среднее}; }}
static {тип_ср} dan_median_{с}(const {S} *l, int line) {{
    dan_need_items_{с}(l, line);
    {S} *s = dan_sorted_{с}(l);
    int64_t m = s->len / 2;
    if (s->len % 2 == 0) return {половина};
    return {середина};
}}
static {T} dan_min_{с}(const {S} *l, int line) {{
    dan_need_items_{с}(l, line);
    {T} r = l->items[0];
    for (int64_t i = 1; i < l->len; i++) if (dan_cmp_{с}(l->items[i], r) < 0) r = l->items[i];
    return r;
}}
static {T} dan_max_{с}(const {S} *l, int line) {{
    dan_need_items_{с}(l, line);
    {T} r = l->items[0];
    for (int64_t i = 1; i < l->len; i++) if (dan_cmp_{с}(l->items[i], r) > 0) r = l->items[i];
    return r;
}}
static {T} dan_min2_{с}({T} x, {T} y) {{ return dan_cmp_{с}(x, y) <= 0 ? x : y; }}
static {T} dan_max2_{с}({T} x, {T} y) {{ return dan_cmp_{с}(x, y) >= 0 ? x : y; }}
static {T} dan_range_{с}(const {S} *l, int line) {{
    {T} a = dan_min_{с}(l, line), b = dan_max_{с}(l, line);
    return {вычесть};
}}""")
    куски.append("""
static int64_t dan_mode_i(const dl_Li *l, int line) {
    dan_need_items_i(l, line);
    dl_Li *s = dan_sorted_i(l);
    int64_t best = s->items[0], best_count = 0;
    for (int64_t i = 0; i < s->len;) {
        int64_t j = i;
        while (j < s->len && s->items[j] == s->items[i]) j++;
        if (j - i > best_count) { best_count = j - i; best = s->items[i]; }
        i = j;
    }
    return best;
}""")
    return "\n".join(куски)


ДОП_КОД_C_РУЧНОЙ = r"""
/* ---- fractions, rounding, roots, units ---- */

static dan_rat dan_rat_abs(dan_rat a, int line) { return a.n < 0 ? dan_rat_neg(a, line) : a; }

/* 7/3 -> "2 1/3", -7/3 -> "-2 1/3", 1/3 -> "1/3", 6/3 -> "2" */
static dan_str dan_mixed_str(dan_rat a) {
    char b[96];
    uint64_t un = a.n < 0 ? (uint64_t)0 - (uint64_t)a.n : (uint64_t)a.n;
    uint64_t whole = un / (uint64_t)a.d, rest = un % (uint64_t)a.d;
    const char *sign = a.n < 0 ? "-" : "";
    if (rest == 0) snprintf(b, sizeof b, "%s%llu", sign, (unsigned long long)whole);
    else if (whole == 0) snprintf(b, sizeof b, "%s%llu/%lld", sign, (unsigned long long)rest, (long long)a.d);
    else snprintf(b, sizeof b, "%s%llu %llu/%lld", sign, (unsigned long long)whole,
                  (unsigned long long)rest, (long long)a.d);
    return dan_strdup(b);
}

static dan_rat dan_mixed_make(int64_t whole, int64_t num, int64_t den, int line) {
    if (num < 0 || den <= 0) dan_error(DAN_MSG_MIXED, line);
    dan_rat part = dan_rat_of(num, den, line);
    dan_rat w = dan_rat_from_int(whole < 0 ? -whole : whole, line);
    dan_rat r = dan_rat_add(w, part, line);
    return whole < 0 ? dan_rat_neg(r, line) : r;
}

/* Decimal notation with the repeating part in brackets: 1/6 -> "0,1(6)" */
static dan_str dan_periodic(dan_rat a, int line) {
    uint64_t un = a.n < 0 ? (uint64_t)0 - (uint64_t)a.n : (uint64_t)a.n;
    uint64_t d = (uint64_t)a.d;
    char head[48];
    snprintf(head, sizeof head, "%s%llu", a.n < 0 ? "-" : "", (unsigned long long)(un / d));
    uint64_t r = un % d;
    if (r == 0) return dan_strdup(head);
    /* pre-period length: the larger power of 2 or 5 in the denominator */
    int c2 = 0, c5 = 0;
    uint64_t t = d;
    while (t % 2 == 0) { t /= 2; c2++; }
    while (t % 5 == 0) { t /= 5; c5++; }
    int pre = c2 > c5 ? c2 : c5;
    size_t cap = 64, len = 0;
    char *digits = dan_alloc(cap);
    for (int i = 0; i < pre; i++) {
        dan_u128 x = (dan_u128)r * 10;
        if (len + 2 >= cap) { cap *= 2; digits = dan_realloc(digits, cap); }
        digits[len++] = (char)('0' + (int)(x / d));
        r = (uint64_t)(x % d);
    }
    size_t period_start = len;
    if (r != 0) {
        uint64_t start = r;
        do {
            dan_u128 x = (dan_u128)r * 10;
            if (len + 4 >= cap) { cap *= 2; digits = dan_realloc(digits, cap); }
            digits[len++] = (char)('0' + (int)(x / d));
            r = (uint64_t)(x % d);
            if (len - period_start > 100000) dan_error(DAN_MSG_PERIOD_LONG, line);
        } while (r != start);
    }
    size_t hl = strlen(head);
    char *out = dan_alloc(hl + len + 4);
    memcpy(out, head, hl);
    out[hl] = ',';
    memcpy(out + hl + 1, digits, period_start);
    size_t pos = hl + 1 + period_start;
    if (len > period_start) {
        out[pos++] = '(';
        memcpy(out + pos, digits + period_start, len - period_start);
        pos += len - period_start;
        out[pos++] = ')';
    }
    out[pos] = 0;
    free(digits);
    return out;
}

/* School rounding (half away from zero) on the shortest decimal form of the
   number, so that 2.675 rounds to 2.68 exactly as written. */
static double dan_round_decimal(double v, int64_t places) {
    if (v != v || v - v != 0 || places > 17) return v;
    char buf[64];
    for (int prec = 0; prec <= 16; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    const char *p = buf;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    char digits[32];
    int nd = 0;
    for (; *p && *p != 'e'; p++) if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    long e10 = strtol(p + 1, NULL, 10);
    long keep = e10 + 1 + (long)places;      /* digits kept before the cut */
    if (keep >= nd) return v;
    if (keep < 0) return neg ? -0.0 : 0.0;
    uint64_t kept = 0;
    for (long i = 0; i < keep; i++) kept = kept * 10 + (uint64_t)(digits[i] - '0');
    if (digits[keep] >= '5') kept++;
    char out[64];
    snprintf(out, sizeof out, "%s%llue%ld", neg ? "-" : "", (unsigned long long)kept, e10 + 1 - keep);
    return strtod(out, NULL);
}

static int64_t dan_round_float(double v, int line) {
    double r = dan_round_decimal(v, 0);
    return dan_float_to_int(r, line);
}

static int64_t dan_round_rat(dan_rat a, int line) {
    (void)line;
    dan_i128 n = a.n < 0 ? -(dan_i128)a.n : a.n;
    dan_i128 q = (2 * n + a.d) / (2 * (dan_i128)a.d);
    return (int64_t)(a.n < 0 ? -q : q);
}

static double dan_round_float_places(double v, int64_t places, int line) {
    (void)line;
    return dan_round_decimal(v, places);
}

static dan_i128 dan_pow10_128(int64_t k) { dan_i128 r = 1; while (k-- > 0) r *= 10; return r; }

static double dan_round_rat_places(dan_rat a, int64_t places, int line) {
    if (places > 18 || places < -18) return dan_round_decimal(dan_rat_to_float(a), places);
    dan_i128 n = a.n < 0 ? -(dan_i128)a.n : a.n, d = a.d;
    if (places >= 0) n *= dan_pow10_128(places); else d *= dan_pow10_128(-places);
    dan_i128 q = (2 * n + d) / (2 * d);
    double r = places >= 0 ? (double)q / (double)dan_pow10_128(places) : (double)q * (double)dan_pow10_128(-places);
    (void)line;
    r = dan_round_decimal(r, places);
    return a.n < 0 ? -r : r;
}

/* Rounding an integer to tens, hundreds, ...: (1234, -2) -> 1200 */
static int64_t dan_round_int_places(int64_t v, int64_t places, int line) {
    if (places >= 0) return v;
    if (places < -18) return 0;
    dan_i128 p = dan_pow10_128(-places);
    dan_i128 n = v < 0 ? -(dan_i128)v : v;
    dan_i128 q = (2 * n + p) / (2 * p) * p;
    if (q > INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    return (int64_t)(v < 0 ? -q : q);
}

static double dan_sqrt(double v, int line) {
    if (v < 0) dan_error(DAN_MSG_SQRT_NEG, line);
    return sqrt(v);
}

static int64_t dan_isqrt(int64_t n, int line) {
    if (n < 0) dan_error(DAN_MSG_SQRT_NEG, line);
    uint64_t r = (uint64_t)sqrt((double)n);
    while (r * r > (uint64_t)n) r--;
    while ((r + 1) * (r + 1) <= (uint64_t)n) r++;
    return (int64_t)r;
}

static const dan_unit *dan_find_unit(dan_str name, int line) {
    for (size_t i = 0; i < sizeof dan_units / sizeof dan_units[0]; i++)
        if (strcmp(dan_units[i].name, name) == 0) return &dan_units[i];
    dan_conv_error(DAN_MSG_UNIT_UNKNOWN, name, line);
    return NULL;
}

static double dan_convert_units(double v, dan_str from, dan_str to, int line) {
    const dan_unit *a = dan_find_unit(from, line), *b = dan_find_unit(to, line);
    if (a->kind != b->kind) {
        char buf[300];
        snprintf(buf, sizeof buf, DAN_MSG_UNIT_KIND, from, to);
        dan_error(buf, line);
    }
    double r = v * (a->num * b->den) / (a->den * b->num);
    char t[64];
    snprintf(t, sizeof t, "%.15g", r);          /* remove binary noise: 0.30000000000000004 */
    return strtod(t, NULL);
}
"""


def код_c_дополнения():
    return "\n".join([_код_единиц(), ДОП_КОД_C_РУЧНОЙ, _код_списков(), *ДОП_КОД_C])
