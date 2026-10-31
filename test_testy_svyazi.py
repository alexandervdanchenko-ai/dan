"""Быстрый запуск тестов (testy.py) и выбор тестов по изменённым файлам (testy_svyazi.py)."""

import os
import subprocess
import unittest

import testy
import testy_svyazi

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def файлы_репозитория():
    р = subprocess.run(["git", "-c", "core.quotepath=off", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=ПАПКА, capture_output=True, text=True,
                       encoding="utf-8")
    return [с for с in р.stdout.splitlines() if с.strip()] if р.returncode == 0 else []


class ТестыВыбора(unittest.TestCase):

    def test_единицы_как_у_unittest(self):
        # testy.py находит те же классы тестов, что unittest discover
        набор = unittest.defaultTestLoader.discover(ПАПКА, pattern="test_*.py")
        классы = set()

        def обойти(н):
            if isinstance(н, unittest.TestSuite):
                for ч in н:
                    обойти(ч)
            elif not type(н).__name__.startswith("_Failed"):
                классы.add(f"{type(н).__module__}.{type(н).__name__}")
        обойти(набор)
        self.assertEqual(set(testy.единицы()), классы)

    def test_каждый_тест_выбирается_изменением_кода(self):
        # новый файл тестов не выпадает из выбора: его выбирает изменение хотя бы одного файла,
        # кроме него самого (таблица, подключение модуля или упоминание папки)
        файлы = [ф for ф in файлы_репозитория() if not os.path.basename(ф).startswith("test_")]
        if not файлы:
            self.skipTest("нет git")
        выбранные = set()
        for ф in файлы:
            тесты = testy_svyazi.тесты_для_файла(ф)
            if тесты:
                выбранные |= {т.split(".")[0] for т in тесты}
        невыбранные = sorted(set(testy_svyazi._файлы_тестов()) - выбранные)
        self.assertEqual(невыбранные, [], "эти тесты не выбирает ни одно изменение: добавьте их в СВЯЗИ")

    def test_пересоздать_знает_все_сценарии(self):
        import glob
        import пересоздать
        сценарии = {os.path.relpath(п, ПАПКА).replace(os.sep, "/")
                    for п in glob.glob(os.path.join(ПАПКА, "создать_*.py")) + glob.glob(os.path.join(ПАПКА, "сам", "создать_*.py"))
                    + glob.glob(os.path.join(ПАПКА, "semya", "создать_*.py"))}
        self.assertEqual({с for с, _ in пересоздать.СЦЕНАРИИ}, сценарии)
        self.assertEqual(пересоздать.главная(["--проверить"]), 0)

    def test_примеры_выбора(self):
        т = testy_svyazi.тесты_для
        self.assertIn("test_json", т(["test_json.py"]))
        self.assertIn("test_json", т(["библиотека/json.dan"]))
        self.assertIn("test_sam.ТестыСамосборки", т(["библиотека/json.dan"]))      # сам/отчёты.dan подключает json
        self.assertIn("test_sam.ТестыПорожденияНаДане", т(["сам/порождение.dan"]))
        self.assertNotIn("test_sam.ТестыДокументацииНаДане", т(["сам/порождение.dan"]))
        self.assertIn("test_dannye", т(["sreda/dannye.c"]))
        self.assertIn("test_sam.ТестыПроверкиНаДане", т(["test_proverka.py"]))  # программы из тестов проверки
        self.assertEqual(т(["LICENSE"]), set())
        self.assertIsNone(т(["новый_неизвестный_файл.xyz"]))
        # класс не повторяется, если выбран весь файл
        self.assertNotIn("test_sam.ТестыСамосборки", т(["lexer.py", "dan.py"]))


if __name__ == "__main__":
    unittest.main()
