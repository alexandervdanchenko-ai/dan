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

    @unittest.skipUnless(shutil.which("x86_64-w64-mingw32-gcc"), "нет MinGW")
    def test_сборка_для_windows(self):
        with tempfile.TemporaryDirectory() as папка:
            код, ошибки = собрать("x86_64-w64-mingw32-gcc", os.path.join(папка, "т.exe"))
            self.assertEqual(код, 0, ошибки)

    @unittest.skipUnless(shutil.which("aarch64-linux-gnu-gcc") and shutil.which("qemu-aarch64"), "нет сборки для ARM")
    def test_arm_в_qemu(self):
        with tempfile.TemporaryDirectory() as папка:
            выход = os.path.join(папка, "т")
            код, ошибки = собрать("aarch64-linux-gnu-gcc", выход, ["-static"])
            self.assertEqual(код, 0, ошибки)
            вывод = self.выполнить(["qemu-aarch64", "-cpu", "max", выход])
            self.assertIn("dotprod", вывод)
            self.выполнить(["qemu-aarch64", "-cpu", "cortex-a53", выход])


if __name__ == "__main__":
    unittest.main()
