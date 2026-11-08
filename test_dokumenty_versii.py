"""Документы не отстают от версии: номер в versiya.py совпадает с номером в README, spec.md,
справочнике и SECURITY.md, у версии есть описание выпуска, README загружает выпуск этой версии,
а ROADMAP.md называет её среди сделанного и планирует следующую.

Номер в versiya.py меняется только при выпуске (вместе с файлом выпуски/v<номер>.md), а то, что
ещё не выпущено, копится в разделе CHANGELOG.md «Не выпущено»; поэтому версия в versiya.py -
всегда последний выпуск, и все документы называют её.
"""

import importlib.util
import os
import re
import unittest

import versiya

ПАПКА = os.path.dirname(os.path.abspath(__file__))
# сценарий проверки выпуска лежит в папке поставка: загружается по пути, без изменения sys.path
_описание = importlib.util.spec_from_file_location("проверить_выпуск", os.path.join(ПАПКА, "поставка", "проверить_выпуск.py"))
проверить_выпуск = importlib.util.module_from_spec(_описание)
_описание.loader.exec_module(проверить_выпуск)

ВЕРСИЯ = versiya.ВЕРСИЯ
НОМЕР = r"\d+(?:\.\d+)*(?:[a-z]+\d*)?"


def прочитать(имя):
    with open(os.path.join(ПАПКА, имя), encoding="utf-8") as ф:
        return ф.read()


def ключ(номер):
    """Номер версии для сравнения: 1.6.1 -> (1, 6, 1); буквенная часть (1.7b1) раньше выпуска."""
    м = re.fullmatch(r"(\d+(?:\.\d+)*)([a-z]+\d*)?", номер)
    части = tuple(int(ч) for ч in м.group(1).split("."))
    части += (0,) * (4 - len(части))
    return части + ((0,) if м.group(2) else (1,))


def раздел(текст, заголовок):
    """Текст раздела «## заголовок...» до следующего заголовка второго уровня."""
    м = re.search(r"^## " + заголовок + r"[^\n]*\n(.*?)(?=^## |\Z)", текст, re.M | re.S)
    return м.group(1) if м else None


class ТестыДокументовВерсии(unittest.TestCase):
    def test_readme(self):
        м = re.search(r"Текущая версия - (" + НОМЕР + r")\.", прочитать("README.md"))
        self.assertIsNotNone(м, "в README.md нет строки «Текущая версия - <номер>.»")
        self.assertEqual(м.group(1), ВЕРСИЯ, "README.md: «Текущая версия» не совпадает с versiya.py")

    def test_readme_загружает_выпуск_этой_версии(self):
        # команды установки в Linux и macOS проверяет сценарий proverka_vypuska.yml
        self.assertEqual(проверить_выпуск.метка_readme(прочитать("README.md")), "v" + ВЕРСИЯ,
                         "README.md: «git clone --branch» должен загружать выпуск из versiya.py")

    def test_spec(self):
        первая = прочитать("spec.md").split("\n", 1)[0]
        self.assertRegex(первая, r"^# .*Спецификация версии " + НОМЕР + "$")
        self.assertTrue(первая.endswith("версии " + ВЕРСИЯ), f"spec.md: заголовок «{первая}», а версия {ВЕРСИЯ}")

    def test_справочник(self):
        первая = прочитать("справочник.md").split("\n", 1)[0]
        self.assertIn(f"(версия {ВЕРСИЯ},", первая, f"справочник.md: заголовок «{первая}», а версия {ВЕРСИЯ}")

    def test_security(self):
        self.assertIn(f"(сейчас {ВЕРСИЯ})", прочитать("SECURITY.md"),
                      "SECURITY.md: последняя версия, которая получает исправления, не совпадает с versiya.py")

    def test_описание_выпуска(self):
        self.assertTrue(os.path.isfile(os.path.join(ПАПКА, "выпуски", f"v{ВЕРСИЯ}.md")),
                        f"нет описания выпуска выпуски/v{ВЕРСИЯ}.md (его берёт сценарий «Выпуск»)")

    def test_changelog(self):
        текст = прочитать("CHANGELOG.md")
        self.assertIsNotNone(раздел(текст, "Не выпущено"), "в CHANGELOG.md нет раздела «Не выпущено»")
        self.assertIsNotNone(re.search(r"^## " + re.escape(ВЕРСИЯ) + r"\s*$", текст, re.M),
                             f"в CHANGELOG.md нет раздела «## {ВЕРСИЯ}»")

    def test_roadmap(self):
        текст = прочитать("ROADMAP.md")
        сейчас = re.search(r"^## Сейчас: выпуск (" + НОМЕР + r")\s*$", текст, re.M)
        self.assertIsNotNone(сейчас, "в ROADMAP.md нет раздела «## Сейчас: выпуск <номер>»")
        self.assertGreater(ключ(сейчас.group(1)), ключ(ВЕРСИЯ),
                           f"ROADMAP.md: «Сейчас: выпуск {сейчас.group(1)}», а версия {ВЕРСИЯ} уже выпущена")
        сделано = раздел(текст, "Сделано недавно")
        self.assertIsNotNone(сделано, "в ROADMAP.md нет раздела «## Сделано недавно»")
        self.assertRegex(сделано, r"(?<![\d.])" + re.escape(ВЕРСИЯ) + r"(?![\d.]\d)",
                         f"ROADMAP.md: выпуск {ВЕРСИЯ} не упомянут в «Сделано недавно»")

    def test_сравнение_номеров(self):
        self.assertLess(ключ("1.6.1"), ключ("1.7"))
        self.assertLess(ключ("1.6"), ключ("1.6.1"))
        self.assertLess(ключ("1.7b1"), ключ("1.7"))
        self.assertLess(ключ("1.9"), ключ("1.10"))


class ТестыКомандREADME(unittest.TestCase):
    ОБРАЗЕЦ = ("# Дан\n\n## Установка в Linux и macOS\n\nтекст\n\n```\n"
               "git clone --branch v1.6.1 --depth 1 https://github.com/x/dan.git\ncd dan\nsh semya/sobrat.sh\n```\n\n"
               "Библиотеку ставит команда `semya/dan модели установить`.\n\n## Лицензия\n\n```\nrm -rf /\n```\n")

    def test_команды_и_метка(self):
        self.assertEqual(проверить_выпуск.команды_readme(self.ОБРАЗЕЦ, "v2.0"),
                         ["git clone --branch v2.0 --depth 1 https://github.com/x/dan.git", "cd dan", "sh semya/sobrat.sh"])
        self.assertEqual(проверить_выпуск.метка_readme(self.ОБРАЗЕЦ), "v1.6.1")

    def test_clone_без_метки_ошибка(self):
        with self.assertRaises(ValueError):
            проверить_выпуск.команды_readme(self.ОБРАЗЕЦ.replace("--branch v1.6.1 ", ""))

    def test_нет_установки_моделей_ошибка(self):
        with self.assertRaises(ValueError):
            проверить_выпуск.команды_readme(self.ОБРАЗЕЦ.replace("semya/dan модели установить", "x"))

    def test_суммы(self):
        self.assertEqual(проверить_выпуск.суммы_из_файла("ABC  a.zip\ndef *b.exe\n\n"), {"a.zip": "abc", "b.exe": "def"})


if __name__ == "__main__":
    unittest.main()
