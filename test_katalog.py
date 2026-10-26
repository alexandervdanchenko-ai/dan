"""Проверка каталога программ (папка «каталог»).

каталог/программы: для каждой программы «имя.dan» в файле «имя.вывод» записан
ожидаемый вывод; если есть «имя.ввод», он подаётся на клавиатурный ввод.

каталог/ошибки: «имя.dan» - неверная программа, «имя.сообщение» - сообщение,
которое должен выдать компилятор или программа при выполнении,
«имя.исправлено.dan» и «имя.исправлено.вывод» - исправленная программа и её вывод.

Каталог служит одновременно примерами для изучения языка, материалом для
ассистентов ИИ и проверкой того, что сообщения об ошибках не ухудшаются.
"""

import glob
import os
import shutil
import subprocess
import tempfile
import unittest

import dan
from lexer import ОшибкаЛексера
from parser import ОшибкаРазбора
from proverka import ОшибкаПроверки
from moduli import ОшибкаМодуля

КАТАЛОГ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "каталог")
КОМПИЛЯТОР_ЕСТЬ = any(shutil.which(к) for к in dan.КОМПИЛЯТОРЫ)


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


def ошибка_компиляции(текст):
    """Текст сообщений компилятора или None, если программа верна."""
    try:
        dan.перевести(текст)
    except (ОшибкаЛексера, ОшибкаРазбора, ОшибкаПроверки, ОшибкаМодуля) as о:
        return str(о)
    return None


def выполнить(текст, ввод_данных=""):
    with tempfile.TemporaryDirectory() as папка:
        путь = os.path.join(папка, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8",
                           timeout=60, input=ввод_данных, cwd=папка)
        return р.returncode, р.stdout, р.stderr


def программы(папка):
    return sorted(п for п in glob.glob(os.path.join(КАТАЛОГ, папка, "*.dan"))
                  if not п.endswith(".исправлено.dan"))


class ТестыСоставаКаталога(unittest.TestCase):

    def test_у_каждой_программы_есть_ожидаемый_результат(self):
        self.assertGreater(len(программы("программы")), 0)
        for путь in программы("программы"):
            with self.subTest(путь=путь):
                self.assertTrue(os.path.exists(путь[:-4] + ".вывод"))
        for путь in программы("ошибки"):
            основа = путь[:-4]
            for конец in (".сообщение", ".исправлено.dan", ".исправлено.вывод"):
                with self.subTest(путь=путь, файл=конец):
                    self.assertTrue(os.path.exists(основа + конец))

    def test_каждая_программа_начинается_с_описания(self):
        for путь in программы("программы") + программы("ошибки"):
            with self.subTest(путь=путь):
                self.assertTrue(прочитать(путь).startswith("// "))

    def test_ошибки_компиляции(self):
        for путь in программы("ошибки"):
            ожидается = прочитать(путь[:-4] + ".сообщение").strip()
            if ожидается.startswith("Ошибка выполнения"):
                continue
            with self.subTest(путь=путь):
                сообщение = ошибка_компиляции(прочитать(путь))
                self.assertIsNotNone(сообщение, "программа должна содержать ошибку")
                self.assertIn(ожидается, сообщение)

    def test_исправленные_программы_верны(self):
        for путь in программы("ошибки"):
            with self.subTest(путь=путь):
                self.assertIsNone(ошибка_компиляции(прочитать(путь[:-4] + ".исправлено.dan")))


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыВыполненияКаталога(unittest.TestCase):

    def test_программы(self):
        for путь in программы("программы"):
            основа = путь[:-4]
            ввод_данных = прочитать(основа + ".ввод") if os.path.exists(основа + ".ввод") else ""
            with self.subTest(путь=путь):
                код, вывод, ошибки = выполнить(прочитать(путь), ввод_данных)
                self.assertEqual(код, 0, ошибки)
                self.assertEqual(вывод.splitlines(), прочитать(основа + ".вывод").splitlines())

    def test_ошибки_выполнения(self):
        for путь in программы("ошибки"):
            ожидается = прочитать(путь[:-4] + ".сообщение").strip()
            if not ожидается.startswith("Ошибка выполнения"):
                continue
            with self.subTest(путь=путь):
                код, вывод, ошибки = выполнить(прочитать(путь))
                self.assertEqual(код, 1)
                self.assertIn(ожидается, ошибки + вывод)

    def test_исправленные_программы(self):
        for путь in программы("ошибки"):
            основа = путь[:-4]
            with self.subTest(путь=путь):
                код, вывод, ошибки = выполнить(прочитать(основа + ".исправлено.dan"))
                self.assertEqual(код, 0, ошибки)
                self.assertEqual(вывод.splitlines(), прочитать(основа + ".исправлено.вывод").splitlines())


if __name__ == "__main__":
    unittest.main()
