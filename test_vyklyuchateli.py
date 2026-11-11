"""Выключатели оптимизаций компилятора на Дане: DAN_BEZ_PERENOSA и DAN_BEZ_DIAPAZONOV.

Каждый выключатель меняет код C только там, где работает его оптимизация, а вывод программы
остаётся тем же. Задание CI «Выключатели оптимизаций» прогоняет каталог со всеми выключателями,
«Сверка сборок» добавляет сборку без оптимизаций Дана к случайным программам.
"""

import os
import subprocess
import tempfile
import unittest

import sam_kompilyator

ПРОГРАММА = """функ добавь(с: [цел], х: цел) -> [цел] {
    перем л = с
    л.добавить(х)
    вернуть л
}

перем с: [цел] = []
для к в 0..9 {
    с = добавь(с, к * к)
}
перем сумма = 0
для к в 0..с.длина - 1 {
    сумма += с[к]
}
перем н = 0
пока н < с.длина {
    сумма += с[н] * 2
    н += 1
}
вывод(сумма)
вывод(с)
"""


def выполнить(аргументы, папка, **выключатели):
    окружение = sam_kompilyator.среда()
    for имя in ("DAN_BEZ_PERENOSA", "DAN_BEZ_DIAPAZONOV"):
        окружение.pop(имя, None)
    окружение.update({к: "1" for к in выключатели})
    р = subprocess.run([sam_kompilyator.исполняемый(), *аргументы], cwd=папка, capture_output=True,
                       env=окружение, timeout=600)
    return (р.returncode, р.stdout.decode("utf-8", "replace").replace("\r\n", "\n"),
            р.stderr.decode("utf-8", "replace"))


class ТестыВыключателей(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.папка = tempfile.mkdtemp(prefix="dan-vykl-")
        with open(os.path.join(cls.папка, "п.dan"), "w", encoding="utf-8") as ф:
            ф.write(ПРОГРАММА)

    def код_c(self, **выключатели):
        код, вывод, ош = выполнить(["си", "п.dan"], self.папка, **выключатели)
        self.assertEqual(код, 0, ош)
        return вывод

    def test_каждый_выключатель_меняет_код(self):
        обычный = self.код_c()
        без_переноса = self.код_c(DAN_BEZ_PERENOSA=1)
        без_диапазонов = self.код_c(DAN_BEZ_DIAPAZONOV=1)
        без_всего = self.код_c(DAN_BEZ_PERENOSA=1, DAN_BEZ_DIAPAZONOV=1)
        self.assertEqual(len({обычный, без_переноса, без_диапазонов, без_всего}), 4)
        # анализ диапазонов: проверка границ один раз до цикла и умножение без проверки переполнения
        self.assertIn("_inb", обычный)
        self.assertIn("(d_k * d_k)", обычный)
        self.assertNotIn("_inb", без_диапазонов)
        self.assertIn("dan_mul(d_k, d_k", без_диапазонов)
        # перенос: параметр «с» функция берёт себе без второго места и без копии
        self.assertIn("dl_Li* d_l = d_s;", обычный)
        self.assertIn("dl_Li* d_l = dl_Li_share(d_s);", без_переноса)
        self.assertIn("_inb", без_переноса)

    def test_вывод_тот_же(self):
        итоги = set()
        for выключатели in ({}, {"DAN_BEZ_PERENOSA": 1}, {"DAN_BEZ_DIAPAZONOV": 1},
                            {"DAN_BEZ_PERENOSA": 1, "DAN_BEZ_DIAPAZONOV": 1}):
            код, вывод, ош = выполнить(["запустить", "п.dan"], self.папка, **выключатели)
            self.assertEqual(код, 0, ош)
            итоги.add(вывод)
        self.assertEqual(итоги, {"855\n[0, 1, 4, 9, 16, 25, 36, 49, 64, 81]\n"})


if __name__ == "__main__":
    unittest.main()
