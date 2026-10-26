"""Гарантия совместимости: программы, записанные для редакции языка, выполняются
каждой следующей версией компилятора так же, как при их записи.

Папка «совместимость/<редакция>» - замороженная копия каталога программ
(см. совместимость/README.md). Её файлы не изменяются: это проверяют
контрольные суммы из файла СУММЫ.
"""

import glob
import hashlib
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

import dan
import versiya
from parser import разобрать, ОшибкаРазбора
from test_katalog import КОМПИЛЯТОР_ЕСТЬ, выполнить, ошибка_компиляции, прочитать

ПАПКА = os.path.join(os.path.dirname(os.path.abspath(__file__)), "совместимость")


def редакции():
    return sorted(и for и in os.listdir(ПАПКА) if os.path.isdir(os.path.join(ПАПКА, и)))


def программы(редакция, под):
    return sorted(п for п in glob.glob(os.path.join(ПАПКА, редакция, под, "*.dan"))
                  if not п.endswith(".исправлено.dan"))


class ТестыСоставаПапкиСовместимости(unittest.TestCase):

    def test_папка_есть_для_каждой_редакции(self):
        self.assertEqual(редакции(), sorted(versiya.РЕДАКЦИИ))

    def test_файлы_не_изменены(self):
        for редакция in редакции():
            корень = os.path.join(ПАПКА, редакция)
            записано = {}
            for строка in прочитать(os.path.join(корень, "СУММЫ")).splitlines():
                сумма, путь = строка.split("  ", 1)
                записано[путь] = сумма
            есть = {f"{под}/{имя}" for под in ("программы", "ошибки")
                    for имя in os.listdir(os.path.join(корень, под))}
            self.assertEqual(есть, set(записано), "в папке совместимости добавлены или удалены файлы")
            for путь, сумма in записано.items():
                with self.subTest(путь=путь), open(os.path.join(корень, путь), "rb") as ф:
                    self.assertEqual(hashlib.sha256(ф.read()).hexdigest(), сумма,
                                     "файл папки совместимости изменён; такие файлы не изменяются")

    def test_программы_указывают_редакцию(self):
        for редакция in редакции():
            for путь in программы(редакция, "программы"):
                with self.subTest(путь=путь):
                    self.assertEqual(versiya.строка_редакции(прочитать(путь)), (1, редакция))

    def test_неверные_программы_по_прежнему_отклоняются(self):
        for редакция in редакции():
            for путь in программы(редакция, "ошибки"):
                ожидается = прочитать(путь[:-4] + ".сообщение").strip()
                if ожидается.startswith("Ошибка выполнения"):
                    continue
                with self.subTest(путь=путь):
                    self.assertIsNotNone(ошибка_компиляции(прочитать(путь)))
                    self.assertIsNone(ошибка_компиляции(прочитать(путь[:-4] + ".исправлено.dan")))


