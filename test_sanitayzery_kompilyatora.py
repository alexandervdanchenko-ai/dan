"""Сценарий sanitayzery_kompilyatora.py: выбор программ, распознавание находок, задание CI.

Сам прогон под санитайзерами (около 20 минут) идёт отдельным заданием CI «Компилятор под санитайзерами».
"""

import os
import stat
import tempfile
import unittest

import sanitayzery_kompilyatora as с

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def поддельный(папка, имя, текст):
    путь = os.path.join(папка, имя)
    with open(путь, "w", encoding="utf-8") as ф:
        ф.write("#!/bin/sh\n" + текст + "\n")
    os.chmod(путь, os.stat(путь).st_mode | stat.S_IEXEC)
    return путь


@unittest.skipIf(os.name == "nt", "сценарий для Linux и macOS")
class ТестыСанитайзеровКомпилятора(unittest.TestCase):
    def test_программы_всех_папок(self):
        файлы = с.программы()
        for папка in с.ПАПКИ:
            self.assertTrue(any(ф.startswith(папка + os.sep) for ф in файлы), папка)
        self.assertTrue(any(os.sep + "отказы" + os.sep in ф for ф in файлы))
        self.assertEqual(с.программы(3), файлы[:3])

    def test_находки_и_расхождения(self):
        with tempfile.TemporaryDirectory() as п:
            обычный = поддельный(п, "обычный", "echo код; exit 0")
            такой_же = поддельный(п, "такой_же", "echo код; exit 0")
            другой = поддельный(п, "другой", "echo иной; exit 0")
            asan = поддельный(п, "asan", "echo код; echo '==1==ERROR: AddressSanitizer: heap-use-after-free' >&2; exit 0")
            ubsan = поддельный(п, "ubsan", "echo код; echo 'а.c:1:2: runtime error: signed integer overflow' >&2; exit 0")
            утечка = поддельный(п, "lsan", "echo код; echo '==1==ERROR: LeakSanitizer: detected memory leaks' >&2; exit 0")
            self.assertEqual(с.сверить(обычный, такой_же, ["си", "х.dan"]), "")
            self.assertIn("вывод разный", с.сверить(обычный, другой, ["си", "х.dan"]))
            for проверяемый in (asan, ubsan, утечка):
                self.assertTrue(с.сверить(обычный, проверяемый, ["си", "х.dan"]).startswith("санитайзер"))

    def test_задание_ci(self):
        with open(os.path.join(ПАПКА, ".github", "workflows", "proverka.yml"), encoding="utf-8") as ф:
            текст = ф.read()
        self.assertIn("name: Компилятор под санитайзерами", текст)
        self.assertIn("run: python sanitayzery_kompilyatora.py\n", текст)


if __name__ == "__main__":
    unittest.main()
