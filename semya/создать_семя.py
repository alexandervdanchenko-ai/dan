"""Семя semya/dan.c - код C компилятора на Дане (сам/дан.dan) - для `python пересоздать.py`.

Пока компилятор на Python даёт побайтно тот же код C, что компилятор на Дане (test_sam.py),
семя создаётся им: так `python пересоздать.py` после правки файлов в «сам» обновляет и семя.
Без Python то же делает программа на Дане semya/obnovit.dan.
"""

import os
import sys

ПАПКА = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def текст():
    if ПАПКА not in sys.path:
        sys.path.insert(0, ПАПКА)
    import dan
    путь = os.path.join(ПАПКА, "сам", "дан.dan")
    with open(путь, encoding="utf-8") as ф:
        код = dan.перевести(ф.read(), путь)
    return код if код.endswith("\n") else код + "\n"


if __name__ == "__main__":
    with open(os.path.join(ПАПКА, "semya", "dan.c"), "w", encoding="utf-8", newline="\n") as ф:
        ф.write(текст())
