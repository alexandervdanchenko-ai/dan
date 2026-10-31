"""Компилятор на Python заморожен: его файлы не меняются без записанной причины (zamorozka.py)."""

import io
import json
import os
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

import zamorozka


class ТестыЗаморозки(unittest.TestCase):

    def test_замороженные_файлы_не_изменены(self):
        изменено = zamorozka.изменённые()
        self.assertEqual(изменено, [],
                         "компилятор на Python заморожен: изменения языка делаются только в папке «сам». "
                         "Если правка в Python всё же нужна (ошибка эталона, проверки, сборка), запишите её "
                         "причину: python zamorozka.py --принять \"причина\"")

    def test_запись_полная(self):
        запись = zamorozka.прочитать_запись()
        self.assertTrue(запись["начало"]["причина"])
        self.assertEqual(sorted(запись["отпечатки"]), sorted(zamorozka.ЗАМОРОЖЕННЫЕ))
        for исключение in запись["исключения"]:
            self.assertTrue(исключение["причина"].strip())
            self.assertTrue(исключение["файлы"])

    def test_принять_записывает_причину(self):
        with tempfile.TemporaryDirectory() as папка:
            for имя in zamorozka.ЗАМОРОЖЕННЫЕ:
                shutil.copy(os.path.join(zamorozka.ПАПКА, имя), папка)
            запись = os.path.join(папка, "zamorozka.json")
            with mock.patch.object(zamorozka, "ПАПКА", папка), mock.patch.object(zamorozka, "ЗАПИСЬ", запись):
                with redirect_stdout(io.StringIO()):
                    zamorozka.принять("начало")
                    self.assertEqual(zamorozka.main([]), 0)
                with open(os.path.join(папка, "kodogen.py"), "a", encoding="utf-8") as ф:
                    ф.write("\n# правка\n")
                self.assertEqual(zamorozka.изменённые(), ["kodogen.py"])
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(zamorozka.main([]), 1)
                    self.assertEqual(zamorozka.main(["--принять", " "]), 2)
                    self.assertEqual(zamorozka.main(["--принять", "исправление эталона"]), 0)
                self.assertEqual(zamorozka.изменённые(), [])
                with open(запись, encoding="utf-8") as ф:
                    исключения = json.load(ф)["исключения"]
                self.assertEqual([и["файлы"] for и in исключения], [["kodogen.py"]])
                self.assertEqual(исключения[-1]["причина"], "исправление эталона")

    def test_изменение_выбирает_этот_тест(self):
        import testy_svyazi
        for имя in zamorozka.ЗАМОРОЖЕННЫЕ + ["zamorozka.json"]:
            with self.subTest(файл=имя):
                self.assertIn("test_zamorozka", testy_svyazi.тесты_для_файла(имя))

    def test_переводы_строк_не_важны(self):
        with tempfile.TemporaryDirectory() as папка:
            а, б = os.path.join(папка, "а"), os.path.join(папка, "б")
            with open(а, "wb") as ф:
                ф.write(b"x = 1\ny = 2\n")
            with open(б, "wb") as ф:
                ф.write(b"x = 1\r\ny = 2\r\n")
            self.assertEqual(zamorozka.отпечаток(а), zamorozka.отпечаток(б))


if __name__ == "__main__":
    unittest.main()
