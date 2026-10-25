"""Примеры кода в документации должны быть записаны без ошибок.

Каждый блок кода из spec.md и справочник.md разбирается парсером Дана.
Пропускаются сокращённые примеры (с «...»), тексты сообщений и команды.
Блок, сразу за которым идёт текст сообщения об ошибке («Строка ...»),
показывает ошибку намеренно и обязан её вызывать.
"""

import os
import re
import unittest

from lexer import ОшибкаЛексера
from parser import ОшибкаРазбора, разобрать

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ФАЙЛЫ = ("spec.md", "справочник.md")


def блоки(файл):
    with open(os.path.join(ПАПКА, файл), encoding="utf-8") as ф:
        return re.findall(r"```\n(.*?)```", ф.read(), re.S)


def сообщение(блок):
    return блок.lstrip().startswith(("Строка", "Ошибка", "python", "dan "))


class ТестыПримеровДокументации(unittest.TestCase):

    def test_примеры_разбираются(self):
        for файл in ФАЙЛЫ:
            б = блоки(файл)
            for номер, блок in enumerate(б):
                if "..." in блок or сообщение(блок):
                    continue
                намеренная = номер + 1 < len(б) and б[номер + 1].lstrip().startswith("Строка")
                with self.subTest(файл=файл, пример=блок.strip().splitlines()[0]):
                    try:
                        разобрать(блок)
                        ошибка = None
                    except (ОшибкаЛексера, ОшибкаРазбора) as о:
                        ошибка = str(о)
                    if намеренная:
                        self.assertIsNotNone(ошибка, "пример должен показывать ошибку")
                    else:
                        self.assertIsNone(ошибка)


if __name__ == "__main__":
    unittest.main()
