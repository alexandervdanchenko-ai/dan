"""Тесты запросов HTTP (модуль библиотека/http.dan и функция сетевой_запрос).

Программы обращаются к серверу HTTP, который тест запускает на этом же
компьютере (127.0.0.1), поэтому подключение к интернету не нужно. Если
задана переменная DAN_WINDOWS=1 и есть MinGW и Wine, те же запросы
проверяются в программе для Windows (WinHTTP).
"""

import gzip
import http.server
import json
import os
import shutil
import tempfile
import threading
import time
import unittest
import urllib.parse

import dan
from test_kodogen import выполнить, КОМПИЛЯТОР_ЕСТЬ
import test_windows

ЕСТЬ_CURL = any(os.path.exists(os.path.join(п, и)) for п in ("/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/lib64",
                                                              "/usr/lib/aarch64-linux-gnu")
                for и in ("libcurl.so.4", "libcurl-gnutls.so.4"))


class Обработчик(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *аргументы):
        pass

    def ответить(self, код, тело, заголовки=()):
        self.send_response(код)
        for имя, значение in заголовки:
            self.send_header(имя, значение)
        self.send_header("Content-Length", str(len(тело)))
        self.end_headers()
        self.wfile.write(тело)

    def do_GET(self):
        путь = urllib.parse.unquote(self.path)
        if путь.startswith("/текст"):
            тело = ("путь " + путь + "; X-Дело " + str(self.headers.get("X-Delo"))).encode()
            self.ответить(200, тело, [("Content-Type", "text/plain; charset=utf-8"), ("Set-Cookie", "a=1"),
                                     ("Set-Cookie", "b=2")])
        elif путь == "/нет":
            self.ответить(404, "не найдено".encode())
        elif путь == "/переезд":
            self.ответить(302, b"", [("Location", urllib.parse.quote("/текст/после"))])
        elif путь == "/сжато":
            тело = "сжатый ответ ".encode() * 100
            if "gzip" in self.headers.get("Accept-Encoding", ""):
                self.ответить(200, gzip.compress(тело), [("Content-Encoding", "gzip")])
            else:
                self.ответить(200, тело)
        elif путь == "/файл":
            self.ответить(200, bytes(range(256)) * 40, [("Content-Type", "application/octet-stream")])
        elif путь == "/долго":
            time.sleep(3)
            try:
                self.ответить(200, "поздно".encode())
            except OSError:       # программа уже закрыла соединение, не дождавшись ответа
                pass
        elif путь.startswith("/номер/"):
            self.ответить(200, путь[len("/номер/"):].encode())
        else:
            self.ответить(400, b"?")

    def do_POST(self):
        длина = int(self.headers.get("Content-Length", 0))
        данные = self.rfile.read(длина)
        ответ = {"путь": urllib.parse.unquote(self.path), "тип": self.headers.get("Content-Type"),
                 "данные": данные.decode("utf-8", "replace"), "метод": "POST"}
        self.ответить(201, json.dumps(ответ, ensure_ascii=False).encode(), [("Content-Type", "application/json")])

    def do_DELETE(self):
        self.ответить(204, b"")

    def do_PUT(self):
        длина = int(self.headers.get("Content-Length", 0))
        self.ответить(200, b"PUT " + self.rfile.read(длина))


class Сервер:
    def __enter__(self):
        self.сервер = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Обработчик)
        self.поток = threading.Thread(target=self.сервер.serve_forever, daemon=True)
        self.поток.start()
        return f"http://127.0.0.1:{self.сервер.server_address[1]}"

    def __exit__(self, *а):
        self.сервер.shutdown()
        self.сервер.server_close()


ПРОГРАММА = """
подключить "http"
подключить "json"
пусть адрес = "АДРЕС"
пусть о = http.получить(адрес + "/текст/Иванов И.И.?дело=А40")!
вывод(о.код)
вывод(о.текст())
вывод(о.заголовок("Content-Type") иначе "")
вывод(о.заголовок("set-cookie") иначе "")
вывод(о.успешно())
пусть н = http.получить(адрес + "/нет")!
вывод(стр(н.код) + " " + н.текст() + " " + стр(н.успешно()))
если ошибка е = http.получить_текст(адрес + "/нет") { вывод(е) }
вывод(http.получить_текст(адрес + "/переезд") иначе "ошибка")
вывод(http.получить_текст(адрес + "/сжато") иначе "ошибка")
пусть ф = http.получить(адрес + "/файл")!
вывод(ф.данные.длина)
вывод(ф.данные[255])
пусть п = http.отправить_json(адрес + "/приём", json.Объект({"истец": json.Строка("Иванов")}))!
вывод(п.код)
пусть ответ = п.как_json()!
вывод(ответ.поле("тип").как_стр() иначе "")
вывод(ответ.поле("данные").как_стр() иначе "")
пусть ф2 = http.отправить_форму(адрес + "/форма", {"фио": "Иванов И.И.", "сумма": "100 & 200"})!
вывод(ф2.как_json()!.поле("данные").как_стр() иначе "")
пусть у = http.запрос("delete", адрес + "/дело/1", {}, байты(""), 10)!
вывод(у.код)
пусть з = http.запрос("PUT", адрес + "/дело/1", {"X-Delo": "1"}, байты("новое"), 10)!
вывод(з.текст())
если ошибка е = http.запрос("GET", адрес + "/долго", {}, байты(""), 1) { вывод(е) }
если ошибка е = http.получить("http://127.0.0.1:1/") { вывод(е) }
если ошибка е = http.получить("ftp://127.0.0.1/") { вывод(е) }
"""

