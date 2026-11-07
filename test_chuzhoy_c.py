"""Сборка не затирает чужой файл C рядом с программой (находка потока замеров этапа 20).

«dan собрать x.dan» записывает код C программы в «x.c» рядом с ней. Прежде файл с таким именем,
написанный человеком, молча заменялся. Теперь Дан перезаписывает только свой файл (он начинается с
метки Дана), а на чужом останавливается с объяснением. Проверяются оба компилятора.
"""

import os
import subprocess
import sys
import tempfile
import unittest

import sam_kompilyator
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ЧУЖОЙ = "/* мой код на C */\nint сложить(int а, int б) { return а + б; }\n"


def собрать(папка, компилятор):
    """«собрать x.dan» компилятором на Дане (собранный исполняемый файл, как в других тестах) или на Python."""
    программа = os.path.join(папка, "x.dan")
    if компилятор == "сам":
        return subprocess.run([sam_kompilyator.исполняемый(), "собрать", программа], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", env=sam_kompilyator.среда(), timeout=600)
    окружение = dict(os.environ)
    окружение.pop("DAN_KOMPILYATOR", None)
    return subprocess.run([sys.executable, os.path.join(ПАПКА, "dan.py"), "собрать", программа], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", env=окружение, timeout=600)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыЧужогоФайлаC(unittest.TestCase):

    def проверить(self, компилятор):
        with tempfile.TemporaryDirectory() as папка:
            with open(os.path.join(папка, "x.dan"), "w", encoding="utf-8") as ф:
                ф.write('вывод("привет")\n')
            путь_c = os.path.join(папка, "x.c")
            with open(путь_c, "w", encoding="utf-8") as ф:
                ф.write(ЧУЖОЙ)
            р = собрать(папка, компилятор)
            весь = f"код {р.returncode}: {р.stdout}{р.stderr}"
            self.assertNotEqual(р.returncode, 0, весь)
            self.assertIn("не Даном", весь)
            with open(путь_c, encoding="utf-8") as ф:
                self.assertEqual(ф.read(), ЧУЖОЙ)
            # свой прежний файл Дан перезаписывает, как раньше
            os.remove(путь_c)
            р = собрать(папка, компилятор)
            self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
            р = собрать(папка, компилятор)
            self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
            with open(путь_c, encoding="utf-8") as ф:
                self.assertTrue(ф.read().startswith("/* Создано компилятором языка Дан."))

    def test_компилятор_на_дане(self):
        self.проверить("сам")

    def test_компилятор_на_python(self):
        self.проверить("python")

    def test_нечитаемый_чужой_файл(self):
        with tempfile.TemporaryDirectory() as папка:
            with open(os.path.join(папка, "x.dan"), "w", encoding="utf-8") as ф:
                ф.write('вывод("привет")\n')
            with open(os.path.join(папка, "x.c"), "wb") as ф:
                ф.write(b"\xff\xfe\x00")
            for компилятор in ("сам", "python"):
                р = собрать(папка, компилятор)
                self.assertNotEqual(р.returncode, 0, f"{компилятор}: {р.stdout}{р.stderr}")
                self.assertIn("не Даном", р.stdout + р.stderr, компилятор)
            with open(os.path.join(папка, "x.c"), "rb") as ф:
                self.assertEqual(ф.read(), b"\xff\xfe\x00")


if __name__ == "__main__":
    unittest.main()
