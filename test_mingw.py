"""Сборка компилятором MinGW, установленным в папку с пробелом в имени.

GCC в Windows подставляет путь к служебному файлу default-manifest.o без
кавычек, и при пути вида C:\\Users\\Имя Фамилия\\... компоновщик разрывает его
на пробеле. Дан передаёт GCC параметр -B с коротким именем (8.3) этой папки.
"""

import os
import subprocess
import unittest
from unittest import mock

import dan

ДЛИННЫЙ = ("C:/Users/Alexander Danchenko/AppData/Local/Microsoft/WinGet/Packages/WinLibs/mingw64/bin/"
           "../lib/gcc/x86_64-w64-mingw32/16.2.0/../../../../x86_64-w64-mingw32/lib/../lib/default-manifest.o")
КОРОТКИЙ = "C:\\Users\\ALEXAN~1\\AppData\\Local\\MICROS~1\\WinGet\\Packages\\WinLibs\\mingw64\\X86_64~1\\lib"


def ответ_gcc(команда, **_):
    return subprocess.CompletedProcess(команда, 0, stdout=ДЛИННЫЙ + "\n", stderr="")


class ТестыMinGW(unittest.TestCase):
    def setUp(self):
        dan._ФЛАГИ_MINGW.clear()
        self.addCleanup(dan._ФЛАГИ_MINGW.clear)

    def test_не_windows_без_параметров(self):
        with mock.patch.object(dan.os, "name", "posix"):
            self.assertEqual(dan.флаги_mingw("gcc"), [])

    def test_путь_с_пробелом_даёт_короткую_папку(self):
        with mock.patch.object(dan.os, "name", "nt"), \
             mock.patch.object(dan.subprocess, "run", side_effect=ответ_gcc), \
             mock.patch.object(dan.os.path, "isfile", return_value=True), \
             mock.patch.object(dan, "_путь_ascii", return_value=КОРОТКИЙ) as короткое:
            флаги = dan.флаги_mingw("gcc")
        self.assertEqual(флаги, ["-B" + КОРОТКИЙ + "\\"])
        self.assertNotIn(" ", флаги[0])
        self.assertTrue(короткое.call_args[0][0].endswith("lib"))

    def test_путь_без_пробела_без_параметров(self):
        def без_пробела(команда, **_):
            return subprocess.CompletedProcess(команда, 0, stdout=ДЛИННЫЙ.replace(" ", "") + "\n", stderr="")
        with mock.patch.object(dan.os, "name", "nt"), \
             mock.patch.object(dan.subprocess, "run", side_effect=без_пробела), \
             mock.patch.object(dan.os.path, "isfile", return_value=True):
            self.assertEqual(dan.флаги_mingw("gcc"), [])

    def test_нет_короткого_имени_без_параметров(self):
        with mock.patch.object(dan.os, "name", "nt"), \
             mock.patch.object(dan.subprocess, "run", side_effect=ответ_gcc), \
             mock.patch.object(dan.os.path, "isfile", return_value=True), \
             mock.patch.object(dan, "_путь_ascii", return_value=None):
            self.assertEqual(dan.флаги_mingw("gcc"), [])

    def test_сборка_передаёт_параметр_компилятору(self):
        команды = []

        def скомпилировать(команда, путь_c, путь_exe, после):
            команды.append(команда)
            return subprocess.CompletedProcess(команда, 0, stdout="", stderr="")
        with mock.patch.object(dan, "флаги_mingw", return_value=["-BC:\\KOROTKO\\"]), \
             mock.patch.object(dan, "_скомпилировать", side_effect=скомпилировать), \
             mock.patch.dict(os.environ, {"DAN_COMPILER": ""}):
            import tempfile
            with tempfile.TemporaryDirectory() as папка:
                путь = os.path.join(папка, "p.dan")
                with open(путь, "w", encoding="utf-8") as ф:
                    ф.write("вывод(1)\n")
                with open(путь, encoding="utf-8") as ф:
                    исходник = ф.read()
                dan.собрать(путь, исходник)
        self.assertTrue(команды)
        self.assertIn("-BC:\\KOROTKO\\", команды[0])


if __name__ == "__main__":
    unittest.main()
