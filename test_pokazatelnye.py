"""Тесты показательных программ (папка «показательные»): рабочих образцов программ общего назначения.

Программы пользуются разбором JSON в собственные типы (json.разобрать_в), который есть только у
компилятора на Дане, поэтому их переводит он (sam_kompilyator.перевести) в любом режиме тестов.
Сеть наружу не нужна: бот для Telegram обращается к местному серверу, который отвечает как
Telegram Bot API, веб-сервис слушает свободный порт на 127.0.0.1; документы Word и Excel для
сводки создаёт программа на Дане, файлы для поиска дубликатов - сам тест.
"""

import http.server
import json
import os
import sqlite3
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request

import dan
import sam_kompilyator
from test_http import ЕСТЬ_CURL
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ
from test_sqlite import ЕСТЬ_SQLITE

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ПРОГРАММЫ = os.path.join(ПАПКА, "показательные")
# запросы к местному серверу не должны уходить на прокси-сервер
СРЕДА = dict(os.environ, NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
for _имя in ("http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"):
    СРЕДА.pop(_имя, None)


def путь_программы(имя):
    return os.path.join(ПРОГРАММЫ, имя + ".dan")


def собрать(имя, папка, тесты=False):
    """Исполняемый файл программы папки «показательные» во временной папке."""
    путь = путь_программы(имя)
    with open(путь, encoding="utf-8") as ф:
        текст = ф.read()
    код_c = sam_kompilyator.перевести(текст, путь, тесты=тесты)
    return dan.собрать_код(код_c, os.path.join(папка, имя + ("_тесты" if тесты else "")), ПРОГРАММЫ, выпуск=True)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыПоказательных(unittest.TestCase):
    """Каждая программа проходит проверку и свои блоки «тест»."""

    def test_программы_описаны(self):
        имена = sorted(и[:-4] for и in os.listdir(ПРОГРАММЫ) if и.endswith(".dan"))
        self.assertGreaterEqual(len(имена), 2)
        with open(os.path.join(ПРОГРАММЫ, "README.md"), encoding="utf-8") as ф:
            обзор = ф.read()
        for имя in имена:
            with self.subTest(имя=имя):
                with open(путь_программы(имя), encoding="utf-8") as ф:
                    self.assertTrue(ф.readline().startswith("// "), "в начале программы - описание")
                self.assertIn(имя + ".dan", обзор)

    def test_единый_вид(self):
        for имя in sorted(и for и in os.listdir(ПРОГРАММЫ) if и.endswith(".dan")):
            with self.subTest(имя=имя):
                р = subprocess.run([sam_kompilyator.исполняемый(), "формат", "--проверить", os.path.join(ПРОГРАММЫ, имя)],
                                   capture_output=True, text=True, encoding="utf-8", timeout=120,
                                   env=sam_kompilyator.среда())
                self.assertEqual(р.returncode, 0, р.stdout + р.stderr)

    def test_встроенные_тесты(self):
        for имя in sorted(и[:-4] for и in os.listdir(ПРОГРАММЫ) if и.endswith(".dan")):
            with open(путь_программы(имя), encoding="utf-8") as ф:
                if "\nтест \"" not in ф.read():
                    continue
            with self.subTest(имя=имя), tempfile.TemporaryDirectory() as папка:
                р = subprocess.run([собрать(имя, папка, тесты=True)], capture_output=True, text=True,
                                   encoding="utf-8", timeout=120, cwd=папка)
                self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
                self.assertNotIn("ПРОВАЛЕН", р.stdout)


# ---------- бот для Telegram ----------

class ПоддельныйTelegram(http.server.BaseHTTPRequestHandler):
    """Отвечает как Telegram Bot API: getUpdates отдаёт заготовленные пачки обновлений,
    sendMessage записывает присланное. Состояние - в атрибутах класса сервера."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *аргументы):
        pass

    def ответить(self, код, данные):
        тело = json.dumps(данные, ensure_ascii=False).encode("utf-8") if not isinstance(данные, bytes) else данные
        self.send_response(код)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(тело)))
        self.end_headers()
        self.wfile.write(тело)

    def do_GET(self):
        с = self.server
        адрес = urllib.parse.urlsplit(self.path)
        с.пути.append(адрес.path)
        if not адрес.path.endswith("/getUpdates"):
            return self.ответить(404, {"ok": False, "error_code": 404, "description": "Not Found"})
        параметры = urllib.parse.parse_qs(адрес.query)
        с.опросы.append((int(параметры["offset"][0]), int(параметры["timeout"][0])))
        if not с.пачки:
            return self.ответить(200, {"ok": True, "result": []})
        пачка = с.пачки.pop(0)
        if пачка == "сбой":
            return self.ответить(502, b"<html>Bad Gateway</html>")
        self.ответить(200, {"ok": True, "result": пачка})

    def do_POST(self):
        с = self.server
        с.пути.append(self.path)
        длина = int(self.headers.get("Content-Length", "0"))
        данные = json.loads(self.rfile.read(длина).decode("utf-8"))
        с.отправленные.append((self.headers.get("Content-Type"), данные))
        if данные["chat_id"] == 666:
            return self.ответить(403, {"ok": False, "error_code": 403, "description": "Forbidden: bot was blocked by the user"})
        self.ответить(200, {"ok": True, "result": {"message_id": 1000 + len(с.отправленные), "chat": {"id": данные["chat_id"]},
                                                   "date": 0, "text": данные["text"]}})


def сообщение(номер, чат, текст=None, имя="Анна", **ещё):
    с = {"message_id": номер * 10, "date": 1762858800, "chat": {"id": чат, "type": "private"},
         "from": {"id": чат, "is_bot": False, "first_name": имя, "language_code": "ru"}, **ещё}
    if текст is not None:
        с["text"] = текст
    return {"update_id": номер, "message": с}


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
@unittest.skipUnless(ЕСТЬ_CURL, "нужна библиотека libcurl")
class ТестыБотаTelegram(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.исполняемый = собрать("телеграм_бот", cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.сервер = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ПоддельныйTelegram)
        self.сервер.пачки, self.сервер.опросы, self.сервер.отправленные, self.сервер.пути = [], [], [], []
        threading.Thread(target=self.сервер.serve_forever, daemon=True).start()
        self.addCleanup(self.сервер.server_close)
        self.addCleanup(self.сервер.shutdown)
        self.api = f"http://127.0.0.1:{self.сервер.server_address[1]}"

    def запустить(self, *аргументы, токен="123:ТЕСТ", настройки=None):
        среда = dict(СРЕДА)
        среда.pop("TELEGRAM_TOKEN", None)
        if токен:
            среда["TELEGRAM_TOKEN"] = токен
        if настройки is not None:
            with open(os.path.join(self.tmp.name, "бот.toml"), "w", encoding="utf-8") as ф:
                ф.write(настройки)
        elif os.path.exists(os.path.join(self.tmp.name, "бот.toml")):
            os.remove(os.path.join(self.tmp.name, "бот.toml"))
        return subprocess.run([self.исполняемый, *аргументы], capture_output=True, text=True, encoding="utf-8",
                              timeout=120, cwd=self.tmp.name, env=среда)

    def test_отвечает_на_сообщения(self):
        self.сервер.пачки = [
            "сбой",                                        # сервер временно недоступен: бот повторяет опрос
            [сообщение(41, 5, "/start"), сообщение(42, 5, None, sticker={"file_id": "x"})],
            [сообщение(43, 7, "/слова Мама мыла раму, мама!", имя="Иван"), сообщение(44, 666, "Привет"),
             сообщение(45, 7, "Добрый вечер всем")],
        ]
        р = self.запустить("--api", self.api, "--обновлений", "5", настройки="ожидание = 2\n")
        self.assertEqual(р.returncode, 0, р.stderr)
        # подтверждение обновлений: следующий опрос начинается после последнего полученного
        self.assertEqual(self.сервер.опросы, [(0, 2), (0, 2), (43, 2)])
        self.assertTrue(all(п.startswith("/bot123:%D0%A2%D0%95%D0%A1%D0%A2/") or п.startswith("/bot123:ТЕСТ/")
                            for п in self.сервер.пути), self.сервер.пути)
        отправленные = [д for _, д in self.сервер.отправленные]
        self.assertEqual({т for т, _ in self.сервер.отправленные}, {"application/json"})
        self.assertEqual([(д["chat_id"], д["reply_to_message_id"]) for д in отправленные],
                         [(5, 410), (7, 430), (666, 440), (7, 450)])
        self.assertTrue(отправленные[0]["text"].startswith("Здравствуйте, Анна!"))
        self.assertEqual(отправленные[1]["text"], "Слов: 4, разных: 3\nЧаще всего: «мама» (2 раза)")
        self.assertEqual(отправленные[3]["text"], "Добрый вечер всем\n(3 слова)")
        # журнал: сбой опроса, отказ отправки и итог
        self.assertIn("ПРЕДУПРЕЖДЕНИЕ бот: опрос не удался", р.stderr)
        self.assertIn("ОШИБКА бот: ответ не отправлен обновление=44", р.stderr)
        self.assertIn("bot was blocked by the user", р.stderr)
        self.assertIn("работа закончена обработано=5", р.stderr)
        self.assertNotIn("123:ТЕСТ", р.stderr, "токен не попадает в журнал")

    def test_адрес_и_токен_из_настроек(self):
        self.сервер.пачки = [[сообщение(1, 9, "/помощь")]]
        р = self.запустить("--обновлений", "1", токен="",
                           настройки=f'токен = "777:abc"\nадрес_api = "{self.api}"\nожидание = 1\n')
        self.assertEqual(р.returncode, 0, р.stderr)
        self.assertEqual(self.сервер.пути[0], "/bot777:abc/getUpdates")
        self.assertIn("/слова текст", self.сервер.отправленные[0][1]["text"])

    def test_без_токена(self):
        р = self.запустить("--api", self.api, токен="")
        self.assertEqual(р.returncode, 1)
        self.assertIn("нет токена", р.stderr)
        self.assertEqual(self.сервер.пути, [])

    def test_справка_и_неверный_ключ(self):
        р = self.запустить("--справка")
        self.assertEqual(р.returncode, 0)
        self.assertIn("--обновлений", р.stdout)
        р = self.запустить("--погода")
        self.assertEqual(р.returncode, 2)
        self.assertIn("неизвестный ключ --погода", р.stderr)


# ---------- веб-сервис списка задач ----------

@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ and ЕСТЬ_SQLITE, "нужны компилятор C и библиотека SQLite")
class ТестыСервисаЗадач(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.исполняемый = собрать("задачи_сервис", cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def запустить(self, запросов):
        self.база = os.path.join(self.tmp.name, f"задачи_{self.id().rsplit('.', 1)[-1]}.db")
        if os.path.exists(self.база):
            os.remove(self.база)
        self.процесс = subprocess.Popen([self.исполняемый, "--порт", "0", "--база", self.база,
                                         "--запросов", str(запросов)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=self.tmp.name, env=СРЕДА)
        self.addCleanup(self.остановить)
        строка = self.процесс.stdout.readline()
        if not строка.strip().isdigit():
            self.процесс.kill()
            self.fail((строка + self.процесс.stderr.read()).decode("utf-8", "replace"))
        self.адрес = f"http://127.0.0.1:{int(строка)}"

    def остановить(self):
        if self.процесс.poll() is None:
            self.процесс.kill()
        self.процесс.wait(30)
        self.процесс.stdout.close()
        self.процесс.stderr.close()

    def завершение(self):
        код = self.процесс.wait(60)
        журнал = self.процесс.stderr.read().decode("utf-8")
        self.assertEqual(код, 0, журнал)
        return журнал

    def запрос(self, метод, путь, данные=None):
        тело = None if данные is None else (данные if isinstance(данные, bytes) else json.dumps(данные).encode())
        з = urllib.request.Request(self.адрес + urllib.parse.quote(путь, safe="/?=&"), data=тело, method=метод,
                                   headers={"Content-Type": "application/json"} if тело is not None else {})
        try:
            with urllib.request.urlopen(з, timeout=30) as о:
                return о.status, о.headers, о.read().decode("utf-8")
        except urllib.error.HTTPError as е:
            return е.code, е.headers, е.read().decode("utf-8")

    def json(self, метод, путь, данные=None):
        код, заголовки, текст = self.запрос(метод, путь, данные)
        if код != 204:
            self.assertEqual(заголовки["Content-Type"], "application/json; charset=utf-8")
        return код, json.loads(текст) if текст else None

    def test_создание_изменение_удаление(self):
        self.запустить(15)
        код, з1 = self.json("POST", "/задачи", {"title": "  Позвонить в суд  ", "лишнее": [1, 2]})
        self.assertEqual(код, 201)
        self.assertEqual((з1["id"], з1["title"], з1["done"]), (1, "Позвонить в суд", False))
        self.assertRegex(з1["created"], r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d$")
        код, з2 = self.json("POST", "/задачи", {"title": "Купить \"бумагу\" 😀"})
        self.assertEqual((код, з2["id"]), (201, 2))
        self.assertEqual(self.json("GET", "/задачи/2"), (200, з2))
        код, изм = self.json("PATCH", "/задачи/1", {"done": True})
        self.assertEqual((код, изм["done"], изм["title"]), (200, True, "Позвонить в суд"))
        код, изм = self.json("PATCH", "/задачи/2", {"title": "Купить бумагу", "done": None})
        self.assertEqual((код, изм["title"], изм["done"]), (200, "Купить бумагу", False))
        self.assertEqual(self.json("GET", "/задачи?готово=да")[1], [{**з1, "done": True}])
        self.assertEqual([з["id"] for з in self.json("GET", "/задачи?готово=нет")[1]], [2])
        self.assertEqual(self.json("DELETE", "/задачи/1"), (204, None))
        self.assertEqual(self.json("GET", "/задачи/1"), (404, {"error": "нет задачи 1"}))
        self.assertEqual([з["id"] for з in self.json("GET", "/задачи")[1]], [2])
        # ошибки клиента
        код, о = self.json("POST", "/задачи", {"name": "без заголовка"})
        self.assertEqual(код, 400)
        self.assertIn("нет поля «title»", о["error"])
        self.assertEqual(self.json("POST", "/задачи", {"title": "   "}), (400, {"error": "пустой заголовок"}))
        self.assertEqual(self.json("PATCH", "/задачи/2", "{\"done\": \"да\"}".encode())[0], 400)
        self.assertEqual(self.json("GET", "/задачи/abc")[0], 400)
        self.assertEqual(self.json("PUT", "/задачи")[0], 405)
        журнал = self.завершение()
        # база - обычный файл SQLite: её читает и Python
        with sqlite3.connect(self.база) as б:
            self.assertEqual(б.execute("SELECT номер, заголовок, готово FROM задачи").fetchall(),
                             [(2, "Купить бумагу", 0)])
        строки = [с for с in журнал.splitlines() if " СВЕДЕНИЯ задачи: " in с]
        self.assertIn("сервис запущен", строки[0])
        self.assertIn("POST /задачи код=201", строки[1])
        self.assertIn("DELETE /задачи/1 код=204", журнал)
        self.assertIn("PUT /задачи код=405", журнал)
        self.assertEqual(sum(" код=" in с for с in строки), 15)

    def test_неизвестный_адрес_и_пустой_список(self):
        self.запустить(2)
        self.assertEqual(self.json("GET", "/задачи"), (200, []))
        self.assertEqual(self.json("GET", "/дела")[0], 404)
        self.завершение()


# ---------- сводка документов ----------

# документы Word и Excel для сводки создаёт программа на Дане (модули word и excel)
ДОКУМЕНТЫ = """подключить "word"
подключить "excel"
word.создать("док/счёт_1.docx", [word.заголовок("Счёт № 1", 1), word.абзац("Поставщик: ООО «Ромашка»"),
    word.таблица([["№", "Наименование", "Количество", "Цена", "Сумма"], ["1", "Бумага А4", "10", "350", "3 500,00"],
        ["2", "Ручка", "5", "40,5", "202,50"], ["", "Итого", "", "", "3 702,50"]])])!
excel.создать("док/счёт_2.xlsx", [excel.Лист("Счёт", [["Счёт № 2"], [], ["Товар", "Кол-во", "Сумма"],
    ["бумага а4", "4", "1400"], ["Степлер", "1", "не указана"]], ложь)])!
"""


def листы_xlsx(путь):
    """{имя листа: строки} книги .xlsx (ячейки - текст и числа, как записаны в файле)."""
    import re
    import zipfile
    with zipfile.ZipFile(путь) as з:
        имена = re.findall(r'<sheet name="([^"]*)"', з.read("xl/workbook.xml").decode("utf-8"))
        итог = {}
        for номер, имя in enumerate(имена, 1):
            лист = з.read(f"xl/worksheets/sheet{номер}.xml").decode("utf-8")
            итог[имя] = [[(т or з_).replace("&amp;", "&").replace("&lt;", "<").replace("&quot;", '"')
                          for т, з_ in re.findall(r"<c [^>]*>(?:<is><t[^>]*>([^<]*)</t></is>|<v>([^<]*)</v>)</c>", ряд)]
                         for ряд in re.findall(r"<row[^>]*>(.*?)</row>", лист)]
        return итог


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыСводкиДокументов(unittest.TestCase):

    def test_сводка_по_папке(self):
        with tempfile.TemporaryDirectory() as папка:
            os.makedirs(os.path.join(папка, "док"))
            код_c = sam_kompilyator.перевести(ДОКУМЕНТЫ, os.path.join(папка, "документы.dan"))
            создатель = dan.собрать_код(код_c, os.path.join(папка, "документы"), папка)
            subprocess.run([создатель], check=True, cwd=папка, timeout=120)
            with open(os.path.join(папка, "док", "счёт_3.csv"), "w", encoding="utf-8") as ф:
                ф.write("Наименование;Количество;Сумма\nРучка;10;405,00\nСкрепки;100;99,90\n")
            with open(os.path.join(папка, "док", "заметка.csv"), "w", encoding="utf-8") as ф:
                ф.write("просто текст\n")
            with open(os.path.join(папка, "док", "~$счёт_1.docx"), "wb") as ф:
                ф.write(b"\x00")                              # файл блокировки Word не читается
            программа = собрать("сводка_документов", папка)
            for раз in range(2):                              # повторный запуск не учитывает свою сводку
                р = subprocess.run([программа, "док", "--вывод", "док/сводка.xlsx"], capture_output=True, text=True,
                                   encoding="utf-8", timeout=120, cwd=папка)
                self.assertEqual(р.returncode, 0, р.stderr)
                self.assertEqual(р.stdout.splitlines(), ["Файлов: 4, строк: 5, пропущено: 2", "Всего: 5 607,40",
                                                         "Сводка: док/сводка.xlsx"])
            self.assertIn("прочитан файл=счёт_1.docx строк=2", р.stderr)
            листы = листы_xlsx(os.path.join(папка, "док", "сводка.xlsx"))
            self.assertEqual(list(листы), ["Итоги", "Строки", "Пропущено"])
            self.assertEqual(листы["Итоги"], [["Наименование", "Количество", "Сумма", "Файлов"],
                                              ["Бумага А4", "14", "4900.00", "2"], ["Ручка", "15", "607.50", "2"],
                                              ["Скрепки", "100", "99.90", "1"], ["Итого", "5607.40", "4"]])
            self.assertEqual(листы["Строки"][1], ["счёт_1.docx", "Бумага А4", "10", "3500.00"])
            self.assertEqual(len(листы["Строки"]), 6)
            self.assertEqual(листы["Пропущено"][1:], [
                ["заметка.csv", "нет таблицы со столбцами «Наименование», «Количество», «Сумма»"],
                ["счёт_2.xlsx", "строка 5: не число в столбце количества или суммы"]])
            р = subprocess.run([программа, "нет_такой"], capture_output=True, text=True, encoding="utf-8",
                               timeout=60, cwd=папка)
            self.assertEqual(р.returncode, 1)
            self.assertIn("нет папки «нет_такой»", р.stderr)


# ---------- поиск дубликатов ----------

@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыДубликатов(unittest.TestCase):

    def test_группы_одинаковых(self):
        import hashlib
        with tempfile.TemporaryDirectory() as папка:
            программа = собрать("дубликаты", папка)
            корень = os.path.join(папка, "файлы")
            for путь, данные in (("1.txt", "один"), ("а/копия.txt", "один"), ("а/б/ещё.txt", "один"),
                                 ("в/почти.txt", "одиН"), ("в/2.txt", "другой"), ("2.txt", "другой"),
                                 ("пусто1", ""), ("пусто2", ""), ("большой.bin", "х" * 5000),
                                 ("в/большой копия.bin", "х" * 5000)):
                os.makedirs(os.path.dirname(os.path.join(корень, путь)), exist_ok=True)
                with open(os.path.join(корень, путь), "w", encoding="utf-8") as ф:
                    ф.write(данные)
            if os.name != "nt":
                os.symlink("../1.txt", os.path.join(корень, "в", "ссылка.txt"))
                os.symlink("..", os.path.join(корень, "в", "петля"))      # ссылка на папку выше не зацикливает обход
            р = subprocess.run([программа, "файлы", "--json", "-м", "0"], capture_output=True, text=True,
                               encoding="utf-8", timeout=120, cwd=папка)
            self.assertEqual(р.returncode, 0, р.stderr)
            отчёт = json.loads(р.stdout)
            self.assertEqual(отчёт["scanned"], 10)
            self.assertEqual(отчёт["wasted_bytes"], 2 * len("один".encode()) + len("другой".encode()) + 10000)
            группы = [(г["size"], г["files"]) for г in отчёт["groups"]]
            self.assertEqual(группы, [
                (10000, ["файлы/большой.bin", "файлы/в/большой копия.bin"]),
                (8, ["файлы/1.txt", "файлы/а/б/ещё.txt", "файлы/а/копия.txt"]),     # 16 лишних байтов
                (12, ["файлы/2.txt", "файлы/в/2.txt"]),                                 # 12
                (0, ["файлы/пусто1", "файлы/пусто2"])])
            self.assertEqual(отчёт["groups"][1]["sha256"], hashlib.sha256("один".encode()).hexdigest())
            р = subprocess.run([программа, "файлы/"], capture_output=True, text=True, encoding="utf-8",
                               timeout=120, cwd=папка)
            self.assertEqual(р.returncode, 0, р.stderr)
            self.assertIn("3 одинаковых по 8 Б", р.stdout)
            self.assertIn("2 одинаковых по 9.8 КБ", р.stdout)
            self.assertNotIn("пусто1", р.stdout)                  # пустые файлы - только с --мин-размер 0
            self.assertTrue(р.stdout.splitlines()[-1].startswith("Просмотрено файлов: 8, групп одинаковых: 3"))



# ---------- чат в окне ----------

@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыЧатаВОкне(unittest.TestCase):
    """Окна есть только в Windows; режим без окна порождает ответ теми же шагами, что окно,
    и с --сверить сравнивает его с порождением без прерываний на крошечной модели (gguf_malaya)."""

    def test_пошаговый_ответ_совпадает(self):
        import gguf_malaya
        with tempfile.TemporaryDirectory() as папка:
            модель = os.path.join(папка, "м.gguf")
            gguf_malaya.создать(модель)
            исп = собрать("чат_в_окне", папка)
            р = subprocess.run([исп, "--без-окна", "--сверить", модель, "Привет", "Как дела?"],
                               capture_output=True, text=True, encoding="utf-8", timeout=600, cwd=папка)
            self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
            строки = р.stdout.splitlines()
            self.assertEqual(len(строки), 2, р.stdout)
            for с in строки:
                self.assertTrue(с.startswith("совпадает: "), р.stdout)
            р = subprocess.run([исп, "--без-окна", модель], capture_output=True, text=True, encoding="utf-8",
                               timeout=120, cwd=папка)
            self.assertEqual(р.returncode, 2)
            self.assertIn("Укажите модель и вопросы", р.stdout)


if __name__ == "__main__":
    unittest.main()
