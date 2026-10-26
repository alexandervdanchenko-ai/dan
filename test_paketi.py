"""Тесты менеджера пакетов (paketi.py): пакеты - локальные репозитории git."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import dan
from paketi import команда, Проект, ОшибкаПакетов, контрольная_сумма
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ЕСТЬ_GIT = shutil.which("git") is not None


def git(папка, *аргументы):
    subprocess.run(["git", "-c", "user.name=Тест", "-c", "user.email=test@example.com", "-c", "commit.gpgsign=false",
                    "-c", "tag.gpgsign=false", *аргументы], cwd=папка, check=True, capture_output=True)


def читать(путь):
    with open(путь, encoding="utf-8") as ф:
        return json.load(ф)


def записать(путь, данные):
    with open(путь, "w", encoding="utf-8") as ф:
        json.dump(данные, ф, ensure_ascii=False)


def репозиторий(папка, файлы, метка=None):
    """Создаёт (или дополняет) репозиторий git с файлами и, если нужно, ставит метку версии."""
    os.makedirs(папка, exist_ok=True)
    if not os.path.isdir(os.path.join(папка, ".git")):
        git(папка, "init", "-q", "-b", "main")
    for имя, текст in файлы.items():
        путь = os.path.join(папка, имя)
        os.makedirs(os.path.dirname(путь), exist_ok=True)
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
    git(папка, "add", "-A")
    git(папка, "commit", "-q", "-m", "изменение")
    if метка:
        git(папка, "tag", метка)


@unittest.skipUnless(ЕСТЬ_GIT, "нужна программа git")
class ТестыПакетов(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.корень = self.tmp.name
        self.ставки = os.path.join(self.корень, "репо_ставки")
        репозиторий(self.ставки, {"ставки.dan": "общий функ ключевая() -> рац { вернуть рац(16, 100) }\n",
                                  "лишнее.sh": "echo вредно\n", "LICENSE": "MIT\n"}, "v1.0")
        репозиторий(self.ставки, {"ставки.dan": "общий функ ключевая() -> рац { вернуть рац(21, 100) }\n"}, "v2.0")
        self.пени = os.path.join(self.корень, "репо_пени")
        репозиторий(self.пени, {
            "пени.dan": 'подключить "ставки"\nподключить "пени/дни.dan"\n'
                        "общий функ пени(сумма: рац, дней: цел) -> рац {\n"
                        "    вернуть сумма * ставки.ключевая() * дни.доля(дней)\n}\n",
            "дни.dan": "общий функ доля(дней: цел) -> рац { вернуть рац(дней, 365) }\n",
            "пакеты.json": json.dumps({"зависимости": {"ставки": {"адрес": self.ставки, "версия": "v1.0"}}},
                                      ensure_ascii=False)}, "v0.1")
        self.проект = os.path.join(self.корень, "проект")
        os.makedirs(self.проект)
        # пакеты запоминаются в кэше Дана: у тестов свой кэш
        self.среда = mock.patch.dict(os.environ, {"DAN_CACHE": os.path.join(self.корень, "кэш")})
        self.среда.start()
        os.environ.pop("DAN_NO_CACHE", None)

    def tearDown(self):
        self.среда.stop()
        self.tmp.cleanup()

    def выполнить(self, *аргументы):
        return команда(list(аргументы), self.проект)

    def test_добавить_установить_удалить(self):
        self.assertEqual(self.выполнить("добавить", "пени", self.пени, "v0.1"), 0)
        замок = читать(os.path.join(self.проект, "пакеты.замок.json"))
        self.assertEqual(set(замок), {"пени", "ставки"})
        self.assertEqual(замок["ставки"]["версия"], "v1.0")
        self.assertEqual(замок["ставки"]["нужен_для"], "пени")
        # копируются только файлы Дана, лицензия и описание зависимостей
        self.assertEqual(sorted(os.listdir(os.path.join(self.проект, "пакеты", "ставки"))), ["LICENSE", "ставки.dan"])
        # программа пользуется пакетом
        if КОМПИЛЯТОР_ЕСТЬ:
            текст = 'подключить "пени"\nвывод(пени.пени(36500, 10))\n'
            путь = os.path.join(self.проект, "главная.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            р = subprocess.run([dan.собрать(путь, текст)], capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(р.stdout.strip(), "160")
        # установка по замку восстанавливает удалённую папку
        shutil.rmtree(os.path.join(self.проект, "пакеты"))
        self.assertEqual(self.выполнить("установить"), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.проект, "пакеты", "ставки", "ставки.dan")))
        self.assertEqual(self.выполнить("проверить"), 0)
        # удаление убирает и зависимость, нужную только ему
        self.assertEqual(self.выполнить("удалить", "пени"), 0)
        self.assertEqual(читать(os.path.join(self.проект, "пакеты.замок.json")), {})
        self.assertFalse(os.path.exists(os.path.join(self.проект, "пакеты", "ставки")))

    def test_изменённый_пакет_обнаруживается(self):
        self.выполнить("добавить", "ставки", self.ставки, "v1.0")
        with open(os.path.join(self.проект, "пакеты", "ставки", "ставки.dan"), "a", encoding="utf-8") as ф:
            ф.write("// вставка\n")
        self.assertEqual(self.выполнить("проверить"), 1)
        # установка по замку возвращает подлинные файлы
        self.assertEqual(self.выполнить("установить"), 0)
        self.assertEqual(self.выполнить("проверить"), 0)

    def test_передвинутая_метка_не_принимается(self):
        self.выполнить("добавить", "ставки", self.ставки, "v1.0")
        # злоумышленник передвигает метку v1.0 на другой коммит с другим содержимым
        репозиторий(self.ставки, {"ставки.dan": "общий функ ключевая() -> рац { вернуть рац(99, 100) }\n"})
        git(self.ставки, "tag", "-f", "v1.0")
        shutil.rmtree(os.path.join(self.проект, "пакеты"))
        # замок хранит коммит: ставится прежнее содержимое
        self.assertEqual(self.выполнить("установить"), 0)
        with open(os.path.join(self.проект, "пакеты", "ставки", "ставки.dan"), encoding="utf-8") as ф:
            self.assertIn("рац(16, 100)", ф.read())
        # если же подменена контрольная сумма в замке - установка останавливается
        замок_путь = os.path.join(self.проект, "пакеты.замок.json")
        замок = читать(замок_путь)
        замок["ставки"]["сумма"] = "0" * 64
        записать(замок_путь, замок)
        shutil.rmtree(os.path.join(self.проект, "пакеты"))
        with self.assertRaises(ОшибкаПакетов) as о:
            Проект(self.проект).установить()
        self.assertIn("не совпадает с записанным", str(о.exception))

    def test_обновить_и_конфликт_версий(self):
        self.выполнить("добавить", "ставки", self.ставки, "v1.0")
        описание_путь = os.path.join(self.проект, "пакеты.json")
        описание = читать(описание_путь)
        описание["зависимости"]["ставки"]["версия"] = "v2.0"
        записать(описание_путь, описание)
        self.assertEqual(self.выполнить("обновить"), 0)
        with open(os.path.join(self.проект, "пакеты", "ставки", "ставки.dan"), encoding="utf-8") as ф:
            self.assertIn("рац(21, 100)", ф.read())
        # пени требуют ставки v1.0, а в проекте v2.0
        with self.assertRaises(ОшибкаПакетов) as о:
            Проект(self.проект).добавить("пени", self.пени, "v0.1")
        self.assertIn("согласуйте версии", str(о.exception))

    def test_замороженная_установка_и_согласованность(self):
        self.выполнить("добавить", "ставки", self.ставки, "v1.0")
        self.assertEqual(self.выполнить("установить", "--заморожено"), 0)
        self.assertEqual(Проект(self.проект).несогласованность(), [])
        # в пакеты.json сменили версию, а замок не обновили
        описание_путь = os.path.join(self.проект, "пакеты.json")
        описание = читать(описание_путь)
        описание["зависимости"]["ставки"]["версия"] = "v2.0"
        записать(описание_путь, описание)
        замок_до = читать(os.path.join(self.проект, "пакеты.замок.json"))
        with self.assertRaises(ОшибкаПакетов) as о:
            Проект(self.проект).установить(заморожено=True)
        self.assertIn("ставки: в пакеты.json версия v2.0", str(о.exception))
        self.assertEqual(читать(os.path.join(self.проект, "пакеты.замок.json")), замок_до)
        self.assertEqual(self.выполнить("проверить"), 1)
        # новый пакет в пакеты.json без записи в замке
        описание["зависимости"]["ставки"]["версия"] = "v1.0"
        описание["зависимости"]["пени"] = {"адрес": self.пени, "версия": "v0.1"}
        записать(описание_путь, описание)
        self.assertEqual(Проект(self.проект).несогласованность(),
                         ["пени: есть в пакеты.json, но не записан в пакеты.замок.json"])
        self.assertEqual(self.выполнить("установить", "--заморожено"), 1)
        # обычная установка дописывает замок
        self.assertEqual(self.выполнить("установить"), 0)
        self.assertEqual(self.выполнить("установить", "--заморожено"), 0)

    def test_установка_без_сети_из_кэша(self):
        self.выполнить("добавить", "ставки", self.ставки, "v1.0")
        сумма = читать(os.path.join(self.проект, "пакеты.замок.json"))["ставки"]["сумма"]
        self.assertTrue(os.path.isdir(os.path.join(self.корень, "кэш", "пакеты", сумма)))
        # репозитория больше нет: пакет ставится из кэша (папку переименовываем: в Windows файлы git
        # только для чтения, и rmtree их не удаляет)
        os.rename(self.ставки, self.ставки + "_убран")
        shutil.rmtree(os.path.join(self.проект, "пакеты"))
        проект = Проект(self.проект)
        проект.установить()
        self.assertTrue(any("(из кэша)" in с for с in проект.сообщения), проект.сообщения)
        self.assertEqual(self.выполнить("проверить"), 0)
        # испорченная копия в кэше не принимается
        with open(os.path.join(self.корень, "кэш", "пакеты", сумма, "ставки.dan"), "a", encoding="utf-8") as ф:
            ф.write("// подмена\n")
        shutil.rmtree(os.path.join(self.проект, "пакеты"))
        with self.assertRaises(ОшибкаПакетов) as о:
            Проект(self.проект).установить()
        self.assertIn("не удалось получить пакет", str(о.exception))
        # с DAN_NO_CACHE=1 кэш не используется
        with mock.patch.dict(os.environ, {"DAN_NO_CACHE": "1"}):
            import paketi
            self.assertIsNone(paketi.хранилище())

    def test_неверные_имя_и_адрес(self):
        with self.assertRaises(ОшибкаПакетов):
            Проект(self.проект).добавить("1плохое", self.ставки)
        with self.assertRaises(ОшибкаПакетов) as о:
            Проект(self.проект).добавить("нет", os.path.join(self.корень, "нет_такого"))
        self.assertIn("не удалось получить пакет", str(о.exception))
        with self.assertRaises(ОшибкаПакетов):
            Проект(self.проект).добавить("ставки", self.ставки, "v9.9")

    def test_контрольная_сумма_зависит_от_имён_и_содержимого(self):
        а = os.path.join(self.корень, "а")
        os.makedirs(а)
        with open(os.path.join(а, "x.dan"), "w") as ф:
            ф.write("1")
        первая = контрольная_сумма(а)
        os.rename(os.path.join(а, "x.dan"), os.path.join(а, "y.dan"))
        self.assertNotEqual(контрольная_сумма(а), первая)


if __name__ == "__main__":
    unittest.main()
