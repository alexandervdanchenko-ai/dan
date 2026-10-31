"""Семя semya/dan.c - код C компилятора на Дане (сам/дан.dan) - для `python пересоздать.py`.

Семя - это код C, который компилятор на Дане даёт для самого себя. Быстрый путь: код C от
компилятора на Python; пока он совпадает с семенем, семя не устарело. Если не совпадает (правка
в «сам», а после заморозки компилятор на Python уже не обязан давать тот же код), компилятор на
Дане собирается из нового кода (sam_kompilyator) и переводит сам себя - это и есть новое семя.
Без Python то же делает программа на Дане semya/obnovit.dan; окончательную проверку делает
задание CI «Компилятор из семени».
"""

import os
import subprocess
import sys

ПАПКА = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ДАН = os.path.join(ПАПКА, "сам", "дан.dan")
СЕМЯ = os.path.join(ПАПКА, "semya", "dan.c")


def _с_концом(код):
    код = код.replace("\r\n", "\n")
    return код if код.endswith("\n") else код + "\n"


def _код_от_python():
    import dan
    with open(ДАН, encoding="utf-8") as ф:
        return _с_концом(dan.перевести(ф.read(), ДАН))


def _код_от_компилятора_на_дане():
    import sam_kompilyator
    р = subprocess.run([sam_kompilyator.исполняемый(), "си", ДАН], capture_output=True, timeout=1800,
                       env=dict(os.environ, DAN_HOME=ПАПКА), cwd=ПАПКА)
    if р.returncode != 0:
        raise RuntimeError("компилятор на Дане не перевёл сам себя:\n" + р.stdout.decode("utf-8", "replace")
                           + р.stderr.decode("utf-8", "replace"))
    return _с_концом(р.stdout.decode("utf-8"))


def текст():
    if ПАПКА not in sys.path:
        sys.path.insert(0, ПАПКА)
    try:
        with open(СЕМЯ, encoding="utf-8", newline="") as ф:
            было = ф.read()
    except OSError:
        было = None
    try:
        быстрый = _код_от_python()
    except Exception:
        быстрый = None
    if быстрый is not None and быстрый == было:
        return было
    return _код_от_компилятора_на_дане()


if __name__ == "__main__":
    with open(СЕМЯ, "w", encoding="utf-8", newline="\n") as ф:
        ф.write(текст())
