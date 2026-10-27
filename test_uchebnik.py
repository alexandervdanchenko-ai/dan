"""Учебник «Дан с нуля» и задачник: всё, что показано в книге, - правда.

Каждая программа книги собирается и печатает ровно то, что напечатано под ней;
каждая метка «{{листинг ...}}» ведёт к существующей программе, и каждая
программа показана в книге. У каждой задачи задачника эталонное решение
проходит все проверки, а заготовка - нет (иначе задачу нечего решать).
"""

import os
import re
import shutil
import tempfile
import unittest

import dan
import uchebnik
import zadachnik


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыУчебника(unittest.TestCase):

    def test_программы_печатают_то_что_в_книге(self):
        for имя in uchebnik.листинги():
            with self.subTest(листинг=имя):
                код, вывод, ошибки = uchebnik.выполнить(имя)
                self.assertEqual(код, 0, ошибки)
                if uchebnik.вид(имя) in ("выполнить", "ошибка"):
                    self.assertTrue(os.path.exists(uchebnik.путь(имя, ".вывод")),
                                    "нет образца вывода: python uchebnik.py вывод")
                    self.assertEqual(вывод.splitlines(), прочитать(uchebnik.путь(имя, ".вывод")).splitlines())

    def test_метки_и_программы_соответствуют(self):
        упомянутые = set()
        for глава in uchebnik.главы():
            for имя in uchebnik.МЕТКА.findall(прочитать(глава)):
                with self.subTest(глава=os.path.basename(глава), листинг=имя):
                    self.assertTrue(os.path.exists(uchebnik.путь(имя)))
                упомянутые.add(имя)
        self.assertEqual(sorted(set(uchebnik.листинги()) - упомянутые), [], "программы, которых нет в книге")

    def test_главы_по_порядку(self):
        номера = [int(os.path.basename(г)[:2]) for г in uchebnik.главы()]
        self.assertEqual(номера, list(range(1, len(номера) + 1)))
        for г in uchebnik.главы():
            self.assertTrue(прочитать(г).startswith(f"# Глава {int(os.path.basename(г)[:2])}."), г)

    @unittest.skipUnless(__import__("importlib").util.find_spec("markdown"), "нет пакета markdown")
    def test_книга_собирается(self):
        with tempfile.TemporaryDirectory() as папка:
            uchebnik.собрать_html(папка)
            self.assertEqual(len([ф for ф in os.listdir(папка) if ф.endswith(".html")]), len(uchebnik.главы()) + 1)
            текст = прочитать(os.path.join(папка, "03_числа_и_переменные.html"))
            self.assertIn("7/5", текст)
            self.assertNotIn("{{листинг", текст)


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыЗадачника(unittest.TestCase):

    def test_решения_проходят_заготовки_нет(self):
        for номер, имя in zadachnik.задачи():
            проверки = os.path.join(zadachnik.ПАПКА, имя + ".проверки")
            with self.subTest(задача=имя):
                self.assertGreaterEqual(len(zadachnik.проверки(проверки)), 2)
                with tempfile.TemporaryDirectory() as папка:
                    for откуда, куда in ((os.path.join(zadachnik.ПАПКА, "решения", имя + ".dan"), "решение.dan"),
                                         (os.path.join(zadachnik.ПАПКА, имя + ".dan"), "заготовка.dan")):
                        shutil.copy(откуда, os.path.join(папка, куда))
                    пройдено, всего, первое = zadachnik.проверить_решение(
                        os.path.join(папка, "решение.dan"), проверки, dan.собрать)
                    self.assertEqual(пройдено, всего, первое)
                    пройдено, всего, _ = zadachnik.проверить_решение(
                        os.path.join(папка, "заготовка.dan"), проверки, dan.собрать)
                    self.assertLess(пройдено, всего)

    def test_условие_и_номер(self):
        for номер, имя in zadachnik.задачи():
            текст = прочитать(os.path.join(zadachnik.ПАПКА, имя + ".dan"))
            self.assertTrue(текст.startswith(f"// Задача {номер}. "), имя)
            self.assertIn(f"dan задача {номер}", текст)


if __name__ == "__main__":
    unittest.main()
