"""dan обновить: выбор выпуска по каналу, сверка контрольной суммы, распаковка рядом."""

import hashlib
import io
import json
import os
import pathlib
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from unittest import mock

import obnovlenie as о
import versiya


def адрес(путь):
    return pathlib.Path(путь).as_uri()


class ТестыНомеров(unittest.TestCase):

    def test_сравнение(self):
        self.assertLess(о.номер("1.3"), о.номер("1.4"))
        self.assertLess(о.номер("1.4b1"), о.номер("1.4"))
        self.assertLess(о.номер("1.4b1"), о.номер("1.4b2"))
        self.assertLess(о.номер("1.9"), о.номер("1.10"))
        self.assertEqual(о.номер("v1.4"), о.номер("1.4.0"))
        self.assertTrue(о.пробный("1.4rc1"))
        self.assertFalse(о.пробный("1.4"))
        self.assertIsNone(о.номер("не версия"))

    def test_выбор_по_каналу(self):
        список = [{"tag_name": "v1.3"}, {"tag_name": "v1.4"}, {"tag_name": "v1.5b1", "prerelease": True},
                  {"tag_name": "v1.6", "draft": True}, {"tag_name": "v1.5rc2"}, {"tag_name": "ночная"}]
        self.assertEqual(о.выбрать(список, "стабильный")[0], "1.4")
        self.assertEqual(о.выбрать(список, "пробный")[0], "1.5rc2")
        self.assertEqual(о.выбрать([], "пробный"), (None, None))


class ТестыКоманды(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name
        # «установленный» Дан: папка установки внутри временной папки
        self.установка = os.path.join(self.п, "программы", "dan")
        os.makedirs(self.установка)
        self.среда = mock.patch.dict(os.environ, {"DAN_CONFIG": os.path.join(self.п, "настройки")})
        self.среда.start()
        for имя in ("DAN_RELEASES_URL", "DAN_UPDATE_MODE", "DAN_UPDATE_DRY_RUN"):
            os.environ.pop(имя, None)
        self.папка_установки = mock.patch.object(о, "папка_установки", return_value=self.установка)
        self.папка_установки.start()

    def tearDown(self):
        self.папка_установки.stop()
        self.среда.stop()
        self.tmp.cleanup()

    def выпуск(self, версия, пробный=False, испортить=False):
        """Файлы выпуска на диске и описание выпусков в духе GitHub."""
        папка = os.path.join(self.п, "выпуск_" + версия)
        os.makedirs(папка)
        архив = os.path.join(папка, f"dan-{версия}-windows.zip")
        with zipfile.ZipFile(архив, "w") as з:
            з.writestr("dan/dan.exe", "новая версия " + версия)
            з.writestr("dan/справочник.html", "<p>справка</p>")
        установщик = os.path.join(папка, f"dan-{версия}-setup.exe")
        with open(установщик, "wb") as ф:
            ф.write(b"MZ installer")
        суммы = os.path.join(папка, "SHA256SUMS.txt")
        with open(суммы, "w", encoding="utf-8") as ф:
            for путь in (архив, установщик):
                with open(путь, "rb") as фф:
                    сумма = hashlib.sha256(фф.read()).hexdigest()
                if испортить:
                    сумма = "0" * 64
                ф.write(f"{сумма}  {os.path.basename(путь)}\n")
        файлы = [{"name": os.path.basename(п), "size": os.path.getsize(п), "browser_download_url": адрес(п)}
                 for п in (архив, установщик, суммы)]
        return {"tag_name": "v" + версия, "prerelease": пробный, "html_url": "https://example.org/" + версия,
                "assets": файлы}

    def опубликовать(self, *выпуски):
        путь = os.path.join(self.п, "releases.json")
        with open(путь, "w", encoding="utf-8") as ф:
            json.dump(list(выпуски), ф, ensure_ascii=False)
        os.environ["DAN_RELEASES_URL"] = адрес(путь)

    def выполнить(self, *аргументы):
        вывод = io.StringIO()
        with redirect_stdout(вывод):
            код = о.команда(list(аргументы))
        return код, вывод.getvalue()

    def test_последняя_версия(self):
        self.опубликовать(self.выпуск(versiya.ВЕРСИЯ))
        код, вывод = self.выполнить("--проверить")
        self.assertEqual(код, 0)
        self.assertIn("Установлена последняя версия", вывод)

    def test_канал_запоминается(self):
        self.опубликовать(self.выпуск(versiya.ВЕРСИЯ), self.выпуск("99.0b1", пробный=True))
        код, вывод = self.выполнить("--проверить")
        self.assertIn("Установлена последняя версия", вывод)
        код, вывод = self.выполнить("--проверить", "--канал=пробный")
        self.assertIn("Есть новая версия Дана: 99.0b1", вывод)
        self.assertEqual(о.канал(), "пробный")
        код, вывод = self.выполнить("--проверить")
        self.assertIn("99.0b1", вывод)
        self.assertEqual(self.выполнить("--канал=ночной")[0], 2)

    def test_архив_распаковывается_рядом(self):
        os.environ["DAN_UPDATE_MODE"] = "архив"
        self.опубликовать(self.выпуск("99.0"))
        код, вывод = self.выполнить()
        self.assertEqual(код, 0, вывод)
        новая = os.path.join(self.п, "программы", "dan-99.0", "dan")
        with open(os.path.join(новая, "dan.exe"), encoding="utf-8") as ф:
            self.assertEqual(ф.read(), "новая версия 99.0")
        self.assertIn(новая, вывод)

    def test_неверная_сумма_ничего_не_меняет(self):
        os.environ["DAN_UPDATE_MODE"] = "архив"
        self.опубликовать(self.выпуск("99.0", испортить=True))
        код, вывод = self.выполнить()
        self.assertEqual(код, 1)
        self.assertIn("не совпадает с контрольной суммой", вывод)
        self.assertEqual(os.listdir(os.path.join(self.п, "программы")), ["dan"])

    def test_установщик_скачивается(self):
        os.environ["DAN_UPDATE_MODE"] = "установщик"
        os.environ["DAN_UPDATE_DRY_RUN"] = "1"
        self.опубликовать(self.выпуск("99.0"))
        код, вывод = self.выполнить()
        self.assertEqual(код, 0, вывод)
        self.assertIn("Установщик готов:", вывод)
        self.assertTrue(вывод.strip().endswith("dan-99.0-setup.exe"))

    def test_из_исходников_только_совет(self):
        self.опубликовать(self.выпуск("99.0"))
        код, вывод = self.выполнить()
        self.assertEqual(код, 0)
        self.assertIn("git pull", вывод)

    def test_нет_связи(self):
        os.environ["DAN_RELEASES_URL"] = адрес(os.path.join(self.п, "нет.json"))
        код, вывод = self.выполнить("--проверить")
        self.assertEqual(код, 1)
        self.assertIn("не удалось связаться", вывод)


if __name__ == "__main__":
    unittest.main()
