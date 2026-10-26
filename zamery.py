"""Воспроизводимые замеры скорости Дана в сравнении с C и Python.

Каждая задача из папки «замеры» записана на трёх языках: «дан/имя.dan»,
«си/имя.c» (эталон: обычный код на C без проверок, gcc -O2) и
«питон/имя.py». Программы собираются заново, каждая запускается
несколько раз, берётся наименьшее время. Перед сравнением проверяется,
что все три программы вывели одно и то же, иначе замер не имеет смысла.

Запуск: python zamery.py [число_повторов]
Результат печатается и записывается в «замеры/результаты.md».
"""

import datetime
import glob
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

import dan

ПАПКА = os.path.join(os.path.dirname(os.path.abspath(__file__)), "замеры")


def время(команда, повторов):
    лучшее, вывод = None, None
    for _ in range(повторов):
        начало = time.perf_counter()
        р = subprocess.run(команда, capture_output=True, text=True, encoding="utf-8")
        прошло = time.perf_counter() - начало
        if р.returncode:
            raise RuntimeError(f"{команда[0]}: код {р.returncode}\n{р.stderr}")
        лучшее = прошло if лучшее is None else min(лучшее, прошло)
        вывод = р.stdout
    return лучшее, вывод


def описание(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.readline().lstrip("/# ").strip()


def процессор():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as ф:
            for строка in ф:
                if строка.startswith("model name"):
                    return строка.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def замерить(повторов=3):
    компилятор = dan.найти_компилятор()
    строки = []
    with tempfile.TemporaryDirectory() as врем:
        for путь_дан in sorted(glob.glob(os.path.join(ПАПКА, "дан", "*.dan"))):
            имя = os.path.splitext(os.path.basename(путь_дан))[0]
            копия = os.path.join(врем, имя + ".dan")
            shutil.copy(путь_дан, копия)
            with open(копия, encoding="utf-8") as ф:
                exe_дан = dan.собрать(копия, ф.read())
            т_дан, в_дан = время([exe_дан], повторов)
            т_си = т_пи = None
            путь_си = os.path.join(ПАПКА, "си", имя + ".c")
            if os.path.exists(путь_си):
                exe_си = os.path.join(врем, имя + "_c")
                subprocess.run([компилятор, "-O2", путь_си, "-o", exe_си, "-lm"], check=True)
                т_си, в_си = время([exe_си], повторов)
                if в_си.split() != в_дан.split():
                    raise RuntimeError(f"{имя}: вывод C {в_си!r} не совпадает с выводом Дана {в_дан!r}")
            путь_пи = os.path.join(ПАПКА, "питон", имя + ".py")
            if os.path.exists(путь_пи):
                т_пи, в_пи = время([sys.executable, путь_пи], max(1, повторов - 1))
                if в_пи.split() != в_дан.split():
                    raise RuntimeError(f"{имя}: вывод Python {в_пи!r} не совпадает с выводом Дана {в_дан!r}")
            строки.append((имя, описание(путь_дан), т_дан, т_си, т_пи))
            print(f"{имя}: Дан {т_дан:.3f} с, C {т_си if т_си is None else round(т_си, 3)} с, "
                  f"Python {т_пи if т_пи is None else round(т_пи, 3)} с", flush=True)
    return строки


def отчёт(строки):
    def с(т):
        return "-" if т is None else f"{т:.2f}".replace(".", ",")

    def раз(а, б):
        return "-" if а is None or б is None else f"{а / б:.1f}".replace(".", ",")

    версия_c = subprocess.run([dan.найти_компилятор(), "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    текст = [
        "# Замеры скорости",
        "",
        f"Дата: {datetime.date.today():%d.%m.%Y}. Процессор: {процессор()}. Система: {platform.system()} "
        f"{platform.release()}. Компилятор C: {версия_c}. Python {platform.python_version()}.",
        "",
        "Время - наименьшее из нескольких запусков, в секундах, включая запуск программы. Эталон на C - обычный "
        "код без проверок переполнения и границ, собранный с `-O2`. Все три программы каждой задачи выводят "
        "одно и то же (это проверяется перед замером). Повторить: `python zamery.py`.",
        "",
        "| Задача | Дан | C | Python | Дан медленнее C, раз | Python медленнее Дана, раз |",
        "|---|---|---|---|---|---|",
    ]
    for имя, опис, т_дан, т_си, т_пи in строки:
        текст.append(f"| {опис} | {с(т_дан)} | {с(т_си)} | {с(т_пи)} | {раз(т_дан, т_си)} | {раз(т_пи, т_дан)} |")
    текст.append("")
    return "\n".join(текст)


if __name__ == "__main__":
    повторов = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    итог = отчёт(замерить(повторов))
    with open(os.path.join(ПАПКА, "результаты.md"), "w", encoding="utf-8") as ф:
        ф.write(итог)
    print()
    print(итог)
