"""Проверка тестами через компилятор на Дане (sam_kompilyator.py, DAN_KOMPILYATOR=сам)."""

import contextlib
import io
import os
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


if __name__ == "__main__":
    unittest.main()
