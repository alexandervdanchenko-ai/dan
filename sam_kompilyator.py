"""Проверка тестами компилятора на Дане (папка «сам») вместо компилятора на Python.

После заморозки компилятора на Python новые возможности языка появляются только в компиляторе
на Дане, поэтому тот же набор тестов должен уметь проверять их через него. Включается переменной
среды:

    DAN_KOMPILYATOR=сам python testy.py

В этом режиме:
- dan.перевести (а с ним dan.собрать и всё, что собирает программы) получает код C от команды
  «дан си» компилятора на Дане; ошибки разбора, подключения и проверки превращаются в те же
  исключения (ОшибкаРазбора, ОшибкаМодуля, ОшибкаПроверки) с теми же текстами;
- «python dan.py <команда>» выполняет команду компилятором на Дане, если она в нём есть
  (иначе - на Python, но перевод программы всё равно делает компилятор на Дане);
- тесты, которые по смыслу проверяют сам компилятор на Python (его лексер, дерево, типы узлов),
  пропускаются: они помечены декоратором «про_компилятор_на_python».

Компилятор на Дане собирается один раз и хранится в кэше (папка кэша Дана, «сам/<отпечаток>»);
отпечаток - по файлам папок «сам» и «sreda» и файлам компилятора на Python, которым он собирается.
Готовый исполняемый файл можно указать переменной DAN_SAM_EXE. Код C компилятора на Дане даёт компилятор
на Python, а если не может (или DAN_SAM_IZ_SEMENI=1) - компилятор, собранный из семени semya/dan.c.

Поставка для Windows до версии 1.8 (dan.exe, собранный PyInstaller): рядом с dan.exe лежал готовый
компилятор на Дане «компилятор/дан.exe», собранный из семени, и этот режим в ней был включён всегда
(без DAN_KOMPILYATOR): все команды выполнял компилятор на Дане. С версии 1.8 поставка собирается без
PyInstaller: dan.exe - сам компилятор на Дане (поставка/собрать.py), и этот режим ей не нужен; код
режима остаётся для поставки, собранной прежним способом. DAN_KOMPILYATOR=python возвращает прежний
компилятор на Python целиком. Компилятор C для компилятора на Дане - из DAN_COMPILER, gcc, clang или cc в системе,
иначе zig из поставки (через обёртку zig/zig-cc.exe, поставка/zig_cc.c).
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))
САМ = os.path.join(ПАПКА, "сам")

# Команды «python dan.py ...», которых нет в компиляторе на Дане: они выполняются на Python. С версии 1.8
# компилятор на Дане выполняет все команды (песочницу, сервер языка, обновление, --веб, --для=, --сжать).
ТОЛЬКО_PYTHON = set()
ПАРАМЕТРЫ_ТОЛЬКО_PYTHON = ()
# Команды компилятора на Дане (сам/дан.dan, функция «главная»).
КОМАНДЫ_САМ = {"--версия", "проверить", "исправить", "си", "собрать", "запустить", "формат", "токены", "дерево",
               "отладка", "тестировать", "замерить", "профиль", "объяснить", "документация", "редакция", "пакеты",
               "задачи", "задача", "модели", "песочница", "обновить", "сервер",
               "mcp", "новый"}


def _dan():
    """Модуль dan.py. В поставке (PyInstaller) dan.py выполняется как __main__, и «import dan» загрузил бы
    его второй раз с другими классами ошибок: тогда берётся __main__."""
    главный = sys.modules.get("__main__")
    if getattr(главный, "ОшибкаСборки", None) is not None and hasattr(главный, "справка"):
        return главный
    import dan
    return dan


def папка_поставки():
    """Папка с dan.exe, если это собранная поставка (PyInstaller), иначе None."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return None


def компилятор_поставки():
    """Готовый компилятор на Дане из поставки или None."""
    папка = папка_поставки()
    if папка is None:
        return None
    путь = os.path.join(папка, "компилятор", "дан" + (".exe" if os.name == "nt" else ""))
    return путь if os.path.isfile(путь) else None


def в_поставке():
    """Собранная поставка с компилятором на Дане (и он не отключён DAN_KOMPILYATOR=python)."""
    return os.environ.get("DAN_KOMPILYATOR", "") != "python" and компилятор_поставки() is not None


def включён():
    return os.environ.get("DAN_KOMPILYATOR", "") == "сам" or в_поставке()


про_компилятор_на_python = unittest.skipIf(
    включён(), "проверяет устройство компилятора на Python (DAN_KOMPILYATOR=сам)")


