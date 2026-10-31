"""Быстрый запуск тестов Дана: параллельно и только нужные.

    python testy.py                      все тесты в несколько процессов (по числу ядер)
    python testy.py --потоков=4          число процессов
    python testy.py --изменения          только тесты, связанные с изменёнными файлами
                                         (по git diff от origin/main и незафиксированным правкам)
    python testy.py --изменения=ветка    то же, но изменения считаются от указанной ветки или коммита
    python testy.py test_json test_sam.ТестыСамосборки   только эти файлы или классы
    python testy.py --список             показать, что будет запущено, не запуская
    python testy.py --записать-время     записать время тестов этой машины в testy_vremya.json
    python testy.py --часть=1/2          только первая из двух частей (для деления на несколько машин CI)

Единица работы - класс тестов: каждый запускается отдельным процессом
`python -m unittest файл.Класс`, процессы идут параллельно, первыми - самые
долгие (по времени прошлого запуска). Тесты, которым нельзя мешать (замеры
скорости), выполняются в конце по одному. Связь «файл - тесты» задана в
testy_svyazi.py.

Код завершения: 0 - всё прошло, 1 - есть упавшие, 2 - ошибка в параметрах.
"""

import ast
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys
import time

ПАПКА = os.path.dirname(os.path.abspath(__file__))

# Классы и файлы, которые выполняются в конце по одному: они меряют время,
# и соседние процессы искажали бы замер.
ПО_ОДНОМУ = {"test_zamery", "test_json.ТестыJSON", "test_sqlite"}


def файлы_тестов():
    return sorted(os.path.basename(п)[:-3] for п in glob.glob(os.path.join(ПАПКА, "test_*.py")))


def классы(модуль):
    """Классы тестов файла (наследники unittest.TestCase в самом файле), у которых есть тесты."""
    with open(os.path.join(ПАПКА, модуль + ".py"), encoding="utf-8") as ф:
        дерево = ast.parse(ф.read())
    объявления = {у.name: у for у in дерево.body if isinstance(у, ast.ClassDef)}

    def имя_базы(б):
        if isinstance(б, ast.Name):
            return б.id
        if isinstance(б, ast.Attribute):
            return б.attr
        return ""

    def тестовый(имя, видел=()):
        у = объявления.get(имя)
        if у is None or имя in видел:
            return False
        for б in у.bases:
            база = имя_базы(б)
            if база == "TestCase" or тестовый(база, видел + (имя,)):
                return True
        return False

    def есть_тесты(имя, видел=()):
        у = объявления.get(имя)
        if у is None or имя in видел:
            return False
        if any(isinstance(ч, (ast.FunctionDef, ast.AsyncFunctionDef)) and ч.name.startswith("test")
               for ч in у.body):
            return True
        return any(есть_тесты(имя_базы(б), видел + (имя,)) for б in у.bases)

    return [имя for имя in объявления if тестовый(имя) and есть_тесты(имя)]


def единицы(выбор=None):
    """Список «файл.Класс» для запуска; выбор - файлы или классы (None - все)."""
    итог = []
    for модуль in файлы_тестов():
        for класс in классы(модуль):
            полное = f"{модуль}.{класс}"
            if выбор is None or модуль in выбор or полное in выбор:
                итог.append(полное)
    return итог


# ---------- время прошлых запусков ----------

def файл_времени():
    основа = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    if os.name == "nt":
        основа = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(основа, "dan", "время_тестов.json")


# Время из репозитория - для машины, где тесты ещё не запускались (например, в CI):
# по нему долгие классы делятся на части с первого же запуска. Обновить:
# python testy.py --записать-время (после полного прогона).
ВРЕМЯ_В_РЕПОЗИТОРИИ = os.path.join(ПАПКА, "testy_vremya.json")


def прочитать_время():
    итог = {}
    for путь in (ВРЕМЯ_В_РЕПОЗИТОРИИ, файл_времени()):
        try:
            with open(путь, encoding="utf-8") as ф:
                итог.update(json.load(ф))
        except (OSError, ValueError):
            pass
    return итог


def прочитать_время_репозитория():
    try:
        with open(ВРЕМЯ_В_РЕПОЗИТОРИИ, encoding="utf-8") as ф:
            return json.load(ф)
    except (OSError, ValueError):
        return {}


def записать_время(время):
    try:
        os.makedirs(os.path.dirname(файл_времени()), exist_ok=True)
        with open(файл_времени(), "w", encoding="utf-8") as ф:
            json.dump(время, ф, ensure_ascii=False, indent=0, sort_keys=True)
    except OSError:
        pass


# ---------- запуск ----------

# Класс, который в прошлый раз шёл дольше ДОЛГИЙ секунд, делится на части по его тестам
# (по времени каждого теста в прошлый раз) так, чтобы часть шла около ЧАСТЬ секунд:
# иначе самый долгий класс один определяет время всего запуска.
ДОЛГИЙ = 60
ЧАСТЬ = 40


