"""Статический анализ (staticheskiy_analiz.py): разбор вывода gcc -fanalyzer и списка известных."""

import os
import unittest

import staticheskiy_analiz as са

ВЫВОД = """п1.c: In function 'dan_zct_push':
п1.c:326:28: warning: dereference of NULL 'dan_zct' [CWE-476] [-Wanalyzer-null-dereference]
  326 |     dan_zct[dan_zct_n].h = h;
п1.c:300:13: note: (1) entry to 'dan_zct_push'
п1.c: In function 'dan_sb_region':
п1.c:225:8: warning: leak of 'slab' [CWE-401] [-Wanalyzer-malloc-leak]
п1.c:900:1: warning: unused variable 'x' [-Wunused-variable]
"""


class ТестыРазбора(unittest.TestCase):

    def test_пары_вид_и_функция(self):
        self.assertEqual(са.предупреждения(ВЫВОД), {("-Wanalyzer-null-dereference", "dan_zct_push"),
                                                     ("-Wanalyzer-malloc-leak", "dan_sb_region")})

    def test_обычные_предупреждения_не_считаются(self):
        self.assertEqual(са.предупреждения("а.c:1:1: warning: unused [-Wunused-variable]\n"), set())

    def test_список_известных(self):
        текст = "# заголовок\n\n-Wanalyzer-malloc-leak f # живёт до конца\n"
        self.assertEqual(са.прочитать_известные(текст), {("-Wanalyzer-malloc-leak", "f")})

    def test_строка_без_причины_ошибка(self):
        with self.assertRaises(ValueError):
            са.прочитать_известные("-Wanalyzer-malloc-leak f\n")

    def test_сравнение(self):
        новые, исчезнувшие = са.сравнить({("в", "а"), ("в", "б")}, {("в", "б"), ("в", "в")})
        self.assertEqual(новые, [("в", "а")])
        self.assertEqual(исчезнувшие, [("в", "в")])

    def test_файл_известных_читается(self):
        with open(са.ИЗВЕСТНЫЕ, encoding="utf-8") as ф:
            известные = са.прочитать_известные(ф.read())
        self.assertTrue(известные)
        self.assertTrue(all(вид.startswith("-Wanalyzer-") for вид, _ in известные))


if __name__ == "__main__":
    unittest.main()