# Модули библиотеки, появившиеся после заморозки компилятора на Python: в них можно пользоваться
# возможностями версий 1.5 и новее (плотные массивы, «параллельно для»), которых замороженный
# компилятор не знает. Программу, которая их подключает, переводит и проверяет только компилятор
# на Дане; остальные программы по-прежнему сверяются с компилятором на Python.
МОДУЛИ_ПОСЛЕ_ЗАМОРОЗКИ = ("модели", "нейро")
_ПОДКЛЮЧЕНИЕ_ПОСЛЕ_ЗАМОРОЗКИ = re.compile(
    r'^[ \t]*подключить[ \t]+"(?:' + "|".join(МОДУЛИ_ПОСЛЕ_ЗАМОРОЗКИ) + r')"', re.MULTILINE)


def требует_компилятор_на_дане(текст):
    """Программа подключает модуль, написанный после заморозки компилятора на Python
    (МОДУЛИ_ПОСЛЕ_ЗАМОРОЗКИ): её переводит только компилятор на Дане."""
    return _ПОДКЛЮЧЕНИЕ_ПОСЛЕ_ЗАМОРОЗКИ.search(текст) is not None


class _БезДерева:
    """Дерево программы есть только у компилятора на Python: тест, которому оно нужно, пропускается."""

    def __getattr__(self, имя):
        raise unittest.SkipTest("дерево программы и типы узлов есть только у компилятора на Python")


БЕЗ_ДЕРЕВА = _БезДерева()


def отпечаток():
    о = hashlib.sha256()
    файлы = []
    for корень in (САМ, os.path.join(ПАПКА, "sreda")):
        for папка, _, имена in os.walk(корень):
            файлы += [os.path.join(папка, и) for и in имена if not и.endswith((".o", ".exe", ".c~"))]
    # файлы на Python, которыми компилятор на Дане собирается (тесты и этот файл его не меняют)
    файлы += [os.path.join(ПАПКА, и) for и in os.listdir(ПАПКА)
              if и.endswith(".py") and not и.startswith(("test_", "testy", "sam_kompilyator"))]
    for путь in sorted(файлы):
        о.update(os.path.relpath(путь, ПАПКА).encode("utf-8"))
        with open(путь, "rb") as ф:
            о.update(hashlib.sha256(ф.read()).digest())
    return о.hexdigest()[:20]


_ИСПОЛНЯЕМЫЙ = []
# Папка кэша на момент запуска: тесты подменяют DAN_CACHE временной папкой, а компилятор на Дане
# должен собираться один раз на прогон, а не в каждой такой папке.
_DAN_CACHE = os.environ.get("DAN_CACHE")


def папка_кэша():
    if _DAN_CACHE:
        return _DAN_CACHE
    if os.name == "nt":
        основа = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(основа, "Dan", "cache")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "dan")


