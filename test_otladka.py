"""Тесты отладчика (python dan.py отладка программа.dan)."""

import glob
import os
import subprocess
import tempfile
import unittest

import dan
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ
import test_katalog as каталог

ПРОГРАММА = """тип Дело { номер: стр, сумма: цел }

функ удвоить(х: цел) -> цел {
    пусть р = х * 2
    вернуть р
}

функ итог(дела: [Дело]) -> цел {
    перем всего = 0
    для д в дела {
        всего += удвоить(д.сумма)
    }
    вернуть всего
}

пусть дела = [Дело("А-1", 10), Дело("Б-2", 20)]
пусть с = итог(дела)
вывод(с)
перем н = 0
пока н < 3 { н += 1 }
если пусть ч = разобрать_цел("7") { вывод(ч) }
вывод(10 / (н - 3))
"""


def отладить(текст, команды, файлы=None):
    with tempfile.TemporaryDirectory() as п:
        for имя, содержимое in (файлы or {}).items():
            with open(os.path.join(п, имя), "w", encoding="utf-8") as ф:
                ф.write(содержимое)
        путь = os.path.join(п, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исп = dan.собрать(путь, текст, отладка=True)
        р = subprocess.run([исп], input="\n".join(команды) + "\n", capture_output=True, text=True,
                           encoding="utf-8", timeout=60)
        return р.returncode, р.stdout, р.stderr


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыОтладчика(unittest.TestCase):

    def test_шаги_переменные_стек(self):
        код, вывод, ош = отладить(ПРОГРАММА, ["ш", "ш", "пк", "тч 4", "п", "показать х", "ст", "вы", "пк", "д", "",
                                              "текст", "п", "показать н", "ш", "п"])
        self.assertEqual(код, 1)
        части = вывод.split("(отладка) ")
        self.assertIn("программа.dan:16 (главная программа)", части[0])
        self.assertIn("->   17  пусть с = итог(дела)", части[1])
        self.assertIn("программа.dan:9 (итог)", части[2])
        self.assertIn('дела = [Дело(номер = "А-1", сумма = 10), Дело(номер = "Б-2", сумма = 20)]', части[3])
        self.assertIn("точка остановки: программа.dan:4", части[4])
        self.assertIn("программа.dan:4 (удвоить)", части[5])
        self.assertIn("х = 10", части[6])
        self.assertIn("удвоить, программа.dan:4\n  итог, программа.dan:11\n  главная программа, программа.dan:17", части[7])
        self.assertIn("всего = 20", части[9])
        self.assertIn('д = Дело(номер = "Б-2", сумма = 20)', части[9])
        self.assertIn("пусть р = х * 2", вывод)
        self.assertIn("Ошибка выполнения: деление на ноль", вывод)
        self.assertIn("н = 3", вывод)
        self.assertIn("после ошибки программа продолжиться не может", вывод)
        self.assertIn("Ошибка выполнения в строке 22: деление на ноль", ош)

    def test_точка_в_модуле_и_конец_ввода(self):
        модуль = "общий функ пени(сумма: цел) -> цел {\n    пусть доля = сумма / 10\n    вернуть доля\n}\n"
        код, вывод, ош = отладить('подключить "расчёты.dan"\nвывод(расчёты.пени(500))\nвывод("конец")\n',
                                  ["тч расчёты.dan:3", "п", "пк", "ст"], {"расчёты.dan": модуль})
        self.assertEqual(код, 0, ош)
        self.assertIn("расчёты.dan:3 (расчеты.пени)", вывод)
        self.assertIn("доля = 50", вывод)
        self.assertIn("сумма = 500", вывод)
        # команды закончились: программа выполняется до конца
        self.assertTrue(вывод.rstrip().endswith("50\nконец"), вывод[-200:])

    def test_программы_каталога_в_отладочной_сборке(self):
        """Без команд отладчика программа выполняется до конца и выводит то же, что обычно."""
        for путь in каталог.программы("программы")[:30]:
            основа = путь[:-4]
            if os.path.exists(основа + ".ввод"):
                continue
            with self.subTest(путь=путь):
                текст = каталог.прочитать(путь)
                with tempfile.TemporaryDirectory() as п:
                    копия = os.path.join(п, "п.dan")
                    with open(копия, "w", encoding="utf-8") as ф:
                        ф.write(текст)
                    исп = dan.собрать(копия, текст, отладка=True)
                    р = subprocess.run([исп], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                       encoding="utf-8", timeout=60)
                self.assertEqual(р.returncode, 0, р.stderr)
                ожидается = каталог.прочитать(основа + ".вывод")
                self.assertTrue(р.stdout.endswith(ожидается), р.stdout[-300:])


if __name__ == "__main__":
    unittest.main()