class ТестыРедакций(unittest.TestCase):

    def test_указание_редакции(self):
        self.assertEqual(разобрать("редакция 2026\nвывод(1)").редакция, "2026")
        self.assertEqual(разобрать("// описание\n\nредакция 2026 // комментарий\nвывод(1)").редакция, "2026")
        self.assertIsNone(разобрать("вывод(1)").редакция)
        # «редакция» остаётся обычным именем
        self.assertIsNone(ошибка_компиляции("перем редакция = 1\nредакция = 2026\nвывод(редакция)"))

    def test_ошибки_в_указании(self):
        for текст, часть in [("редакция 2031\nвывод(1)", "обновите компилятор"),
                             ("редакция 1999\nвывод(1)", "неизвестная редакция языка «1999»"),
                             ("вывод(1)\nредакция 2026", "пишется первой строкой файла")]:
            with self.subTest(текст=текст), self.assertRaises(ОшибкаРазбора) as о:
                разобрать(текст)
            self.assertIn(часть, str(о.exception))

    def test_редакция_модуля_проверяется(self):
        with tempfile.TemporaryDirectory() as п:
            with open(os.path.join(п, "м.dan"), "w", encoding="utf-8") as ф:
                ф.write("редакция 2099\nобщий функ ф() -> цел { вернуть 1 }\n")
            путь = os.path.join(п, "главная.dan")
            текст = 'редакция 2026\nподключить "м"\nвывод(м.ф())\n'
            with self.assertRaises(Exception) as о:
                dan.перевести(текст, путь)
            self.assertIn("2099", str(о.exception))

    def test_перевод_по_цепочке(self):
        поддельные = {"2026": "первая", "2027": "вторая", "2028": "третья"}
        переводы = {("2026", "2027"): lambda т: т.replace("старое", "новое"),
                    ("2027", "2028"): lambda т: т + "// 2028\n"}
        with mock.patch.object(versiya, "РЕДАКЦИИ", поддельные), mock.patch.object(versiya, "МИГРАЦИИ", переводы), \
                mock.patch.object(versiya, "ТЕКУЩАЯ_РЕДАКЦИЯ", "2028"):
            текст, шаги = versiya.перевести("// описание\nвывод(\"старое\")\n")
            self.assertEqual(шаги, ["2026 -> 2027", "2027 -> 2028"])
            self.assertEqual(текст, "редакция 2028\n// описание\nвывод(\"новое\")\n// 2028\n")
            текст, шаги = versiya.перевести("редакция 2027\nвывод(\"старое\")\n", "2028")
            self.assertEqual((текст, шаги), ("редакция 2028\nвывод(\"старое\")\n// 2028\n", ["2027 -> 2028"]))
            with self.assertRaises(versiya.ОшибкаРедакции):
                versiya.перевести("редакция 2028\n", "2026")

    def test_команды(self):
        вывод = io.StringIO()
        with redirect_stdout(вывод):
            self.assertEqual(dan.main(["--версия"]), 0)
        self.assertIn(f"Дан {versiya.ВЕРСИЯ}", вывод.getvalue())
        with tempfile.TemporaryDirectory() as п:
            путь = os.path.join(п, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write("// описание\nвывод(1)\n")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(dan.main(["редакция", путь, versiya.ТЕКУЩАЯ_РЕДАКЦИЯ]), 0)
                self.assertEqual(dan.main(["редакция", путь]), 0)
                self.assertEqual(dan.main(["редакция", путь, "1999"]), 1)
            self.assertTrue(прочитать(путь).startswith(f"редакция {versiya.ТЕКУЩАЯ_РЕДАКЦИЯ}\n// описание"))


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыВыполненияПапкиСовместимости(unittest.TestCase):

    def test_программы_выводят_то_же(self):
        for редакция in редакции():
            for путь in программы(редакция, "программы"):
                основа = путь[:-4]
                ввод_данных = прочитать(основа + ".ввод") if os.path.exists(основа + ".ввод") else ""
                with self.subTest(путь=путь):
                    код, вывод, ошибки = выполнить(прочитать(путь), ввод_данных)
                    self.assertEqual(код, 0, ошибки)
                    self.assertEqual(вывод.splitlines(), прочитать(основа + ".вывод").splitlines())

    def test_ошибки_выполнения_остаются_ошибками(self):
        for редакция in редакции():
            for путь in программы(редакция, "ошибки"):
                основа = путь[:-4]
                with self.subTest(путь=путь):
                    if прочитать(основа + ".сообщение").strip().startswith("Ошибка выполнения"):
                        код, _, ошибки = выполнить(прочитать(путь))
                        self.assertEqual(код, 1)
                        self.assertIn("Ошибка выполнения", ошибки)
                    код, вывод, ошибки = выполнить(прочитать(основа + ".исправлено.dan"))
                    self.assertEqual(код, 0, ошибки)
                    self.assertEqual(вывод.splitlines(), прочитать(основа + ".исправлено.вывод").splitlines())


if __name__ == "__main__":
    unittest.main()
