"""Профиль программы: dan профиль замеряет время и число вызовов каждой функции."""

import os
import subprocess
import sys
import tempfile
import unittest

import dan

ПАПКА_ДАНА = os.path.dirname(os.path.abspath(__file__))
КОМПИЛЯТОР_ЕСТЬ = dan.компилятор_есть()

ПРОГРАММА = '''подключить "счёт"

функ фиб(н: цел) -> цел {
    если н < 2 { вернуть н }
    вернуть фиб(н - 1) + фиб(н - 2)
}

функ Точка.сдвиг(шаг: цел) -> цел {
    вернуть шаг + 1
}

тип Точка { х: цел }

функ показать() {
    пусть т = Точка(х = 1)
    вывод(т.сдвиг(2))
}

пусть первая = запустить фиб(15)
пусть вторая = запустить фиб(16)
вывод(первая.дождаться() + вторая.дождаться())
вывод(фиб(20))
показать()
вывод(счёт.сумма([1, 2, 3]))
'''

СЧЁТ = '''общий функ сумма(с: [цел]) -> цел {
    перем итог = 0
    для х в с { итог += х }
    вернуть итог
}
'''


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыПрофиля(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name
        for имя, текст in (("п.dan", ПРОГРАММА), ("счёт.dan", СЧЁТ)):
            with open(os.path.join(self.п, имя), "w", encoding="utf-8") as ф:
                ф.write(текст)

    def tearDown(self):
        self.tmp.cleanup()

    def dan(self, *аргументы):
        return subprocess.run([sys.executable, os.path.join(ПАПКА_ДАНА, "dan.py"), *аргументы],
                              capture_output=True, text=True, encoding="utf-8", cwd=self.п, timeout=300)

    def строки_отчёта(self, вывод):
        итог = {}
        for с in вывод.strip().splitlines()[2:]:
            части = с.split()
            if len(части) < 7 or not части[5].isdigit():
                continue                     # пояснения под таблицей
            # своё время (2 слова), доля, всего (2 слова), вызовов, функция ...
            итог[" ".join(части[6:])] = int(части[5])
        return итог

    def test_число_вызовов_и_места(self):
        р = self.dan("профиль", "п.dan")
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        # вывод программы не смешан с отчётом
        self.assertEqual(р.stdout, "1597\n6765\n3\n6\n")
        self.assertIn("Профиль: программа работала", р.stderr)
        вызовы = self.строки_отчёта(р.stderr)
        # фиб(15) - 1973 вызова, фиб(16) - 3193, фиб(20) - 21891; задачи считаются вместе с основной частью
        self.assertEqual(вызовы["фиб (п.dan:3)"], 1973 + 3193 + 21891)
        self.assertEqual(вызовы["Точка.сдвиг (п.dan:8)"], 1)
        self.assertEqual(вызовы["показать (п.dan:14)"], 1)
        self.assertEqual(вызовы["(основная часть программы)"], 1)
        self.assertTrue(any(к.endswith("сумма (счёт.dan:1)") for к in вызовы), вызовы)

    def test_время_рекурсии_не_удваивается(self):
        # без задач: у параллельных задач время складывается и может быть больше времени программы
        with open(os.path.join(self.п, "р.dan"), "w", encoding="utf-8") as ф:
            ф.write("функ фиб(н: цел) -> цел {\n    если н < 2 { вернуть н }\n    вернуть фиб(н - 1) + фиб(н - 2)\n}\n"
                    "вывод(фиб(22))\n")
        р = self.dan("профиль", "р.dan")
        for с in р.stderr.splitlines():
            if "фиб (" in с:
                части = с.split()
                # «всего» у рекурсивной функции не больше времени работы всей программы
                всего = float(части[3].replace(",", ".")) * {"с": 1e9, "мс": 1e6, "мкс": 1e3}[части[4]]
        первая = р.stderr.strip().splitlines()[0].split("работала ")[1].split(".")[0].split()
        программа = float(первая[0].replace(",", ".")) * {"с": 1e9, "мс": 1e6, "мкс": 1e3}[первая[1]]
        self.assertLessEqual(всего, программа * 1.01 + 1000)

    def test_ошибка_выполнения_не_мешает_отчёту(self):
        with open(os.path.join(self.п, "о.dan"), "w", encoding="utf-8") as ф:
            ф.write("функ д(н: цел) -> цел { вернуть 10 % н }\nвывод(д(2))\nвывод(д(0))\n")
        р = self.dan("профиль", "о.dan")
        self.assertEqual(р.returncode, 1)
        self.assertIn("остаток от деления на ноль", р.stderr)
        self.assertIn("д (о.dan:1)", р.stderr)

    def test_значения_для_программы_и_обычная_сборка_без_замеров(self):
        with open(os.path.join(self.п, "а.dan"), "w", encoding="utf-8") as ф:
            ф.write("вывод(аргументы())\n")
        р = self.dan("профиль", "а.dan", "один", "два")
        self.assertEqual(р.stdout, '["один", "два"]\n')
        код = dan.перевести(ПРОГРАММА, os.path.join(self.п, "п.dan"))
        self.assertNotIn("dan_prof", код)
        self.assertIn("dan_prof_enter", dan.перевести(ПРОГРАММА, os.path.join(self.п, "п.dan"), профиль=True))

    def test_без_программы(self):
        self.assertEqual(self.dan("профиль").returncode, 2)


if __name__ == "__main__":
    unittest.main()
