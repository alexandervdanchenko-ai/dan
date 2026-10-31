"""Единая точка запуска инструментов языка Дан.

Использование:
    python dan.py запустить программа.dan   - собрать программу и сразу выполнить
    python dan.py собрать программа.dan     - собрать исполняемый файл
    python dan.py проверить программа.dan   - найти ошибки в программе (с --json - для программ и моделей)
    python dan.py исправить программа.dan   - применить готовые исправления из сообщений об ошибках
    python dan.py си программа.dan          - показать полученный код на C (с --строки-дан - с указаниями
                                              «#line» на строки программы для gdb и санитайзеров)
    python dan.py дерево программа.dan      - показать дерево программы
    python dan.py токены программа.dan      - показать токены
    python dan.py формат программа.dan      - привести текст программы к единому виду
                                              (с --проверить - только проверить)
    python dan.py отладка программа.dan     - выполнить программу по шагам в отладчике
    python dan.py тестировать [файл|папка]  - выполнить тесты программы (тест "название" { ... });
                                              --покрытие - показать строки, которые тесты не выполнили
    python dan.py замерить [файл|папка]     - замерить время блоков «замер "название" { ... }» и сравнить
                                              с прошлым замером
    python dan.py объяснить программа.dan:12 - объяснить по-русски, что делает строка 12 программы
    python dan.py профиль программа.dan     - выполнить программу и показать, сколько времени заняла
                                              каждая функция и сколько раз её вызывали
    python dan.py документация [файл|папка] - страницы HTML из описаний «///» (в папке «документация»;
                                              --в=папка - в другой; --проверить - выполнить примеры из описаний)
    python dan.py песочница                 - страница в браузере: написать программу и сразу выполнить
    python dan.py сервер                    - сервер языка для редакторов (протокол LSP)
    python dan.py пакеты добавить имя адрес [версия] - добавить пакет (репозиторий git) в проект;
                  также: пакеты установить, обновить [имя], удалить имя, список, проверить
    python dan.py редакция программа.dan    - показать, для какой редакции языка написана программа
    python dan.py редакция программа.dan 2026 - перевести программу в редакцию и указать её в файле
    python dan.py --версия                  - версия компилятора и известные ему редакции языка
    python dan.py обновить [--проверить]    - узнать о новой версии Дана и поставить её
                  (--канал=пробный - получать и пробные выпуски, --канал=стабильный - только обычные)
    python dan.py задачи                    - задачник: скопировать задачи в папку «задачи» и показать список
    python dan.py задача 3                  - проверить решение задачи 3

Значения после имени файла в командах «запустить» и «отладка» передаются программе:
    python dan.py запустить программа.dan отчёт.txt 2026

Параметры для «собрать» и «запустить»:
    --малый   оптимизировать программу по размеру, а не по скорости
    --веб     собрать в WebAssembly: файл .wasm и страница .html для браузера
              (нужен компилятор zig: pip install ziglang)
    --для=система   собрать для другой системы: windows, linux, macos (Apple M1 и новее),
              macos-intel (нужен компилятор zig: pip install ziglang)
    --сжать   сжать готовую программу упаковщиком UPX (алгоритм LZMA):
              файл становится в 2-3 раза меньше и запускается как обычно

Для сборки нужен компилятор C: gcc или clang.
"""

import os
import re
import shutil
import subprocess
import sys

from lexer import разобрать as токены, ОшибкаЛексера
from parser import разобрать as дерево, ОшибкаРазбора
from proverka import проверить, ОшибкаПроверки
from kodogen import сгенерировать
from moduli import загрузить, ОшибкаМодуля
import uzly

КОМАНДЫ = ("запустить", "собрать", "проверить", "си", "дерево", "токены", "формат", "отладка")
КОМПИЛЯТОРЫ = ("gcc", "clang", "cc")
# Служебная информация (символы отладки, таблицы раскрутки стека,
# неиспользуемые функции) из готовой программы удаляется всегда.
# Компоновщик macOS удаляет неиспользуемый код своим параметром.
УДАЛИТЬ_НЕНУЖНОЕ = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
ФЛАГИ_ОБЩИЕ = ["-std=c11", "-fwrapv", "-ffp-contract=off", "-w", "-s", "-ffunction-sections", "-fdata-sections",
               УДАЛИТЬ_НЕНУЖНОЕ, "-fno-asynchronous-unwind-tables", "-fno-ident"]
# «собрать» и «замерить» - выпускная сборка с наибольшей оптимизацией; «запустить», тесты и профиль -
# -O2: программу при правке собирают часто, и сборка с -O3 заметно дольше.
ФЛАГИ_СКОРОСТЬ = ["-O2"]
ФЛАГИ_ВЫПУСК = ["-O3"]
ФЛАГИ_РАЗМЕР = ["-Os"]
# Размер стека в Windows по умолчанию 1 МБ, в Linux обычно 8 МБ; 64 МБ дают
# глубину рекурсии порядка сотен тысяч вызовов.
БИБЛИОТЕКИ = ["-lm"] + (["-Wl,--stack,67108864"] if os.name == "nt" else [])


class ОшибкаСборки(Exception):
    pass


НЕТ_КОМПИЛЯТОРА = ("не найден компилятор C (gcc, clang или zig).\n"
                   "  Проще всего установить Дан установщиком со страницы выпуска: в нём компилятор уже есть.\n"
                   "  Или установите компилятор отдельно: winget install BrechtSanders.WinLibs.POSIX.UCRT\n"
                   "  и откройте новое окно командной строки.")


