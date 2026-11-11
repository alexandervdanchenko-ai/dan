"""Таблица букв Юникода (сам/буквы.dan) и её сверка с lexer.py.

Буква для обоих компиляторов - символ категорий Lu, Ll, Lt, Lm, Lo закреплённой версии Юникода
(сам/создать_буквы.ВЕРСИЯ, lexer.py - str.isalpha Python 3.12), алфавит - первое слово названия.
"""

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

import sam_kompilyator

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def создатель():
    сп = importlib.util.spec_from_file_location("создать_буквы", os.path.join(ПАПКА, "сам", "создать_буквы.py"))
    м = importlib.util.module_from_spec(сп)
    сп.loader.exec_module(м)
    return м


ОБХОД = """подключить "буквы"

перем начало = -1
перем номер = 0
перем к = 128
пока к <= 1114112 {
    перем н = 0
    если к < 1114112 { н = буквы.номер_алфавита(к) }
    если н != номер {
        если номер > 0 { вывод(стр(начало) + " " + стр(к - 1) + " " + буквы.название(номер)) }
        начало = к
        номер = н
    }
    к += 1
}
"""


class ТестыБукв(unittest.TestCase):
    def test_файл_не_устарел(self):
        с = создатель()
        if с.база() is None:
            self.skipTest("нет базы Юникода " + с.ВЕРСИЯ + " (unicodedata2 или Python 3.12)")
        with open(с.ФАЙЛ, encoding="utf-8", newline="") as ф:
            self.assertEqual(ф.read(), с.текст(), "выполните: python пересоздать.py")

    def test_таблица_на_дане_совпадает_с_базой(self):
        с = создатель()
        б = с.база()
        if б is None:
            self.skipTest("нет базы Юникода " + с.ВЕРСИЯ)
        ожидается = "".join(f"{н} {к} {а}\n" for н, к, а in с.диапазоны(б))
        with tempfile.TemporaryDirectory() as папка:
            shutil.copy(с.ФАЙЛ, папка)
            with open(os.path.join(папка, "обход.dan"), "w", encoding="utf-8") as ф:
                ф.write(ОБХОД)
            р = subprocess.run([sam_kompilyator.исполняемый(), "запустить", "обход.dan"], cwd=папка,
                               capture_output=True, env=sam_kompilyator.среда(), timeout=900)
            self.assertEqual(р.returncode, 0, р.stderr.decode("utf-8", "replace"))
            self.assertEqual(р.stdout.decode("utf-8"), ожидается)

    def test_лексеры_согласны(self):
        import unicodedata
        if unicodedata.unidata_version != создатель().ВЕРСИЯ:
            self.skipTest("lexer.py пользуется Юникодом этого Python: " + unicodedata.unidata_version)
        import lexer
        # грузинский, тайский, эфиопский, тибетский, монгольский, математические буквы, черокки
        имена = ["გამარჯობა", "สวสด", "ሰላም", "བད", "ᠮᠣᠩᠭᠣᠯ", "𝐱𝐲", "ᏣᎳᎩ", "ʰ", "ª", "µ"]
        текст = "".join(f"пусть {и} = {н}\n" for н, и in enumerate(имена)) + "пусть сoк = 1\n"
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            р = subprocess.run([sam_kompilyator.исполняемый(), "токены", "п.dan"], cwd=папка,
                               capture_output=True, env=sam_kompilyator.среда(), timeout=300)
        try:
            токены = lexer.разобрать(текст)
            ожидается = None
        except lexer.ОшибкаЛексера as о:
            ожидается = str(о)
        вывод = р.stdout.decode("utf-8") + р.stderr.decode("utf-8")
        # смешение «с» и латинской «o» - ошибка у обоих, с одинаковым описанием алфавитов
        self.assertIsNotNone(ожидается)
        self.assertIn(ожидается.split(": ", 1)[1], вывод)
        текст_без_ошибки = текст.replace("пусть сoк = 1\n", "")
        токены = lexer.разобрать(текст_без_ошибки)
        with tempfile.TemporaryDirectory() as папка:
            with open(os.path.join(папка, "п.dan"), "w", encoding="utf-8") as ф:
                ф.write(текст_без_ошибки)
            р = subprocess.run([sam_kompilyator.исполняемый(), "токены", "п.dan"], cwd=папка,
                               capture_output=True, env=sam_kompilyator.среда(), timeout=300)
        self.assertEqual(р.returncode, 0, р.stderr.decode("utf-8", "replace"))
        имена_дан = [и for и in имена]
        for и in имена_дан:
            self.assertIn(и, р.stdout.decode("utf-8"))
        self.assertEqual(sum(1 for т in токены if т.вид.name == "ИМЯ"), len(имена))


if __name__ == "__main__":
    unittest.main()