def исполняемый():
    """Путь к собранному компилятору на Дане (собирается при первом обращении)."""
    if os.environ.get("DAN_SAM_EXE"):
        return os.environ["DAN_SAM_EXE"]
    if в_поставке():
        return компилятор_поставки()
    if _ИСПОЛНЯЕМЫЙ:
        return _ИСПОЛНЯЕМЫЙ[0]
    import dan
    папка = os.path.join(папка_кэша(), "сам", отпечаток())
    путь = os.path.join(папка, "дан" + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(путь):
        os.makedirs(папка, exist_ok=True)
        замок = os.path.join(папка, "сборка")
        начало = time.time()
        свой_замок = False
        while True:
            try:
                os.mkdir(замок)
                свой_замок = True
                break
            except FileExistsError:
                # другой процесс тестов уже собирает компилятор
                if os.path.isfile(путь):
                    break
                if time.time() - начало > 1800:
                    raise RuntimeError("компилятор на Дане не собрался за 30 минут: " + замок)
                time.sleep(1)
        try:
            if not os.path.isfile(путь):
                with tempfile.TemporaryDirectory(prefix="dan_sam_") as временная:
                    готовый = dan.собрать_код(код_компилятора(временная), os.path.join(временная, "дан"), САМ)
                    # кэш может быть на другом диске, чем временная папка (Windows): os.replace туда
                    # не переносит; копия рядом с целью, затем замена - на одном диске
                    копия = путь + f".{os.getpid()}.tmp"
                    shutil.copyfile(готовый, копия)
                    shutil.copymode(готовый, копия)
                    os.replace(копия, путь)
        finally:
            # замок снимает только тот процесс, который его поставил: иначе процесс, дождавшийся
            # чужой сборки, снимал замок третьего процесса, и тот падал на os.rmdir
            if свой_замок:
                try:
                    os.rmdir(замок)
                except FileNotFoundError:
                    pass
    _ИСПОЛНЯЕМЫЙ.append(путь)
    return путь


def код_компилятора(временная):
    """Код C компилятора на Дане из текущих файлов папки «сам». Переводит его компилятор на Python, а если
    не может (после заморозки компилятор на Дане вправе пользоваться новыми возможностями языка) или
    задано DAN_SAM_IZ_SEMENI=1 - компилятор, собранный из семени semya/dan.c."""
    import dan
    исходный = os.path.join(САМ, "дан.dan")
    if os.environ.get("DAN_SAM_IZ_SEMENI") != "1":
        try:
            with open(исходный, encoding="utf-8") as ф:
                return dan.перевести_python(ф.read(), исходный)
        except Exception:
            pass
    with open(os.path.join(ПАПКА, "semya", "dan.c"), encoding="utf-8") as ф:
        семя = dan.собрать_код(ф.read(), os.path.join(временная, "семя"), САМ)
    р = subprocess.run([семя, "си", исходный], capture_output=True, env=среда(), timeout=900)
    if р.returncode != 0:
        raise RuntimeError("компилятор из семени не перевёл сам/дан.dan:\n"
                           + р.stdout.decode("utf-8", "replace") + р.stderr.decode("utf-8", "replace"))
    return р.stdout.decode("utf-8").replace("\r\n", "\n")


def среда():
    с = dict(os.environ, DAN_HOME=ПАПКА)
    папка = папка_поставки()
    if в_поставке():
        # папки sreda, библиотека и учебник/задачи - внутри поставки (PyInstaller кладёт их в sys._MEIPASS)
        с["DAN_HOME"] = getattr(sys, "_MEIPASS", ПАПКА)
        # llama.dll лежит рядом с dan.exe: программы и «dan модели проверить» находят её через PATH
        имя_path = next((к for к in с if к.upper() == "PATH"), "PATH")
        с[имя_path] = папка + os.pathsep + с.get(имя_path, "")
        if not с.get("DAN_COMPILER", "").strip() and not any(shutil.which(и) for и in ("gcc", "clang", "cc")):
            обёртка = os.path.join(папка, "zig", "zig-cc.exe" if os.name == "nt" else "zig-cc")
            if os.path.isfile(обёртка):
                с["DAN_COMPILER"] = обёртка
    return с


ПОЯСНЕНИЕ_ПОСТАВКИ = "Команды выполняет компилятор на Дане (DAN_KOMPILYATOR=python - всё на Python)."


def _до_файла(аргументы):
    """Аргументы команды без значений для программы (после имени файла в «запустить» и «отладка»)."""
    if аргументы[0] in ("запустить", "отладка"):
        for н, а in enumerate(аргументы[1:], 1):
            if not а.startswith("--"):
                return аргументы[:н + 1]
    return аргументы


def _выполнить_в_поставке(аргументы):
    """Команда в поставке: компилятором на Дане, справка - здесь, остальное (None) - на Python."""
    if not аргументы or (аргументы[0] not in КОМАНДЫ_САМ and аргументы[0] not in ТОЛЬКО_PYTHON):
        print(_dan().справка())
        print("\nКоманды для модуля «модели» (библиотека llama.cpp):\n"
              "    dan модели установить         - скачать библиотеку llama.cpp (в поставке для Windows она уже есть)\n"
              "    dan модели проверить          - найти библиотеку llama.cpp, загрузить её и показать версию\n\n"
              + ПОЯСНЕНИЕ_ПОСТАВКИ)
        return 2
    if аргументы[0] in ТОЛЬКО_PYTHON or (ПАРАМЕТРЫ_ТОЛЬКО_PYTHON and any(
            а.startswith(ПАРАМЕТРЫ_ТОЛЬКО_PYTHON) for а in _до_файла(аргументы))):
        return None
    код = _запустить(аргументы)
    if аргументы == ["--версия"] and код == 0:
        print(ПОЯСНЕНИЕ_ПОСТАВКИ)
    return код


def выполнить_команду(аргументы):
    """«python dan.py <аргументы>» компилятором на Дане или None, если команды в нём нет."""
    if в_поставке() and os.environ.get("DAN_KOMPILYATOR", "") != "сам":
        return _выполнить_в_поставке(аргументы)
    if not аргументы or аргументы[0] in ТОЛЬКО_PYTHON or (ПАРАМЕТРЫ_ТОЛЬКО_PYTHON and any(
            а.startswith(ПАРАМЕТРЫ_ТОЛЬКО_PYTHON) for а in аргументы)):
        return None
    return _запустить(аргументы)


def _запустить(аргументы):
    sys.stdout.flush()
    sys.stderr.flush()
    if sys.stdout is sys.__stdout__ and sys.stderr is sys.__stderr__:
        return subprocess.run([исполняемый(), *аргументы], env=среда()).returncode
    # вывод перенаправлен (тесты вызывают dan.main внутри процесса): передаём его через sys.stdout
    р = subprocess.run([исполняемый(), *аргументы], env=среда(), capture_output=True)
    print(р.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), end="")
    print(р.stderr.decode("utf-8", "replace").replace("\r\n", "\n"), end="", file=sys.stderr)
    return р.returncode


