"""Порядок вычисления в составном присваивании элементу: «с[и] += ф()» читает элемент и проверяет
индекс до правой части, как Python. Раньше порядок выбирал компилятор C: gcc сначала вызывал ф(),
clang - сначала проверял индекс (находка задания «Сверка сборок»)."""

import os
import shutil
import subprocess
import tempfile
import unittest

import dan
import sam_kompilyator

ПРОГРАММА = """функ ф(х: цел) -> цел {
    вывод("вызов")
    вернуть х
}
перем с: [цел] = [1, 2, 3]
перем м = массив(дроб, 3)
перем т: [стр] = ["а", "б"]
с[1] += ф(5)
м[2] += дроб(ф(1))
т[0] += "в"
вывод(с)
вывод(м)
вывод(т)
перем ин = -1
если аргументы().длина > 0 { м[ин] += дроб(ф(2)) } иначе { с[ин] += ф(3) }
"""


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыПорядкаВычисления(unittest.TestCase):
    def test_элемент_до_правой_части(self):
        код_c = sam_kompilyator.перевести(ПРОГРАММА)
        компиляторы = [к for к in ("gcc", "clang") if shutil.which(к)] or [None]
        for к in компиляторы:
            for арг in ([], ["массив"]):
                with self.subTest(компилятор=к, массив=bool(арг)), tempfile.TemporaryDirectory() as папка:
                    прежний = os.environ.get("DAN_COMPILER")
                    if к:
                        os.environ["DAN_COMPILER"] = к
                    try:
                        исп = dan.собрать_код(код_c, os.path.join(папка, "p"), папка)
                    finally:
                        if прежний is None:
                            os.environ.pop("DAN_COMPILER", None)
                        else:
                            os.environ["DAN_COMPILER"] = прежний
                    р = subprocess.run([исп, *арг], capture_output=True, text=True, encoding="utf-8")
                    self.assertEqual(р.returncode, 1, р.stdout + р.stderr)
                    self.assertEqual(р.stdout.replace("\r\n", "\n"),
                                     'вызов\nвызов\n[1, 7, 3]\n[0.0, 0.0, 1.0]\n["ав", "б"]\n')
                    self.assertIn("индекс -1 за пределами (длина 3)", р.stderr)


if __name__ == "__main__":
    unittest.main()
