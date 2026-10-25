"""Генератор кода языка Дан (версия 0.1).

Переводит проверенное дерево программы в текст на языке C. Полученный
файл самодостаточен: в его начало встраивается небольшая среда
выполнения Дан (вывод значений, работа со строками, защита от деления
на ноль). Затем файл компилируется обычным компилятором C (gcc или
clang) в исполняемую программу.

Соответствие типов:
    цел  -> int64_t
    дроб -> double
    лог  -> bool
    стр  -> const char*   (строка в кодировке UTF-8)

Все имена в получаемом C-коде латинские: имена из программы на Дан
транслитерируются (сумма -> d_summa), а русские тексты записываются
восьмеричными кодами байтов. Поэтому код собирается любым компилятором
C независимо от его настроек кодировки.

Инструкции верхнего уровня помещаются в функцию main, поэтому функции
программы их не видят, как и требует спецификация.
"""

import uzly as у
from proverka import ЦЕЛ, ДРОБ, СТР, ЛОГ, ПУСТО

ТИПЫ_C = {ЦЕЛ: "int64_t", ДРОБ: "double", СТР: "dan_str", ЛОГ: "bool", ПУСТО: "void"}

СРЕДА = r"""/* Создано компилятором языка Дан. Редактировать не нужно. */
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef const char *dan_str;

static void dan_error(const char *text, int line) {
    fflush(stdout);
    fprintf(stderr, DAN_MSG_RUNTIME, line, text);
    exit(1);
}

static int64_t dan_div(int64_t a, int64_t b, int line) {
    if (b == 0) dan_error(DAN_MSG_DIV_ZERO, line);
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}

static int64_t dan_mod(int64_t a, int64_t b, int line) {
    if (b == 0) dan_error(DAN_MSG_MOD_ZERO, line);
    if (b == -1) return 0;
    return a % b;
}

static dan_str dan_concat(dan_str a, dan_str b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 1);
    if (!r) dan_error(DAN_MSG_NO_MEMORY, 0);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

static bool dan_str_eq(dan_str a, dan_str b) { return strcmp(a, b) == 0; }

static void dan_print_int(int64_t v) { printf("%" PRId64 "\n", v); }
static void dan_print_str(dan_str v) { puts(v); }
static void dan_print_bool(bool v) { puts(v ? DAN_TRUE : DAN_FALSE); }

static void dan_print_float(double v) {
    char buf[64];
    /* Shortest text that reads back as the same number; the integer
       part is never shortened to exponent form (10.0, not 1e+01). */
    double mag = v < 0 ? -v : v;
    int start = 1;
    while (mag >= 10.0 && start < 17) { mag /= 10.0; start++; }
    for (int prec = start; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
    puts(buf);
}

static void dan_start(void) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}
"""

# Транслитерация русских букв для имён в C. Имена в C делаются
# латинскими, чтобы программа собиралась любым компилятором.
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


СООБЩЕНИЯ = {
    "DAN_MSG_RUNTIME": "Ошибка выполнения в строке %d: %s\n",
    "DAN_MSG_DIV_ZERO": "деление на ноль",
    "DAN_MSG_MOD_ZERO": "остаток от деления на ноль",
    "DAN_MSG_NO_MEMORY": "недостаточно памяти",
    "DAN_TRUE": "истина",
    "DAN_FALSE": "ложь",
}

ВЫВОД = {ЦЕЛ: "dan_print_int", ДРОБ: "dan_print_float", СТР: "dan_print_str", ЛОГ: "dan_print_bool"}


class Генератор:
    def __init__(self):
        self.имена = {}            # имя в Дан -> имя в C
        self.занятые = set()
        self.строки = []
        self.отступ = 0
        self.счётчик = 0

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

        шапка, тело = СРЕДА.split("typedef", 1)
        self.строки.extend(шапка.rstrip().splitlines())
        for имя, текст in СООБЩЕНИЯ.items():
            self.строки.append(f"#define {имя} {строка_c(текст)}")
        self.строки.extend(("typedef" + тело).splitlines())
        self.пиши()

        if функции:
            self.пиши("/* Объявления функций программы */")
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
        return "\n".join(self.строки) + "\n"

    def _заголовок(self, ф):
        результат = ТИПЫ_C[ф.тип_результата] if ф.тип_результата else "void"
        параметры = ", ".join(f"const {ТИПЫ_C[п.тип]} {self.имя(п.имя)}" for п in ф.параметры) or "void"
        return f"static {результат} {self.имя(ф.имя)}({параметры})"

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
        тип = и.тип or и.значение.тип_выр
        модификатор = "" if и.изменяемая else "const "
        self.пиши(f"{модификатор}{ТИПЫ_C[тип]} {self.имя(и.имя)} = {self.выр(и.значение)};")

    def и_Присваивание(self, и):
        цель = self.имя(и.имя)
        if и.оператор == "=":
            self.пиши(f"{цель} = {self.выр(и.значение)};")
            return
        оп = и.оператор[0]
        тип_цели = и.тип_цели
        значение = self.выр(и.значение)
        self.пиши(f"{цель} = {self._арифметика(оп, цель, тип_цели, значение, и.значение.тип_выр, и.строка)};")

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
        конец = self.служебное_имя("end")
        начало = self.выр(и.источник.начало)
        граница = self.выр(и.источник.конец)
        # Граница вычисляется один раз до начала цикла.
        self.пиши(f"for (int64_t {перем} = {начало}, {конец} = {граница}; "
                  f"{перем} <= {конец}; {перем}++) {{")
        self.блок(и.тело)
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
        return getattr(self, "в_" + type(в).__name__)(в)

    def в_ЦелоеЧисло(self, в):
        return f"INT64_C({в.значение})"

    def в_ДробноеЧисло(self, в):
        return дробь_c(в.значение)

    def в_Строка(self, в):
        return строка_c(в.значение)

    def в_Логическое(self, в):
        return "true" if в.значение else "false"

    def в_Имя(self, в):
        return self.имя(в.имя)

    def в_Унарная(self, в):
        оп = "-" if в.оператор == "-" else "!"
        return f"({оп}{self.выр(в.операнд)})"

    def в_Бинарная(self, в):
        л, п = self.выр(в.левый), self.выр(в.правый)
        тл, тп = в.левый.тип_выр, в.правый.тип_выр
        оп = в.оператор
        if оп in ("+", "-", "*", "/", "%"):
            return self._арифметика(оп, л, тл, п, тп, в.строка)
        if оп == "и":
            return f"({л} && {п})"
        if оп == "или":
            return f"({л} || {п})"
        if тл == СТР:
            равно = f"dan_str_eq({л}, {п})"
            return равно if оп == "==" else f"(!{равно})"
        return f"({л} {оп} {п})"

    def _арифметика(self, оп, л, тл, п, тп, строка):
        if тл == СТР:
            return f"dan_concat({л}, {п})"
        if тл == ЦЕЛ and тп == ЦЕЛ:
            if оп == "/":
                return f"dan_div({л}, {п}, {строка})"
            if оп == "%":
                return f"dan_mod({л}, {п}, {строка})"
        return f"({л} {оп} {п})"

    def в_Вызов(self, в):
        имя = в.функция.имя
        аргументы = [self.выр(а) for а in в.аргументы]
        if имя == "вывод":
            return f"{ВЫВОД[в.аргументы[0].тип_выр]}({аргументы[0]})"
        return f"{self.имя(имя)}({', '.join(аргументы)})"


def сгенерировать(программа):
    """Возвращает текст программы на C для проверенного дерева."""
    return Генератор().программа(программа)
