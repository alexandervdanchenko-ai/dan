"""Образцы и находки фаззинга (папка «фаззинг») проходят через мишени без ошибок.

Каждая находка фаззинга после исправления остаётся в «фаззинг/находки» и
проверяется здесь: исправленная ошибка не должна вернуться. Нужен clang с
libFuzzer; без него проверка пропускается. Сам фаззинг: python fazzing.py.
"""

import unittest

import fazzing

ЕСТЬ_LIBFUZZER = fazzing.libfuzzer_есть()


@unittest.skipUnless(ЕСТЬ_LIBFUZZER, "нет clang с libFuzzer")
class ТестыФаззинга(unittest.TestCase):

    def test_образцы_и_находки(self):
        for имя in fazzing.МИШЕНИ:
            with self.subTest(мишень=имя):
                успех, вывод = fazzing.повторить(имя)
                self.assertTrue(успех, вывод[-4000:])


class ТестыМишениКомпилятора(unittest.TestCase):
    """Программа-мишень «компилятор» верна для компилятора на Дане (без libFuzzer: только проверка)."""

    def test_мишень_проверяется(self):
        import os
        import shutil
        import subprocess
        import tempfile
        import sam_kompilyator
        with tempfile.TemporaryDirectory() as папка:
            копия = os.path.join(папка, "сам")
            shutil.copytree(os.path.join(fazzing.ПАПКА, "сам"), копия, ignore=shutil.ignore_patterns("*.py"))
            путь = os.path.join(копия, "цель.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(fazzing.текст_мишени(fazzing.КОМПИЛЯТОР))
            р = subprocess.run([sam_kompilyator.исполняемый(), "проверить", путь], capture_output=True,
                               env=sam_kompilyator.среда())
            self.assertEqual(р.returncode, 0, р.stdout.decode("utf-8", "replace") + р.stderr.decode("utf-8", "replace"))

    def test_образцы_из_каталога(self):
        self.assertIn(fazzing.КОМПИЛЯТОР, fazzing.МИШЕНИ)
        self.assertIn("каталог", fazzing.ПАПКИ_ПРОГРАММ)


if __name__ == "__main__":
    unittest.main()