def папка_установки():
    """Папка, в которой установлен Дан: рядом с dan.exe в собранной поставке,
    иначе папка с исходным кодом компилятора."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def найти_компилятор():
    """Путь к gcc, clang или cc, если один из них есть в системе."""
    for имя in КОМПИЛЯТОРЫ:
        путь = shutil.which(имя)
        if путь:
            return путь
    raise ОшибкаСборки(НЕТ_КОМПИЛЯТОРА)


def это_zig(команда):
    return len(команда) >= 2 and команда[-1] == "cc" and "zig" in os.path.basename(команда[0]).lower() + \
        " ".join(команда[:-1]).lower()


def команда_компилятора():
    """Команда компилятора C (список) для сборки программ на этой системе.

    Порядок: компилятор из переменной DAN_COMPILER (gcc, clang, zig или путь),
    затем gcc, clang или cc из системы, затем zig, поставляемый вместе с Даном
    (папка «zig» рядом с dan.exe) или установленный пакетом Python ziglang."""
    выбор = os.environ.get("DAN_COMPILER", "").strip()
    if выбор == "zig":
        zig = найти_zig()
        if not zig:
            raise ОшибкаСборки("DAN_COMPILER=zig, но компилятор zig не найден")
        return zig
    if выбор:
        путь = shutil.which(выбор) or выбор
        return [путь, *флаги_mingw(путь)]
    for имя in КОМПИЛЯТОРЫ:
        путь = shutil.which(имя)
        if путь:
            return [путь, *флаги_mingw(путь)]
    zig = найти_zig()
    if zig:
        return zig
    raise ОшибкаСборки(НЕТ_КОМПИЛЯТОРА)


def компилятор_есть():
    try:
        команда_компилятора()
        return True
    except ОшибкаСборки:
        return False


_ФЛАГИ_MINGW = {}


def флаги_mingw(компилятор):
    """Дополнительные параметры для компилятора MinGW в Windows.
    Если компилятор установлен в папку с пробелом в имени (например,
    C:\\Users\\Имя Фамилия\\...), GCC подставляет путь к своему служебному файлу
    default-manifest.o без кавычек, компоновщик разрывает его на пробеле и
    сборка не удаётся. Параметр -B с коротким именем (8.3) этой папки, в котором
    пробелов нет, указывает GCC, где взять файл."""
    if os.name != "nt":
        return []
    if компилятор not in _ФЛАГИ_MINGW:
        флаги = []
        try:
            путь = subprocess.run([компилятор, "-print-file-name=default-manifest.o"], capture_output=True,
                                  text=True, encoding="utf-8", errors="replace").stdout.strip()
            путь = os.path.normpath(путь) if путь else ""
            if " " in путь and os.path.isfile(путь):
                короткая = _путь_ascii(os.path.dirname(путь))
                if короткая and " " not in короткая:
                    флаги = ["-B" + короткая.rstrip("\\") + "\\"]
        except OSError:
            pass
        _ФЛАГИ_MINGW[компилятор] = флаги
    return _ФЛАГИ_MINGW[компилятор]


def перевести(исходник, путь=None, отладка=False, тесты=False, профиль=False, покрытие=False, замеры=False,
              строки_дан=False):
    """Текст программы на Дан -> текст на C. Выбрасывает ошибки разбора и проверки.
    Путь нужен, чтобы найти подключённые файлы (подключить "файл.dan");
    «тесты» - программа выполняет свои тесты (тест "..." { }) вместо себя.
    При DAN_KOMPILYATOR=сам код создаёт компилятор на Дане (sam_kompilyator.py)."""
    import sam_kompilyator
    if sam_kompilyator.включён():
        return sam_kompilyator.перевести(исходник, путь, отладка, тесты, профиль, покрытие, замеры, строки_дан)
    return перевести_python(исходник, путь, отладка, тесты, профиль, покрытие, замеры, строки_дан)


def перевести_python(исходник, путь=None, отладка=False, тесты=False, профиль=False, покрытие=False,
                     замеры=False, строки_дан=False):
    """Перевод компилятором на Python."""
    программа = загрузить(исходник, путь)
    проверить(программа)
    показать_предупреждения(программа, путь)
    return сгенерировать(программа, отладка, тесты, профиль, покрытие, замеры, строки_дан)


def показать_предупреждения(программа, путь):
    """Предупреждения не останавливают сборку и выводятся в поток ошибок."""
    for п in getattr(программа, "предупреждения", []):
        print(f"{путь or 'программа'}: предупреждение: {п}", file=sys.stderr)
        фрагмент = uzly.фрагмент_места(п.строка, п.столбец)
        if фрагмент:
            print(фрагмент, file=sys.stderr)


def сжать_программу(путь_exe):
    """Сжимает исполняемый файл упаковщиком UPX. Возвращает (было, стало) в байтах."""
    upx = shutil.which("upx")
    if not upx:
        raise ОшибкаСборки(
            "для параметра --сжать нужен упаковщик UPX.\n"
            "  В Windows его можно установить командой: winget install UPX.UPX\n"
            "  после установки откройте новое окно командной строки.")
    было = os.path.getsize(путь_exe)
    результат = subprocess.run([upx, "--best", "--lzma", "-q", "-q", путь_exe],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
    if результат.returncode != 0:
        raise ОшибкаСборки("UPX не смог сжать программу:\n" + (результат.stderr or результат.stdout).strip())
    return было, os.path.getsize(путь_exe)


def библиотеки_c(код_c, папка):
    """Библиотеки C, которые подключает программа («внешняя "m" { ... }»): параметры компоновщика."""
    итог = []
    for имя in re.findall(r"^/\* DAN_LINK: (.+) \*/$", код_c, re.M):
        if any(имя.endswith(р) for р in (".a", ".so", ".dll", ".lib", ".o")) or "/" in имя or "\\" in имя:
            путь = имя if os.path.isabs(имя) else os.path.join(папка, имя)
            параметр = путь
        else:
            параметр = "-l" + имя
        if параметр not in итог:
            итог.append(параметр)
    return итог


def найти_zig():
    """Команда «zig cc» (компилятор C, умеющий собирать для любой системы) или None."""
    свой = os.path.join(папка_установки(), "zig", "zig.exe" if os.name == "nt" else "zig")
    if os.path.isfile(свой):
        return [свой, "cc"]
    zig = shutil.which("zig")
    if zig:
        return [zig, "cc"]
    try:
        import importlib.util
        if importlib.util.find_spec("ziglang"):
            return [sys.executable, "-m", "ziglang", "cc"]
    except ImportError:
        pass
    return None


# Сборка для другой системы (--для=...): цель zig и окончание имени файла.
СИСТЕМЫ = {
    "windows": ("x86_64-windows-gnu", "-windows.exe"),
    "linux": ("x86_64-linux-gnu.2.28", "-linux"),
    "macos": ("aarch64-macos", "-macos"),
    "macos-intel": ("x86_64-macos", "-macos-intel"),
}


def найти_компилятор_wasm():
    """Команда компилятора C для WebAssembly (WASI): zig или clang с WASI SDK."""
    zig = найти_zig()
    if zig:
        return [*zig, "-target", "wasm32-wasi"]
    sdk = os.environ.get("WASI_SDK_PATH")
    if sdk and shutil.which("clang"):
        return [shutil.which("clang"), "--target=wasm32-wasi", f"--sysroot={os.path.join(sdk, 'share', 'wasi-sysroot')}"]
    raise ОшибкаСборки("для сборки программы в WebAssembly нужен компилятор zig. Установите его командой:\n"
                       "  pip install ziglang\n"
                       "(или укажите в переменной WASI_SDK_PATH папку WASI SDK для clang)")


def собрать_веб(путь, исходник, малый=False):
    """Собирает программу в WebAssembly: файл .wasm (WASI) и страницу .html для браузера."""
    import veb
    код_c = перевести(исходник, путь)
    if "#define DAN_TASKS 1" in код_c:
        raise ОшибкаСборки("параллельные задачи (запустить, параллельно, каналы) пока не поддерживаются "
                           "в программах для WebAssembly")
    if "#define DAN_GUI 1" in код_c:
        raise ОшибкаСборки("окна (подключить \"окна\") есть только в программах для Windows")
    if "__asm__(DAN_SYM(" in код_c:
        raise ОшибкаСборки("функции библиотек C («внешняя») пока не поддерживаются в программах для WebAssembly: "
                           "там другие размеры чисел C, и вызов с неточными типами останавливает программу")
    основа = os.path.splitext(путь)[0]
    путь_c, путь_wasm, путь_html = основа + ".c", основа + ".wasm", основа + ".html"
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(код_c)
    флаги = [ф if ф != УДАЛИТЬ_НЕНУЖНОЕ else "-Wl,--gc-sections"
             for ф in ФЛАГИ_ОБЩИЕ if ф not in ("-fno-asynchronous-unwind-tables",)] + \
        (ФЛАГИ_РАЗМЕР if малый else ФЛАГИ_СКОРОСТЬ) + ["-Wl,-z,stack-size=8388608"]
    результат = _скомпилировать([*найти_компилятор_wasm(), *флаги], путь_c, путь_wasm, [])
    if результат.returncode != 0:
        raise ОшибкаСборки("компилятор C для WebAssembly сообщил об ошибке. Это ошибка самого компилятора Дан, "
                           "а не вашей программы; сохраните файл " + путь_c + " для разбора.\n" + результат.stderr.strip())
    with open(путь_wasm, "rb") as ф:
        байты = ф.read()
    with open(путь_html, "w", encoding="utf-8") as ф:
        ф.write(veb.страница(байты, os.path.basename(основа)))
    return путь_wasm, путь_html


def выполнить_wasm(путь_wasm, аргументы):
    """Выполняет модуль WebAssembly исполнителем wasmtime (модуль Python или программа)."""
    try:
        import wasmtime
    except ImportError:
        wasmtime = None
    if wasmtime is None:
        программа = shutil.which("wasmtime")
        if not программа:
            raise ОшибкаСборки("для запуска программы WebAssembly здесь нужен исполнитель wasmtime "
                               "(pip install wasmtime); страницу .html можно открыть в браузере")
        return subprocess.run([программа, "run", "-W", "max-wasm-stack=33554432", "--dir=.", путь_wasm,
                               *аргументы]).returncode
    import threading
    # Стек исполнителя (2 МБ) рассчитан так, чтобы глубокую рекурсию останавливала проверка
    # Дана с понятным сообщением, а не сам исполнитель.
    настройки_движка = wasmtime.Config()
    настройки_движка.max_wasm_stack = 2 * 1024 * 1024
    движок = wasmtime.Engine(настройки_движка)
    хранилище = wasmtime.Store(движок)
    связи = wasmtime.Linker(движок)
    связи.define_wasi()
    настройки = wasmtime.WasiConfig()
    настройки.argv = [os.path.basename(путь_wasm), *аргументы]
    настройки.inherit_stdin()
    настройки.inherit_stdout()
    настройки.inherit_stderr()
    настройки.preopen_dir(".", ".")
    хранилище.set_wasi(настройки)
    модуль = wasmtime.Module.from_file(движок, путь_wasm)
    экземпляр = связи.instantiate(хранилище, модуль)
    sys.stdout.flush()
    итог = {"код": 0}

    def выполнить():
        try:
            экземпляр.exports(хранилище)["_start"](хранилище)
        except wasmtime.ExitTrap as выход:
            итог["код"] = выход.code
        except wasmtime.Trap as о:
            print(f"Ошибка выполнения: программа WebAssembly остановлена исполнителем ({о.message.splitlines()[0]})",
                  file=sys.stderr)
            итог["код"] = 1

    прежний = threading.stack_size()
    # Python в Windows не принимает стек 256 МБ (ValueError): берём наибольший допустимый.
    for мб in (256, 128, 64):
        try:
            threading.stack_size(мб * 1024 * 1024)
            break
        except ValueError:
            continue
    try:
        поток = threading.Thread(target=выполнить)
        поток.start()
        поток.join()
    finally:
        threading.stack_size(прежний)
    return итог["код"]


def _путь_ascii(путь):
    """Короткое имя Windows (8.3) для пути с русскими буквами или None."""
    try:
        import ctypes
        буфер = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.kernel32.GetShortPathNameW(путь, буфер, len(буфер)) and буфер.value.isascii():
            return буфер.value
    except Exception:
        pass
    return None


def _скомпилировать(команда, путь_c, путь_exe, после, входы=()):
    """Запускает компилятор C: команда + [путь_c, "-o", путь_exe] + входы + после
    (входы - готовые объектные файлы).
    Компилятор MinGW в Windows передаёт имена файлов в кодировке системы и не
    может записать файл с русскими буквами в имени, если система не русская.
    Поэтому в Windows такие файлы собираются во временной папке с латинским
    именем и переносятся на место."""
    if os.name != "nt" or (путь_c + путь_exe + "".join(входы)).isascii():
        return subprocess.run([*команда, путь_c, "-o", путь_exe, *входы, *после],
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
    import tempfile
    папка = None
    for где in (None, os.environ.get("PUBLIC"), os.environ.get("SystemDrive", "C:") + "\\"):
        try:
            п = tempfile.mkdtemp(prefix="dan", dir=где)
        except OSError:
            continue
        к = п if п.isascii() else _путь_ascii(п)
        if к:
            папка = (п, к)
            break
        shutil.rmtree(п, ignore_errors=True)
    if папка is None:       # нет ни одной папки с латинским именем: пробуем как есть
        return subprocess.run([*команда, путь_c, "-o", путь_exe, *входы, *после],
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
    настоящая, короткая = папка
    try:
        shutil.copyfile(путь_c, os.path.join(настоящая, "p.c"))
        копии = []
        for н, вход in enumerate(входы):
            shutil.copyfile(вход, os.path.join(настоящая, f"o{н}.o"))
            копии.append(os.path.join(короткая, f"o{н}.o"))
        итог = subprocess.run([*команда, os.path.join(короткая, "p.c"), "-o", os.path.join(короткая, "p.exe"),
                               *копии, *после],
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
        if итог.returncode == 0:
            if os.path.exists(путь_exe):
                os.remove(путь_exe)
            shutil.move(os.path.join(настоящая, "p.exe"), путь_exe)
        return итог
    finally:
        shutil.rmtree(настоящая, ignore_errors=True)


def собрать_для(путь, исходник, система, малый=False):
    """Собирает программу для другой системы компилятором zig. Возвращает путь к файлу."""
    if система not in СИСТЕМЫ:
        raise ОшибкаСборки(f"неизвестная система «{система}»; можно: {', '.join(СИСТЕМЫ)}")
    zig = найти_zig()
    if not zig:
        raise ОшибкаСборки("для сборки программы для другой системы нужен компилятор zig. Установите его командой:\n"
                           "  pip install ziglang")
    цель, окончание = СИСТЕМЫ[система]
    код_c = перевести(исходник, путь)
    основа = os.path.splitext(путь)[0]
    путь_c, путь_exe = основа + ".c", основа + окончание
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(код_c)
    флаги = [ф for ф in ФЛАГИ_ОБЩИЕ if ф != УДАЛИТЬ_НЕНУЖНОЕ] + флаги_оптимизации(малый, выпуск=True)
    библиотеки = ["-lm"]
    if система == "windows":
        библиотеки.append("-Wl,--stack,67108864")
        if "#define DAN_GUI 1" in код_c:
            библиотеки += ["-Wl,--subsystem,windows", "-luser32", "-lgdi32", "-lcomdlg32"]
    elif "DAN_TASKS" in код_c:
        библиотеки.append("-pthread")
    if система == "linux" and "#define DAN_NET 1" in код_c:
        библиотеки.append("-ldl")
    результат = _скомпилировать([*zig, "-target", цель, *флаги], путь_c, путь_exe, библиотеки)
    if результат.returncode != 0:
        raise ОшибкаСборки("компилятор zig сообщил об ошибке. Это ошибка самого компилятора Дан, а не вашей программы; "
                           "сохраните файл " + путь_c + " для разбора.\n" + результат.stderr.strip())
    return путь_exe


def библиотеки_программы(код_c, папка, команда=None):
    """Параметры компоновки программы на этой системе (после имени выходного файла)."""
    библиотеки = БИБЛИОТЕКИ + (["-pthread"] if "DAN_TASKS" in код_c and os.name != "nt" else [])
    библиотеки += библиотеки_c(код_c, папка)
    if "#define DAN_GUI 1" in код_c and os.name == "nt":
        if команда and это_zig(команда):    # zig не знает -mwindows: то же самое по частям
            библиотеки += ["-Wl,--subsystem,windows", "-luser32", "-lgdi32", "-lcomdlg32"]
        else:
            библиотеки.append("-mwindows")    # программа с окнами: без окна консоли за ним
    if "#define DAN_NET 1" in код_c and os.name != "nt":
        библиотеки.append("-ldl")        # загрузка libcurl (в новых glibc dlopen уже в самой libc)
    return библиотеки


# ---- кэш собранных программ ----
# Компилятор C занимает почти всё время сборки, а перевод в C - сотые доли
# секунды. Поэтому готовая программа запоминается по отпечатку полного кода C,
# параметров сборки, компилятора и версии Дана: повторная сборка той же
# программы берёт готовый файл. Отключается переменной DAN_NO_CACHE=1.

КЭШ_НАИБОЛЬШЕ = 300            # программ в кэше; самые давние удаляются


def папка_кэша():
    if os.environ.get("DAN_CACHE"):
        return os.environ["DAN_CACHE"]
    if os.name == "nt":
        основа = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(основа, "Dan", "cache")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "dan")


def ключ_кэша(код_c, команда, флаги, библиотеки):
    """Отпечаток сборки или None, если программу кэшировать нельзя."""
    if os.environ.get("DAN_NO_CACHE") == "1":
        return None
    import hashlib
    import versiya
    о = hashlib.sha256()
    for часть in (versiya.ВЕРСИЯ, sys.platform, " ".join(команда), " ".join(флаги), " ".join(библиотеки)):
        о.update(часть.encode("utf-8", "surrogatepass") + b"\0")
    # сам компилятор: путь, размер и время изменения (обновлённый компилятор - другая сборка)
    for путь in [к for к in команда if os.path.isfile(к)] + [б for б in библиотеки if os.path.isfile(б)]:
        с = os.stat(путь)
        о.update(f"{путь}|{с.st_size}|{с.st_mtime_ns}".encode("utf-8", "surrogatepass") + b"\0")
    о.update(код_c.encode("utf-8", "surrogatepass"))
    return о.hexdigest()


def взять_из_кэша(ключ, путь_exe):
    готовый = os.path.join(папка_кэша(), ключ + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(готовый):
        return False
    try:
        if os.path.exists(путь_exe):
            os.remove(путь_exe)
        shutil.copyfile(готовый, путь_exe)
        shutil.copymode(готовый, путь_exe)
        os.utime(готовый)             # недавно использованный не удаляется первым
        return True
    except OSError:
        return False


def положить_в_кэш(ключ, путь_exe):
    папка = папка_кэша()
    try:
        os.makedirs(папка, exist_ok=True)
        цель = os.path.join(папка, ключ + (".exe" if os.name == "nt" else ""))
        временный = цель + f".{os.getpid()}.tmp"
        shutil.copyfile(путь_exe, временный)
        shutil.copymode(путь_exe, временный)
        os.replace(временный, цель)
        файлы = [os.path.join(папка, и) for и in os.listdir(папка) if not и.endswith(".tmp")]
        файлы = [п for п in файлы if os.path.isfile(п)]      # папка «пакеты» - хранилище пакетов, не программа
        if len(файлы) > КЭШ_НАИБОЛЬШЕ:
            файлы.sort(key=lambda п: os.stat(п).st_mtime)
            for п in файлы[:len(файлы) - КЭШ_НАИБОЛЬШЕ]:
                os.remove(п)
    except OSError:
        pass                          # кэш - только ускорение: без него сборка всё равно удалась


# ---- заранее собранная среда выполнения ----
# Большие библиотеки среды (данные, PDF, система, сеть, база данных, математика) собираются
# один раз и хранятся в кэше как объектные файлы (gotovaya_sreda.py); программа собирается
# из своей части кода C и компонуется с ними. Отключается вместе с кэшем (DAN_NO_CACHE=1)
# или отдельно: DAN_GOTOVAYA_SREDA=0.

def _готовая_часть(группа, команда, флаги):
    """Путь к объектному файлу готовой части группы (собирается при первом обращении) или None."""
    import gotovaya_sreda
    текст, _ = gotovaya_sreda.часть_библиотеки(группа, lambda исходник: перевести(исходник, "образец.dan"))
    отпечаток = gotovaya_sreda.отпечаток(текст, команда, флаги)
    папка = os.path.join(папка_кэша(), "среда")
    готовый = os.path.join(папка, отпечаток + ".o")
    if os.path.isfile(готовый):
        return готовый
    try:
        os.makedirs(папка, exist_ok=True)
        временный = os.path.join(папка, f"{отпечаток}.{os.getpid()}")
        with open(временный + ".c", "w", encoding="utf-8") as ф:
            ф.write(текст)
        итог = _скомпилировать([*команда, *флаги, "-c"], временный + ".c", временный + ".o", [])
        if итог.returncode != 0 or not os.path.isfile(временный + ".o"):
            return None
        os.replace(временный + ".o", готовый)       # другая сборка могла успеть раньше: файл тот же
        return готовый
    except OSError:
        return None
    finally:
        for конец in (".c", ".o"):
            try:
                os.remove(временный + конец)
            except (OSError, UnboundLocalError):
                pass


def _собрать_с_готовой_средой(код_c, команда, флаги, путь_c, путь_exe, библиотеки):
    """Сборка части программы с готовыми частями среды; None - если готовая среда не подходит."""
    if os.environ.get("DAN_GOTOVAYA_SREDA") == "0":
        return None
    import gotovaya_sreda
    try:
        разделение = gotovaya_sreda.разделить(код_c, lambda исходник: перевести(исходник, "образец.dan"))
    except Exception:              # разделение - только ускорение: при любой неожиданности - обычная сборка
        return None
    if разделение is None:
        return None
    программа, группы = разделение
    готовые = []
    for группа in группы:
        готовый = _готовая_часть(группа, команда, флаги)
        if готовый is None:
            return None
        готовые.append(готовый)
    путь_части = путь_c[:-2] + ".часть.c"
    try:
        with open(путь_части, "w", encoding="utf-8") as ф:
            ф.write(программа)
        return _скомпилировать([*команда, *флаги], путь_части, путь_exe, библиотеки, готовые)
    finally:
        try:
            os.remove(путь_части)
        except OSError:
            pass


def собрать(путь, исходник, малый=False, сжать=False, отладка=False, тесты=False, профиль=False, покрытие=False,
            замеры=False, выпуск=False):
    """Собирает исполняемый файл рядом с исходным. Возвращает путь к нему.
    «выпуск» - сборка с -O3 (команды «собрать» и «замерить»), иначе -O2."""
    код_c = перевести(исходник, путь, отладка, тесты, профиль, покрытие, замеры)
    основа = (os.path.splitext(путь)[0] + ("_отладка" if отладка else "") + ("_тесты" if тесты else "")
              + ("_профиль" if профиль else "") + ("_замеры" if замеры else ""))
    return собрать_код(код_c, основа, os.path.dirname(os.path.abspath(путь)), малый, сжать, отладка, выпуск)


def флаги_оптимизации(малый=False, выпуск=False):
    return ФЛАГИ_РАЗМЕР if малый else ФЛАГИ_ВЫПУСК if выпуск else ФЛАГИ_СКОРОСТЬ


def собрать_код(код_c, основа, папка, малый=False, сжать=False, отладка=False, выпуск=False):
    """Собирает исполняемый файл «основа[.exe]» из готового кода C (файл «основа.c»).
    «папка» - папка программы: в ней ищутся библиотеки C, которые подключает программа."""
    путь_c = основа + ".c"
    путь_exe = основа + (".exe" if os.name == "nt" else "")
    if os.path.isdir(путь_exe):
        raise ОшибкаСборки(f"собранная программа должна называться «{путь_exe}», но так называется папка; "
                           "переименуйте папку или файл программы")
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(код_c)
    команда = команда_компилятора()
    флаги = ФЛАГИ_ОБЩИЕ + флаги_оптимизации(малый, выпуск)
    if отладка:
        флаги = [ф for ф in ФЛАГИ_ОБЩИЕ if ф != "-s"] + ["-O0", "-g"]
    библиотеки = библиотеки_программы(код_c, папка, команда)
    ключ = ключ_кэша(код_c, команда, флаги, библиотеки) if not сжать else None
    if ключ and взять_из_кэша(ключ, путь_exe):
        return путь_exe
    результат = None
    if ключ and not отладка:
        результат = _собрать_с_готовой_средой(код_c, команда, флаги, путь_c, путь_exe, библиотеки)
    if результат is None or результат.returncode != 0:
        результат = _скомпилировать([*команда, *флаги], путь_c, путь_exe, библиотеки)
    if результат.returncode != 0:
        raise ОшибкаСборки("компилятор C сообщил об ошибке. Это ошибка самого компилятора Дан, "
                           "а не вашей программы; сохраните файл " + путь_c +
                           " для разбора.\n" + результат.stderr.strip())
    if ключ:
        положить_в_кэш(ключ, путь_exe)
    if сжать:
        было, стало = сжать_программу(путь_exe)
        print(f"Сжато упаковщиком UPX: {было} -> {стало} байт ({100 - стало * 100 // было}% меньше)")
    return путь_exe


def форматировать_файл(путь, исходник, только_проверить):
    from formatirovanie import отформатировать, ОшибкаФорматирования
    try:
        новый = отформатировать(исходник)
    except ОшибкаФорматирования as о:
        print(f"{путь}: файл не отформатирован: {о}")
        return 1
    if новый == исходник:
        print(f"{путь}: уже в едином виде")
        return 0
    if только_проверить:
        print(f"{путь}: текст отличается от единого вида; выполните: python dan.py формат {путь}")
        return 1
    with open(путь, "w", encoding="utf-8", newline="\n") as ф:
        ф.write(новый)
    print(f"{путь}: отформатирован")
    return 0


def команда_редакция(путь, год):
    """Показать редакцию программы или перевести программу в редакцию «год»."""
    import versiya
    try:
        with open(путь, encoding="utf-8") as ф:
            текст = ф.read()
    except (OSError, UnicodeDecodeError):
        print(f"Не удалось прочитать файл «{путь}» (нужен текст в кодировке UTF-8)")
        return 2
    _, указана = versiya.строка_редакции(текст)
    if год is None:
        if указана is None:
            print(f"{путь}: редакция не указана; программа выполняется по правилам первой редакции "
                  f"({list(versiya.РЕДАКЦИИ)[0]}). Чтобы закрепить её, выполните: python dan.py редакция {путь} "
                  f"{versiya.ТЕКУЩАЯ_РЕДАКЦИЯ}")
        else:
            ошибка = versiya.проверить_редакцию(указана)
            print(f"{путь}: {ошибка}" if ошибка else f"{путь}: редакция {указана}, {versiya.РЕДАКЦИИ[указана]}")
            return 1 if ошибка else 0
        return 0
    try:
        новый, шаги = versiya.перевести(текст, год)
    except versiya.ОшибкаРедакции as о:
        print(f"{путь}: {о}")
        return 1
    if новый == текст:
        print(f"{путь}: программа уже написана для редакции {год}")
        return 0
    with open(путь, "w", encoding="utf-8", newline="\n") as ф:
        ф.write(новый)
    print(f"{путь}: переведена в редакцию {год}" + (f" (шаги: {', '.join(шаги)})" if шаги else ""))
    return 0


def есть_тесты(исходник):
    """Есть ли в тексте программы блоки «тест "название" {»."""
    return re.search(r'^\s*тест\s+"', исходник, re.M) is not None


def тестировать(цели):
    """dan тестировать [файл или папка ...]: собрать и выполнить тесты.
    Без аргументов - все файлы .dan текущей папки, в которых есть тесты.
    «--покрытие» - после тестов показать строки программы, которые тесты не выполнили."""
    покрытие = "--покрытие" in цели
    цели = [ц for ц in цели if ц != "--покрытие"]
    файлы = []
    for цель in цели or ["."]:
        if os.path.isdir(цель):
            файлы += sorted(os.path.join(цель, и) for и in os.listdir(цель) if и.endswith(".dan"))
        else:
            файлы.append(цель)
    провалено = нет_тестов = 0
    for путь in файлы:
        try:
            with open(путь, encoding="utf-8") as ф:
                исходник = ф.read()
        except (OSError, UnicodeDecodeError) as о:
            print(f"Не удалось открыть файл «{путь}»: {uzly.причина(о)}")
            провалено += 1
            continue
        if not есть_тесты(исходник):
            нет_тестов += 1
            if цели and not os.path.isdir(цели[0]):
                print(f"{путь}: в программе нет тестов; тест пишется так: тест \"что проверяется\" {{ утвердить(...) }}")
            continue
        print(f"== {путь}")
        sys.stdout.flush()
        try:
            исполняемый = собрать(путь, исходник, тесты=True, покрытие=покрытие)
        except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки) as о:
            показать_ошибку(путь, исходник, о)
            провалено += 1
            continue
        тексты = dict(zip(uzly.ФАЙЛЫ, uzly.ТЕКСТЫ))
        окружение = dict(os.environ)
        данные = None
        if покрытие:
            import tempfile
            данные = os.path.join(tempfile.mkdtemp(prefix="dan_cov_"), "покрытие.txt")
            окружение["DAN_COVERAGE"] = данные
        код = subprocess.run([os.path.abspath(исполняемый)], cwd=os.path.dirname(os.path.abspath(путь)),
                             env=окружение).returncode
        if код != 0:
            провалено += 1
        if данные:
            try:
                print(отчёт_покрытия(данные, тексты))
            except (OSError, ValueError):
                print("Покрытие не записано: программа тестов завершилась аварийно")
            sys.stdout.flush()
    if нет_тестов == len(файлы):
        print("Тестов не найдено")
        return 1
    return 1 if провалено else 0


def время_по_русски(нс):
    """Время в наносекундах: «1,25 с», «12,5 мс», «152,3 мкс», «35,1 нс»."""
    if нс >= 1e9:
        return f"{нс / 1e9:.2f} с".replace(".", ",")
    if нс >= 1e6:
        return f"{нс / 1e6:.1f} мс".replace(".", ",")
    if нс >= 1e3:
        return f"{нс / 1e3:.1f} мкс".replace(".", ",")
    return f"{нс:.1f} нс".replace(".", ",")


def есть_замеры(исходник):
    """Есть ли в тексте программы блоки «замер "название" {»."""
    return re.search(r'^\s*замер\s+"', исходник, re.M) is not None


def файл_прошлых_замеров(путь):
    import hashlib
    ключ = hashlib.sha256(os.path.abspath(путь).encode("utf-8", "surrogatepass")).hexdigest()[:32]
    return os.path.join(папка_кэша(), "замеры", ключ + ".json")


def отчёт_замеров(путь_данных, прошлые):
    """Строки отчёта о замерах и новые значения для сравнения в следующий раз.
    Изменение считается заметным, если оно больше 2% и больше утроенного разброса."""
    with open(путь_данных, encoding="utf-8", errors="replace") as ф:
        записи = [с.split("\t") for с in ф.read().split("\n") if с]
    итог, новые = [], {}
    for запись in записи:
        имя = запись[0]
        if len(запись) == 5 and запись[1] == "!":
            итог.append(f"  {имя}: ОШИБКА ({запись[3]}, строка {запись[4]}): {запись[2]}")
            continue
        среднее, отклонение, _, раз = float(запись[1]), float(запись[2]), float(запись[3]), int(запись[4])
        разброс = отклонение * 100 / среднее if среднее > 0 else 0.0
        новые[имя] = [среднее, разброс]
        строка = (f"  {имя}: {время_по_русски(среднее)} "
                  f"(разброс ±{разброс:.1f}%, выполнено раз: {раз})".replace(".", ","))
        прошлое = прошлые.get(имя)
        if isinstance(прошлое, list) and len(прошлое) == 2 and прошлое[0] > 0 and среднее > 0:
            изменение = (среднее - прошлое[0]) * 100 / прошлое[0]
            порог = max(2.0, 3 * max(разброс, float(прошлое[1])))
            было = f"в прошлый раз {время_по_русски(прошлое[0])}"
            if abs(изменение) <= порог:
                строка += f"; {было}, разница {abs(изменение):.1f}% в пределах разброса".replace(".", ",")
            elif изменение < 0:
                строка += f"; {было}: быстрее в {прошлое[0] / среднее:.2f} раза".replace(".", ",")
            else:
                строка += f"; {было}: медленнее в {среднее / прошлое[0]:.2f} раза".replace(".", ",")
        итог.append(строка)
    return итог, новые


def замерить(цели):
    """dan замерить [файл или папка ...]: собрать программу, замерить время её блоков «замер»
    и сравнить с прошлым замером. Без аргументов - все файлы .dan текущей папки, в которых есть замеры."""
    import json
    import tempfile
    файлы = []
    for цель in цели or ["."]:
        if os.path.isdir(цель):
            файлы += sorted(os.path.join(цель, и) for и in os.listdir(цель) if и.endswith(".dan"))
        else:
            файлы.append(цель)
    сбоев = нет_замеров = 0
    for путь in файлы:
        try:
            with open(путь, encoding="utf-8") as ф:
                исходник = ф.read()
        except (OSError, UnicodeDecodeError) as о:
            print(f"Не удалось открыть файл «{путь}»: {uzly.причина(о)}")
            сбоев += 1
            continue
        if not есть_замеры(исходник):
            нет_замеров += 1
            if цели and not os.path.isdir(цели[0]):
                print(f"{путь}: в программе нет замеров; замер пишется так: замер \"что замеряется\" {{ ... }}")
            continue
        print(f"== {путь}")
        sys.stdout.flush()
        try:
            исполняемый = собрать(путь, исходник, замеры=True, выпуск=True)
        except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки) as о:
            показать_ошибку(путь, исходник, о)
            сбоев += 1
            continue
        папка = tempfile.mkdtemp(prefix="dan_bench_")
        данные = os.path.join(папка, "замеры.txt")
        try:
            код = subprocess.run([os.path.abspath(исполняемый)], cwd=os.path.dirname(os.path.abspath(путь)),
                                 env=dict(os.environ, DAN_BENCH=данные)).returncode
            прошлые_путь = файл_прошлых_замеров(путь)
            try:
                with open(прошлые_путь, encoding="utf-8") as ф:
                    прошлые = json.load(ф)
            except (OSError, ValueError):
                прошлые = {}
            try:
                строки, новые = отчёт_замеров(данные, прошлые if isinstance(прошлые, dict) else {})
            except (OSError, ValueError, IndexError):
                строки, новые = ["  программа замеров была прервана"], {}
            for с in строки:
                print(с)
            if код != 0 or any(" ОШИБКА " in с for с in строки):
                сбоев += 1
            if новые:
                try:
                    os.makedirs(os.path.dirname(прошлые_путь), exist_ok=True)
                    with open(прошлые_путь, "w", encoding="utf-8") as ф:
                        json.dump({**(прошлые if isinstance(прошлые, dict) else {}), **новые}, ф, ensure_ascii=False)
                except OSError:
                    pass
        finally:
            shutil.rmtree(папка, ignore_errors=True)
        sys.stdout.flush()
    if нет_замеров == len(файлы):
        print("Замеров не найдено")
        return 1
    return 1 if сбоев else 0


def отчёт_покрытия(путь_данных, тексты, наибольше=40):
    """Отчёт о покрытии из файла, записанного программой тестов (переменная DAN_COVERAGE):
    сколько строк программы выполнено и какие не выполнялись. Строка считается выполненной,
    если выполнены все инструкции, записанные в ней."""
    with open(путь_данных, encoding="utf-8", errors="replace") as ф:
        записи = [с.split("\t") for с in ф.read().split("\n") if с]
    строки = {}
    for файл, строка, выполнена in записи:
        ключ = (файл, int(строка))
        строки[ключ] = строки.get(ключ, True) and выполнена == "1"
    if not строки:
        return "Покрытие: в программе нет строк, кроме тестов"
    порядок = {ф: н for н, ф in enumerate(dict.fromkeys(ф for ф, _ in строки))}
    пропущены = sorted((к for к, в in строки.items() if not в), key=lambda к: (порядок[к[0]], к[1]))
    всего = len(строки)
    выполнено = всего - len(пропущены)
    # доля в процентах с одним знаком после запятой, округлённая точно (целыми числами)
    десятые = (выполнено * 2000 + всего) // (2 * всего)
    доля = str(десятые // 10) + ("," + str(десятые % 10) if десятые % 10 else "")
    if not пропущены:
        return f"Покрытие: тесты выполнили все строки программы ({всего})"
    if выполнено % 10 == 1 and выполнено % 100 != 11:
        слово = "строку"
    elif 2 <= выполнено % 10 <= 4 and not 12 <= выполнено % 100 <= 14:
        слово = "строки"
    else:
        слово = "строк"
    итог = [f"Покрытие: тесты выполнили {выполнено} {слово} из {всего} ({доля}%). "
            "Не выполнены (целиком или частично):"]
    for файл, строка in пропущены[:наибольше]:
        текст = (тексты.get(файл) or "").split("\n")
        линия = текст[строка - 1].strip() if 0 < строка <= len(текст) else ""
        итог.append(f"  {файл}, строка {строка}: {линия}")
    if len(пропущены) > наибольше:
        итог.append(f"  ... и ещё строк: {len(пропущены) - наибольше}")
    return "\n".join(итог)


def отчёт_профиля(путь_данных, наибольше=30):
    """Таблица профиля из файла, записанного программой при выходе (переменная DAN_PROFILE)."""
    with open(путь_данных, encoding="utf-8", errors="replace") as ф:
        строки = ф.read().split("\n")
    всего = int(строки[0])
    записи = []
    for с in строки[1:]:
        части = с.split("\t")
        if len(части) == 6 and int(части[3]) > 0:
            имя, файл, строка, вызовов, полное, своё = части
            записи.append((int(своё), int(полное), int(вызовов), имя, файл, int(строка)))
    записи.sort(reverse=True)

    def время(нс):
        if нс >= 1_000_000_000:
            return f"{нс / 1e9:.2f} с".replace(".", ",")
        if нс >= 1_000_000:
            return f"{нс / 1e6:.1f} мс".replace(".", ",")
        return f"{нс / 1e3:.1f} мкс".replace(".", ",")

    итог = [f"Профиль: программа работала {время(всего)}. Своё время - без вызванных функций.",
            f"{'своё время':>18} {'всего':>10} {'вызовов':>12}  функция"]
    for своё, полное, вызовов, имя, файл, строка in записи[:наибольше]:
        доля = f"{своё * 100 / всего:.1f}%".replace(".", ",") if всего else ""
        место = f" ({os.path.basename(файл)}:{строка})" if строка else ""
        итог.append(f"{время(своё):>11} {доля:>6} {время(полное):>10} {вызовов:>12}  {имя}{место}")
    if len(записи) > наибольше:
        итог.append(f"... и ещё функций: {len(записи) - наибольше}")
    if всего and sum(з[0] for з in записи) > всего * 1.05:
        итог.append("Задачи выполнялись одновременно, поэтому сумма долей больше 100%.")
    return "\n".join(итог)


def профиль(аргументы):
    """dan профиль программа.dan [значения]: собрать с замером времени функций, выполнить, показать отчёт."""
    if not аргументы:
        print("Укажите программу: python dan.py профиль программа.dan [значения для программы]")
        return 2
    путь, для_программы = аргументы[0], аргументы[1:]
    try:
        with open(путь, encoding="utf-8") as ф:
            исходник = ф.read()
    except (OSError, UnicodeDecodeError) as о:
        print(f"Не удалось открыть файл «{путь}»: {uzly.причина(о)}")
        return 2
    try:
        исполняемый = собрать(путь, исходник, профиль=True)
    except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки) as о:
        показать_ошибку(путь, исходник, о)
        return 1
    import tempfile
    данные = os.path.join(tempfile.mkdtemp(prefix="dan_prof_"), "профиль.txt")
    sys.stdout.flush()
    код = subprocess.run([os.path.abspath(исполняемый), *для_программы],
                         env=dict(os.environ, DAN_PROFILE=данные)).returncode
    try:
        отчёт = отчёт_профиля(данные)
    except (OSError, ValueError, IndexError):
        print("Программа не записала профиль (она была прервана)", file=sys.stderr)
        return код or 1
    finally:
        shutil.rmtree(os.path.dirname(данные), ignore_errors=True)
    sys.stdout.flush()
    print("\n" + отчёт, file=sys.stderr)
    return код


def справка():
    """Описание команд; в установленном Дане команда называется просто «dan»."""
    текст = __doc__.strip()
    if getattr(sys, "frozen", False):
        текст = текст.replace("python dan.py", "dan")
    return текст


# Код C для особых сборок (dan си --тесты программа.dan): те же режимы, что у «тестировать», «отладка»,
# «профиль» и «замерить»; нужны для проверки компилятора на Дане тестами (sam_kompilyator.py).
РЕЖИМЫ_СИ = {"--отладка": "отладка", "--тесты": "тесты", "--профиль": "профиль", "--покрытие": "покрытие",
             "--замеры": "замеры"}


def main(аргументы):
    import sam_kompilyator
    if sam_kompilyator.включён():
        код = sam_kompilyator.выполнить_команду(аргументы)
        if код is not None:
            return код
    if аргументы == ["--версия"]:
        import versiya
        print(f"Дан {versiya.ВЕРСИЯ}; редакции языка: " +
              ", ".join(f"{г} - {о}" for г, о in versiya.РЕДАКЦИИ.items()))
        return 0
    if аргументы and аргументы[0] == "редакция" and len(аргументы) in (2, 3):
        return команда_редакция(аргументы[1], аргументы[2] if len(аргументы) == 3 else None)
    if аргументы and аргументы[0] == "пакеты":
        import paketi
        return paketi.команда(аргументы[1:])
    if len(аргументы) == 2 and аргументы[0] == "исправить":
        return исправить(аргументы[1])
    if аргументы == ["задачи"]:
        import zadachnik
        return zadachnik.команда_задачи()
    if len(аргументы) == 2 and аргументы[0] == "задача":
        import zadachnik
        return zadachnik.команда_задача(аргументы[1], собрать,
                                        (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки))
    if аргументы and аргументы[0] == "тестировать":
        return тестировать(аргументы[1:])
    if аргументы and аргументы[0] == "замерить":
        return замерить(аргументы[1:])
    if аргументы and аргументы[0] == "объяснить":
        import objasnenie
        return objasnenie.команда(аргументы[1:])
    if аргументы and аргументы[0] == "песочница":
        import pesochnica
        return pesochnica.команда(аргументы[1:], собрать,
                                  (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки))
    if аргументы and аргументы[0] == "обновить":
        import obnovlenie
        return obnovlenie.команда(аргументы[1:])
    if аргументы and аргументы[0] == "профиль":
        return профиль(аргументы[1:])
    if аргументы and аргументы[0] == "документация":
        import dokumentaciya
        return dokumentaciya.команда(аргументы[1:], собрать,
                                     (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки))
    if аргументы == ["сервер"]:
        import server
        server.main()
        return 0
    # параметры компилятора стоят до имени файла программы; после него при «запустить» - значения для программы
    для_программы = []
    if len(аргументы) >= 2 and аргументы[0] in ("запустить", "отладка"):
        номер = next((н for н, а in enumerate(аргументы[1:], 1) if not а.startswith("--")), None)
        if номер is not None:
            для_программы = аргументы[номер + 1:]
            аргументы = аргументы[:номер + 1]
    система = None
    for а in list(аргументы):
        if а.startswith("--для="):
            система = а[len("--для="):]
            аргументы.remove(а)
    параметры = {а for а in аргументы if а.startswith("--")}
    аргументы = [а for а in аргументы if not а.startswith("--")]
    неизвестные = параметры - {"--малый", "--сжать", "--проверить", "--веб", "--json", "--строки-дан"}
    if аргументы and аргументы[0] == "си":
        неизвестные -= set(РЕЖИМЫ_СИ)
    if len(аргументы) != 2 or аргументы[0] not in КОМАНДЫ or неизвестные:
        if неизвестные:
            print(f"Неизвестный параметр: {', '.join(sorted(неизвестные))}\n")
        print(справка())
        return 2
    команда, путь = аргументы
    малый, сжать = "--малый" in параметры, "--сжать" in параметры
    try:
        with open(путь, encoding="utf-8") as ф:
            исходник = ф.read()
    except OSError as о:
        print(f"Не удалось открыть файл «{путь}»: {uzly.причина(о)}")
        return 2
    except UnicodeDecodeError:
        print(f"Файл «{путь}» должен быть в кодировке UTF-8")
        return 2

    try:
        if команда == "формат":
            return форматировать_файл(путь, исходник, "--проверить" in параметры)
        if команда == "токены":
            for т in токены(исходник):
                print(т)
        elif команда == "дерево":
            print(uzly.показать(загрузить(исходник, путь)))
        elif команда == "проверить" and "--json" in параметры:
            import kody
            найдено = замечания(путь, исходник)
            print(kody.запись_json(путь, найдено))
            return 1 if any(з[0] == "ошибка" for з in найдено) else 0
        elif команда == "проверить":
            программа = загрузить(исходник, путь)
            проверить(программа)
            показать_предупреждения(программа, путь)
            print(f"{путь}: ошибок не найдено")
        elif команда == "си":
            режимы = {РЕЖИМЫ_СИ[п]: True for п in параметры if п in РЕЖИМЫ_СИ}
            if режимы.get("покрытие"):
                режимы["тесты"] = True
            print(перевести(исходник, путь, строки_дан="--строки-дан" in параметры, **режимы), end="")
        elif команда == "отладка":
            исполняемый = собрать(путь, исходник, отладка=True)
            sys.stdout.flush()
            return subprocess.run([os.path.abspath(исполняемый), *для_программы]).returncode
        elif команда == "собрать" and система:
            исполняемый = собрать_для(путь, исходник, система, малый)
            print(f"Готово: {исполняемый} ({os.path.getsize(исполняемый)} байт) для системы {система}")
        elif команда in ("собрать", "запустить") and "--веб" in параметры:
            путь_wasm, путь_html = собрать_веб(путь, исходник, малый)
            if команда == "собрать":
                print(f"Готово: {путь_wasm} ({os.path.getsize(путь_wasm)} байт) и страница {путь_html} "
                      f"(открывается в браузере)")
            else:
                return выполнить_wasm(путь_wasm, для_программы)
        elif команда == "собрать":
            исполняемый = собрать(путь, исходник, малый, сжать, выпуск=True)
            print(f"Готово: {исполняемый} ({os.path.getsize(исполняемый)} байт)")
        else:
            исполняемый = собрать(путь, исходник, малый, сжать)
            sys.stdout.flush()
            return subprocess.run([os.path.abspath(исполняемый), *для_программы]).returncode
    except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки, ОшибкаСборки) as о:
        показать_ошибку(путь, исходник, о)
        return 1
    return 0


def _без_места(текст):
    """Текст сообщения без «Строка N, позиция M: » в начале."""
    if текст.startswith("Строка ") and ": " in текст:
        return текст.split(": ", 1)[1]
    return текст


def _разобрать_ошибку_модуля(текст):
    """(файл, строка, столбец, текст, фрагмент) из сообщения об ошибке подключённого файла:
    «файл: Строка N, позиция M: текст», «файл: текст» или «файл: ошибка в записи программы» и
    сообщение следующей строкой; после них - фрагмент со строкой программы."""
    строки = текст.split("\n")
    голова = строки[0]
    н = голова.find(": ")
    файл, остаток, следующая = (голова[:н], голова[н + 2:], 1) if н >= 0 else ("", голова, 1)
    if остаток == "ошибка в записи программы" and len(строки) > 1:
        остаток, следующая = строки[1].strip(), 2
    строка = столбец = 0
    if остаток.startswith("Строка ") and ": " in остаток:
        место_ = остаток.split(": ", 1)[0][len("Строка "):]
        if ", позиция " in место_:
            а, б = место_.split(", позиция ", 1)
            if а.isdigit() and б.isdigit():
                строка, столбец = int(а), int(б)
                остаток = _без_места(остаток)
    return файл, строка, столбец, остаток, "\n".join(строки[следующая:])


def замечания(путь, исходник):
    """Ошибки или (если ошибок нет) предупреждения программы для машинного вывода:
    [(вид, файл, строка, столбец, текст, фрагмент)]."""
    try:
        программа = загрузить(исходник, путь)
        проверить(программа)
    except (ОшибкаЛексера, ОшибкаРазбора) as о:
        return [("ошибка", путь, о.строка, о.столбец, _без_места(str(о)), uzly.фрагмент(исходник, о.строка, о.столбец))]
    except ОшибкаМодуля as о:
        return [("ошибка", *_разобрать_ошибку_модуля(str(о)))]
    except ОшибкаПроверки as о:
        return [("ошибка", uzly.место_в_файле(с.строка)[0] or путь, uzly.место_в_файле(с.строка)[1], с.столбец, с.текст,
                 uzly.фрагмент_места(с.строка, с.столбец)) for с in о.ошибки]
    return [("предупреждение", uzly.место_в_файле(п.строка)[0] or путь, uzly.место_в_файле(п.строка)[1], п.столбец,
             п.текст, uzly.фрагмент_места(п.строка, п.столбец)) for п in getattr(программа, "предупреждения", [])]


def применить_исправления(путь, исходник, найдено):
    """(новый текст, [(строка, было, стало)]): готовые исправления сообщений об этом файле, начиная с конца;
    исправления, которые задевают уже исправленное место, откладываются до следующего прохода."""
    import kody
    правки = []
    for вид, файл, строка, столбец, текст, фрагмент in найдено:
        позиция, длина = kody.место(фрагмент, столбец)
        замена = kody.исправление(текст) if длина > 0 else None
        if файл == путь and замена is not None:
            правки.append((строка, позиция, длина, замена))
    строки = исходник.split("\n")
    сделано = []
    занято = {}
    for строка, позиция, длина, замена in sorted(правки, reverse=True):
        с = строки[строка - 1]
        сдвиг = 1 if строка == 1 and с.startswith("\ufeff") else 0
        начало = позиция - 1 + сдвиг
        было = с[начало:начало + длина]
        if было == замена or any(начало < к and н < начало + длина for н, к in занято.get(строка, [])):
            continue
        строки[строка - 1] = с[:начало] + замена + с[начало + длина:]
        занято.setdefault(строка, []).append((начало, начало + длина))
        сделано.append((строка, было, замена))
    return "\n".join(строки), сделано


def исправить(путь):
    """dan исправить файл.dan: применить все готовые исправления из сообщений компилятора."""
    try:
        with open(путь, encoding="utf-8") as ф:
            исходник = ф.read()
    except OSError as о:
        print(f"Не удалось открыть файл «{путь}»: {uzly.причина(о)}")
        return 2
    except UnicodeDecodeError:
        print(f"Файл «{путь}» должен быть в кодировке UTF-8")
        return 2
    if исходник.startswith("\ufeff"):
        исходник = исходник[1:]     # исправленный файл записывается без метки порядка байтов, как в Дане
    текст = исходник
    всего = 0
    for _ in range(20):
        найдено = замечания(путь, текст)
        новый, сделано = применить_исправления(путь, текст, найдено)
        if not сделано:
            break
        for строка, было, стало in sorted(сделано):
            print(f"  строка {строка}: «{было}» -> «{стало}»")
        всего += len(сделано)
        текст = новый
    else:
        найдено = замечания(путь, текст)
    if текст != исходник:
        with open(путь, "w", encoding="utf-8", newline="") as ф:
            ф.write(текст)
    print(f"{путь}: исправлено мест: {всего}" if всего else f"{путь}: готовых исправлений нет")
    ошибок = sum(1 for з in найдено if з[0] == "ошибка")
    if ошибок:
        print(f"{путь}: осталось ошибок: {ошибок}; подробности - команда «проверить»")
        return 1
    print(f"{путь}: ошибок не найдено")
    return 0


def показать_ошибку(путь, исходник, о):
    """Выводит ошибку записи, подключения, проверки или сборки программы со строкой программы
    и указателем на место ошибки."""
    if isinstance(о, (ОшибкаЛексера, ОшибкаРазбора)):
        print(f"{путь}: ошибка в записи программы")
        print(f"  {о}")
        фрагмент = uzly.фрагмент(исходник, о.строка, о.столбец)
        if фрагмент:
            print(фрагмент)
    elif isinstance(о, ОшибкаПроверки):
        print(f"{путь}: найдено ошибок: {len(о.ошибки)}")
        for сообщение in о.ошибки:
            print(f"  {сообщение}")
            фрагмент = uzly.фрагмент_места(сообщение.строка, сообщение.столбец)
            if фрагмент:
                print(фрагмент)
    elif isinstance(о, ОшибкаСборки):
        print(f"Сборка не удалась: {о}")
    else:
        print(о)


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
