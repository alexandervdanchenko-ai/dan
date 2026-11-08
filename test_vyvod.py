"""Встроенная функция «напечатать» (компилятор на Дане): вывод строки без перевода строки с
выталкиванием буфера, чтобы текст по частям был виден сразу. «модели.напечатать» работает как прежде."""

import os
import subprocess
import tempfile
import unittest

import dan
import sam_kompilyator
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ
from test_parallelno import ошибки_проверки


def выполнить(текст):
    код_c = sam_kompilyator.перевести(текст)
    with tempfile.TemporaryDirectory() as папка:
        исполняемый = dan.собрать_код(код_c, os.path.join(папка, "программа"), папка, выпуск=True)
        р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8", timeout=60)
        return р.returncode, р.stdout, р.stderr


class ТестыПроверки(unittest.TestCase):

    def test_тип_аргумента(self):
        self.assertEqual(ошибки_проверки("напечатать(\"а\")\n"), "")
        self.assertIn("напечатать", ошибки_проверки("напечатать(5)\n"))

    def test_своя_функция_с_тем_же_именем(self):
        """Название не зарезервировано: своя функция «напечатать» в программе означает её."""
        текст = "функ напечатать(х: цел) {\n    вывод(х * 2)\n}\nнапечатать(21)\n"
        self.assertEqual(ошибки_проверки(текст), "")

    def test_в_параллельном_цикле_нельзя(self):
        текст = ошибки_проверки("параллельно для к в 0..2 {\n    напечатать(стр(к))\n}\n")
        self.assertIn("нельзя выводить и вводить: «напечатать» выводит или вводит", текст)

    def test_код_c_выталкивает_буфер(self):
        код_c = sam_kompilyator.перевести("напечатать(\"а\")\n")
        self.assertIn("dan_write_str(", код_c)
        self.assertIn("fflush(stdout)", код_c)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыВыполнения(unittest.TestCase):

    def test_без_перевода_строки(self):
        код, вывод_, ошибки = выполнить("для с в [\"Привет\", \", \", \"мир\"] { напечатать(с) }\nвывод(\"!\")\n"
                                        "напечатать(\"конец\")\n")
        self.assertEqual(код, 0, ошибки)
        self.assertEqual(вывод_, "Привет, мир!\nконец")

    def test_своя_функция(self):
        код, вывод_, ошибки = выполнить("функ напечатать(х: цел) {\n    вывод(х * 2)\n}\nнапечатать(21)\n")
        self.assertEqual(код, 0, ошибки)
        self.assertEqual(вывод_, "42\n")

    def test_видно_сразу(self):
        """Текст виден до конца программы: вывод в канал читается, пока программа ждёт ввода."""
        код_c = sam_kompilyator.перевести("напечатать(\"вопрос: \")\nпусть о = ввод()\nвывод(о)\n")
        with tempfile.TemporaryDirectory() as папка:
            исполняемый = dan.собрать_код(код_c, os.path.join(папка, "программа"), папка, выпуск=True)
            п = subprocess.Popen([исполняемый], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(п.stdout.read(len("вопрос: ".encode())), "вопрос: ".encode())
                вывод_, _ = п.communicate(b"\xd0\xb4\xd0\xb0\n", timeout=60)
            finally:
                if п.poll() is None:
                    п.kill()
            self.assertEqual(вывод_.decode(), "да\n")


if __name__ == "__main__":
    unittest.main()