class Единица:
    """Что запускается одним процессом: класс целиком или часть его тестов."""

    def __init__(self, класс, тесты=None, номер=0, всего=1):
        self.класс, self.тесты, self.номер, self.всего = класс, тесты, номер, всего

    def имена(self):
        return [f"{self.класс}.{т}" for т in self.тесты] if self.тесты else [self.класс]

    def __str__(self):
        return self.класс + (f" (часть {self.номер + 1} из {self.всего})" if self.тесты else "")


def прямые_тесты(класс):
    """Тесты класса, объявленные в нём самом, или None, если часть тестов унаследована."""
    модуль, имя = класс.split(".")
    with open(os.path.join(ПАПКА, модуль + ".py"), encoding="utf-8") as ф:
        дерево = ast.parse(ф.read())
    for у in дерево.body:
        if isinstance(у, ast.ClassDef) and у.name == имя:
            базы = [б.attr if isinstance(б, ast.Attribute) else getattr(б, "id", "") for б in у.bases]
            if базы != ["TestCase"]:
                return None
            return [ч.name for ч in у.body if isinstance(ч, ast.FunctionDef) and ч.name.startswith("test")]
    return None


def разбить(класс, прошлое):
    """Единицы для класса: долгий класс с известным временем тестов делится на части."""
    if прошлое.get(класс, 0) <= ДОЛГИЙ:
        return [Единица(класс)]
    тесты = прямые_тесты(класс)
    if not тесты or len(тесты) < 2:
        return [Единица(класс)]
    время = {т: прошлое.get(f"{класс}.{т}", 1.0) for т in тесты}
    частей = min(len(тесты), max(1, round(sum(время.values()) / ЧАСТЬ)))
    if частей < 2:
        return [Единица(класс)]
    корзины = [[] for _ in range(частей)]
    сумма = [0.0] * частей
    for т in sorted(тесты, key=lambda т: -время[т]):
        н = сумма.index(min(сумма))
        корзины[н].append(т)
        сумма[н] += время[т]
    return [Единица(класс, к, н, частей) for н, к in enumerate(корзины)]


def оценка(единица, прошлое):
    if not единица.тесты:
        return прошлое.get(единица.класс, 60)
    return sum(прошлое.get(f"{единица.класс}.{т}", 1.0) for т in единица.тесты) + 5


def запустить(единица):
    """(единица, код, число тестов, секунды, вывод, {тест: секунды})."""
    начало = time.perf_counter()
    длительности = ["--durations", "0"] if sys.version_info >= (3, 12) else []
    р = subprocess.run([sys.executable, "-m", "unittest", *длительности, *единица.имена()], cwd=ПАПКА,
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       env=dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8"))
    секунд = time.perf_counter() - начало
    вывод = р.stdout + р.stderr
    н = re.search(r"^Ran (\d+) tests?", вывод, re.M)
    тесты = {}
    for сек, полное in re.findall(r"^([\d.]+)s\s+\S+ \(([^()\s]+)\)\s*$", вывод, re.M):
        тесты[полное] = float(сек)
    return единица, р.returncode, int(н.group(1)) if н else 0, секунд, вывод, тесты


def по_одному(класс):
    модуль = класс.split(".")[0]
    return модуль in ПО_ОДНОМУ or класс in ПО_ОДНОМУ


def секунды(с):
    return f"{с:6.1f} с".replace(".", ",")


def доля(единицы_, часть, частей, прошлое):
    """Единицы части «часть» из «частей» (номер с 1): все единицы делятся между частями так,
    чтобы оценка времени частей была примерно одинаковой. Деление зависит только от списка
    и времени из репозитория, поэтому на разных машинах части не пересекаются и покрывают всё."""
    корзины = [[] for _ in range(частей)]
    сумма = [0.0] * частей
    for е in sorted(единицы_, key=lambda е: (-оценка(е, прошлое), str(е))):
        н = сумма.index(min(сумма))
        корзины[н].append(е)
        сумма[н] += оценка(е, прошлое)
    return корзины[часть - 1]


def выполнить(список, потоков, часть=None):
    прошлое = прочитать_время()
    вместе = [е for к in список if not по_одному(к) for е in разбить(к, прошлое)]
    отдельно = [Единица(к) for к in список if по_одному(к)]
    if часть:
        # деление - по времени из репозитория, а не этой машины: части на разных машинах согласованы
        в_репозитории = прочитать_время_репозитория()
        вместе = [е for к in список if not по_одному(к) for е in разбить(к, в_репозитории)]
        выбранные = set(map(id, доля(вместе + отдельно, *часть, в_репозитории)))
        вместе = [е for е in вместе if id(е) in выбранные]
        отдельно = [е for е in отдельно if id(е) in выбранные]
    вместе.sort(key=lambda е: -оценка(е, прошлое))
    итоги = []
    начало = time.perf_counter()

    def показать(р):
        единица, код, тестов, сек, вывод, _ = р
        знак = "ок    " if код == 0 else "ОШИБКА"
        пропущено = re.search(r"skipped=(\d+)", вывод)
        доп = f", пропущено {пропущено.group(1)}" if пропущено else ""
        print(f"{знак} {секунды(сек)}  {единица} ({тестов}{доп})", flush=True)
        итоги.append(р)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, потоков)) as пул:
        задания = [пул.submit(запустить, е) for е in вместе]
        for ф in concurrent.futures.as_completed(задания):
            показать(ф.result())
    for е in отдельно:
        показать(запустить(е))

    по_классам = {}
    for единица, код, _, сек, вывод, тесты in итоги:
        по_классам[единица.класс] = по_классам.get(единица.класс, 0) + сек
        for полное, т in тесты.items():
            прошлое[полное] = round(т, 2)
    for класс, сек in по_классам.items():
        прошлое[класс] = round(сек, 1)
    записать_время(прошлое)

    упавшие = [р for р in итоги if р[1] != 0]
    for единица, код, _, _, вывод, _ in упавшие:
        print(f"\n===== {единица} =====\n{вывод.rstrip()}")
    всего = sum(р[2] for р in итоги)
    print(f"\nПроцессов: {len(итоги)}, тестов: {всего}, с ошибками: {len(упавшие)}; "
          f"время {секунды(time.perf_counter() - начало).strip()} при {потоков} одновременно")
    return 1 if упавшие else 0


