"""Журнал решений `решения/`: устройство записей и ссылки из таблицы «Чего не делаем»."""

import glob
import os
import re
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))
РЕШЕНИЯ = os.path.join(ПАПКА, "решения")
РАЗДЕЛЫ = ["## Контекст", "## Решение", "## Альтернативы", "## Когда пересмотреть"]


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


def записи():
    return sorted(os.path.basename(п) for п in glob.glob(os.path.join(РЕШЕНИЯ, "*.md")) if not п.endswith("README.md"))


class ТестыЖурналаРешений(unittest.TestCase):
    def test_имена_и_номера_по_порядку(self):
        имена = записи()
        self.assertTrue(имена)
        for н, имя in enumerate(имена, 1):
            self.assertRegex(имя, r"^\d{4}-[a-z0-9-]+\.md$")
            self.assertEqual(int(имя[:4]), н, "номера записей идут подряд с 0001: " + имя)

    def test_устройство_записи(self):
        for имя in записи():
            текст = прочитать(os.path.join(РЕШЕНИЯ, имя))
            with self.subTest(имя=имя):
                self.assertTrue(текст.startswith("# " + имя[:4] + ". "), "заголовок «# NNNN. ...»")
                self.assertRegex(текст, r"\nДата: \d{4}-\d{2}-\d{2}\n")
                места = [текст.find(р + "\n") for р in РАЗДЕЛЫ]
                self.assertNotIn(-1, места, "нужны разделы " + ", ".join(РАЗДЕЛЫ))
                self.assertEqual(места, sorted(места), "разделы по порядку")
                for р, следующий in zip(РАЗДЕЛЫ, РАЗДЕЛЫ[1:] + [None]):
                    начало = текст.index(р + "\n") + len(р)
                    конец = текст.index(следующий + "\n") if следующий else len(текст)
                    self.assertTrue(текст[начало:конец].strip(), "раздел не пуст: " + р)
                for ссылка in re.findall(r"решени[еяи] (\d{4})", текст):
                    self.assertTrue(glob.glob(os.path.join(РЕШЕНИЯ, ссылка + "-*.md")), "ссылка на решение " + ссылка)

    def test_чего_не_делаем_ссылается_на_решения(self):
        текст = прочитать(os.path.join(ПАПКА, "ROADMAP.md"))
        раздел = текст.split("## Чего не делаем и почему", 1)[1].split("\n## ", 1)[0]
        строки = [с for с in раздел.splitlines() if с.startswith("| ") and not с.startswith("| Что |")]
        self.assertTrue(строки)
        for с in строки:
            with self.subTest(строка=с[:40]):
                ссылки = re.findall(r"\]\((решения/[^)]+\.md)\)", с)
                self.assertTrue(ссылки, "строка таблицы ссылается на запись журнала решений")
                for ссылка in ссылки:
                    self.assertTrue(os.path.isfile(os.path.join(ПАПКА, ссылка)), ссылка)

    def test_правило_для_агентов(self):
        self.assertIn("решения/", прочитать(os.path.join(ПАПКА, "AGENTS.md")))


if __name__ == "__main__":
    unittest.main()
