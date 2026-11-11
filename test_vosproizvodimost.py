"""Воспроизводимая сборка поставки: архив зависит только от содержимого, сверка двух сборок."""

import hashlib
import importlib.util
import os
import tempfile
import time
import unittest
import zipfile

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def модуль(имя, файл):
    спец = importlib.util.spec_from_file_location(имя, os.path.join(ПАПКА, "поставка", файл))
    м = importlib.util.module_from_spec(спец)
    спец.loader.exec_module(м)
    return м


собрать = модуль("sobrat_postavku", "собрать.py")
проверка = модуль("proverit_vosproizvodimost", "проверить_воспроизводимость.py")


def папка_поставки(корень, порядок):
    """Папка dan с вложенными файлами; порядок создания и время файлов задаются извне."""
    файлы = {"dan.exe": b"MZ" + bytes(100), "zig/lib/a.h": b"int a;\n", "библиотека/json.dan": "модуль".encode(),
             "README.md": b"# Dan\n"}
    for имя in (sorted(файлы) if порядок else sorted(файлы, reverse=True)):
        путь = os.path.join(корень, "dan", *имя.split("/"))
        os.makedirs(os.path.dirname(путь), exist_ok=True)
        with open(путь, "wb") as ф:
            ф.write(файлы[имя])
        os.utime(путь, (time.time() - (1000 if порядок else 5), time.time() - (1000 if порядок else 5)))
    os.makedirs(os.path.join(корень, "dan", "пусто"))


def сумма(путь):
    with open(путь, "rb") as ф:
        return hashlib.sha256(ф.read()).hexdigest()


class ТестыВоспроизводимости(unittest.TestCase):
    def test_архив_не_зависит_от_порядка_и_времени_файлов(self):
        with tempfile.TemporaryDirectory() as а, tempfile.TemporaryDirectory() as б:
            папка_поставки(а, True)
            папка_поставки(б, False)
            первый = собрать.упаковать(а, "dan", os.path.join(а, "d.zip"), 1_700_000_000)
            второй = собрать.упаковать(б, "dan", os.path.join(б, "d.zip"), 1_700_000_000)
            self.assertEqual(сумма(первый), сумма(второй))
            with zipfile.ZipFile(первый) as з:
                имена = з.namelist()
                self.assertEqual(имена, sorted(имена))
                self.assertIn("dan/библиотека/json.dan", имена)
                self.assertIn("dan/пусто/", имена)
                self.assertEqual({и.date_time for и in з.infolist()}, {time.gmtime(1_700_000_000)[:6]})
                self.assertEqual(з.read("dan/библиотека/json.dan"), "модуль".encode())
                self.assertIsNone(з.testzip())
            # другое время - другой архив: время берётся из коммита, а не из часов сборки
            третий = собрать.упаковать(б, "dan", os.path.join(б, "e.zip"), 1_700_100_000)
            self.assertNotEqual(сумма(первый), сумма(третий))

    def test_время_сборки_из_source_date_epoch(self):
        прежнее = os.environ.get("SOURCE_DATE_EPOCH")
        try:
            os.environ["SOURCE_DATE_EPOCH"] = "1234567890"
            self.assertEqual(собрать.время_сборки(), 1234567890)
            os.environ["SOURCE_DATE_EPOCH"] = ""
            self.assertIsInstance(собрать.время_сборки(), int)   # время коммита или 0 вне git
        finally:
            if прежнее is None:
                os.environ.pop("SOURCE_DATE_EPOCH", None)
            else:
                os.environ["SOURCE_DATE_EPOCH"] = прежнее

    def test_время_до_1980(self):
        with tempfile.TemporaryDirectory() as а:
            папка_поставки(а, True)
            with zipfile.ZipFile(собрать.упаковать(а, "dan", os.path.join(а, "d.zip"), 0)) as з:
                self.assertEqual(з.infolist()[0].date_time, (1980, 1, 1, 0, 0, 0))

    def test_сравнение_сборок(self):
        self.assertEqual(проверка.сравнить({"a": "1", "b": "2"}, {"a": "1", "b": "2"}), [])
        различия = проверка.сравнить({"a": "1", "b": "2"}, {"a": "9", "c": "3"})
        self.assertEqual(различия, ["разное содержимое: a", "только в первой сборке: b",
                                    "только во второй сборке: c"])

    def test_компоновка_без_времени(self):
        with open(os.path.join(ПАПКА, "поставка", "zig_cc.c"), encoding="utf-8") as ф:
            обёртка = ф.read()
        self.assertIn("--no-insert-timestamp", обёртка)
        with open(os.path.join(ПАПКА, "поставка", "собрать.py"), encoding="utf-8") as ф:
            self.assertIn('"-Wl,--no-insert-timestamp"', ф.read())
        with open(os.path.join(ПАПКА, ".github", "workflows", "proverka.yml"), encoding="utf-8") as ф:
            self.assertIn("поставка/проверить_воспроизводимость.py", ф.read())


if __name__ == "__main__":
    unittest.main()