def изменённые_файлы(от):
    """Файлы, изменённые относительно «от» (коммит или ветка), плюс незафиксированные и новые."""
    def git(*арг):
        р = subprocess.run(["git", *арг], cwd=ПАПКА, capture_output=True, text=True, encoding="utf-8")
        if р.returncode != 0:
            raise RuntimeError(р.stderr.strip())
        return [с for с in р.stdout.splitlines() if с.strip()]
    основа = git("merge-base", от, "HEAD")[0]
    файлы = set(git("-c", "core.quotepath=off", "diff", "--name-only", основа))
    файлы |= set(git("-c", "core.quotepath=off", "ls-files", "--others", "--exclude-standard"))
    return sorted(файлы)


def главная(аргументы):
    потоков = os.cpu_count() or 2
    выбор = []
    изменения = None
    только_список = False
    часть = None
    for а in аргументы:
        if а.startswith("--потоков="):
            потоков = int(а.split("=", 1)[1])
        elif а == "--изменения":
            изменения = "origin/main"
        elif а.startswith("--изменения="):
            изменения = а.split("=", 1)[1]
        elif а == "--список":
            только_список = True
        elif а.startswith("--часть="):
            м = re.fullmatch(r"(\d+)/(\d+)", а.split("=", 1)[1])
            if not м or not 1 <= int(м.group(1)) <= int(м.group(2)):
                print(f"Часть задаётся как «номер/всего», например --часть=1/2: {а}")
                return 2
            часть = (int(м.group(1)), int(м.group(2)))
        elif а == "--записать-время":
            # время последних запусков на этой машине - в файл репозитория (по нему CI делит долгие классы)
            время = прочитать_время()
            нужные = set(единицы())
            время = {к: в for к, в in время.items() if к in нужные or ".".join(к.split(".")[:2]) in нужные}
            with open(ВРЕМЯ_В_РЕПОЗИТОРИИ, "w", encoding="utf-8", newline="\n") as ф:
                json.dump(время, ф, ensure_ascii=False, indent=0, sort_keys=True)
                ф.write("\n")
            print(f"Записано: {ВРЕМЯ_В_РЕПОЗИТОРИИ} ({len(время)} записей)")
            return 0
        elif а.startswith("--"):
            print(f"Неизвестный параметр: {а}\n\n{__doc__}")
            return 2
        else:
            выбор.append(а[:-3] if а.endswith(".py") else а)
    if изменения is not None:
        import testy_svyazi
        try:
            файлы = изменённые_файлы(изменения)
        except (RuntimeError, IndexError) as о:
            print(f"Не удалось узнать изменения от «{изменения}»: {о}")
            return 2
        нужные = testy_svyazi.тесты_для(файлы)
        print(f"Изменено файлов: {len(файлы)}")
        if нужные is None:
            print("Есть изменения, для которых не задан выбор тестов: запускаются все тесты.")
        elif not нужные:
            print("Изменения не затрагивают тестов.")
            return 0
        else:
            print("Выбраны тесты: " + ", ".join(sorted(нужные)))
            выбор += sorted(нужные)
    список = единицы(выбор or None)
    if выбор:
        неизвестные = [в for в in выбор if not any(е == в or е.split(".")[0] == в for е in список)]
        if неизвестные:
            print("Нет таких файлов или классов тестов: " + ", ".join(неизвестные))
            return 2
    if только_список:
        прошлое = прочитать_время_репозитория() if часть else прочитать_время()
        все = [е for к in список for е in ([Единица(к)] if по_одному(к) else разбить(к, прошлое))]
        выбранные = set(map(id, доля(все, *часть, прошлое))) if часть else set(map(id, все))
        for е in все:
            if id(е) in выбранные:
                print(str(е) + ("  (по одному)" if по_одному(е.класс) else ""))
        return 0
    return выполнить(список, потоков, часть)


if __name__ == "__main__":
    sys.exit(главная(sys.argv[1:]))
