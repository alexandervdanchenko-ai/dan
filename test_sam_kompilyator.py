"""Проверка тестами через компилятор на Дане (sam_kompilyator.py, DAN_KOMPILYATOR=сам)."""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import dan
import sam_kompilyator
from lexer import ОшибкаЛексера
from moduli import ОшибкаМодуля
from parser import ОшибкаРазбора
from proverka import ОшибкаПроверки

ПРОГРАММА = 'функ удвоить(х: цел) -> цел {\n    вернуть х * 2\n}\nвывод(удвоить(21))\n' \
            'тест "удвоение" {\n    утвердить_равно(удвоить(2), 4)\n}\n'


class ТестыРазбораВывода(unittest.TestCase):
    """Вывод «дан си» при ошибке превращается в то же исключение, что у компилятора на Python."""

    def test_ошибка_записи(self):
        о = sam_kompilyator.исключение(
            "п.dan: ошибка в записи программы\n  Строка 1, позиция 9: лишняя закрывающая скобка «)»\n"
            "    1 | вывод(1))\n      |         ^\n", "п.dan")
        self.assertIsInstance(о, ОшибкаРазбора)
        self.assertIsInstance(о, ОшибкаЛексера)
        self.assertEqual((str(о), о.строка, о.столбец), ("Строка 1, позиция 9: лишняя закрывающая скобка «)»", 1, 9))

    def test_ошибки_проверки(self):
        о = sam_kompilyator.исключение(
            "п.dan: найдено ошибок: 2\n  Строка 1, позиция 16: переменная «а» имеет тип цел\n"
            "    1 | пусть а: цел = \"x\"\n      |                ^^^\n"
            "  Файл м.dan, строка 2, позиция 7: переменная «б» не объявлена\n", "п.dan")
        self.assertIsInstance(о, ОшибкаПроверки)
        self.assertEqual([(с.строка, с.столбец, с.текст) for с in о.ошибки],
                         [(1, 16, "переменная «а» имеет тип цел"), (2, 7, "переменная «б» не объявлена")])
        self.assertEqual(str(о), "Строка 1, позиция 16: переменная «а» имеет тип цел\n"
                                 "Файл м.dan, строка 2, позиция 7: переменная «б» не объявлена")

    def test_ошибка_подключения(self):
        о = sam_kompilyator.исключение('п.dan: Строка 1, позиция 1: не найден подключаемый файл «нет»\n'
                                       '    1 | подключить "нет"\n      | ^^^^^^^^^^\n', "п.dan")
        self.assertIsInstance(о, ОшибкаМодуля)
        self.assertEqual(о.текст, "п.dan: Строка 1, позиция 1: не найден подключаемый файл «нет»")
        self.assertTrue(str(о).endswith("^^^^^^^^^^"))

    def test_без_дерева(self):
        with self.assertRaises(unittest.SkipTest):
            sam_kompilyator.БЕЗ_ДЕРЕВА.инструкции


class ТестыРежимовСи(unittest.TestCase):
    """«dan си --тесты» и другие режимы дают тот же код, что сборка для этих команд."""

    def си(self, *параметры):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(ПРОГРАММА)
            вывод_ = io.StringIO()
            with contextlib.redirect_stdout(вывод_), mock.patch.dict(os.environ, {"DAN_KOMPILYATOR": ""}):
                код = dan.main(["си", *параметры, путь])
            return код, вывод_.getvalue(), путь

    def test_режимы(self):
        for параметр, режим in dan.РЕЖИМЫ_СИ.items():
            with self.subTest(режим=режим):
                код, вывод_, путь = self.си(параметр)
                self.assertEqual(код, 0, вывод_)
                нужно = {режим: True} | ({"тесты": True} if режим == "покрытие" else {})
                self.assertEqual(вывод_, dan.перевести_python(ПРОГРАММА, путь, **нужно))
        self.assertNotEqual(self.си("--тесты")[1], self.си()[1])

    def test_режим_только_для_си(self):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(ПРОГРАММА)
            вывод_ = io.StringIO()
            with contextlib.redirect_stdout(вывод_), mock.patch.dict(os.environ, {"DAN_KOMPILYATOR": ""}):
                self.assertEqual(dan.main(["собрать", "--тесты", путь]), 2)
            self.assertIn("Неизвестный параметр: --тесты", вывод_.getvalue())


