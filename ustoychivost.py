"""Проверка устойчивости компилятора на случайно испорченных программах.

Программы каталога и примеров случайно портятся: удаляются и
переставляются строки, вставляются и удаляются слова и знаки. Для любой
такой программы компилятор обязан либо выдать понятное сообщение об
ошибке, либо перевести её в код C, который собирается. Внутренняя ошибка
компилятора (исключение Python) или неверный код C - ошибка самого
компилятора. С ключом --запуск собранные программы ещё и выполняются:
они должны завершаться обычно или с сообщением об ошибке, но не аварийно.

Запуск: python ustoychivost.py [число_программ] [зерно] [--запуск]
"""

import glob
import os
import random
import shutil
import subprocess
import sys
import tempfile
import traceback

import dan
from lexer import ОшибкаЛексера
from parser import ОшибкаРазбора
from proverka import ОшибкаПроверки
from moduli import ОшибкаМодуля

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ВСТАВКИ = ["(", ")", "{", "}", "[", "]", ",", ":", ".", "=", "==", "+", "-", "*", "/", "!", "?", "..", "^",
           "²", "\n", "иначе", "если", "пусть", "перем", "ничего", "вернуть", "функ", "тип", "для", "в",
           "0", "-1", "9223372036854775807", "\"\"", "[]", "{}", "х", "1.5", "рац(1, 0)", "истина",
           "ошибка(\"т\")"]


def исходники():
    пути = sorted(glob.glob(os.path.join(ПАПКА, "каталог", "*", "*.dan")) +
                  glob.glob(os.path.join(ПАПКА, "примеры", "*.dan")))
    итог = []
    for путь in пути:
        with open(путь, encoding="utf-8") as ф:
            итог.append(ф.read())
    return итог


def испортить(текст, сл):
    строки = текст.split("\n")
    вид = сл.randrange(6)
    if вид == 0 and len(строки) > 1:
        del строки[сл.randrange(len(строки))]
    elif вид == 1:
        строки.insert(сл.randrange(len(строки)), строки[сл.randrange(len(строки))])
    elif вид == 2 and len(строки) > 1:
        а, б = сл.randrange(len(строки)), сл.randrange(len(строки))
        строки[а], строки[б] = строки[б], строки[а]
    else:
        и = сл.randrange(len(строки))
        с = строки[и]
        п = сл.randrange(len(с) + 1)
        if вид == 3:
            с = с[:п] + " " + сл.choice(ВСТАВКИ) + " " + с[п:]
        elif вид == 4 and с:
            с = с[:п] + с[п + сл.randrange(1, 6):]
        else:
            с = с[:п] + сл.choice(ВСТАВКИ) + с[п:]
        строки[и] = с
    return "\n".join(строки)


def проверить(число=1000, зерно=1, собирать=50, запускать=False):
    """Возвращает список найденных проблем: (вид, программа, подробности)."""
    сл = random.Random(зерно)
    тексты = исходники()
    проблемы, виды = [], set()
    компилятор = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    собрано = 0
    for _ in range(число):
        т = сл.choice(тексты)
        for _ in range(сл.randint(1, 3)):
            т = испортить(т, сл)
        try:
            код = dan.перевести(т)
        except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаПроверки, ОшибкаМодуля):
            continue
        except Exception as о:     # внутренняя ошибка компилятора
            место = traceback.extract_tb(о.__traceback__)[-1]
            вид = f"{type(о).__name__} в {os.path.basename(место.filename)}:{место.lineno}"
            if вид not in виды:
                виды.add(вид)
                проблемы.append((вид, т, traceback.format_exc()))
            continue
        if not компилятор or собрано >= собирать:
            continue
        собрано += 1
        with tempfile.TemporaryDirectory() as папка:
            путь_c, exe = os.path.join(папка, "p.c"), os.path.join(папка, "p")
            with open(путь_c, "w", encoding="utf-8") as ф:
                ф.write(код)
            сборка = subprocess.run([компилятор, *dan.ФЛАГИ_ОБЩИЕ, *dan.ФЛАГИ_СКОРОСТЬ, путь_c, "-o", exe,
                                     *dan.библиотеки_программы(код, папка)], capture_output=True, text=True)
            if сборка.returncode:
                проблемы.append(("код C не собирается", т, сборка.stderr[-1000:]))
                continue
            if not запускать:
                continue
            try:
                р = subprocess.run([exe], input=b"5\n4\n\n", capture_output=True, timeout=10, cwd=папка)
            except subprocess.TimeoutExpired:
                continue           # испорченная программа может зациклиться: это не ошибка компилятора
            if р.returncode not in (0, 1) or (р.returncode == 1 and not р.stderr.strip()):
                проблемы.append((f"аварийное завершение (код {р.returncode})", т,
                                 р.stderr.decode("utf-8", "replace")[-1000:]))
    return проблемы


if __name__ == "__main__":
    аргументы = [а for а in sys.argv[1:] if not а.startswith("--")]
    число = int(аргументы[0]) if аргументы else 3000
    зерно = int(аргументы[1]) if len(аргументы) > 1 else 1
    запуск = "--запуск" in sys.argv
    найдено = проверить(число, зерно, собирать=число // 10, запускать=запуск)
    for вид, т, подробности in найдено:
        print("=" * 60)
        print(вид)
        print(т)
        print(подробности)
    print(f"Проверено программ: {число}; найдено проблем: {len(найдено)}")
    sys.exit(1 if найдено else 0)
