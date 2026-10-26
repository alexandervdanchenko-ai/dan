"""Сверка сборок: генератор даёт правильные программы, найденные расхождения не возвращаются."""

import glob
import os
import tempfile
import unittest

import dan
import sverka_sborok as с

СПОСОБЫ = с.сборки()


class ТестыГенератора(unittest.TestCase):

    def test_программы_проходят_проверку(self):
        for зерно in range(40):
            текст = с.случайная_программа(зерно)
            with self.subTest(зерно=зерно):
                dan.перевести(текст)

    def test_одно_зерно_одна_программа(self):
        self.assertEqual(с.случайная_программа(5), с.случайная_программа(5))
        self.assertNotEqual(с.случайная_программа(5), с.случайная_программа(6))

    def test_расхождение(self):
        self.assertIsNone(с.расхождение({"а": (0, "1\n", ""), "б": (0, "1\n", "")}))
        self.assertIn("б: код 1", с.расхождение({"а": (0, "1\n", ""), "б": (1, "", "ошибка")}))


@unittest.skipUnless(len(СПОСОБЫ) >= 2, "нужны два способа сборки (gcc или clang)")
class ТестыСборок(unittest.TestCase):

    def test_несколько_программ(self):
        # быстрая проверка: две сборки с разной оптимизацией; длинная сверка - python sverka_sborok.py
        способы = [СПОСОБЫ[0], СПОСОБЫ[1]]
        проверено, ошибки, расхождения = с.сверить(4, зерно=3, способы=способы, сохранять=False)
        self.assertEqual(ошибки, [])
        self.assertEqual(расхождения, [], расхождения[:1])

    def test_находки_больше_не_расходятся(self):
        for путь in sorted(glob.glob(os.path.join(с.НАХОДКИ, "*.dan"))):
            with open(путь, encoding="utf-8") as ф:
                текст = ф.read()
            with self.subTest(находка=os.path.basename(путь)), tempfile.TemporaryDirectory() as папка:
                self.assertIsNone(с.расхождение(с.выполнить_сборки(текст, папка, СПОСОБЫ)))


if __name__ == "__main__":
    unittest.main()
