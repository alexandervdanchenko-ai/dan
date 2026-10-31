"""Семя - готовый код C компилятора на Дане (semya/dan.c) и сценарии сборки из него без Python.

Что семя не устарело и что собранный из него компилятор собирает сам себя, проверяет задание CI
«Компилятор из семени» на всех системах: сборка из семени занимает около полутора минут, поэтому
здесь - только быстрые проверки самих файлов.
"""

import os
import shutil
import subprocess
import sys
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))
СЕМЯ = os.path.join(ПАПКА, "semya")


def прочитать(путь):
    with open(os.path.join(ПАПКА, путь), encoding="utf-8", newline="") as ф:
        return ф.read()


class ТестыСемени(unittest.TestCase):

    def test_семя_есть_и_похоже_на_компилятор(self):
        текст = прочитать("semya/dan.c")
        self.assertGreater(len(текст), 1_000_000)
        self.assertNotIn("\r", текст)
        self.assertTrue(текст.endswith("\n"))
        self.assertIn("int main(", текст)
        # в семени нет путей машины, на которой оно создано
        for лишнее in ("/home/", "/tmp/", "C:\\\\Users", "/Users/"):
            self.assertNotIn(лишнее, текст)

    def test_семя_в_git_и_отмечено_созданным(self):
        игнор = прочитать(".gitignore")
        self.assertIn("!semya/dan.c", игнор)
        атрибуты = прочитать(".gitattributes")
        self.assertIn("semya/dan.c linguist-generated=true", атрибуты)
        self.assertIn("*.cmd text eol=crlf", атрибуты)

    def test_сценарии_сборки(self):
        sh = прочитать("semya/sobrat.sh")
        cmd = прочитать("semya/sobrat.cmd")
        for сценарий in (sh, cmd):
            self.assertIn("-std=c11", сценарий)
            self.assertIn("-fwrapv", сценарий)
            self.assertIn("-ffp-contract=off", сценарий)
            self.assertIn("-lm", сценарий)
        self.assertIn("-Wl,--stack,67108864", cmd)       # глубокая рекурсия компилятора в Windows
        # в строках команд cmd.exe - только латиница: кодовая страница консоли может быть любой
        for строка in cmd.splitlines():
            if not строка.lower().startswith("rem "):
                self.assertTrue(строка.isascii(), строка)
        if shutil.which("sh"):
            р = subprocess.run(["sh", "-n", os.path.join(СЕМЯ, "sobrat.sh")], capture_output=True, text=True)
            self.assertEqual(р.returncode, 0, р.stderr)

    @unittest.skipUnless(shutil.which("x86_64-w64-mingw32-gcc"), "нет MinGW")
    def test_семя_собирается_для_windows(self):
        """Только стандартный C и windows.h: без объявления функции сборка для Windows не идёт."""
        р = subprocess.run(["x86_64-w64-mingw32-gcc", "-std=c11", "-fsyntax-only",
                            "-Werror=implicit-function-declaration", os.path.join(СЕМЯ, "dan.c")],
                           capture_output=True, text=True, timeout=900)
        self.assertEqual(р.returncode, 0, р.stderr[-3000:])

    def test_сценарий_обновления(self):
        р = subprocess.run([sys.executable, os.path.join(ПАПКА, "dan.py"), "тестировать",
                            os.path.join(СЕМЯ, "obnovit.dan")], capture_output=True, timeout=600, cwd=ПАПКА)
        вывод_ = р.stdout.decode("utf-8", "replace")
        self.assertEqual(р.returncode, 0, вывод_ + р.stderr.decode("utf-8", "replace"))
        self.assertIn("провалено: 0", вывод_)


if __name__ == "__main__":
    unittest.main()
