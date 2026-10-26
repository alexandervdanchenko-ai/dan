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


if __name__ == "__main__":
    unittest.main()
