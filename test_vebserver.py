"""Тесты модуля вебсервер (библиотека/вебсервер.dan) и функций TCP, на которых он построен.

Программа на Дане слушает свободный порт на 127.0.0.1 и печатает его номер,
тест обращается к ней клиентом из стандартной библиотеки Python: так
проверяется и разбор запроса сервером, и то, что ответ понимает чужой клиент.
"""

import concurrent.futures
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

import dan
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ, выполнить

СЕРВЕР = """
подключить "вебсервер"
подключить "json"

функ ответить(з: вебсервер.Запрос) -> вебсервер.Ответ {
    если з.путь == "/" { вернуть вебсервер.текст("Привет, " + (з.параметр("имя") иначе "мир")) }
    если пусть п = з.шаблон("/дела/{номер}/файлы/{имя}") {
        вернуть вебсервер.текст(п["номер"] + "|" + п["имя"] + "|" + з.метод)
    }
    если з.путь == "/эхо" {
        вернуть вебсервер.ответ(201, "application/octet-stream", з.тело).с_заголовком("X-Length", стр(з.тело.длина))
    }
    если з.путь == "/форма" { вернуть вебсервер.текст(з.форма()["фио"]) }
    если з.путь == "/json" {
        если пусть вх = з.как_json() {
            вернуть вебсервер.ответ_json(json.Объект({"имя": вх.поле("имя"), "метод": json.Строка(з.метод)}))
        } иначе ошибка о {
            вернуть вебсервер.ошибка_http(400, о)
        }
    }
    если з.путь == "/адрес" { вернуть вебсервер.текст(з.адрес + " " + (з.заголовок("X-Delo") иначе "-")) }
    если з.путь == "/туда" { вернуть вебсервер.перенаправить("/") }
    если з.путь == "/разрыв" { вернуть вебсервер.текст("х").с_заголовком("X-A", "1\\r\\nSet-Cookie: взлом=1") }
    если з.путь == "/медленно" {
        пауза(0.3)
        вернуть вебсервер.текст("ок")
    }
    вернуть вебсервер.файл(з, ПАПКА, "/статика/")
}

перем с = вебсервер.слушать(АДРЕС, 0)!
с.работников = РАБОТНИКОВ
с.предел_запросов = ПРЕДЕЛ
с.секунд = 1
с.наибольшее_тело = 300000
с.наибольшие_заголовки = 4096
вывод(с.порт)
с.обслуживать(ответить)
вывод("готово")
"""


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыВебсервера(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.папка = self.tmp.name
        os.makedirs(os.path.join(self.папка, "сайт", "внутри"))
        with open(os.path.join(self.папка, "сайт", "стиль.css"), "w", encoding="utf-8") as ф:
            ф.write("body { color: red }")
        with open(os.path.join(self.папка, "сайт", "внутри", "index.html"), "w", encoding="utf-8") as ф:
            ф.write("<p>Дан</p>")
        with open(os.path.join(self.папка, "секрет.txt"), "w", encoding="utf-8") as ф:
            ф.write("секрет")

    def tearDown(self):
        self.tmp.cleanup()

    def запустить(self, предел, работников=4, адрес="127.0.0.1"):
        текст = (СЕРВЕР.replace("ПАПКА", json.dumps(os.path.join(self.папка, "сайт").replace("\\", "/"), ensure_ascii=False))
                 .replace("АДРЕС", json.dumps(адрес)).replace("РАБОТНИКОВ", str(работников))
                 .replace("ПРЕДЕЛ", str(предел)))
        путь = os.path.join(self.папка, "сервер.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        self.процесс = subprocess.Popen([исполняемый], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.остановить)
        self.порт = int(self.процесс.stdout.readline())
        self.адрес = f"http://127.0.0.1:{self.порт}"

    def остановить(self):
        if self.процесс.poll() is None:
            self.процесс.kill()
        self.процесс.wait(30)
        self.процесс.stdout.close()
        self.процесс.stderr.close()

    def завершение(self):
        код = self.процесс.wait(60)      # сервер принял ровно предел_запросов соединений и завершился
        вывод = self.процесс.stdout.read().decode("utf-8")
        self.assertEqual(код, 0, self.процесс.stderr.read().decode("utf-8"))
        self.assertEqual(вывод.splitlines(), ["готово"])

    def запрос(self, путь, данные=None, заголовки=None, метод=None):
        з = urllib.request.Request(self.адрес + urllib.parse.quote(путь, safe="/?=&%+"), data=данные,
                                   headers=заголовки or {}, method=метод)
        try:
            with urllib.request.urlopen(з, timeout=30) as о:
                return о.status, о.headers, о.read()
        except urllib.error.HTTPError as е:
            return е.code, е.headers, е.read()

    def сырой(self, данные, читать=True):
        with socket.create_connection(("127.0.0.1", self.порт), timeout=30) as с:
            с.sendall(данные)
            ответ = b""
            while читать:
                часть = с.recv(65536)
                if not часть:
                    break
                ответ += часть
        return ответ

    def test_запросы_и_ответы(self):
        self.запустить(19)
        код, з, т = self.запрос("/?имя=" + urllib.parse.quote("Иван Иванов"))
        self.assertEqual((код, т.decode("utf-8")), (200, "Привет, Иван Иванов"))
        self.assertEqual(з["Content-Type"], "text/plain; charset=utf-8")
        self.assertEqual(з["Connection"], "close")
        self.assertEqual(self.запрос("/?имя=%D0%94%D0%B0%D0%BD+%2B+1")[2].decode("utf-8"), "Привет, Дан + 1")
        self.assertEqual(self.запрос("/дела/А40-1/файлы/иск.pdf", данные=b"", метод="DELETE")[2].decode("utf-8"),
                         "А40-1|иск.pdf|DELETE")
        self.assertEqual(self.запрос("/дела//файлы/иск.pdf")[0], 404)
        данные = bytes(range(256)) * 1000
        код, з, т = self.запрос("/эхо", данные=данные)
        self.assertEqual((код, з["X-Length"], т == данные), (201, "256000", True))
        форма = urllib.parse.urlencode({"фио": "Петров П.П. & сын"}).encode()
        self.assertEqual(self.запрос("/форма", данные=форма)[2].decode("utf-8"), "Петров П.П. & сын")
        код, з, т = self.запрос("/json", данные=json.dumps({"имя": "Дан"}).encode(), метод="PUT")
        self.assertEqual((код, json.loads(т)), (200, {"имя": "Дан", "метод": "PUT"}))
        self.assertEqual(з["Content-Type"], "application/json; charset=utf-8")
        self.assertEqual(self.запрос("/json", данные=b"{")[0], 400)
        self.assertEqual(self.запрос("/адрес", заголовки={"x-delo": "7"})[2], b"127.0.0.1 7")
        self.assertEqual(self.запрос("/туда")[2].decode("utf-8"), "Привет, мир")      # 303 и переход
        код, з, т = self.запрос("/разрыв")
        self.assertEqual((код, з["X-A"], з["Set-Cookie"]), (200, "1  Set-Cookie: взлом=1".encode().decode("latin-1"), None))
        # файлы
        код, з, т = self.запрос("/статика/стиль.css")
        self.assertEqual((код, з["Content-Type"], т), (200, "text/css; charset=utf-8", b"body { color: red }"))
        self.assertEqual(self.запрос("/статика/внутри/")[2].decode("utf-8"), "<p>Дан</p>")
        self.assertEqual(self.запрос("/статика/внутри")[0], 404)
        self.assertEqual(self.запрос("/статика/%2E%2E/секрет.txt")[0], 404)
        self.assertEqual(self.запрос("/статика/..%5Cсекрет.txt")[0], 404)
        self.assertIn(b"HTTP/1.1 404 Not Found", self.сырой("GET /статика/../секрет.txt HTTP/1.1\r\n\r\n".encode()))
        # HEAD: заголовки без тела
        ответ = self.сырой(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertTrue(ответ.startswith(b"HTTP/1.1 200 OK\r\n"), ответ)
        self.assertIn(b"Content-Length: 20\r\n", ответ)
        self.assertTrue(ответ.endswith(b"\r\n\r\n"), ответ)
        self.завершение()

    def test_неверные_запросы(self):
        self.запустить(7)
        self.assertTrue(self.сырой(b"BAD\r\n\r\n").startswith(b"HTTP/1.1 400 Bad Request\r\n"))
        self.assertTrue(self.сырой(b"GET / HTTP/1.1\r\nno colon\r\n\r\n").startswith(b"HTTP/1.1 400 "))
        self.assertTrue(self.сырой(b"POST / HTTP/1.1\r\nContent-Length: 300001\r\n\r\n").startswith(b"HTTP/1.1 413 "))
        self.assertTrue(self.сырой(b"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n").startswith(b"HTTP/1.1 400 "))
        self.assertTrue(self.сырой(b"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").startswith(b"HTTP/1.1 411 "))
        self.assertTrue(self.сырой(b"GET / HTTP/1.1\r\nX: " + b"a" * 5000 + b"\r\n\r\n").startswith(b"HTTP/1.1 431 "))
        # клиент прислал не всё и замолчал: через секунду - 408
        начало = time.monotonic()
        self.assertTrue(self.сырой(b"POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n123").startswith(b"HTTP/1.1 408 "))
        self.assertLess(time.monotonic() - начало, 20)
        self.завершение()

    def test_несколько_работников_одновременно(self):
        # 8 медленных запросов по 0,3 с на 8 работников - заметно быстрее, чем по очереди (2,4 с)
        self.запустить(8 + 200, работников=8)
        начало = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(8) as п:
            ответы = list(п.map(lambda _: self.запрос("/медленно")[2], range(8)))
        прошло = time.monotonic() - начало
        self.assertEqual(ответы, [b"\xd0\xbe\xd0\xba"] * 8)
        self.assertLess(прошло, 2.0)
        with concurrent.futures.ThreadPoolExecutor(16) as п:
            ответы = list(п.map(lambda н: self.запрос(f"/?имя={н}")[2], range(200)))
        self.assertEqual(ответы, [f"Привет, {н}".encode("utf-8") for н in range(200)])
        self.завершение()

    def test_все_адреса(self):
        self.запустить(1, работников=1, адрес="0.0.0.0")
        self.assertEqual(self.запрос("/адрес")[2], b"127.0.0.1 -")
        self.завершение()


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыФункцийСоединений(unittest.TestCase):
    """слушать_порт с адресом, ждать_соединение_не_дольше, адрес_соединения; ошибки адреса и порта."""

    def test_ошибки(self):
        код, вывод, ошибки = выполнить(
            'подключить "вебсервер"\n'
            'для а в ["1.2.3", "256.0.0.1", "01.0.0.1", "a.b.c.d", "1.2.3.4.5", ""] {\n'
            '    если ошибка о = вебсервер.слушать(а, 0) { вывод(о) }\n'
            '}\n'
            'если ошибка о = вебсервер.слушать("127.0.0.1", 70000) { вывод(о) }\n'
            'пусть с = слушать_порт(0, "127.0.0.1")\n'
            'утвердить(порт_слушателя(с) > 0)\n'
            'утвердить(слушать_порт(порт_слушателя(с), "127.0.0.1") == -1)\n'
            'утвердить(не ждать_соединение_не_дольше(12345, 1))\n'
            'вывод(адрес_соединения(12345))\n'
            'закрыть_соединение(с)\n')
        self.assertEqual(код, 0, ошибки)
        строки = вывод.splitlines()
        self.assertEqual(len(строки), 8, вывод)
        self.assertEqual(строки[0], 'адрес "1.2.3": нужен IPv4 из четырёх чисел от 0 до 255, например "127.0.0.1"')
        self.assertTrue(all(с.startswith("адрес ") for с in строки[:6]), вывод)
        self.assertEqual(строки[6], "порт 70000: нужно число от 0 до 65535")
        self.assertEqual(строки[7], "")
        код, вывод, ошибки = выполнить('пусть с = слушать_порт(0, "localhost")\n')
        self.assertNotEqual(код, 0)
        self.assertIn("IPv4 из четырёх чисел", ошибки)
        код, вывод, ошибки = выполнить('ждать_соединение_не_дольше(1, -1)\n')
        self.assertNotEqual(код, 0)
        self.assertIn("от 0 до 86400 секунд", ошибки)


if __name__ == "__main__":
    unittest.main()