ОЖИДАЕТСЯ = [
    "200", "путь /текст/Иванов И.И.?дело=А40; X-Дело None", "text/plain; charset=utf-8", "a=1, b=2", "истина",
    "404 не найдено ложь", "АДРЕС/нет: сервер ответил кодом 404", "путь /текст/после; X-Дело None",
    "сжатый ответ " * 99 + "сжатый ответ ", "10240", "255", "201", "application/json; charset=utf-8",
    '{"истец":"Иванов"}', "%D1%84%D0%B8%D0%BE=%D0%98%D0%B2%D0%B0%D0%BD%D0%BE%D0%B2%20%D0%98.%D0%98."
    "&%D1%81%D1%83%D0%BC%D0%BC%D0%B0=100%20%26%20200", "204", "PUT новое",
    "АДРЕС/долго: сервер не ответил за отведённое время", "http://127.0.0.1:1/: не удалось подключиться к серверу",
    "ftp://127.0.0.1/: адрес должен начинаться с http:// или https://",
]


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ and ЕСТЬ_CURL, "нужны компилятор C и библиотека libcurl")
class ТестыHTTP(unittest.TestCase):
    maxDiff = None

    def test_запросы(self):
        with Сервер() as адрес:
            код, вывод, ош = выполнить(ПРОГРАММА.replace("АДРЕС", адрес))
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.rstrip("\n").split("\n"), [с.replace("АДРЕС", адрес) for с in ОЖИДАЕТСЯ])

    def test_скачать_файл(self):
        with Сервер() as адрес, tempfile.TemporaryDirectory() as п:
            путь = os.path.join(п, "скачано.bin")
            код, вывод, ош = выполнить(f'подключить "http"\nhttp.скачать("{адрес}/файл", "{путь}")!\n'
                                       f'если ошибка е = http.скачать("{адрес}/нет", "{путь}.2") {{ вывод(е) }}')
            self.assertEqual(код, 0, ош)
            with open(путь, "rb") as ф:
                self.assertEqual(ф.read(), bytes(range(256)) * 40)
            self.assertFalse(os.path.exists(путь + ".2"))
            self.assertIn("сервер ответил кодом 404", вывод)

    def test_параллельные_запросы(self):
        текст = """
подключить "http"
функ взять(адрес: стр, н: цел) -> цел {
    вернуть разобрать_цел(http.получить_текст(адрес + "/номер/" + стр(н)) иначе "0") иначе 0
}
пусть итоги: канал<цел> = канал(100)
функ шаг(адрес: стр, н: цел, к: канал<цел>) { к.отправить(взять(адрес, н)) }
параллельно {
    для н в 1..40 { запустить шаг("АДРЕС", н, итоги) }
}
итоги.закрыть()
перем сумма = 0
для х в итоги { сумма += х }
вывод(сумма)
"""
        with Сервер() as адрес:
            код, вывод, ош = выполнить(текст.replace("АДРЕС", адрес))
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.strip(), "820")

    def test_русские_имена_серверов(self):
        # внутренняя функция модуля проверяется через его копию, где она объявлена общей
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "библиотека", "http.dan"),
                  encoding="utf-8") as ф:
            модуль = ф.read().replace("\nфунк международное_имя", "\nобщий функ международное_имя")
        имена = ["суд.рф", "user@пример.испытание:8080", "Москва.РФ", "bücher.de", "ёлка-2026.рф", "example.com"]
        with tempfile.TemporaryDirectory() as п:
            with open(os.path.join(п, "сеть.dan"), "w", encoding="utf-8") as ф:
                ф.write(модуль)
            текст = ('подключить "сеть.dan"\n'
                     f'для имя в [{", ".join(chr(34) + и + chr(34) for и in имена)}] {{ вывод(сеть.международное_имя(имя)) }}')
            путь = os.path.join(п, "программа.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            import subprocess
            р = subprocess.run([dan.собрать(путь, текст)], capture_output=True, text=True, encoding="utf-8")
        ожидается = []
        for и in имена:
            пользователь, _, хост = и.rpartition("@")
            хост, _, порт = хост.partition(":")
            ожидается.append((пользователь + "@" if пользователь else "") + хост.encode("idna").decode()
                             + (":" + порт if порт else ""))
        self.assertEqual(р.stdout.splitlines(), ожидается)


@unittest.skipUnless(test_windows.ВКЛЮЧЕНО, "проверка Windows включается переменной DAN_WINDOWS=1")
class ТестыHTTPвWindows(unittest.TestCase):
    maxDiff = None

    def test_запросы_winhttp(self):
        with Сервер() as адрес:
            # время ожидания ответа Wine не соблюдает (в Windows оно работает), поэтому эта проверка исключена
            программа = ПРОГРАММА.replace("АДРЕС", адрес).replace(
                'если ошибка е = http.запрос("GET", адрес + "/долго", {}, байты(""), 1) { вывод(е) }\n', "")
            код, вывод, ош = test_windows.выполнить_в_windows(программа)
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.rstrip("\n").split("\n"),
                         [с.replace("АДРЕС", адрес) for с in ОЖИДАЕТСЯ if "/долго" not in с])


if __name__ == "__main__":
    unittest.main()
