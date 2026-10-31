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
Готовый исполняемый файл можно указать переменной DAN_SAM_EXE (например, собранный из семени).
"""

import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))
САМ = os.path.join(ПАПКА, "сам")

# Команды «python dan.py ...», которых нет в компиляторе на Дане: они выполняются на Python.
ТОЛЬКО_PYTHON = {"песочница", "обновить", "сервер"}
ПАРАМЕТРЫ_ТОЛЬКО_PYTHON = ("--для=", "--веб", "--сжать")


def включён():
    return os.environ.get("DAN_KOMPILYATOR", "") == "сам"


про_компилятор_на_python = unittest.skipIf(
    включён(), "проверяет устройство компилятора на Python (DAN_KOMPILYATOR=сам)")


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
    if _ИСПОЛНЯЕМЫЙ:
        return _ИСПОЛНЯЕМЫЙ[0]
    import dan
    папка = os.path.join(папка_кэша(), "сам", отпечаток())
    путь = os.path.join(папка, "дан" + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(путь):
        os.makedirs(папка, exist_ok=True)
        замок = os.path.join(папка, "сборка")
        начало = time.time()
        while True:
            try:
                os.mkdir(замок)
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
                исходный = os.path.join(САМ, "дан.dan")
                with open(исходный, encoding="utf-8") as ф:
                    код = dan.перевести_python(ф.read(), исходный)
                with tempfile.TemporaryDirectory(prefix="dan_sam_") as временная:
                    готовый = dan.собрать_код(код, os.path.join(временная, "дан"), САМ)
                    os.replace(готовый, путь)
        finally:
            if os.path.isdir(замок):
                os.rmdir(замок)
    _ИСПОЛНЯЕМЫЙ.append(путь)
    return путь


def среда():
    return dict(os.environ, DAN_HOME=ПАПКА)


def выполнить_команду(аргументы):
    """«python dan.py <аргументы>» компилятором на Дане или None, если команды в нём нет."""
    if not аргументы or аргументы[0] in ТОЛЬКО_PYTHON or any(
            а.startswith(ПАРАМЕТРЫ_ТОЛЬКО_PYTHON) for а in аргументы):
        return None
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
        import dan
        return dan.ОшибкаСборки(вывод_.rstrip("\n")[len("Сборка не удалась: "):])
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
