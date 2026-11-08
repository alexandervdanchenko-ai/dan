"""Папка Дана (с папками sreda и библиотека): компилятор на Дане находит её сам.

Порядок поиска (сам/дан.dan, функция папка_дана): переменная DAN_HOME, затем папка исполняемого файла
(поставка) или папка выше (semya/dan в папке проекта), затем текущая папка. Если папки нет, команды,
которым она нужна, сообщают причину и решение, а не внутреннюю «Ошибку выполнения в строке N».
"""

import os
import shutil
import subprocess
import tempfile
import unittest

import sam_kompilyator
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ПАПКА = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"
ИМЯ = "dan.exe" if WINDOWS else "dan"
ПРОГРАММА = 'подключить "json"\nвывод(json.записать(json.Число(2.0)))\nвывод(сжать_gzip(байты("а")).длина > 0)\n'


def без_dan_home(**ещё):
    с = {к: з for к, з in os.environ.items() if к != "DAN_HOME"}
    с.update(ещё)
    return с


def выполнить(компилятор, *аргументы, cwd, env):
    р = subprocess.run([компилятор, *аргументы], capture_output=True, cwd=cwd, env=env, timeout=600)
    return (р.returncode, р.stdout.decode("utf-8", "replace").replace("\r\n", "\n"),
            р.stderr.decode("utf-8", "replace").replace("\r\n", "\n"))


class ТестыПапкиДана(unittest.TestCase):

    def setUp(self):
        self.временная = tempfile.TemporaryDirectory()
        self.корень = self.временная.name
        # рабочая папка программы - без sreda и библиотеки
        self.рабочая = os.path.join(self.корень, "работа")
        os.makedirs(self.рабочая)
        with open(os.path.join(self.рабочая, "п.dan"), "w", encoding="utf-8") as ф:
            ф.write(ПРОГРАММА)

    def tearDown(self):
        self.временная.cleanup()

    def копия_компилятора(self, папка):
        os.makedirs(папка, exist_ok=True)
        путь = os.path.join(папка, ИМЯ)
        shutil.copy2(sam_kompilyator.исполняемый(), путь)
        return путь

    def папка_дана(self, папка):
        for имя in ("sreda", "библиотека"):
            shutil.copytree(os.path.join(ПАПКА, имя), os.path.join(папка, имя),
                            ignore=shutil.ignore_patterns("*.o", "__pycache__"))

    def проверить_работает(self, компилятор, env):
        код, вывод_, ошибки = выполнить(компилятор, "проверить", "п.dan", cwd=self.рабочая, env=env)
        self.assertEqual(код, 0, вывод_ + ошибки)
        if КОМПИЛЯТОР_ЕСТЬ:
            код, вывод_, ошибки = выполнить(компилятор, "запустить", "п.dan", cwd=self.рабочая, env=env)
            self.assertEqual((код, вывод_), (0, "2.0\nистина\n"), ошибки)

    def test_папка_выше_исполняемого_файла(self):
        # semya/dan в папке проекта, вызов из другой папки без DAN_HOME
        проект = os.path.join(self.корень, "проект")
        компилятор = self.копия_компилятора(os.path.join(проект, "semya"))
        self.папка_дана(проект)
        self.проверить_работает(компилятор, без_dan_home())

    def test_папка_исполняемого_файла(self):
        # поставка: sreda и библиотека рядом с dan
        поставка = os.path.join(self.корень, "поставка")
        компилятор = self.копия_компилятора(поставка)
        self.папка_дана(поставка)
        self.проверить_работает(компилятор, без_dan_home())

    def test_с_dan_home(self):
        компилятор = self.копия_компилятора(os.path.join(self.корень, "один"))
        self.проверить_работает(компилятор, без_dan_home(DAN_HOME=ПАПКА))

    def test_папка_не_найдена(self):
        компилятор = self.копия_компилятора(os.path.join(self.корень, "один"))
        for аргументы in (("запустить", "п.dan"), ("собрать", "п.dan"), ("си", "п.dan"), ("проверить", "п.dan"),
                          ("тестировать", "п.dan"), ("задачи",)):
            with self.subTest(аргументы=аргументы):
                код, вывод_, ошибки = выполнить(компилятор, *аргументы, cwd=self.рабочая, env=без_dan_home())
                self.assertEqual(код, 2, вывод_ + ошибки)
                self.assertNotIn("Ошибка выполнения в строке", вывод_ + ошибки)
                self.assertIn("не найдена папка Дана (в ней папки sreda и библиотека)", ошибки)
                self.assertIn("запустите компилятор из папки проекта или задайте переменную среды DAN_HOME", ошибки)
        self.assertFalse(os.path.exists(os.path.join(self.рабочая, "задачи")))
        # --версия и формат папки Дана не требуют
        код, вывод_, _ = выполнить(компилятор, "--версия", cwd=self.рабочая, env=без_dan_home())
        self.assertEqual(код, 0)
        self.assertTrue(вывод_.startswith("Дан "))
        код, _, _ = выполнить(компилятор, "формат", "п.dan", "--проверить", cwd=self.рабочая, env=без_dan_home())
        self.assertNotEqual(код, 2)

    def test_dan_home_без_папок(self):
        компилятор = self.копия_компилятора(os.path.join(self.корень, "один"))
        пустая = os.path.join(self.корень, "пустая")
        os.makedirs(пустая)
        код, вывод_, ошибки = выполнить(компилятор, "запустить", "п.dan", cwd=self.рабочая,
                                         env=без_dan_home(DAN_HOME=пустая))
        self.assertEqual(код, 2)
        self.assertNotIn("Ошибка выполнения в строке", вывод_ + ошибки)
        self.assertIn("из переменной DAN_HOME нет папок sreda и библиотека", ошибки)

    @unittest.skipIf(WINDOWS or not КОМПИЛЯТОР_ЕСТЬ, "кэш проверяется оболочкой в Linux и macOS")
    def test_недоступный_кэш(self):
        # папку кэша нельзя создать: на её месте - обычный файл; программа собирается без готовой среды
        файл = os.path.join(self.корень, "файл")
        with open(файл, "w") as ф:
            ф.write("x")
        env = dict(sam_kompilyator.среда(), DAN_CACHE=os.path.join(файл, "кэш"))
        код, вывод_, ошибки = выполнить(sam_kompilyator.исполняемый(), "запустить", "п.dan", cwd=self.рабочая, env=env)
        self.assertEqual((код, вывод_), (0, "2.0\nистина\n"), ошибки)


if __name__ == "__main__":
    unittest.main()
