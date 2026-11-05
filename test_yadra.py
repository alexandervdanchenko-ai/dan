"""Векторные ядра и пул потоков среды (sreda/yadra.c).

Тест на C (sreda/yadra_test.c) проверяет, что каждый вариант ядер, доступный
на этом процессоре (обычный C, AVX2, AVX-512, NEON, NEON с dotprod), даёт те же
биты, что обычный C, что пул выполняет каждую часть диапазона ровно один раз и
что ядра считают то, что должны. Если есть кросс-компиляторы, тот же тест
собирается для Windows (MinGW) и ARM (с запуском в qemu).
"""

import os
import shutil
import subprocess
import tempfile
import unittest

import dan
import sam_kompilyator

ПАПКА = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sreda")
ТЕСТ = os.path.join(ПАПКА, "yadra_test.c")
ФЛАГИ = ["-std=c11", "-O2", "-ffp-contract=off", "-Werror=implicit-function-declaration", "-w"]


def собрать(компилятор, выход, дополнительно=()):
    команда = [компилятор, *ФЛАГИ, ТЕСТ, "-o", выход, "-lm", *дополнительно]
    if os.name != "nt" and "mingw" not in компилятор:
        команда.append("-pthread")
    итог = subprocess.run(команда, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return итог.returncode, итог.stderr


class ТестыЯдер(unittest.TestCase):

    def выполнить(self, команда, окружение=None):
        итог = subprocess.run(команда, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              env={**os.environ, **(окружение or {})}, timeout=600)
        self.assertEqual(итог.returncode, 0, итог.stdout[-3000:] + итог.stderr[-2000:])
        self.assertIn("ok варианты:", итог.stdout)
        return итог.stdout

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_ядра_и_пул(self):
        with tempfile.TemporaryDirectory() as папка:
            for имя in ("gcc", "clang"):
                путь = shutil.which(имя)
                if not путь or (имя == "clang" and os.name == "nt"):
                    continue
                выход = os.path.join(папка, имя + (".exe" if os.name == "nt" else ""))
                код, ошибки = собрать(путь, выход)
                self.assertEqual(код, 0, ошибки)
                вывод = self.выполнить([выход])
                # один поток и выбор варианта переменной DAN_YADRA
                self.выполнить([выход], {"DAN_POTOKI": "1", "DAN_YADRA": "c"})
                print(имя + ":", вывод.strip().splitlines()[-1])

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_замер(self):
        """Печатает ГФлопс ядер по вариантам на одном потоке и на всех (для замеры/результаты.md)."""
        имя = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
        with tempfile.TemporaryDirectory() as папка:
            выход = os.path.join(папка, "b" + (".exe" if os.name == "nt" else ""))
            команда = [имя, "-std=c11", "-O3", "-ffp-contract=off", "-w", ТЕСТ, "-o", выход, "-lm"]
            if os.name != "nt":
                команда.append("-pthread")
            итог = subprocess.run(команда, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(итог.returncode, 0, итог.stderr)
            for потоков in ("1", ""):
                итог = subprocess.run([выход, "zamer"], capture_output=True, text=True, encoding="utf-8",
                                      errors="replace", env={**os.environ, "DAN_POTOKI": потоков}, timeout=600)
                self.assertEqual(итог.returncode, 0, итог.stderr)
                print("\nЗАМЕР ЯДЕР:", итог.stdout.strip().replace("\n", "\nЗАМЕР ЯДЕР: "))

    @unittest.skipUnless(shutil.which("x86_64-w64-mingw32-gcc"), "нет MinGW")
    def test_сборка_для_windows(self):
        with tempfile.TemporaryDirectory() as папка:
            код, ошибки = собрать("x86_64-w64-mingw32-gcc", os.path.join(папка, "t.exe"))
            self.assertEqual(код, 0, ошибки)

    @unittest.skipUnless(shutil.which("aarch64-linux-gnu-gcc") and shutil.which("qemu-aarch64"), "нет сборки для ARM")
    def test_arm_в_qemu(self):
        with tempfile.TemporaryDirectory() as папка:
            выход = os.path.join(папка, "t")
            код, ошибки = собрать("aarch64-linux-gnu-gcc", выход, ["-static"])
            self.assertEqual(код, 0, ошибки)
            вывод = self.выполнить(["qemu-aarch64", "-cpu", "max", выход])
            self.assertIn("dotprod", вывод)
            self.выполнить(["qemu-aarch64", "-cpu", "cortex-a53", выход])


ПРОГРАММА = """
пусть а = массив(дроб32, [1.0, 2.0, 3.0, 4.0])
пусть б = массив(дроб32, [0.5, 0.5, 0.5, 0.5])
вывод(скаляр(а, б))
вывод(матвек(массив(дроб32, [1.0, 0.0, 0.0, 1.0, 2.0, 2.0]), массив(дроб32, [3.0, 4.0])))
вывод(матмат(массив(дроб, [1.0, 2.0, 3.0, 4.0]), массив(дроб, [5.0, 6.0, 7.0, 8.0]), 2))
вывод(софтмакс(массив(дроб, [0.0, 0.0])))
вывод(нормировать_скв(массив(дроб32, [3.0, 4.0]), массив(дроб32, 0), 0.0))
вывод(нормировать_скв(массив(дроб, [3.0, 4.0]), массив(дроб, [1.0, 2.0]), 0.0))
вывод(нормировать_слой(массив(дроб, [1.0, 3.0]), массив(дроб, 0), массив(дроб, 0), 0.0))
вывод(нормировать_слой(массив(дроб32, [1.0, 3.0]), массив(дроб32, [2.0, 2.0]), массив(дроб32, [1.0, 1.0]), 0.0))
вывод(прибавить_кратное(а, 2.0, б))
перем х = массив(дроб32, 64)
для н в 0..63 { х[н] = н * 0.25 - 4.0 }
пусть к = квантовать_q8(х)
вывод(к.длина)
вывод(модуль(из_q8(к)[0] + 4.0) < 0.01)
пусть у = матвек_q8(к, массив(дроб32, 32, 1.0))
вывод(у.длина)
вывод(модуль(у[0] + 4.0) < 0.01 и модуль(у[1] - 252.0) < 0.1)
вывод(["c", "avx2", "avx512", "neon", "dotprod"].содержит(ядра_процессора()))
вывод(число_потоков() >= 1)
// большие размеры: на всех ядрах, тот же результат при одном потоке
перем м = массив(дроб32, 512 * 300)
для н в 0..м.длина - 1 { м[н] = (н % 17) * 0.125 - 1.0 }
перем вк = массив(дроб32, 300)
для н в 0..299 { вк[н] = (н % 5) * 0.5 }
пусть р = матвек(м, вк)
вывод(р[0] + р[511])
пусть п = матмат(м, м, 300)
вывод(п.длина)
"""

ОЖИДАНИЕ = """5.0
[3.0, 4.0, 14.0]
[19.0, 22.0, 43.0, 50.0]
[0.5, 0.5]
[0.84852815, 1.1313709]
[0.848528137423857, 2.262741699796952]
[-1.0, 1.0]
[-1.0, 3.0]
[2.0, 3.0, 4.0, 5.0]
68
истина
2
истина
истина
истина
"""


def запустить(текст, окружение=None):
    with tempfile.TemporaryDirectory() as папка:
        путь = os.path.join(папка, "p.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        return subprocess.run([sam_kompilyator.исполняемый(), "запустить", путь], capture_output=True,
                              env={**sam_kompilyator.среда(), **(окружение or {})}, timeout=900)


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыФункцийЯдер(unittest.TestCase):
    """Функции Дана над ядрами (только компилятор на Дане)."""

    def test_функции(self):
        выводы = []
        for окружение in ({}, {"DAN_POTOKI": "1", "DAN_YADRA": "c"}):
            р = запустить(ПРОГРАММА, окружение)
            вывод = р.stdout.decode("utf-8").replace("\r\n", "\n")
            self.assertEqual(р.returncode, 0, вывод + р.stderr.decode("utf-8", "replace"))
            self.assertTrue(вывод.startswith(ОЖИДАНИЕ), вывод)
            выводы.append(вывод)
        # любой вариант ядер и любое число потоков - те же биты
        self.assertEqual(выводы[0], выводы[1])

    def test_дроб16(self):
        текст = """
пусть а = массив(дроб16, [1.0, 2.0, 0.5, -4.0])
пусть б = массив(дроб16, [2.0, 0.25, 4.0, 1.0])
вывод(скаляр(а, б))
вывод(матвек(а, массив(дроб32, [1.0, 1.0])))
вывод(в_дроб32(а))
вывод(в_дроб16(массив(дроб32, [0.1, 65504.0])))
перем вх = массив(дроб32, 3000)
для н в 0..2999 { вх[н] = (н % 7) * 0.5 }
пусть п = в_дроб16(вх)
вывод(скаляр(п, п) == скаляр(в_дроб32(п), в_дроб32(п)))
"""
        р = запустить(текст)
        вывод = р.stdout.decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(р.returncode, 0, вывод + р.stderr.decode("utf-8", "replace"))
        self.assertEqual(вывод, "0.5\n[3.0, -3.5]\n[1.0, 2.0, 0.5, -4.0]\n[0.1, 65500.0]\nистина\n")
        р = запустить("вывод(в_дроб16(массив(дроб32, [70000.0])))")
        self.assertEqual(р.returncode, 1)
        self.assertIn("70000", р.stderr.decode("utf-8", "replace"))

    def test_приближённо(self):
        текст = """
перем а = массив(дроб32, 64 * 64)
для н в 0..а.длина - 1 { а[н] = (н % 13) * 0.0625 - 0.3 }
пусть точно = матмат(а, а, 64)
перем быстро = массив(дроб32, 0)
перем с = 0.0
приближённо {
    быстро = матмат(а, а, 64)
    с = скаляр(а, а)
    вывод(матвек(а, а[0..63]).длина)
}
перем разница = 0.0
для н в 0..точно.длина - 1 { разница = макс(разница, модуль(точно[н] - быстро[н])) }
вывод(разница < 0.001)
вывод(модуль(с - скаляр(а, а)) < 0.01)
// слово не зарезервировано
пусть приближённо = 5
вывод(приближённо + 1)
"""
        р = запустить(текст)
        вывод = р.stdout.decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(р.returncode, 0, вывод + р.stderr.decode("utf-8", "replace"))
        self.assertEqual(вывод, "64\nистина\nистина\n6\n")
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "p.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            си = subprocess.run([sam_kompilyator.исполняемый(), "си", путь], capture_output=True,
                                env=sam_kompilyator.среда(), timeout=900).stdout.decode("utf-8")
        self.assertIn("dan_k_matmat_f32_fast(", си)
        self.assertIn("dan_k_dot_f32_fast(", си)
        self.assertIn("dan_k_matmat_f32(", си.replace("dan_k_matmat_f32(const", ""))

    def test_ошибки(self):
        for текст, сообщение in [
            ("вывод(скаляр(массив(дроб32, 3), массив(дроб32, 4)))", "скаляр: массивы разной длины (3 и 4)"),
            ("вывод(матвек(массив(дроб, 5), массив(дроб, 2)))", "длина матрицы (5) должна делиться на длину вектора (2)"),
            ("вывод(матмат(массив(дроб, 6), массив(дроб, 4), 3))", "длина второй матрицы (4) не делится на 3"),
            ("вывод(матмат(массив(дроб, 6), массив(дроб, 4), 0))", "должно быть положительным"),
            ("вывод(квантовать_q8(массив(дроб32, 33)))", "длина массива (33) должна делиться на 32"),
            ("вывод(из_q8(массив(байт, 35)))", "должна делиться на 34"),
            ("перем м = массив(дроб32, 32)\nм[3] = 1.0e7\nвывод(квантовать_q8(м).длина)",
             "элемент 3 равен 1e+07, а в весах в 8 битах могут быть только конечные числа"),
            ("вывод(нормировать_скв(массив(дроб32, 3), массив(дроб32, 2), 0.0))", "должна быть 0 или длиной массива (3)"),
        ]:
            р = запустить(текст)
            self.assertEqual(р.returncode, 1, текст)
            self.assertIn(сообщение, р.stderr.decode("utf-8", "replace"), текст)


if __name__ == "__main__":
    unittest.main()
