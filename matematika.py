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
    "частное": {
        "варианты": [([ЦЕЛ, ЦЕЛ], ЦЕЛ), (["длинное", "длинное"], "длинное"), (["мн", "мн"], "мн")],
        "c": ["dan_div", "dan_big_div", "dan_poly_div"],
        "строка": [True, True, True],
        "описание": "частное при делении с остатком: частное(7, 2) = 3, частное(-7, 2) = -4 (остаток а % б не бывает отрицательным); для многочленов - частное при делении многочленов",
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
    имя = _БЕЗ_Ё.get(имя.replace("ё", "е"))
    if имя is not None:
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
    dl_Li *lo = dl_Li_from(0, NULL), *hi = dl_Li_from(0, NULL);
    for (int64_t d = 1; d <= n / d; d++) {
        if (n % d == 0) {
            dl_Li_push(lo, d);
            if (d != n / d) dl_Li_push(hi, n / d);
        }
    }
    for (int64_t i = hi->len - 1; i >= 0; i--) dl_Li_push(lo, hi->items[i]);
    return lo;
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
# дроби, округление, единицы, пропорции, проценты,
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
ФУНКЦИИ["модуль"]["варианты"].append((["компл"], ДРОБ))
ФУНКЦИИ["модуль"]["c"].append("dan_cx_abs")
ФУНКЦИИ["модуль"]["строка"].append(False)

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
        else {"dan_str_release" if т == СТР else "DAN_NOREL"}(s->items[i]);
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
    char *out = dan_str_new(hl + len + 3);
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


# =====================================================================
# многочлены, уравнения, системы, неравенства, модуль.
# =====================================================================

МН = "мн"
СПИСОК_МН = "[мн]"
КОНСТАНТЫ["х"] = (МН, "dan_poly_x()")      # кириллическая «х»
КОНСТАНТЫ["x"] = (МН, "dan_poly_x()")      # латинская «x»

_добавить("многочлен", [
    ([СПИСОК_РАЦ], МН, "dan_poly_from_list", True),
], "многочлен по коэффициентам от старшей степени: многочлен([1, -3, 2]) = х² - 3х + 2")
_добавить("значение", [
    ([МН, РАЦ], РАЦ, "dan_poly_eval_rat", True),
    ([МН, ДРОБ], ДРОБ, "dan_poly_eval_float", False),
], "значение многочлена при данном х: значение(х² - 1, 3) = 8")
_добавить("решить", [
    ([МН], СПИСОК_РАЦ, "dan_solve_rat", True),
    ([МН, МН], СПИСОК_РАЦ, "dan_solve_rat2", True),
], "рациональные корни уравнения: решить(2*х + 3, 7) = [2]")
ФУНКЦИИ["корни"] = ФУНКЦИИ["решить"]
_добавить("решить_приближённо", [
    ([МН], СПИСОК_ДРОБ, "dan_solve_real", True),
    ([МН, МН], СПИСОК_ДРОБ, "dan_solve_real2", True),
], "все действительные корни, в том числе иррациональные: решить_приближённо(х² - 2)")
ФУНКЦИИ["решить_приближенно"] = ФУНКЦИИ["решить_приближённо"]
_добавить("разложить", [([МН], СТР, "dan_factor_str", True)],
          "разложение многочлена на множители: разложить(х² - 3*х + 2) = \"(х - 2)(х - 1)\"")
_добавить("вынести_общий_множитель", [([МН], СТР, "dan_common_factor_str", True)],
          "вынесение общего множителя за скобки: 2х² + 6х = \"2х(х + 3)\"")
_добавить("решить_неравенство", [([МН, СТР, МН], СТР, "dan_ineq_str", True)],
          "решение неравенства методом интервалов: решить_неравенство(2*х - 4, \">\", 0) = \"(2; +∞)\"")
_добавить("решить_систему_неравенств", [([СПИСОК_МН, СПИСОК_СТР, СПИСОК_МН], СТР, "dan_ineq_system_str", True)],
          "решение системы неравенств с одной переменной")
_добавить("решить_с_модулем", [([МН, МН], СТР, "dan_abs_eq_str", True)],
          "решение уравнения |левая| = правая: решить_с_модулем(2*х - 3, 5) = \"{-1; 4}\"")
_добавить("решить_неравенство_с_модулем", [([МН, СТР, МН], СТР, "dan_abs_ineq_str", True)],
          "решение неравенства |левая| знак правая методом интервалов")
_добавить("решить_систему", [
    (["[[рац]]"], СПИСОК_РАЦ, "dan_solve_system", True),
], "решение системы линейных уравнений методом Гаусса; каждая строка: коэффициенты и свободный член")
_добавить("корни_квадратного", [([ДРОБ, ДРОБ, ДРОБ], СПИСОК_ДРОБ, "dan_quadratic_roots", True)],
          "действительные корни уравнения а·х² + б·х + в = 0 по возрастанию")
_формула("дискриминант", "абв", ("-", умн(А(1), А(1)), умн(умн(Ч(4), А(0)), А(2))), ТОЧНЫЕ,
         "дискриминант квадратного уравнения: D = б² - 4ав")
ФУНКЦИИ["дискриминант"]["варианты"].insert(0, ([МН], РАЦ))
ФУНКЦИИ["дискриминант"]["c"].insert(0, "dan_poly_discriminant")
ФУНКЦИИ["дискриминант"]["строка"].insert(0, True)

СООБЩЕНИЯ.update({
    "DAN_MSG_POLY_NEG_POW": "многочлен нельзя возводить в отрицательную степень",
    "DAN_MSG_POLY_BIG": "многочлен слишком сложен: степень или коэффициенты слишком велики",
    "DAN_MSG_ANY_X": "уравнение верно при любом значении х",
    "DAN_MSG_BAD_REL": "неизвестный знак сравнения \"%s\"; используйте <, <=, >, >=, =, !=",
    "DAN_MSG_SYS_SHAPE": "в системе из N уравнений каждая строка должна содержать N коэффициентов "
                         "и свободный член",
    "DAN_MSG_SYS_NONE": "система не имеет решений",
    "DAN_MSG_SYS_MANY": "система имеет бесконечно много решений",
    "DAN_MSG_NOT_QUADRATIC": "это не квадратное уравнение: коэффициент при х² равен нулю",
    "DAN_MSG_DEG_TWO": "дискриминант определён для многочлена второй степени",
    "DAN_MSG_INEQ_LISTS": "списки левых частей, знаков и правых частей должны быть одной длины",
    "DAN_TXT_NONE": "нет решений",
    "DAN_TXT_X": "х",
    "DAN_TXT_MINF": "-∞",
    "DAN_TXT_PINF": "+∞",
    "DAN_TXT_UNION": " ∪ ",
    "DAN_TXT_LE": "≤",
    "DAN_TXT_GE": "≥",
    "DAN_TXT_NE": "≠",
})

_НАДСТРОЧНЫЕ = "⁰¹²³⁴⁵⁶⁷⁸⁹"


def _код_многочленов_строки():
    return ("static const char *const dan_sup[10] = {"
            + ", ".join(_строка_c(с) for с in _НАДСТРОЧНЫЕ) + "};")


КОД_МНОГОЧЛЕНОВ = r"""
/* ---- polynomials in х with exact rational coefficients ---- */

/* c[i] - coefficient of х^i; zero polynomial: deg = -1. The polynomial is a
   value in memory with a header: its places are counted like those of strings. */
struct dan_poly { dan_hdr h; int64_t deg; dan_rat c[]; };

static void dan_poly_destroy(dan_hdr *h) { free(h); }
static dan_poly *dan_poly_alloc(int64_t deg) {
    dan_poly *p = dan_alloc(sizeof *p + sizeof(dan_rat) * (size_t)(deg < 0 ? 1 : deg + 1));
    dan_hdr_new(&p->h, dan_poly_destroy);
    p->deg = deg;
    for (int64_t i = 0; i <= deg; i++) { p->c[i].n = 0; p->c[i].d = 1; }
    return p;
}
static dan_mn dan_poly_share(dan_mn p) { if (p) dan_hdr_share((dan_hdr *)&p->h); return p; }
static void dan_poly_release(dan_mn p) { if (p) dan_hdr_release((dan_hdr *)&p->h, dan_poly_destroy); }
static void dan_poly_give(dan_mn p) { if (p) dan_hdr_give((dan_hdr *)&p->h, dan_poly_destroy); }
static dan_mn dan_poly_clone(dan_mn p) {
    size_t n = sizeof(dan_rat) * (size_t)(p->deg < 0 ? 1 : p->deg + 1);
    dan_poly *r = dan_alloc(sizeof *r + n);
    dan_hdr_detached(&r->h);
    r->deg = p->deg;
    memcpy(r->c, p->c, n);
    return r;
}

static dan_mn dan_poly_trim(dan_poly *p) {
    while (p->deg >= 0 && p->c[p->deg].n == 0) p->deg--;
    return p;
}

static dan_rat dan_poly_coef(dan_mn p, int64_t i) {
    dan_rat z = { 0, 1 };
    return (i >= 0 && i <= p->deg) ? p->c[i] : z;
}

static dan_mn dan_poly_const(dan_rat r) {
    dan_poly *p = dan_poly_alloc(0);
    p->c[0] = r;
    return dan_poly_trim(p);
}

static dan_mn dan_poly_from_int(int64_t x, int line) { return dan_poly_const(dan_rat_from_int(x, line)); }

static dan_mn dan_poly_x(void) {
    dan_poly *p = dan_poly_alloc(1);
    p->c[1].n = 1;
    return p;
}

static dan_mn dan_poly_from_list(const dl_Lr *l, int line) {
    (void)line;
    dan_poly *p = dan_poly_alloc(l->len - 1);
    for (int64_t i = 0; i < l->len; i++) p->c[l->len - 1 - i] = l->items[i];
    return dan_poly_trim(p);
}

static dl_Lr *dan_poly_coeffs(dan_mn p) {
    dl_Lr *r = dl_Lr_from(0, NULL);
    if (p->deg < 0) { dan_rat z = { 0, 1 }; dl_Lr_push(r, z); }
    for (int64_t i = p->deg; i >= 0; i--) dl_Lr_push(r, p->c[i]);
    return r;
}

static dan_mn dan_poly_add(dan_mn a, dan_mn b, int line) {
    int64_t d = a->deg > b->deg ? a->deg : b->deg;
    dan_poly *r = dan_poly_alloc(d);
    for (int64_t i = 0; i <= d; i++) r->c[i] = dan_rat_add(dan_poly_coef(a, i), dan_poly_coef(b, i), line);
    return dan_poly_trim(r);
}

static dan_mn dan_poly_neg(dan_mn a, int line) {
    dan_poly *r = dan_poly_alloc(a->deg);
    for (int64_t i = 0; i <= a->deg; i++) r->c[i] = dan_rat_neg(a->c[i], line);
    return r;
}

static dan_mn dan_poly_sub(dan_mn a, dan_mn b, int line) { return dan_poly_add(a, dan_poly_neg(b, line), line); }

static dan_mn dan_poly_mul(dan_mn a, dan_mn b, int line) {
    if (a->deg < 0 || b->deg < 0) return dan_poly_alloc(-1);
    if (a->deg + b->deg > 100000) dan_error(DAN_MSG_POLY_BIG, line);
    dan_poly *r = dan_poly_alloc(a->deg + b->deg);
    for (int64_t i = 0; i <= a->deg; i++)
        for (int64_t j = 0; j <= b->deg; j++)
            r->c[i + j] = dan_rat_add(r->c[i + j], dan_rat_mul(a->c[i], b->c[j], line), line);
    return dan_poly_trim(r);
}

static dan_mn dan_poly_scale(dan_mn a, dan_rat k, int line) { return dan_poly_mul(a, dan_poly_const(k), line); }

static dan_mn dan_poly_pow(dan_mn a, int64_t e, int line) {
    if (e < 0) dan_error(DAN_MSG_POLY_NEG_POW, line);
    if (a->deg > 0 && e > 100000 / a->deg) dan_error(DAN_MSG_POLY_BIG, line);
    dan_mn r = dan_poly_from_int(1, line);
    while (e > 0) {
        if (e & 1) r = dan_poly_mul(r, a, line);
        e >>= 1;
        if (e > 0) a = dan_poly_mul(a, a, line);
    }
    return r;
}

static void dan_poly_divmod(dan_mn a, dan_mn b, dan_mn *q, dan_mn *rem, int line) {
    if (b->deg < 0) dan_error(DAN_MSG_DIV_ZERO, line);
    dan_poly *r = dan_poly_alloc(a->deg);
    for (int64_t i = 0; i <= a->deg; i++) r->c[i] = a->c[i];
    int64_t qd = a->deg - b->deg;
    dan_poly *qq = dan_poly_alloc(qd < 0 ? -1 : qd);
    for (int64_t k = qd; k >= 0; k--) {
        dan_rat coef = dan_rat_div(r->c[b->deg + k], b->c[b->deg], line);
        qq->c[k] = coef;
        if (coef.n == 0) continue;
        for (int64_t j = 0; j <= b->deg; j++)
            r->c[j + k] = dan_rat_sub(r->c[j + k], dan_rat_mul(coef, b->c[j], line), line);
    }
    *q = dan_poly_trim(qq);
    *rem = dan_poly_trim(r);
}

static dan_mn dan_poly_div(dan_mn a, dan_mn b, int line) { dan_mn q, r; dan_poly_divmod(a, b, &q, &r, line); return q; }
static dan_str dan_poly_to_str(dan_mn p);
/* «а / б» для многочленов: точное частное; если остаток не ноль, частное - не многочлен. */
static dan_mn dan_poly_div_exact(dan_mn a, dan_mn b, int line) {
    dan_mn q, r;
    dan_poly_divmod(a, b, &q, &r, line);
    if (r->deg >= 0) dan_not_exact(dan_poly_to_str(a), dan_poly_to_str(b), line);
    return q;
}
static dan_mn dan_poly_mod(dan_mn a, dan_mn b, int line) { dan_mn q, r; dan_poly_divmod(a, b, &q, &r, line); return r; }

static bool dan_poly_eq(dan_mn a, dan_mn b) {
    if (a->deg != b->deg) return false;
    for (int64_t i = 0; i <= a->deg; i++)
        if (a->c[i].n != b->c[i].n || a->c[i].d != b->c[i].d) return false;
    return true;
}

static dan_rat dan_poly_eval_rat(dan_mn p, dan_rat x, int line) {
    dan_rat v = { 0, 1 };
    for (int64_t i = p->deg; i >= 0; i--) v = dan_rat_add(dan_rat_mul(v, x, line), p->c[i], line);
    return v;
}

static double dan_poly_eval_float(dan_mn p, double x) {
    double v = 0;
    for (int64_t i = p->deg; i >= 0; i--) v = v * x + dan_rat_to_float(p->c[i]);
    return v;
}

static dan_mn dan_poly_derivative(dan_mn p, int line) {
    if (p->deg <= 0) return dan_poly_alloc(-1);
    dan_poly *r = dan_poly_alloc(p->deg - 1);
    for (int64_t i = 1; i <= p->deg; i++) r->c[i - 1] = dan_rat_mul(p->c[i], dan_rat_from_int(i, line), line);
    return dan_poly_trim(r);
}

/* ---- text ---- */

static void dan_buf_rat(dan_buf *b, dan_rat r) {
    char t[48];
    if (r.d == 1) snprintf(t, sizeof t, "%" PRId64, r.n);
    else snprintf(t, sizeof t, "%" PRId64 "/%" PRId64, r.n, r.d);
    dan_buf_add(b, t);
}

static void dan_buf_power(dan_buf *b, int64_t k) {
    dan_buf_add(b, DAN_TXT_X);
    if (k < 2) return;
    char digits[24];
    snprintf(digits, sizeof digits, "%" PRId64, k);
    for (char *p = digits; *p; p++) dan_buf_add(b, dan_sup[*p - '0']);
}

/* 2х² - (1/2)х + 3 */
static void dan_buf_poly(dan_buf *b, dan_mn p) {
    if (p->deg < 0) { dan_buf_add(b, "0"); return; }
    bool first = true;
    for (int64_t i = p->deg; i >= 0; i--) {
        dan_rat c = p->c[i];
        if (c.n == 0) continue;
        bool neg = c.n < 0;
        dan_rat a = c;
        if (neg) a.n = -a.n;
        if (first) { if (neg) dan_buf_add(b, "-"); }
        else dan_buf_add(b, neg ? " - " : " + ");
        first = false;
        if (i == 0) dan_buf_rat(b, a);
        else {
            if (!(a.n == 1 && a.d == 1)) {
                if (a.d != 1) dan_buf_add(b, "(");
                dan_buf_rat(b, a);
                if (a.d != 1) dan_buf_add(b, ")");
            }
            dan_buf_power(b, i);
        }
    }
}

static dan_str dan_poly_to_str(dan_mn p) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    dan_buf_poly(&b, p);
    return dan_buf_str(&b);
}

static void dan_write_poly(dan_mn p) { fputs(dan_poly_to_str(p), stdout); }

/* ---- integer form and exact rational roots ---- */

static dan_i128 dan_abs128(dan_i128 a) { return a < 0 ? -a : a; }

static dan_i128 dan_gcd_i128(dan_i128 a, dan_i128 b) {
    return (dan_i128)dan_gcd128((dan_u128)dan_abs128(a), (dan_u128)dan_abs128(b));
}

/* Integer coefficients of a polynomial proportional to p: common factor 1, leading > 0.
   *scale receives p / integer_form (the rational common factor). */
static dan_i128 *dan_poly_integer(dan_mn p, dan_rat *scale, int line) {
    dan_i128 l = 1;
    for (int64_t i = 0; i <= p->deg; i++) {
        dan_i128 d = p->c[i].d;
        l = l / dan_gcd_i128(l, d) * d;
        if (l > ((dan_i128)1 << 62)) dan_error(DAN_MSG_POLY_BIG, line);
    }
    dan_i128 *z = dan_alloc(sizeof(dan_i128) * (size_t)(p->deg + 1));
    dan_i128 g = 0;
    for (int64_t i = 0; i <= p->deg; i++) {
        z[i] = (dan_i128)p->c[i].n * (l / p->c[i].d);
        g = dan_gcd_i128(g, z[i]);
    }
    if (g == 0) g = 1;
    if (z[p->deg] < 0) g = -g;
    for (int64_t i = 0; i <= p->deg; i++) z[i] /= g;
    if (scale) *scale = dan_rat_make(g, l, line);
    return z;
}

typedef struct { dan_i128 *z; int64_t deg; } dan_zpoly;

static bool dan_zpoly_has_root(const dan_zpoly *f, dan_i128 p, dan_i128 q) {
    /* q^n f(p/q) = sum z_i p^i q^(n-i); overflow means the value is far from zero */
    dan_i128 v = f->z[f->deg], qp = 1, t;
    for (int64_t i = f->deg - 1; i >= 0; i--) {
        if (__builtin_mul_overflow(qp, q, &qp)) return false;
        if (__builtin_mul_overflow(v, p, &v)) return false;
        if (__builtin_mul_overflow(f->z[i], qp, &t)) return false;
        if (__builtin_add_overflow(v, t, &v)) return false;
    }
    return v == 0;
}

static void dan_zpoly_divide(dan_zpoly *f, dan_i128 p, dan_i128 q) {
    /* f = (q х - p) * g, exact over the integers */
    int64_t n = f->deg;
    dan_i128 *g = dan_alloc(sizeof(dan_i128) * (size_t)n);
    g[n - 1] = f->z[n] / q;
    for (int64_t k = n - 1; k >= 1; k--) g[k - 1] = (f->z[k] + p * g[k]) / q;
    free(f->z);
    f->z = g;
    f->deg = n - 1;
}

static int64_t dan_divisors128(dan_i128 n, dan_i128 **out, int line) {
    if (n > (dan_i128)1000000000000LL) dan_error(DAN_MSG_POLY_BIG, line);
    int64_t cap = 64, k = 0;
    dan_i128 *d = dan_alloc(sizeof(dan_i128) * (size_t)cap);
    for (dan_i128 i = 1; i * i <= n; i++) {
        if (n % i) continue;
        if (k + 2 > cap) { cap *= 2; d = dan_realloc(d, sizeof(dan_i128) * (size_t)cap); }
        d[k++] = i;
        if (i != n / i) d[k++] = n / i;
    }
    *out = d;
    return k;
}

typedef struct { dan_rat r; int64_t mult; } dan_rroot;

/* Exact rational roots with multiplicities; f is reduced by every root found. */
static int64_t dan_rational_roots(dan_zpoly *f, dan_rroot **out, int line) {
    int64_t cap = 8, k = 0;
    dan_rroot *roots = dan_alloc(sizeof(dan_rroot) * (size_t)cap);
    int64_t zero_mult = 0;
    while (f->deg > 0 && f->z[0] == 0) {           /* root 0 */
        for (int64_t i = 0; i < f->deg; i++) f->z[i] = f->z[i + 1];
        f->deg--;
        zero_mult++;
    }
    if (zero_mult) { roots[k].r.n = 0; roots[k].r.d = 1; roots[k].mult = zero_mult; k++; }
    if (f->deg > 0) {
        dan_i128 *dp, *dq;
        int64_t np = dan_divisors128(dan_abs128(f->z[0]), &dp, line);
        int64_t nq = dan_divisors128(dan_abs128(f->z[f->deg]), &dq, line);
        for (int64_t i = 0; i < np && f->deg > 0; i++)
            for (int64_t j = 0; j < nq && f->deg > 0; j++) {
                if (dan_gcd_i128(dp[i], dq[j]) != 1) continue;
                for (int s = 0; s < 2 && f->deg > 0; s++) {
                    dan_i128 p = s ? -dp[i] : dp[i], q = dq[j];
                    int64_t m = 0;
                    while (f->deg > 0 && dan_zpoly_has_root(f, p, q)) { dan_zpoly_divide(f, p, q); m++; }
                    if (m) {
                        if (k == cap) { cap *= 2; roots = dan_realloc(roots, sizeof(dan_rroot) * (size_t)cap); }
                        roots[k].r = dan_rat_make(p, q, line);
                        roots[k].mult = m;
                        k++;
                    }
                }
            }
        free(dp);
        free(dq);
    }
    *out = roots;
    return k;
}

static int dan_qcmp_rroot(const void *a, const void *b) {
    return dan_rat_cmp(((const dan_rroot *)a)->r, ((const dan_rroot *)b)->r);
}

static dan_zpoly dan_zpoly_of(dan_mn p, dan_rat *scale, int line) {
    dan_zpoly f;
    f.z = dan_poly_integer(p, scale, line);
    f.deg = p->deg;
    return f;
}

static dl_Lr *dan_solve_rat(dan_mn p, int line) {
    if (p->deg < 0) dan_error(DAN_MSG_ANY_X, line);
    dl_Lr *r = dl_Lr_from(0, NULL);
    if (p->deg == 0) return r;
    dan_zpoly f = dan_zpoly_of(p, NULL, line);
    dan_rroot *roots;
    int64_t k = dan_rational_roots(&f, &roots, line);
    qsort(roots, (size_t)k, sizeof(dan_rroot), dan_qcmp_rroot);
    for (int64_t i = 0; i < k; i++) dl_Lr_push(r, roots[i].r);
    free(roots);
    free(f.z);
    return r;
}

static dl_Lr *dan_solve_rat2(dan_mn a, dan_mn b, int line) { return dan_solve_rat(dan_poly_sub(a, b, line), line); }

/* ---- real roots ---- */

typedef struct { double v; int exact; dan_rat r; } dan_root;

/* Error-free transformations (Knuth, Dekker): a + b = s + e and a * b = p + e
   exactly, in plain double arithmetic. They give the same result on every
   processor, unlike long double (80 bits on x86, 64 bits on ARM) or fused
   multiply-add, which the compiler is told not to use (-ffp-contract=off). */
static void dan_two_sum(double a, double b, double *s, double *e) {
    double x = a + b, z = x - a;
    *e = (a - (x - z)) + (b - z);
    *s = x;
}

static void dan_split(double a, double *h, double *l) {
    double c = 134217729.0 * a;          /* 2^27 + 1 */
    double big = c - a;
    *h = c - big;
    *l = a - *h;
}

static void dan_two_prod(double a, double b, double *p, double *e) {
    double x = a * b, ah, al, bh, bl;
    dan_split(a, &ah, &al);
    dan_split(b, &bh, &bl);
    *e = al * bl - (((x - ah * bh) - al * bh) - ah * bl);
    *p = x;
}

/* Value of the polynomial c[0..n] at x by compensated Horner's scheme:
   as accurate as if computed with twice the precision of double. */
static double dan_horner_comp(const double *c, int n, double x) {
    double s = c[n], err = 0;
    for (int j = n - 1; j >= 0; j--) {
        double p, pe, se;
        dan_two_prod(s, x, &p, &pe);
        dan_two_sum(p, c[j], &s, &se);
        err = err * x + (pe + se);
    }
    return s + err;
}

/* Newton steps from a bracketed root with an accurate value of the
   polynomial; then the neighbouring doubles are compared, so the result is
   the double closest to the true root on every processor. */
static double dan_polish(const double *c, int n, double x0) {
    double x = x0;
    for (int it = 0; it < 10; it++) {
        double f = dan_horner_comp(c, n, x), d = 0;
        for (int j = n; j >= 1; j--) d = d * x + c[j] * j;
        if (f == 0 || d == 0) break;
        double y = x - f / d;
        if (y == x) break;
        x = y;
    }
    if (x != x || fabs(x - x0) > 1e-6 * (fabs(x0) + 1)) return x0;   /* the Newton step went astray */
    for (int it = 0; it < 4; it++) {
        double fx = fabs(dan_horner_comp(c, n, x));
        double lo = nextafter(x, -INFINITY), hi = nextafter(x, INFINITY);
        double flo = fabs(dan_horner_comp(c, n, lo)), fhi = fabs(dan_horner_comp(c, n, hi));
        if (flo < fx && flo <= fhi) x = lo;
        else if (fhi < fx) x = hi;
        else break;
    }
    return x;
}

/* Real roots of a polynomial with double coefficients c[0..n] that have
   simple roots: the polynomial is monotone between roots of its derivative. */
static int dan_real_roots_d(const double *c, int n, double *out) {
    while (n > 0 && c[n] == 0) n--;
    if (n <= 0) return 0;
    if (n == 1) { out[0] = -c[0] / c[1]; return 1; }
    double *d = dan_alloc(sizeof(double) * (size_t)n);
    for (int i = 1; i <= n; i++) d[i - 1] = c[i] * i;
    double *crit = dan_alloc(sizeof(double) * (size_t)(n + 1));
    int nc = dan_real_roots_d(d, n - 1, crit);
    double bound = 0;
    for (int i = 0; i < n; i++) { double t = fabs(c[i] / c[n]); if (t > bound) bound = t; }
    bound += 1;
    double *pts = dan_alloc(sizeof(double) * (size_t)(nc + 2));
    int np = 0;
    pts[np++] = -bound;
    for (int i = 0; i < nc; i++) if (crit[i] > -bound && crit[i] < bound) pts[np++] = crit[i];
    pts[np++] = bound;
    int k = 0;
    for (int i = 0; i + 1 < np; i++) {
        double lo = pts[i], hi = pts[i + 1];
        double flo = 0, fhi = 0;
        for (int j = n; j >= 0; j--) { flo = flo * lo + c[j]; fhi = fhi * hi + c[j]; }
        if (flo == 0) { if (k == 0 || out[k - 1] != lo) out[k++] = lo; continue; }
        if ((flo < 0) == (fhi < 0)) continue;
        for (int it = 0; it < 200; it++) {
            double mid = lo + (hi - lo) / 2, fm = 0;
            if (mid <= lo || mid >= hi) break;
            for (int j = n; j >= 0; j--) fm = fm * mid + c[j];
            if (fm == 0) { lo = hi = mid; break; }
            if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
        }
        out[k++] = dan_polish(c, n, lo + (hi - lo) / 2);
    }
    free(d); free(crit); free(pts);
    return k;
}

static dan_mn dan_poly_gcd(dan_mn a, dan_mn b, int line) {
    while (b->deg >= 0) { dan_mn r = dan_poly_mod(a, b, line); a = b; b = r; }
    return a;
}

static int dan_qcmp_root(const void *a, const void *b) {
    double x = ((const dan_root *)a)->v, y = ((const dan_root *)b)->v;
    return (x > y) - (x < y);
}

/* All real roots: rational ones exactly, the others numerically. */
static int64_t dan_poly_real_roots(dan_mn p, dan_root **out, int line) {
    if (p->deg < 0) dan_error(DAN_MSG_ANY_X, line);
    dan_root *roots = dan_alloc(sizeof(dan_root) * (size_t)(p->deg + 2));
    int64_t k = 0;
    if (p->deg <= 0) { *out = roots; return 0; }
    dan_zpoly f = dan_zpoly_of(p, NULL, line);
    dan_rroot *rr;
    int64_t nr = dan_rational_roots(&f, &rr, line);
    for (int64_t i = 0; i < nr; i++) {
        roots[k].v = dan_rat_to_float(rr[i].r);
        roots[k].exact = 1;
        roots[k].r = rr[i].r;
        k++;
    }
    if (f.deg >= 2) {                     /* the rest has no rational roots */
        dan_poly *rest = dan_poly_alloc(f.deg);
        for (int64_t i = 0; i <= f.deg; i++) {
            if (f.z[i] > INT64_MAX || f.z[i] < -INT64_MAX) dan_error(DAN_MSG_POLY_BIG, line);
            rest->c[i] = dan_rat_from_int((int64_t)f.z[i], line);
        }
        dan_mn g = dan_poly_gcd(rest, dan_poly_derivative(rest, line), line);
        dan_mn sf = dan_poly_div(rest, g, line);          /* square-free part */
        double *c = dan_alloc(sizeof(double) * (size_t)(sf->deg + 1));
        for (int64_t i = 0; i <= sf->deg; i++) c[i] = dan_rat_to_float(sf->c[i]);
        double *v = dan_alloc(sizeof(double) * (size_t)(sf->deg + 1));
        int nv = dan_real_roots_d(c, (int)sf->deg, v);
        for (int i = 0; i < nv; i++) { roots[k].v = v[i]; roots[k].exact = 0; k++; }
        free(c);
        free(v);
    }
    free(rr);
    free(f.z);
    qsort(roots, (size_t)k, sizeof(dan_root), dan_qcmp_root);
    *out = roots;
    return k;
}

static dl_Lf *dan_solve_real(dan_mn p, int line) {
    dan_root *roots;
    int64_t k = dan_poly_real_roots(p, &roots, line);
    dl_Lf *r = dl_Lf_from(0, NULL);
    for (int64_t i = 0; i < k; i++) dl_Lf_push(r, roots[i].v);
    free(roots);
    return r;
}

static dl_Lf *dan_solve_real2(dan_mn a, dan_mn b, int line) { return dan_solve_real(dan_poly_sub(a, b, line), line); }

static dl_Lf *dan_quadratic_roots(double a, double b, double c, int line) {
    if (a == 0) dan_error(DAN_MSG_NOT_QUADRATIC, line);
    dl_Lf *r = dl_Lf_from(0, NULL);
    double d = b * b - 4 * a * c;
    if (d < 0) return r;
    if (d == 0) { dl_Lf_push(r, -b / (2 * a)); return r; }
    double s = sqrt(d);
    /* numerically stable form */
    double q = -0.5 * (b + (b >= 0 ? s : -s));
    double coef[3] = { c, b, a };
    double x1 = dan_polish(coef, 2, q / a), x2 = dan_polish(coef, 2, c / q);
    if (x1 > x2) { double t = x1; x1 = x2; x2 = t; }
    dl_Lf_push(r, x1);
    dl_Lf_push(r, x2);
    return r;
}

static dan_rat dan_poly_discriminant(dan_mn p, int line) {
    if (p->deg != 2) dan_error(DAN_MSG_DEG_TWO, line);
    dan_rat a = p->c[2], b = p->c[1], c = p->c[0];
    return dan_rat_sub(dan_rat_mul(b, b, line),
                       dan_rat_mul(dan_rat_from_int(4, line), dan_rat_mul(a, c, line), line), line);
}

/* ---- factorization ---- */

static void dan_buf_factor(dan_buf *b, dan_rat r, int64_t mult) {
    /* root p/q -> (qх - p) */
    dan_buf_add(b, "(");
    if (r.d != 1) { char t[32]; snprintf(t, sizeof t, "%" PRId64, r.d); dan_buf_add(b, t); }
    dan_buf_add(b, DAN_TXT_X);
    char t[32];
    snprintf(t, sizeof t, " %c %" PRId64, r.n > 0 ? '-' : '+', r.n > 0 ? r.n : -r.n);
    dan_buf_add(b, t);
    dan_buf_add(b, ")");
    if (mult > 1) {
        char digits[24];
        snprintf(digits, sizeof digits, "%" PRId64, mult);
        for (char *p = digits; *p; p++) dan_buf_add(b, dan_sup[*p - '0']);
    }
}

static void dan_buf_const_factor(dan_buf *b, dan_rat c, bool alone) {
    if (alone) { dan_buf_rat(b, c); return; }
    if (c.n == 1 && c.d == 1) return;
    if (c.n == -1 && c.d == 1) { dan_buf_add(b, "-"); return; }
    if (c.d != 1) { dan_buf_add(b, "("); dan_buf_rat(b, c); dan_buf_add(b, ")"); }
    else dan_buf_rat(b, c);
}

static dan_str dan_factor_str(dan_mn p, int line) {
    if (p->deg <= 0) return dan_poly_to_str(p);
    dan_rat scale;
    dan_zpoly f = dan_zpoly_of(p, &scale, line);
    dan_rroot *roots;
    int64_t k = dan_rational_roots(&f, &roots, line);
    if (k == 0 && scale.n == 1 && scale.d == 1) { free(roots); free(f.z); return dan_poly_to_str(p); }   /* does not factor */
    qsort(roots, (size_t)k, sizeof(dan_rroot), dan_qcmp_rroot);
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    dan_buf_const_factor(&b, scale, false);
    for (int64_t i = 0; i < k; i++) {
        if (roots[i].r.n == 0) dan_buf_power(&b, roots[i].mult);      /* х^m */
        else dan_buf_factor(&b, roots[i].r, roots[i].mult);
    }
    if (f.deg > 0) {
        dan_poly *rest = dan_poly_alloc(f.deg);
        for (int64_t i = 0; i <= f.deg; i++) rest->c[i] = dan_rat_from_int((int64_t)f.z[i], line);
        dan_buf_add(&b, "(");
        dan_buf_poly(&b, rest);
        dan_buf_add(&b, ")");
    }
    free(roots);
    free(f.z);
    return dan_buf_str(&b);
}

/* 2х² + 6х -> 2х(х + 3) */
static dan_str dan_common_factor_str(dan_mn p, int line) {
    if (p->deg < 0) return dan_poly_to_str(p);
    int64_t low = 0, terms = 0;
    while (p->c[low].n == 0) low++;
    for (int64_t i = 0; i <= p->deg; i++) if (p->c[i].n != 0) terms++;
    dan_rat scale;
    dan_i128 *z = dan_poly_integer(p, &scale, line);
    if (terms == 1 || (low == 0 && scale.n == 1 && scale.d == 1)) { free(z); return dan_poly_to_str(p); }
    dan_poly *rest = dan_poly_alloc(p->deg - low);
    for (int64_t i = low; i <= p->deg; i++) rest->c[i - low] = dan_rat_from_int((int64_t)z[i], line);
    free(z);
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    dan_buf_const_factor(&b, scale, false);
    if (low > 0) dan_buf_power(&b, low);
    dan_buf_add(&b, "(");
    dan_buf_poly(&b, rest);
    dan_buf_add(&b, ")");
    return dan_buf_str(&b);
}

/* ---- inequalities: the interval method ---- */

enum { DAN_LT, DAN_LE, DAN_GT, DAN_GE, DAN_EQ, DAN_NE };

static int dan_parse_rel(dan_str s, int line) {
    if (!strcmp(s, "<")) return DAN_LT;
    if (!strcmp(s, "<=") || !strcmp(s, DAN_TXT_LE)) return DAN_LE;
    if (!strcmp(s, ">")) return DAN_GT;
    if (!strcmp(s, ">=") || !strcmp(s, DAN_TXT_GE)) return DAN_GE;
    if (!strcmp(s, "=") || !strcmp(s, "==")) return DAN_EQ;
    if (!strcmp(s, "!=") || !strcmp(s, DAN_TXT_NE)) return DAN_NE;
    dan_conv_error(DAN_MSG_BAD_REL, s, line);
    return 0;
}

typedef struct { dan_mn p; int rel; dan_root *roots; int64_t nroots; } dan_atom;
/* A condition is an OR of groups; a group is an AND of atoms. */
typedef struct { dan_atom *atoms; int64_t n; } dan_group;

static bool dan_same_point(const dan_root *a, const dan_root *b) {
    if (a->exact && b->exact) return dan_rat_cmp(a->r, b->r) == 0;
    double m = fabs(a->v) > 1 ? fabs(a->v) : 1;
    return fabs(a->v - b->v) <= 1e-9 * m;
}

static bool dan_rel_holds(int rel, int sign) {
    switch (rel) {
        case DAN_LT: return sign < 0;
        case DAN_LE: return sign <= 0;
        case DAN_GT: return sign > 0;
        case DAN_GE: return sign >= 0;
        case DAN_EQ: return sign == 0;
        default: return sign != 0;
    }
}

/* sign of the atom polynomial at a sample point (or at a breakpoint) */
static int dan_atom_sign(const dan_atom *a, const dan_root *pt, double x) {
    if (a->p->deg < 0) return 0;
    if (pt) {
        for (int64_t i = 0; i < a->nroots; i++) if (dan_same_point(&a->roots[i], pt)) return 0;
        x = pt->v;
    }
    double v = dan_poly_eval_float(a->p, x);
    return (v > 0) - (v < 0);
}

static bool dan_condition_holds(dan_group *g, int64_t ng, const dan_root *pt, double x) {
    for (int64_t i = 0; i < ng; i++) {
        bool all = true;
        for (int64_t j = 0; j < g[i].n && all; j++)
            all = dan_rel_holds(g[i].atoms[j].rel, dan_atom_sign(&g[i].atoms[j], pt, x));
        if (all) return true;
    }
    return false;
}

static void dan_buf_point(dan_buf *b, const dan_root *r) {
    if (r->exact) { dan_buf_rat(b, r->r); return; }
    char t[64];
    double v = dan_round_decimal(r->v, 4);
    snprintf(t, sizeof t, "%.4f", v);
    char *e = t + strlen(t) - 1;                 /* drop trailing zeros */
    while (*e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    for (char *p = t; *p; p++) if (*p == '.') *p = ',';
    if (!strcmp(t, "-0")) strcpy(t, "0");
    dan_buf_add(b, t);
}

static dan_str dan_solve_condition(dan_group *g, int64_t ng, int line) {
    /* breakpoints: roots of every polynomial */
    int64_t cap = 16, nb = 0;
    dan_root *bp = dan_alloc(sizeof(dan_root) * (size_t)cap);
    for (int64_t i = 0; i < ng; i++)
        for (int64_t j = 0; j < g[i].n; j++) {
            dan_atom *a = &g[i].atoms[j];
            a->nroots = 0;
            a->roots = NULL;
            if (a->p->deg <= 0) continue;
            a->nroots = dan_poly_real_roots(a->p, &a->roots, line);
            for (int64_t k = 0; k < a->nroots; k++) {
                bool dup = false;
                for (int64_t m = 0; m < nb && !dup; m++) dup = dan_same_point(&bp[m], &a->roots[k]);
                if (dup) continue;
                if (nb == cap) { cap *= 2; bp = dan_realloc(bp, sizeof(dan_root) * (size_t)cap); }
                bp[nb++] = a->roots[k];
            }
        }
    qsort(bp, (size_t)nb, sizeof(dan_root), dan_qcmp_root);
    /* pieces: segment 0, point 0, segment 1, ..., point nb-1, segment nb */
    int64_t np = 2 * nb + 1;
    bool *in = dan_alloc(sizeof(bool) * (size_t)np);
    for (int64_t s = 0; s <= nb; s++) {
        double x;
        if (nb == 0) x = 0;
        else if (s == 0) x = bp[0].v - 1 - fabs(bp[0].v);
        else if (s == nb) x = bp[nb - 1].v + 1 + fabs(bp[nb - 1].v);
        else x = bp[s - 1].v + (bp[s].v - bp[s - 1].v) / 2;
        in[2 * s] = dan_condition_holds(g, ng, NULL, x);
        if (s < nb) in[2 * s + 1] = dan_condition_holds(g, ng, &bp[s], 0);
    }
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    bool only_points = true, any = false;
    for (int64_t i = 0; i < np; i++) if (in[i]) { any = true; if (i % 2 == 0) only_points = false; }
    if (!any) { dan_buf_add(&b, DAN_TXT_NONE); goto done; }
    if (only_points) {                           /* {a; b; c} */
        dan_buf_add(&b, "{");
        bool first = true;
        for (int64_t i = 1; i < np; i += 2) if (in[i]) {
            if (!first) dan_buf_add(&b, "; ");
            dan_buf_point(&b, &bp[i / 2]);
            first = false;
        }
        dan_buf_add(&b, "}");
        goto done;
    }
    bool first = true;
    for (int64_t i = 0; i < np;) {
        if (!in[i]) { i++; continue; }
        int64_t j = i;
        while (j + 1 < np && in[j + 1]) j++;
        if (!first) dan_buf_add(&b, DAN_TXT_UNION);
        first = false;
        if (i == j && i % 2 == 1) {              /* isolated point */
            dan_buf_add(&b, "{");
            dan_buf_point(&b, &bp[i / 2]);
            dan_buf_add(&b, "}");
        } else {
            /* piece i starts at: segment -> open at its left breakpoint; point -> closed */
            if (i % 2 == 0) {
                dan_buf_add(&b, "(");
                if (i == 0) dan_buf_add(&b, DAN_TXT_MINF); else dan_buf_point(&b, &bp[i / 2 - 1]);
            } else { dan_buf_add(&b, "["); dan_buf_point(&b, &bp[i / 2]); }
            dan_buf_add(&b, "; ");
            if (j % 2 == 0) {
                if (j == np - 1) dan_buf_add(&b, DAN_TXT_PINF); else dan_buf_point(&b, &bp[j / 2]);
                dan_buf_add(&b, ")");
            } else { dan_buf_point(&b, &bp[j / 2]); dan_buf_add(&b, "]"); }
        }
        i = j + 1;
    }
done:
    for (int64_t i = 0; i < ng; i++)
        for (int64_t j = 0; j < g[i].n; j++) free(g[i].atoms[j].roots);
    free(bp);
    free(in);
    return dan_buf_str(&b);
}

static dan_str dan_ineq_str(dan_mn left, dan_str rel, dan_mn right, int line) {
    dan_atom a = { dan_poly_sub(left, right, line), dan_parse_rel(rel, line), NULL, 0 };
    dan_group g = { &a, 1 };
    return dan_solve_condition(&g, 1, line);
}

static dan_str dan_ineq_system_str(const dl_Lp *lefts, const dl_Ls *rels, const dl_Lp *rights, int line) {
    if (lefts->len != rels->len || lefts->len != rights->len) dan_error(DAN_MSG_INEQ_LISTS, line);
    dan_atom *atoms = dan_alloc(sizeof(dan_atom) * (size_t)(lefts->len + 1));
    for (int64_t i = 0; i < lefts->len; i++) {
        atoms[i].p = dan_poly_sub(lefts->items[i], rights->items[i], line);
        atoms[i].rel = dan_parse_rel(rels->items[i], line);
    }
    dan_group g = { atoms, lefts->len };
    dan_str r = dan_solve_condition(&g, 1, line);
    free(atoms);
    return r;
}

/* |p| rel c  <=>  (p >= 0 and p - c rel 0) or (p < 0 and -p - c rel 0) */
static dan_str dan_abs_condition(dan_mn p, int rel, dan_mn c, int line) {
    dan_atom g1[2] = { { p, DAN_GE, NULL, 0 }, { dan_poly_sub(p, c, line), rel, NULL, 0 } };
    dan_atom g2[2] = { { p, DAN_LT, NULL, 0 }, { dan_poly_sub(dan_poly_neg(p, line), c, line), rel, NULL, 0 } };
    dan_group g[2] = { { g1, 2 }, { g2, 2 } };
    return dan_solve_condition(g, 2, line);
}

static dan_str dan_abs_eq_str(dan_mn p, dan_mn c, int line) { return dan_abs_condition(p, DAN_EQ, c, line); }
static dan_str dan_abs_ineq_str(dan_mn p, dan_str rel, dan_mn c, int line) {
    return dan_abs_condition(p, dan_parse_rel(rel, line), c, line);
}

/* ---- systems of linear equations: Gaussian elimination with exact fractions ---- */

static dl_Lr *dan_solve_system(const dl_LLr *m, int line) {
    int64_t n = m->len;
    for (int64_t i = 0; i < n; i++) if (m->items[i]->len != n + 1) dan_error(DAN_MSG_SYS_SHAPE, line);
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(n * (n + 1) + 1));
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j <= n; j++) a[i * (n + 1) + j] = m->items[i]->items[j];
#define A(i, j) a[(i) * (n + 1) + (j)]
    int64_t row = 0;
    int64_t *where = dan_alloc(sizeof(int64_t) * (size_t)(n + 1));
    for (int64_t j = 0; j < n; j++) where[j] = -1;
    for (int64_t col = 0; col < n && row < n; col++) {
        int64_t piv = -1;
        for (int64_t i = row; i < n; i++) if (A(i, col).n != 0) { piv = i; break; }
        if (piv < 0) continue;
        for (int64_t j = 0; j <= n; j++) { dan_rat t = A(row, j); A(row, j) = A(piv, j); A(piv, j) = t; }
        dan_rat lead = A(row, col);
        for (int64_t j = 0; j <= n; j++) A(row, j) = dan_rat_div(A(row, j), lead, line);
        for (int64_t i = 0; i < n; i++) {
            if (i == row || A(i, col).n == 0) continue;
            dan_rat f = A(i, col);
            for (int64_t j = 0; j <= n; j++)
                A(i, j) = dan_rat_sub(A(i, j), dan_rat_mul(f, A(row, j), line), line);
        }
        where[col] = row;
        row++;
    }
    for (int64_t i = row; i < n; i++) if (A(i, n).n != 0) dan_error(DAN_MSG_SYS_NONE, line);
    if (row < n) dan_error(DAN_MSG_SYS_MANY, line);
    dl_Lr *r = dl_Lr_from(0, NULL);
    for (int64_t j = 0; j < n; j++) dl_Lr_push(r, A(where[j], n));
#undef A
    free(a);
    free(where);
    return r;
}
"""


# Имена функций без различия «е» и «ё».
_БЕЗ_Ё = {имя.replace("ё", "е"): имя for имя in ФУНКЦИИ}
