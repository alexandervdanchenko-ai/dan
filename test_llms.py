"""Тесты описания Дана для языковых моделей (llms.txt, llms-full.txt).

Файлы не должны устаревать: они совпадают с тем, что создаёт создать_llms.py. Каждая запись
Дана в таблице «Python - Дан» проходит проверку компилятора, а каждая образцовая программа
печатает ровно то, что написано под ней.
"""

import os
import re
import subprocess
import tempfile
import unittest

import dan
import создать_llms as llms
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def прочитать(имя):
    with open(os.path.join(ПАПКА, имя), encoding="utf-8") as ф:
        return ф.read()


class ТестыОписанияДляМоделей(unittest.TestCase):

    def test_файлы_не_устарели(self):
        for имя, текст in (("llms.txt", llms.краткий()), ("llms-full.txt", llms.полный())):
            with self.subTest(файл=имя):
                self.assertEqual(прочитать(имя), текст, f"{имя} устарел: запустите python создать_llms.py")

    def test_размер(self):
        # описание помещается в запрос модели вместе с задачей
        self.assertLess(len(llms.полный()), 55000)

    def test_без_длинных_тире(self):
        for имя in ("llms.txt", "llms-full.txt"):
            self.assertNotRegex(прочитать(имя), "[\u2013\u2014]")

    def test_записи_таблицы_проходят_проверку(self):
        for питон, код, вид, *контекст in llms.ТАБЛИЦА:
            if вид == "-":
                continue
            with self.subTest(питон=питон):
                if вид == "в":
                    код = f"вывод({код})"
                текст = llms.ПРЕДИСЛОВИЕ + "".join(к + "\n" for к in контекст) + код + "\n"
                dan.перевести(текст, "проба.dan")     # ошибка проверки - исключение

    @unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
    def test_программа_записи_выполняется(self):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "запись.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(llms.ГРАММАТИКА + "\n")
            исполняемый = dan.собрать(путь, llms.ГРАММАТИКА + "\n")
            р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8", timeout=120,
                               cwd=папка)
            self.assertEqual(р.returncode, 0, р.stderr)

    @unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
    def test_образцы_печатают_написанное(self):
        текст = прочитать("llms-full.txt")
        образцы = re.findall(r"### (\S+)\n\n```dan\n(.*?)\n```\n\n(?:Ввод:\n\n```ввод\n(.*?)\n```\n\n)?"
                             r"Вывод:\n\n```вывод\n(.*?)\n```", текст, re.S)
        self.assertEqual(len(образцы), len(llms.ОБРАЗЦЫ))
        for имя, программа, ввод_, вывод_ in образцы:
            with self.subTest(образец=имя), tempfile.TemporaryDirectory() as папка:
                путь = os.path.join(папка, "образец.dan")
                with open(путь, "w", encoding="utf-8") as ф:
                    ф.write(программа + "\n")
                исполняемый = dan.собрать(путь, программа + "\n")
                р = subprocess.run([исполняемый], input=(ввод_ + "\n") if ввод_ else "", capture_output=True,
                                   text=True, encoding="utf-8", timeout=120, cwd=папка)
                self.assertEqual(р.returncode, 0, р.stderr)
                self.assertEqual(р.stdout.replace("\r\n", "\n").rstrip("\n"), вывод_)


if __name__ == "__main__":
    unittest.main()
