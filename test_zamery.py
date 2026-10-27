"""Замеры: блок замер "название" { ... } и команда dan замерить."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

import dan
from parser import ОшибкаРазбора

ПАПКА_ДАНА = os.path.dirname(os.path.abspath(__file__))

ПРОГРАММА = '''функ сумма(н: цел) -> цел {
    перем с = 0
    для к в 1..н { с += к }
    вернуть с
}
пусть замер = 3
вывод(сумма(замер))

замер "сумма до тысячи" {
    сумма(1000)
}

замер "строки" {
    перем т = ""
    для к в 1..50 { т += стр(к) }
}

замер "ошибка" {
    пусть н = 0
    вывод(5 % н)
}

тест "сумма" {
    утвердить_равно(сумма(3), 6)
}
'''


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыЗамеров(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name
        self.кэш = os.path.join(self.п, "кэш")

    def tearDown(self):
        self.tmp.cleanup()

    def файл(self, имя, текст):
        путь = os.path.join(self.п, имя)
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        return путь

    def dan(self, *аргументы):
        return subprocess.run([sys.executable, os.path.join(ПАПКА_ДАНА, "dan.py"), *аргументы],
                              capture_output=True, text=True, encoding="utf-8", cwd=self.п, timeout=300,
                              env=dict(os.environ, DAN_CACHE=self.кэш))

    def test_замеры_и_сравнение_с_прошлым(self):
        путь = self.файл("п.dan", ПРОГРАММА)
        р = self.dan("замерить", путь)
        self.assertEqual(р.returncode, 1, р.stdout + р.stderr)     # один замер с ошибкой
        строки = р.stdout.splitlines()
        self.assertEqual(строки[0], f"== {путь}")
        self.assertRegex(строки[1], r"^  сумма до тысячи: [\d,]+ (нс|мкс|мс) \(разброс ±[\d,]+%, выполнено раз: \d+\)$")
        self.assertRegex(строки[2], r"^  строки: ")
        self.assertEqual(строки[3], "  ошибка: ОШИБКА (п.dan, строка 20): остаток от деления на ноль")
        self.assertEqual(len(строки), 4)
        р = self.dan("замерить", путь)
        self.assertRegex(р.stdout, r"сумма до тысячи: .*; в прошлый раз [\d,]+ (нс|мкс|мс)")

    def test_обычный_запуск_и_тесты_замеры_не_выполняют(self):
        путь = self.файл("п.dan", ПРОГРАММА)
        р = self.dan("запустить", путь)
        self.assertEqual(р.returncode, 0, р.stderr)
        self.assertEqual(р.stdout, "6\n")
        р = self.dan("тестировать", "--покрытие", путь)
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertIn("Тестов: 1, пройдено: 1, провалено: 0", р.stdout)
        # строки замеров, как и строки тестов, в покрытии не учитываются
        self.assertIn("Покрытие: тесты выполнили все строки программы (3)", р.stdout)

    def test_нет_замеров(self):
        путь = self.файл("б.dan", 'вывод(1)\n')
        р = self.dan("замерить", путь)
        self.assertEqual(р.returncode, 1)
        self.assertIn("в программе нет замеров", р.stdout)

    def test_замер_без_названия(self):
        with self.assertRaises(ОшибкаРазбора) as о:
            dan.перевести('замер "" { вывод(1) }\n')
        self.assertIn("название", str(о.exception))

    def test_отчёт_сравнения(self):
        путь = os.path.join(self.п, "данные.txt")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write("а\t1000\t10\t990\t5000\nб\t2000\t20\t1990\t100\nв\t1500\t15\t1490\t100\n")
        строки, новые = dan.отчёт_замеров(путь, {"а": [2000, 1.0], "б": [1000, 1.0], "в": [1510, 1.0]})
        self.assertEqual(строки, [
            "  а: 1,0 мкс (разброс ±1,0%, выполнено раз: 5000); в прошлый раз 2,0 мкс: быстрее в 2,00 раза",
            "  б: 2,0 мкс (разброс ±1,0%, выполнено раз: 100); в прошлый раз 1,0 мкс: медленнее в 2,00 раза",
            "  в: 1,5 мкс (разброс ±1,0%, выполнено раз: 100); в прошлый раз 1,5 мкс, разница 0,7% в пределах разброса",
        ])
        self.assertEqual(json.loads(json.dumps(новые))["а"], [1000.0, 1.0])


if __name__ == "__main__":
    unittest.main()
