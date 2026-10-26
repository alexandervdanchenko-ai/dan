"""Тесты базы данных SQLite (модуль библиотека/sqlite.dan).

Файл базы, созданный программой на Дане, проверяется модулем sqlite3
языка Python, и наоборот: так видно, что значения всех типов записываются
и читаются без искажений.
"""

import contextlib
import os
import random
import sqlite3
import subprocess
import tempfile
import time
import unittest

import dan
from test_kodogen import выполнить, КОМПИЛЯТОР_ЕСТЬ
from test_sistema import строка_дан

ЕСТЬ_SQLITE = any(os.path.exists(os.path.join(п, "libsqlite3.so.0"))
                  for п in ("/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/lib64", "/usr/lib/aarch64-linux-gnu"))


def запустить(текст, папка):
    путь = os.path.join(папка, "программа.dan")
    текст = текст.replace("ПАПКА", папка.replace("\\", "/"))
    with open(путь, "w", encoding="utf-8") as ф:
        ф.write(текст)
    начало = time.perf_counter()
    р = subprocess.run([dan.собрать(путь, текст)], capture_output=True, text=True, encoding="utf-8", timeout=120)
    return р.returncode, р.stdout, р.stderr, time.perf_counter() - начало


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ and ЕСТЬ_SQLITE, "нужны компилятор C и библиотека SQLite")
class ТестыSQLite(unittest.TestCase):
    maxDiff = None

    def test_запросы_и_типы(self):
        код, вывод, ош = выполнить(r"""
подключить "sqlite"
пусть база = sqlite.в_памяти()!
база.выполнить("CREATE TABLE дела (номер TEXT UNIQUE NOT NULL, сумма INTEGER, ставка REAL, файл BLOB)", [])!
пусть итог = база.выполнить("INSERT INTO дела VALUES (?, ?, ?, ?)",
    [sqlite.Текст("А40-1/2026"), sqlite.Целое(9223372036854775807), sqlite.Число(0.1), sqlite.Байты(байты([0, 1, 255]))])!
вывод(стр(итог.изменено) + " " + стр(итог.последний_номер))
база.выполнить("INSERT INTO дела (номер, сумма) VALUES (?, ?); INSERT INTO дела (номер) VALUES ('Б-2')",
    [sqlite.Текст("Иванов \"И\"'); DROP TABLE дела; --"), sqlite.Пусто])!
для с в база.выбрать("SELECT * FROM дела ORDER BY rowid", [])! {
    вывод(с["номер"].текст() + " | " + с["сумма"].текст() + " | " + с["ставка"].текст() + " | "
          + стр((с["файл"].как_байты() иначе байты("")).длина))
}
пусть все = база.выполнить("SELECT номер, сумма * 2 AS двойная FROM дела WHERE номер = ?", [sqlite.Текст("Б-2")])!
вывод(все.столбцы)
вывод(все.строки.длина)
вывод(все.строки[0][1].пусто())
вывод(база.значение("SELECT COUNT(*) FROM дела", [])!.как_цел() иначе -1)
вывод(база.значение("SELECT сумма FROM дела WHERE rowid = 1", [])!.как_цел() иначе -1)
вывод(база.значение("SELECT ставка FROM дела WHERE rowid = 1", [])!.как_дроб() иначе -1.0)
вывод(база.значение("SELECT 1 WHERE 0", [])!.пусто())
вывод(база.значение("SELECT upper(?) || '!'", [sqlite.Текст("abc")])!.как_стр() иначе "")
если ошибка о = база.выполнить("INSERT INTO дела (номер) VALUES (?)", [sqlite.Текст("Б-2")]) { вывод(о) }
если ошибка о = база.выполнить("INSERT INTO дела (сумма) VALUES (1)", []) { вывод(о) }
если ошибка о = база.выполнить("SELEC 1", []) { вывод(о) }
если ошибка о = база.выполнить("SELECT ?, ?", [sqlite.Целое(1)]) { вывод(о) }
если ошибка о = база.выполнить("SELECT 1; SELECT ?", [sqlite.Целое(1)]) { вывод(о) }
если ошибка о = база.выполнить("SELECT ?; SELECT ?", [sqlite.Целое(1)]) { вывод(о) }
база.закрыть()
""")
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), [
            "1 1", "А40-1/2026 | 9223372036854775807 | 0.1 | 3", "Иванов \"И\"'); DROP TABLE дела; -- |  |  | 0",
            "Б-2 |  |  | 0", '["номер", "двойная"]', "1", "истина", "3", "9223372036854775807", "0.1", "истина",
            "ABC!", "нарушено ограничение таблицы (UNIQUE constraint failed: дела.номер)",
            "нарушено ограничение таблицы (NOT NULL constraint failed: дела.номер)",
            'ошибка в запросе (near "SELEC": syntax error)',
            "число параметров «?» в запросе - 2, а передано значений - 1",
            "число параметров «?» в запросе - 0, а передано значений - 1",
            "параметры «?» допускаются только в первом запросе из нескольких"])

    def test_файл_читает_python(self):
        сл = random.Random(3)
        значения = [(н, "".join(сл.choice("абвгдеёжзийклмнопрстуфхцчшщъыьэюя ABC'\"%_\\😀") for _ in range(сл.randint(0, 20))),
                     сл.uniform(-1e9, 1e9), bytes(сл.randrange(256) for _ in range(сл.randint(0, 50))))
                    for н in range(200)]
        строки = "\n".join(f"    данные.добавить([sqlite.Целое({н}), sqlite.Текст({строка_дан(т)}), sqlite.Число({repr(ч)}), "
                           f"sqlite.Байты(байты([{', '.join(map(str, б))}]))])"
                           for н, т, ч, б in значения)
        with tempfile.TemporaryDirectory() as п:
            код, вывод, ош, время = запустить(f"""
подключить "sqlite"
пусть база = sqlite.открыть("ПАПКА/дела.db")!
база.выполнить("CREATE TABLE т (н INTEGER PRIMARY KEY, текст TEXT, число REAL, данные BLOB)", [])!
перем данные: [[sqlite.Значение]] = []
{строки}
база.выполнить("BEGIN", [])!
для строка в данные {{ база.выполнить("INSERT INTO т VALUES (?, ?, ?, ?)", строка)! }}
база.выполнить("COMMIT", [])!
база.закрыть()
вывод("готово")
""", п)
            self.assertEqual(код, 0, ош)
            with contextlib.closing(sqlite3.connect(os.path.join(п, "дела.db"))) as соединение:
                прочитано = соединение.execute("SELECT н, текст, число, данные FROM т ORDER BY н").fetchall()
            self.assertEqual(прочитано, значения)

    def test_читает_файл_python(self):
        with tempfile.TemporaryDirectory() as п:
            # with над соединением только подтверждает транзакцию; closing закрывает файл,
            # иначе в Windows временную папку нельзя удалить
            with contextlib.closing(sqlite3.connect(os.path.join(п, "реестр.db"))) as соединение, соединение:
                соединение.execute("CREATE TABLE реестр (фио TEXT, сумма INTEGER, дата TEXT)")
                соединение.executemany("INSERT INTO реестр VALUES (?, ?, ?)",
                                       [("Иванов И.И.", 1000, "2026-10-26"), ("Петрова А.А.", 2500, "2026-11-01"),
                                        ("Сидоров", None, "2026-12-31")])
            код, вывод, ош, _ = запустить("""
подключить "sqlite"
пусть база = sqlite.открыть("ПАПКА/реестр.db")!
для запись в база.выбрать("SELECT * FROM реестр WHERE дата >= ? ORDER BY фио", [sqlite.Текст("2026-11-01")])! {
    пусть д = дата(запись["дата"].текст())
    вывод(запись["фио"].текст() + ": " + стр(д) + ", " + стр(запись["сумма"].как_цел() иначе 0))
}
""", п)
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), ["Петрова А.А.: 01.11.2026, 2500", "Сидоров: 31.12.2026, 0"])

    def test_скорость_и_закрытая_база(self):
        with tempfile.TemporaryDirectory() as п:
            код, вывод, ош, время = запустить("""
подключить "sqlite"
пусть база = sqlite.открыть("ПАПКА/скорость.db")!
база.выполнить("CREATE TABLE т (н INTEGER, т TEXT)", [])!
база.выполнить("BEGIN", [])!
для н в 1..20000 { база.выполнить("INSERT INTO т VALUES (?, ?)", [sqlite.Целое(н), sqlite.Текст("строка " + стр(н))])! }
база.выполнить("COMMIT", [])!
вывод(база.значение("SELECT SUM(н) FROM т", [])!.как_цел() иначе 0)
вывод(база.выбрать("SELECT * FROM т WHERE н % 1000 = 0", [])!.длина)
база.закрыть()
база.выполнить("SELECT 1", [])!
""", п)
        self.assertEqual(код, 1)
        self.assertEqual(вывод.splitlines(), ["200010000", "20"])
        self.assertIn("база данных закрыта", ош)
        self.assertLess(время, 15)

    def test_ошибка_открытия(self):
        код, вывод, ош = выполнить('подключить "sqlite"\n'
                                   'если ошибка о = sqlite.открыть("/нет/такой/папки/база.db") { вывод(о) }\n'
                                   'если ошибка о = sqlite.открыть("/etc/hostname") {} иначе { вывод("открыто") }')
        self.assertEqual(код, 0, ош)
        self.assertIn("/нет/такой/папки/база.db: не удалось открыть файл базы данных", вывод)


if __name__ == "__main__":
    unittest.main()
