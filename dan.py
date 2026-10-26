"""Единая точка запуска инструментов языка Дан.

Использование:
    python dan.py запустить программа.dan   - собрать программу и сразу выполнить
    python dan.py собрать программа.dan     - собрать исполняемый файл
    python dan.py проверить программа.dan   - найти ошибки в программе
    python dan.py си программа.dan          - показать полученный код на C
    python dan.py дерево программа.dan      - показать дерево программы
    python dan.py токены программа.dan      - показать токены
    python dan.py формат программа.dan      - привести текст программы к единому виду
                                              (с --проверить - только проверить)
    python dan.py отладка программа.dan     - выполнить программу по шагам в отладчике
    python dan.py сервер                    - сервер языка для редакторов (протокол LSP)
    python dan.py пакеты добавить имя адрес [версия] - добавить пакет (репозиторий git) в проект;
                  также: пакеты установить, обновить [имя], удалить имя, список, проверить
    python dan.py редакция программа.dan    - показать, для какой редакции языка написана программа
    python dan.py редакция программа.dan 2026 - перевести программу в редакцию и указать её в файле
    python dan.py --версия                  - версия компилятора и известные ему редакции языка
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
ФЛАГИ_ОБЩИЕ = ["-std=c11", "-fwrapv", "-w", "-s", "-ffunction-sections", "-fdata-sections",
               УДАЛИТЬ_НЕНУЖНОЕ, "-fno-asynchronous-unwind-tables", "-fno-ident"]
ФЛАГИ_СКОРОСТЬ = ["-O2"]
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


def перевести(исходник, путь=None, отладка=False):
    """Текст программы на Дан -> текст на C. Выбрасывает ошибки разбора и проверки.
    Путь нужен, чтобы найти подключённые файлы (подключить "файл.dan")."""
    программа = загрузить(исходник, путь)
    проверить(программа)
    показать_предупреждения(программа, путь)
    return сгенерировать(программа, отладка)


def показать_предупреждения(программа, путь):
    """Предупреждения не останавливают сборку и выводятся в поток ошибок."""
    for п in getattr(программа, "предупреждения", []):
        print(f"{путь or 'программа'}: предупреждение: {п}", file=sys.stderr)


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
    threading.stack_size(256 * 1024 * 1024)
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


def _скомпилировать(команда, путь_c, путь_exe, после):
    """Запускает компилятор C: команда + [путь_c, "-o", путь_exe] + после.
    Компилятор MinGW в Windows передаёт имена файлов в кодировке системы и не
    может записать файл с русскими буквами в имени, если система не русская.
    Поэтому в Windows такие файлы собираются во временной папке с латинским
    именем и переносятся на место."""
    if os.name != "nt" or (путь_c + путь_exe).isascii():
        return subprocess.run([*команда, путь_c, "-o", путь_exe, *после],
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
        return subprocess.run([*команда, путь_c, "-o", путь_exe, *после],
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
    настоящая, короткая = папка
    try:
        shutil.copyfile(путь_c, os.path.join(настоящая, "p.c"))
        итог = subprocess.run([*команда, os.path.join(короткая, "p.c"), "-o", os.path.join(короткая, "p.exe"), *после],
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
    флаги = [ф for ф in ФЛАГИ_ОБЩИЕ if ф != УДАЛИТЬ_НЕНУЖНОЕ] + (ФЛАГИ_РАЗМЕР if малый else ФЛАГИ_СКОРОСТЬ)
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


def собрать(путь, исходник, малый=False, сжать=False, отладка=False):
    """Собирает исполняемый файл рядом с исходным. Возвращает путь к нему."""
    код_c = перевести(исходник, путь, отладка)
    основа = os.path.splitext(путь)[0] + ("_отладка" if отладка else "")
    путь_c = основа + ".c"
    путь_exe = основа + (".exe" if os.name == "nt" else "")
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(код_c)
    команда = команда_компилятора()
    флаги = ФЛАГИ_ОБЩИЕ + (ФЛАГИ_РАЗМЕР if малый else ФЛАГИ_СКОРОСТЬ)
    if отладка:
        флаги = [ф for ф in ФЛАГИ_ОБЩИЕ if ф != "-s"] + ["-O0", "-g"]
    библиотеки = библиотеки_программы(код_c, os.path.dirname(os.path.abspath(путь)), команда)
    результат = _скомпилировать([*команда, *флаги], путь_c, путь_exe, библиотеки)
    if результат.returncode != 0:
        raise ОшибкаСборки("компилятор C сообщил об ошибке. Это ошибка самого компилятора Дан, "
                           "а не вашей программы; сохраните файл " + путь_c +
                           " для разбора.\n" + результат.stderr.strip())
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


def справка():
    """Описание команд; в установленном Дане команда называется просто «dan»."""
    текст = __doc__.strip()
    if getattr(sys, "frozen", False):
        текст = текст.replace("python dan.py", "dan")
    return текст


def main(аргументы):
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
    if аргументы == ["задачи"]:
        import zadachnik
        return zadachnik.команда_задачи()
    if len(аргументы) == 2 and аргументы[0] == "задача":
        import zadachnik
        return zadachnik.команда_задача(аргументы[1], собрать,
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
    неизвестные = параметры - {"--малый", "--сжать", "--проверить", "--веб"}
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
        print(f"Не удалось открыть файл «{путь}»: {о.strerror}")
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
        elif команда == "проверить":
            программа = загрузить(исходник, путь)
            проверить(программа)
            показать_предупреждения(программа, путь)
            print(f"{путь}: ошибок не найдено")
        elif команда == "си":
            print(перевести(исходник, путь), end="")
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
            исполняемый = собрать(путь, исходник, малый, сжать)
            print(f"Готово: {исполняемый} ({os.path.getsize(исполняемый)} байт)")
        else:
            исполняемый = собрать(путь, исходник, малый, сжать)
            sys.stdout.flush()
            return subprocess.run([os.path.abspath(исполняемый), *для_программы]).returncode
    except (ОшибкаЛексера, ОшибкаРазбора) as о:
        print(f"{путь}: ошибка в записи программы")
        print(f"  {о}")
        return 1
    except ОшибкаМодуля as о:
        print(о)
        return 1
    except ОшибкаПроверки as о:
        print(f"{путь}: найдено ошибок: {len(о.ошибки)}")
        for сообщение in о.ошибки:
            print(f"  {сообщение}")
        return 1
    except ОшибкаСборки as о:
        print(f"Сборка не удалась: {о}")
        return 1
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
