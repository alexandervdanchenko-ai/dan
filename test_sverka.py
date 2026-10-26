"""Вычисления Дана совпадают с независимым эталоном (см. sverka_vychisleniy.py)."""

import shutil
import unittest

import dan
import sverka_vychisleniy


@unittest.skipUnless(any(shutil.which(к) for к in dan.КОМПИЛЯТОРЫ), "нет компилятора C")
class ТестыСверкиВычислений(unittest.TestCase):

    def test_случайные_выражения(self):
        self.assertEqual(sverka_vychisleniy.сверить(число=400, зерно=2026, ошибочных=8), [])

    def test_списки_и_словари_значения(self):
        self.assertEqual(sverka_vychisleniy.сверить_значения(число=15, зерно=2026), [])

    def test_строки(self):
        self.assertEqual(sverka_vychisleniy.сверить_строки(число=600, зерно=2026), [])


if __name__ == "__main__":
    unittest.main()
