"""Опись математики (математика/охват.md) согласна с таблицами встроенных функций.

Каждое имя описи существует, каждая функция математических таблиц упомянута, у строк
правильные разделы и состояния (сценарий математика/сверить_охват.py). Тест не собирает
программ, поэтому одинаково проходит и при DAN_KOMPILYATOR=сам.
"""

import importlib.util
import os
import tempfile
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))
СЦЕНАРИЙ = os.path.join(ПАПКА, "математика", "сверить_охват.py")


def сценарий():
    спец = importlib.util.spec_from_file_location("сверить_охват", СЦЕНАРИЙ)
    модуль = importlib.util.module_from_spec(спец)
    спец.loader.exec_module(модуль)
    return модуль


class ТестыОхватаМатематики(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.mkdtemp()
        self.addCleanup(os.rmdir, self.папка)

    def test_опись_согласна_с_таблицами(self):
        self.assertEqual(сценарий().сверить(), [])

    def test_неизвестное_имя_и_пропуск_замечаются(self):
        с = сценарий()
        with open(с.ОПИСЬ, encoding="utf-8") as ф:
            текст = ф.read()
        путь = os.path.join(self.папка, "охват.md")
        испорченный = (текст.replace("`факториал`, ", "")
                       .replace("`определитель`, `ранг`", "`определитель`, `ранг_матрицы`", 1))
        try:
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(испорченный)
            ошибки = с.сверить(путь)
        finally:
            os.remove(путь)
        self.assertTrue(any("«ранг_матрицы»" in о for о in ошибки), ошибки)
        self.assertTrue(any("«факториал» не упомянута" in о for о in ошибки), ошибки)

    def test_разделы_и_состояния(self):
        с = сценарий()
        текст = ("| раздел | класс задач | функции Дана | полнота | состояние |\n|---|---|---|---|---|\n"
                 "| М9 | что-то | нет | - | почти |\n")
        путь = os.path.join(self.папка, "разделы.md")
        try:
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            ошибки = с.сверить(путь)
        finally:
            os.remove(путь)
        self.assertTrue(any("неизвестный раздел «М9»" in о for о in ошибки), ошибки)
        self.assertTrue(any("состояние «почти»" in о for о in ошибки), ошибки)
        self.assertTrue(any("нет строк раздела М1" in о for о in ошибки), ошибки)


if __name__ == "__main__":
    unittest.main()
