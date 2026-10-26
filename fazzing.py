"""Фаззинг среды выполнения Дана: разборщики форматов на случайных данных.

Для каждого разборщика (JSON, CSV, XML, документы Word и Excel, письма, PDF,
zip-архивы, сжатые и зашифрованные данные, Base64) собирается программа-
мишень: небольшая программа на Дане, переведённая в C, к которой
добавляется точка входа libFuzzer. Мишень собирается clang с санитайзерами
адресов и неопределённого поведения. libFuzzer подаёт ей миллионы входов,
изменяя их так, чтобы пройти как можно больше ветвей кода разборщика.

Находкой считается:
- выход за границы памяти, использование освобождённой памяти и другие
  ошибки, которые замечает санитайзер;
- зависание (вход обрабатывается дольше нескольких секунд);
- ошибка выполнения Дана внутри разборщика, который обещает возвращать
  ошибку значением (тип «!»): такой разборщик не должен останавливать
  программу ни на каком входе.

Начальные входы («фаззинг/образцы») - настоящие документы, архивы и письма,
созданные самим Даном, и небольшие образцы текстов. Входы, которые фаззер
нашёл сам, копятся в «фаззинг/корпус» (в репозиторий не входят). Каждая
находка сохраняется в «фаззинг/находки/<мишень>», остаётся там после
исправления и проверяется тестами при каждом запуске (test_fazzing.py).

Запуск: python fazzing.py [мишень ...] [--секунд=60] [--потоков=1]
        python fazzing.py --повторить     - только прогнать корпус и находки
Нужен clang с libFuzzer (в Ubuntu: пакеты clang и libclang-rt-dev).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

import dan

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ФАЗЗИНГ = os.path.join(ПАПКА, "фаззинг")
СБОРКА = os.path.join(tempfile.gettempdir(), "dan_fazzing")

# мишень -> (текст функции на Дане с параметром «д: байты»; модули; ошибки выполнения ожидаемы)
МИШЕНИ = {
    "json": ('если пусть з = json.разобрать(стр(д)) { вывод(json.записать(з)) }', ["json"], False),
    "csv": ('если пусть т = csv.разобрать(стр(д)) { вывод(csv.записать(т)) }', ["csv"], False),
    "xml": ('если пусть у = xml.разобрать(стр(д)) { вывод(xml.записать(у)) }', ["xml"], False),
    "word": ('если пусть д2 = word.из_байтов(д) {\n        вывод(д2.текст())\n        вывод(д2.таблицы().длина)\n    }', ["word"], False),
    "excel": ('если пусть к = excel.из_байтов(д) {\n'
              '        для л в к.листы() { если пусть т = к.лист(л) { вывод(т.длина) } }\n    }', ["excel"], False),
    "почта": ('пусть п = почта.разобрать(д)\n    вывод(п.тема + п.текст)\n    вывод(п.вложения.длина)', ["почта"], False),
    "pdf": ('если пусть с = текст_pdf(д) { вывод(с.длина) }', [], False),
    "архив": ('если пусть ф = файлы_архива(д) { вывод(ф.длина) }', [], False),
    "распаковка": ('вывод(распаковать(д).длина)', [], True),
    "расшифровка": ('вывод(распаковать(д, "пароль").длина)', [], True),
    "base64": ('если пусть б = из_base64(стр(д)) { вывод(в_base64(б)) }', [], False),
}


def найти_clang():
    for имя in ("clang", "clang-19", "clang-18", "clang-17"):
        путь = shutil.which(имя)
        if путь:
            return путь
    return None


def libfuzzer_есть():
    clang = найти_clang()
    if not clang:
        return False
    with tempfile.TemporaryDirectory() as п:
        с = os.path.join(п, "f.c")
        with open(с, "w") as ф:
            ф.write("#include <stddef.h>\nint LLVMFuzzerTestOneInput(const unsigned char *d, size_t n) { return 0; }\n")
        р = subprocess.run([clang, "-fsanitize=fuzzer,address", с, "-o", os.path.join(п, "f")],
                           capture_output=True)
        return р.returncode == 0


ТОЧКА_ВХОДА = r'''
/* ---- точка входа libFuzzer ---- */
#include <setjmp.h>
static jmp_buf dan_fuzz_jump;
static int dan_fuzz_inside;
static void dan_fuzz_error(void) {
    if (DAN_FUZZ_ERRORS_EXPECTED && dan_fuzz_inside) longjmp(dan_fuzz_jump, 1);
    abort();            /* ошибка выполнения внутри разборщика - находка */
}
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    dan_argc = *argc; dan_argv = *argv; dan_start();
    dan_on_error = dan_fuzz_error;
    freopen("/dev/null", "w", stdout);
    return 0;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const size_t mark = dan_mark();
    dan_bytes_s *b = dan_bytes_alloc((int64_t)size);
    if (size) memcpy(b->data, data, size);
    if (!setjmp(dan_fuzz_jump)) {
        dan_fuzz_inside = 1;
        DAN_FUZZ_TARGET(b);
    }
    dan_fuzz_inside = 0;
    dan_sweep(mark);
    return 0;
}
'''


def текст_мишени(имя):
    тело, модули, _ = МИШЕНИ[имя]
    подключения = "".join(f'подключить "{м}"\n' for м in модули)
    return (подключения + "функ цель_фаззинга(д: байты) {\n    " + тело + "\n}\n"
            'цель_фаззинга(байты(""))\n')


def собрать_мишень(имя):
    """Путь к собранной мишени."""
    clang = найти_clang()
    os.makedirs(СБОРКА, exist_ok=True)
    путь_dan = os.path.join(СБОРКА, f"цель_{имя}.dan")
    текст = текст_мишени(имя)
    with open(путь_dan, "w", encoding="utf-8") as ф:
        ф.write(текст)
    код = dan.перевести(текст, путь_dan)
    # функция-цель: её вызывает main
    основная = код[код.rindex("int main("):]
    вызов = re.search(r"(\w+)\(dan_bytes_from_str\(", основная)
    if not вызов:
        raise RuntimeError("не найден вызов цели в main")
    код = код.replace("int main(int argc, char **argv) {", "static int dan_unused_main(int argc, char **argv) {", 1)
    код += ТОЧКА_ВХОДА.replace("DAN_FUZZ_TARGET", вызов.group(1)).replace(
        "DAN_FUZZ_ERRORS_EXPECTED", "1" if МИШЕНИ[имя][2] else "0")
    путь_c = os.path.join(СБОРКА, f"fuzz_{list(МИШЕНИ).index(имя)}.c")
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(код)
    путь_исп = путь_c[:-2]
    библиотеки = dan.библиотеки_c(код, СБОРКА)
    if "#define DAN_NET 1" in код:
        библиотеки.append("-ldl")
    р = subprocess.run([clang, "-g", "-O1", "-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=undefined",
                        "-w", путь_c, "-o", путь_исп, "-lm", *библиотеки],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if р.returncode != 0:
        raise RuntimeError(f"мишень «{имя}» не собралась:\n{р.stderr[-3000:]}")
    return путь_исп


# Программа, которая создаёт начальные образцы: настоящие документы, архивы, письма.
ОБРАЗЦЫ = r'''подключить "word"
подключить "excel"
подключить "pdf"
подключить "почта"
пусть о = ОБРАЗЦЫ_ПАПКА
записать(о + "/json/1", "{\"имя\": \"Дан\", \"числа\": [1, -2.5e3, 0], \"вложено\": {\"а\": null, \"б\": true}, \"т\": \"\\u0416\\n\"}")
записать(о + "/json/2", "[[], {}, \"\", 1e400, -0, \"\\ud83d\\ude00\"]")
записать(о + "/csv/1", "Имя;Сумма;Дата\nИванов;1 000,50;01.02.2026\n\"Петров; Пётр\";\"с \"\"кавычкой\"\"\";\n")
записать(о + "/csv/2", "a,b,c\r\n1,\"2\n3\",4\r\n")
записать(о + "/xml/1", "<?xml version=\"1.0\"?><!DOCTYPE к><к а=\"1\" б='&amp;&#1046;'><!-- з --><э>текст &lt; <![CDATA[<сырой>]]></э><п/></к>")
записать(о + "/base64/1", в_base64(байты("Привет, Дан! 0123456789")))
пусть документ = word.документ([word.заголовок("Заголовок", 1), word.абзац("Текст {{поле}} абзаца"),
    word.таблица([["А", "Б"], ["1", "2"]]), word.разрыв_страницы(), word.жирный("Жирный")], word.Оформление())
записать(о + "/word/1", документ)
записать(о + "/архив/1", документ)
записать(о + "/excel/1", excel.книга([excel.Лист("Лист1", [["Имя", "Сумма", "Дата"], ["Иванов", "1000.5", "2026-02-01"]]),
    excel.Лист("Второй", [["=1+2"]], ложь)])!)
если пусть п = pdf.документ([pdf.заголовок("Справка", 1), pdf.абзац("Текст справки."),
        pdf.таблица([["Месяц", "Сумма"], ["Октябрь", "100"]])], pdf.Оформление()) {
    записать(о + "/pdf/1", п)
}
записать(о + "/распаковка/1", сжать("Текст, который сжимается: ".повторить(20)))
записать(о + "/распаковка/2", сжать_gzip("Текст gzip ".повторить(20)))
записать(о + "/расшифровка/1", сжать("секрет", "пароль"))
записать(о + "/вложение.txt", "вложение")
пусть ящик = почта.Ящик("отправитель@пример.рф", "", имя = "Отправитель Тест")
записать(о + "/почта/1", почта.собрать(ящик, почта.Письмо(["Получатель <кому@example.com>"], "Тема письма «ёлки»",
    "Текст письма.\nВторая строка.", html = "<p>Текст</p>", вложения = [о + "/вложение.txt"]))!)
удалить_файл(о + "/вложение.txt")
'''


def папки(имя):
    образцы = os.path.join(ФАЗЗИНГ, "образцы", имя)
    корпус = os.path.join(ФАЗЗИНГ, "корпус", имя)
    находки = os.path.join(ФАЗЗИНГ, "находки", имя)
    for п in (образцы, корпус, находки):
        os.makedirs(п, exist_ok=True)
    return образцы, корпус, находки


def создать_образцы():
    """Создаёт начальные образцы всех мишеней программой на Дане."""
    for имя in МИШЕНИ:
        папки(имя)
    with tempfile.TemporaryDirectory() as п:
        путь = os.path.join(п, "образцы.dan")
        текст = ОБРАЗЦЫ.replace("ОБРАЗЦЫ_ПАПКА", '"' + os.path.join(ФАЗЗИНГ, "образцы").replace("\\", "/") + '"')
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исп = dan.собрать(путь, текст)
        р = subprocess.run([исп], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if р.returncode != 0:
            raise RuntimeError("образцы не созданы: " + р.stderr)


def утечки(имя):
    """Параметр проверки утечек. Где ошибки выполнения ожидаемы, мишень прерывает программу
    посреди работы, и память, выделенная до ошибки, остаётся: это не утечка Дана."""
    return ["-detect_leaks=0"] if МИШЕНИ[имя][2] else []


def окружение(имя):
    if МИШЕНИ[имя][2]:
        return dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    return None


def повторить(имя, исполняемый=None):
    """Прогоняет корпус и сохранённые находки один раз. Возвращает (успех, вывод)."""
    исполняемый = исполняемый or собрать_мишень(имя)
    образцы, _, находки = папки(имя)
    файлы = [os.path.join(п, ф) for п in (образцы, находки) for ф in sorted(os.listdir(п))]
    if not файлы:
        return True, ""
    р = subprocess.run([исполняемый, "-timeout=10", "-rss_limit_mb=4096", *утечки(имя), *файлы],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800,
                       env=окружение(имя))
    return р.returncode == 0, р.stdout + р.stderr


def запустить(имя, секунд, потоков=1):
    """Фаззинг одной мишени. Возвращает список файлов-находок."""
    исполняемый = собрать_мишень(имя)
    образцы, корпус, находки = папки(имя)
    with tempfile.TemporaryDirectory() as временная:
        префикс = os.path.join(временная, "")
        параметры = [f"-max_total_time={секунд}", "-timeout=10", "-rss_limit_mb=4096", "-max_len=262144",
                     f"-artifact_prefix={префикс}", "-print_final_stats=1", *утечки(имя)]
        if потоков > 1:
            параметры += [f"-fork={потоков}", "-ignore_crashes=1", "-ignore_timeouts=1", "-ignore_ooms=1"]
        elif МИШЕНИ[имя][2]:
            # после ожидаемой ошибки память остаётся занятой: процесс мишени время от времени
            # перезапускается, когда она переполняется
            параметры += ["-fork=1", "-ignore_ooms=1"]
        р = subprocess.run([исполняемый, *параметры, корпус, образцы], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=окружение(имя))
        найдено = []
        for ф in sorted(os.listdir(временная)):
            цель = os.path.join(находки, ф)
            shutil.copyfile(os.path.join(временная, ф), цель)
            найдено.append(цель)
    вывод = р.stdout + р.stderr
    статистика = dict(re.findall(r"stat::(number_of_executed_units|new_units_added):\s*(\d+)", вывод))
    вилка = re.findall(r"^#(\d+): cov: \d+ ft: \d+ corp: (\d+)", вывод, flags=re.M)
    if вилка and not статистика:            # режим с перезапуском процесса печатает иначе
        статистика = {"number_of_executed_units": вилка[-1][0], "new_units_added": вилка[-1][1] + " (всего)"}
    return найдено, статистика, вывод


def main(аргументы):
    секунд = 60
    потоков = 1
    только_повторить = False
    имена = []
    for а in аргументы:
        if а.startswith("--секунд="):
            секунд = int(а.split("=", 1)[1])
        elif а.startswith("--потоков="):
            потоков = int(а.split("=", 1)[1])
        elif а == "--повторить":
            только_повторить = True
        elif а in МИШЕНИ:
            имена.append(а)
        else:
            print(f"неизвестная мишень или параметр «{а}»; мишени: {', '.join(МИШЕНИ)}")
            return 2
    if not libfuzzer_есть():
        print("нужен clang с libFuzzer (в Ubuntu: sudo apt install clang libclang-rt-dev)")
        return 2
    имена = имена or list(МИШЕНИ)
    if any(not os.listdir(папки(и)[0]) for и in имена):
        создать_образцы()
    итог = 0
    for имя in имена:
        if только_повторить:
            успех, вывод = повторить(имя)
            print(f"{имя}: {'в порядке' if успех else 'ОШИБКА'}")
            if not успех:
                print(вывод[-4000:])
                итог = 1
            continue
        найдено, стат, вывод = запустить(имя, секунд, потоков)
        print(f"{имя}: входов {стат.get('number_of_executed_units', '?')}, новых в корпусе "
              f"{стат.get('new_units_added', '?')}, находок {len(найдено)}")
        if найдено:
            итог = 1
            print(вывод[-6000:])
            if os.environ.get("GITHUB_ACTIONS"):       # замечание на странице проверки GitHub
                кратко = вывод[-6000:].replace("%", "%25").replace("\r", "").replace("\n", "%0A")
                print(f"::error title=Находка фаззинга%3A {имя}::{кратко}")
            for ф in найдено:
                print("  находка:", os.path.relpath(ф, ПАПКА))
    return итог


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