class ОшибкаЗаписи(Exception):
    """Ошибка записи программы (лексер или разбор): компилятор на Дане их не различает."""


class СообщениеДана:
    """Сообщение проверки: те же поля и тот же текст, что у proverka.Сообщение."""

    def __init__(self, строка_вывода):
        self.показ = строка_вывода
        м = re.match(r"(?:Файл .*?, строка|Строка) (\d+), позиция (\d+): (.*)$", строка_вывода, re.S)
        self.строка, self.столбец, self.текст = (int(м[1]), int(м[2]), м[3]) if м else (0, 0, строка_вывода)

    def __str__(self):
        return self.показ

    def __repr__(self):
        return f"СообщениеДана({self.показ!r})"


def _фрагмент(строка):
    return re.match(r"^ +(\d+ )?\|", строка) is not None


def исключение(вывод_, путь):
    """Исключение, равное тому, что выбросил бы компилятор на Python, по выводу «дан си»."""
    from lexer import ОшибкаЛексера
    from parser import ОшибкаРазбора
    from proverka import ОшибкаПроверки
    from moduli import ОшибкаМодуля
    строки = вывод_.rstrip("\n").split("\n")
    голова = строки[0]
    if голова == f"{путь}: ошибка в записи программы" and len(строки) > 1:
        класс = type("ОшибкаЗаписиДана", (ОшибкаЗаписи, ОшибкаЛексера, ОшибкаРазбора), {})
        о = класс.__new__(класс)
        текст = строки[1].strip()
        Exception.__init__(о, текст)
        м = re.match(r"Строка (\d+), позиция (\d+): ", текст)
        о.строка, о.столбец = (int(м[1]), int(м[2])) if м else (0, 0)
        return о
    if голова.startswith(f"{путь}: найдено ошибок: "):
        сообщения = []
        for с in строки[1:]:
            if с.startswith("  ") and not _фрагмент(с):
                сообщения.append(СообщениеДана(с[2:]))
            elif сообщения and not _фрагмент(с) and с:
                # продолжение многострочного сообщения
                сообщения[-1] = СообщениеДана(str(сообщения[-1]) + "\n" + с)
        return ОшибкаПроверки(сообщения)
    if голова.startswith("Сборка не удалась: "):
        return _dan().ОшибкаСборки(вывод_.rstrip("\n")[len("Сборка не удалась: "):])
    н = next((н for н, с in enumerate(строки) if _фрагмент(с)), len(строки))
    return ОшибкаМодуля("\n".join(строки[:н]), "\n".join(строки[н:]))


def перевести(исходник, путь=None, отладка=False, тесты=False, профиль=False, покрытие=False, замеры=False,
              строки_дан=False):
    """То же, что dan.перевести, но код C создаёт компилятор на Дане."""
    исходник = исходник.replace("\r\n", "\n")
    временные = []
    файл = путь
    показ = путь or "программа"
    if путь and os.path.isfile(путь):
        with open(путь, encoding="utf-8") as ф:
            if ф.read() != исходник:
                файл = None
    elif путь and os.path.isdir(os.path.dirname(os.path.abspath(путь))):
        файл = путь
        временные.append(путь)
    else:
        файл = None
    if файл is None:
        # имя файла попадает в код C (имена файлов для сообщений): временный файл называется так же
        папка = tempfile.mkdtemp(prefix="dan_sam_")
        временные.append(папка)
        файл = os.path.join(папка, os.path.basename(путь) if путь else "программа.dan")
        временные.insert(0, файл)
    if файл in временные:
        with open(файл, "w", encoding="utf-8", newline="") as ф:
            ф.write(исходник)
    параметры = [п for п, нужен in (("--отладка", отладка), ("--тесты", тесты and not покрытие),
                                     ("--профиль", профиль), ("--покрытие", покрытие), ("--замеры", замеры),
                                     ("--строки-дан", строки_дан)) if нужен]
    try:
        р = subprocess.run([исполняемый(), "си", *параметры, файл], capture_output=True, env=среда(), timeout=900)
    finally:
        for в in временные:
            if os.path.isdir(в):
                import shutil
                shutil.rmtree(в, ignore_errors=True)
            elif os.path.exists(в):
                os.remove(в)
    вывод_ = р.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    ошибки = р.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
    if файл != путь:
        ошибки = ошибки.replace(файл, показ)
    if ошибки:
        print(ошибки, end="", file=sys.stderr)
    if р.returncode == 0:
        return вывод_
    if р.returncode == 1:
        raise исключение(вывод_, файл)
    raise RuntimeError(f"компилятор на Дане завершился с кодом {р.returncode}:\n{вывод_}{ошибки}")
