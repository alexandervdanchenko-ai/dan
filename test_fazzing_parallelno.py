"""Метаморфный фаззинг «параллельно для» (fazzing_parallelno.py): генератор даёт верные программы,
их вывод не зависит от числа потоков, а испорченные варианты отвергаются проверкой с кодом Д03xx."""

import unittest

import dan
import fazzing_parallelno


@unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
class ТестыФаззингаПараллельного(unittest.TestCase):
    def test_несколько_программ(self):
        for зерно in (1, 2, 3):
            with self.subTest(зерно=зерно):
                self.assertEqual(fazzing_parallelno.проверить_программу(зерно), [])

    def test_испорченные_отличаются_от_верной(self):
        текст = fazzing_parallelno.Генератор(7).программа()
        варианты = fazzing_parallelno.испорченные(текст)
        self.assertGreaterEqual(len(варианты), 5)
        for описание, испорченный in варианты:
            self.assertNotEqual(испорченный, текст, описание)
            self.assertIn("параллельно для", испорченный)

    def test_обычный_цикл(self):
        текст = fazzing_parallelno.Генератор(5).программа()
        обычный = fazzing_parallelno.в_обычный(текст)
        self.assertNotIn("параллельно", обычный)
        self.assertIn("для к в 0..н - 1 {", обычный)


if __name__ == "__main__":
    unittest.main()
