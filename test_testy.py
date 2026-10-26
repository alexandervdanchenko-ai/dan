"""Встроенные тесты: блок тест "название" { ... } и команда dan тестировать."""

import os
import subprocess
import sys
import tempfile
import unittest

import dan
from parser import ОшибкаРазбора

ПАПКА_ДАНА = os.path.dirname(os.path.abspath(__file__))

ПРОГРАММА = '''функ квадрат(х: цел) -> цел { вернуть х * х }
пусть тест = 5
вывод(квадрат(тест))

тест "квадрат трёх" {
    утвердить(квадрат(3) == 9)
}

тест "неверное ожидание" {
    утвердить_равно(квадрат(4), 15)
}

тест "пояснение и строки" {
    утвердить_равно("а" + "б", "аб")
    утвердить(ложь, "особое пояснение")
}

тест "ошибка выполнения внутри" {
    пусть н = 0
    вывод(5 % н)
}

тест "дробные и логические" {
    утвердить_равно(0.5 + 0.25, 0.75)
    утвердить_равно(квадрат(2) == 4, истина)
}
'''


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыВстроенныхТестов(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def файл(self, имя, текст):
        путь = os.path.join(self.п, имя)
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        return путь

    def dan(self, *аргументы):
        return subprocess.run([sys.executable, os.path.join(ПАПКА_ДАНА, "dan.py"), *аргументы],
                              capture_output=True, text=True, encoding="utf-8", cwd=self.п, timeout=300)

    def test_тесты_выполняются_и_провалы_не_останавливают_остальные(self):
        путь = self.файл("п.dan", ПРОГРАММА)
        р = self.dan("тестировать", путь)
        self.assertEqual(р.returncode, 1, р.stdout + р.stderr)
        строки = р.stdout.splitlines()[1:]
        self.assertEqual(строки, [
            "пройден: квадрат трёх",
            "ПРОВАЛЕН: неверное ожидание (строка 10): ожидалось 15, получено 16",
            "ПРОВАЛЕН: пояснение и строки (строка 15): особое пояснение",
            "ПРОВАЛЕН: ошибка выполнения внутри (строка 20): остаток от деления на ноль",
            "пройден: дробные и логические",
            "Тестов: 5, пройдено: 2, провалено: 3",
        ])

    def test_обычный_запуск_тесты_не_выполняет(self):
        путь = self.файл("п.dan", ПРОГРАММА)
        р = self.dan("запустить", путь)
        self.assertEqual(р.returncode, 0, р.stderr)
        self.assertEqual(р.stdout, "25\n")

    def test_все_тесты_пройдены(self):
        путь = self.файл("а.dan", 'функ удвоить(х: цел) -> цел { вернуть х * 2 }\n'
                                  'тест "удвоение" { утвердить_равно(удвоить(21), 42) }\n')
        р = self.dan("тестировать", путь)
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertIn("Тестов: 1, пройдено: 1, провалено: 0", р.stdout)

    def test_папка_и_подключённый_модуль(self):
        self.файл("счёт.dan", 'общий функ сумма(с: [цел]) -> цел {\n    перем итог = 0\n    для х в с { итог += х }\n'
                              '    вернуть итог\n}\nтест "сумма в модуле" { утвердить_равно(сумма([1, 2, 3]), 6) }\n')
        self.файл("главная.dan", 'подключить "счёт"\nвывод(счёт.сумма([4]))\n'
                                 'тест "сумма из главной" { утвердить_равно(счёт.сумма([]), 0) }\n')
        self.файл("без_тестов.dan", 'вывод(1)\n')
        р = self.dan("тестировать")
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertIn("пройден: сумма из главной", р.stdout)
        self.assertIn("пройден: сумма в модуле", р.stdout)

    def test_нет_тестов(self):
        путь = self.файл("б.dan", 'вывод(1)\n')
        р = self.dan("тестировать", путь)
        self.assertEqual(р.returncode, 1)
        self.assertIn("в программе нет тестов", р.stdout)

    def test_ошибка_проверки_в_тесте_видна_и_при_обычной_сборке(self):
        текст = 'вывод(1)\nтест "тип" { утвердить(1) }\n'
        with self.assertRaises(Exception) as о:
            dan.перевести(текст)
        self.assertIn("утвердить", str(о.exception))

    def test_тест_без_названия(self):
        with self.assertRaises(ОшибкаРазбора) as о:
            dan.перевести('тест "" { утвердить(истина) }\n')
        self.assertIn("название", str(о.exception))


if __name__ == "__main__":
    unittest.main()