class ТестыКэшаКомпилятора(unittest.TestCase):

    def test_кэш_на_другом_диске(self):
        # в Windows временная папка и кэш (DAN_CACHE) могут быть на разных дисках: os.replace между
        # дисками не работает, и собранный компилятор переносится копией рядом с целью
        настоящий = os.replace

        def замена(откуда, куда):
            if os.path.dirname(os.path.abspath(откуда)) != os.path.dirname(os.path.abspath(куда)):
                raise OSError(17, "The system cannot move the file to a different disk drive")
            настоящий(откуда, куда)

        def собрать_код(код, основа, папка):
            with open(основа + ".exe", "w", encoding="utf-8") as ф:
                ф.write(код)
            return основа + ".exe"

        with tempfile.TemporaryDirectory() as кэш, \
                mock.patch.object(sam_kompilyator, "_DAN_CACHE", кэш), \
                mock.patch.object(sam_kompilyator, "_ИСПОЛНЯЕМЫЙ", []), \
                mock.patch.dict(os.environ, {"DAN_SAM_EXE": ""}), \
                mock.patch.object(sam_kompilyator, "код_компилятора", lambda временная: "код"), \
                mock.patch.object(dan, "собрать_код", собрать_код), \
                mock.patch.object(sam_kompilyator.os, "replace", замена):
            путь = sam_kompilyator.исполняемый()
            self.assertTrue(путь.startswith(кэш))
            with open(путь, encoding="utf-8") as ф:
                self.assertEqual(ф.read(), "код")
            self.assertEqual([и for и in os.listdir(os.path.dirname(путь)) if и.endswith(".tmp")], [])


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыЧерезКомпиляторНаДане(unittest.TestCase):
    """dan.перевести при DAN_KOMPILYATOR=сам: код и ошибки от компилятора на Дане."""

    def setUp(self):
        патч = mock.patch.dict(os.environ, {"DAN_KOMPILYATOR": "сам"})
        патч.start()
        self.addCleanup(патч.stop)

    def test_код_как_у_python(self):
        for режим in ({}, {"тесты": True}, {"отладка": True}, {"строки_дан": True}):
            with self.subTest(режим=режим):
                self.assertEqual(dan.перевести(ПРОГРАММА, **режим), dan.перевести_python(ПРОГРАММА, **режим))

    def test_файл_с_другим_текстом_не_меняется(self):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write("вывод(1)\n")
            self.assertEqual(dan.перевести(ПРОГРАММА, путь), dan.перевести_python(ПРОГРАММА, путь))
            новый = os.path.join(папка, "новый.dan")
            self.assertEqual(dan.перевести(ПРОГРАММА, новый), dan.перевести_python(ПРОГРАММА, новый))
            self.assertEqual(sorted(os.listdir(папка)), ["п.dan"])
            with open(путь, encoding="utf-8") as ф:
                self.assertEqual(ф.read(), "вывод(1)\n")

    def test_ошибки_как_у_python(self):
        for текст in ("вывод(1))\n", 'пусть а: цел = "x"\nвывод(б)\n', 'подключить "нет"\n', 'вывод("а\\qб")\n'):
            with self.subTest(текст=текст):
                with self.assertRaises(Exception) as нужно:
                    dan.перевести_python(текст)
                with self.assertRaises(type(нужно.exception)) as есть:
                    dan.перевести(текст)
                self.assertEqual(str(есть.exception), str(нужно.exception))

    def test_предупреждения(self):
        текст = "вывод(выходной(дата(2026, 5, 9)))\nвывод(рабочий_день(дата(2026, 5, 9)))\n"
        поток_python, поток_дана = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(поток_python):
            dan.перевести_python(текст)
        with contextlib.redirect_stderr(поток_дана):
            dan.перевести(текст)
        self.assertIn("программа: предупреждение: Строка 2, позиция 19: функция «рабочий_день» устарела",
                      поток_python.getvalue())
        self.assertEqual(поток_дана.getvalue(), поток_python.getvalue())

    def test_команда(self):
        вывод_ = io.StringIO()
        with contextlib.redirect_stdout(вывод_):
            self.assertEqual(dan.main(["--версия"]), 0)
        self.assertTrue(вывод_.getvalue().startswith("Дан "))


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыПоставки(unittest.TestCase):
    """Собранная поставка (dan.exe из PyInstaller): команды выполняет компилятор на Дане из папки
    «компилятор» рядом с dan.exe, без DAN_KOMPILYATOR."""

    def setUp(self):
        готовый = sam_kompilyator.исполняемый()
        временная = tempfile.TemporaryDirectory()
        self.addCleanup(временная.cleanup)
        self.папка = временная.name
        os.makedirs(os.path.join(self.папка, "компилятор"))
        имя = "дан" + (".exe" if os.name == "nt" else "")
        shutil.copy2(готовый, os.path.join(self.папка, "компилятор", имя))
        for патч in (mock.patch.object(sys, "frozen", True, create=True),
                     mock.patch.object(sys, "executable", os.path.join(self.папка, "dan.exe")),
                     mock.patch.object(sys, "_MEIPASS", sam_kompilyator.ПАПКА, create=True),
                     mock.patch.dict(os.environ, {"DAN_KOMPILYATOR": "", "DAN_SAM_EXE": ""})):
            патч.start()
            self.addCleanup(патч.stop)

    def main(self, *аргументы):
        вывод_ = io.StringIO()
        with contextlib.redirect_stdout(вывод_), contextlib.redirect_stderr(вывод_):
            код = dan.main(list(аргументы))
        return код, вывод_.getvalue()

    def test_включён_без_переменной(self):
        self.assertTrue(sam_kompilyator.включён())
        self.assertEqual(sam_kompilyator.исполняемый(), os.path.join(self.папка, "компилятор",
                                                                     "дан" + (".exe" if os.name == "nt" else "")))
        с = sam_kompilyator.среда()
        self.assertEqual(с["DAN_HOME"], sam_kompilyator.ПАПКА)
        путь = next(з for к, з in с.items() if к.upper() == "PATH")
        self.assertTrue(путь.startswith(self.папка + os.pathsep))
        with mock.patch.dict(os.environ, {"DAN_KOMPILYATOR": "python"}):
            self.assertFalse(sam_kompilyator.включён())

    def test_версия(self):
        код, текст = self.main("--версия")
        self.assertEqual(код, 0)
        self.assertTrue(текст.startswith("Дан "), текст)
        self.assertIn("Команды выполняет компилятор на Дане", текст)

    def test_справка(self):
        код, текст = self.main()
        self.assertEqual(код, 2)
        self.assertIn("dan модели проверить", текст)
        self.assertNotIn("python dan.py", текст)

    def test_новые_возможности(self):
        # плотный массив и векторная функция: их знает только компилятор на Дане
        путь = os.path.join(self.папка, "массив.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write("пусть а = массив(дроб32, 3, 0.5)\nвывод(скаляр(а, а))\n")
        код, текст = self.main("собрать", путь)
        self.assertEqual(код, 0, текст)
        exe = путь[:-4] + (".exe" if os.name == "nt" else "")
        р = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(р.stdout.strip(), "0.75")
        код, текст = self.main("проверить", путь)
        self.assertEqual(код, 0, текст)

    def test_команды_python(self):
        # «--для=» выполняет dan.py, но программу в C переводит компилятор на Дане
        self.assertIsNone(sam_kompilyator.выполнить_команду(["собрать", "п.dan", "--для=linux"]))
        self.assertIsNone(sam_kompilyator.выполнить_команду(["песочница"]))
        # значение для программы после имени файла - не параметр сборки
        with mock.patch.object(sam_kompilyator, "_запустить", lambda а: 5):
            self.assertEqual(sam_kompilyator.выполнить_команду(["запустить", "п.dan", "--веб"]), 5)


if __name__ == "__main__":
    unittest.main()
