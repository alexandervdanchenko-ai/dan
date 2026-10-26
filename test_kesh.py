"""Кэш собранных программ (dan.собрать): повторная сборка берёт готовый файл."""

import os
import subprocess
import tempfile
import unittest
from unittest import mock

import dan

КОМПИЛЯТОР_ЕСТЬ = dan.компилятор_есть()


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыКэша(unittest.TestCase):

    def setUp(self):
        self.кэш = tempfile.TemporaryDirectory()
        self.папка = tempfile.TemporaryDirectory()
        self.среда = mock.patch.dict(os.environ, {"DAN_CACHE": self.кэш.name})
        self.среда.start()
        os.environ.pop("DAN_NO_CACHE", None)

    def tearDown(self):
        self.среда.stop()
        self.кэш.cleanup()
        self.папка.cleanup()

    def собрать(self, текст):
        путь = os.path.join(self.папка.name, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        return dan.собрать(путь, текст)

    def выполнить(self, исп):
        return subprocess.run([исп], capture_output=True, text=True, encoding="utf-8", timeout=60).stdout

    def файлы_кэша(self):
        return sorted(os.listdir(self.кэш.name))

    def test_повторная_сборка_без_компилятора(self):
        исп = self.собрать('вывод("первая")')
        self.assertEqual(self.выполнить(исп), "первая\n")
        self.assertEqual(len(self.файлы_кэша()), 1)
        # компилятор C больше не вызывается: готовая программа берётся из кэша
        with mock.patch.object(dan, "_скомпилировать", side_effect=AssertionError("компилятор вызван")):
            исп = self.собрать('вывод("первая")')
        self.assertEqual(self.выполнить(исп), "первая\n")

    def test_изменённая_программа_собирается_заново(self):
        self.собрать('вывод("а")')
        исп = self.собрать('вывод("б")')
        self.assertEqual(self.выполнить(исп), "б\n")
        self.assertEqual(len(self.файлы_кэша()), 2)

    def test_другие_параметры_сборки_другой_ключ(self):
        текст = 'вывод(1)'
        путь = os.path.join(self.папка.name, "п.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        dan.собрать(путь, текст)
        dan.собрать(путь, текст, малый=True)
        self.assertEqual(len(self.файлы_кэша()), 2)

    def test_отключение_кэша(self):
        with mock.patch.dict(os.environ, {"DAN_NO_CACHE": "1"}):
            self.собрать('вывод(2)')
        self.assertEqual(self.файлы_кэша(), [])

    def test_старые_программы_удаляются(self):
        with mock.patch.object(dan, "КЭШ_НАИБОЛЬШЕ", 2):
            for н in range(4):
                self.собрать(f'вывод({н})')
        self.assertEqual(len(self.файлы_кэша()), 2)

    def test_испорченный_кэш_не_мешает_сборке(self):
        # вместо папки кэша - обычный файл: записать в него нельзя
        мешает = os.path.join(self.папка.name, "файл")
        open(мешает, "w").close()
        with mock.patch.dict(os.environ, {"DAN_CACHE": мешает}):
            исп = self.собрать('вывод("без кэша")')
        self.assertEqual(self.выполнить(исп), "без кэша\n")


if __name__ == "__main__":
    unittest.main()
