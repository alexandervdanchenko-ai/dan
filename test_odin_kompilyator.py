"""Один компилятор: команды, которые раньше выполнял только компилятор на Python (dan.py), выполняет
компилятор на Дане (сам/дан.dan): «собрать --для=», «собрать --веб», «--сжать», «обновить»,
«песочница», «сервер»; новые встроенные функции, на которых они построены.

Поведение сверяется с компилятором на Python: те же команды на тех же входах дают тот же вывод.
"""

import hashlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import zipfile
from contextlib import redirect_stdout

import dan
import sam_kompilyator
import versiya

ПАПКА = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"


def компилятор():
    return sam_kompilyator.исполняемый()


def среда(**ещё):
    с = dict(os.environ, DAN_HOME=ПАПКА)
    с.update(ещё)
    return с


def выполнить(*аргументы, cwd=None, ввод=None, env=None, программа=None):
    р = subprocess.run([программа or компилятор(), *аргументы], capture_output=True, cwd=cwd, input=ввод,
                       env=env or среда(), timeout=900)
    return (р.returncode, р.stdout.decode("utf-8", "replace").replace("\r\n", "\n"),
            р.stderr.decode("utf-8", "replace").replace("\r\n", "\n"))


def python_dan(*аргументы, cwd=None):
    """Вывод и код той же команды у компилятора на Python (dan.main, без DAN_KOMPILYATOR)."""
    было = os.getcwd()
    вывод = io.StringIO()
    прежний = os.environ.pop("DAN_KOMPILYATOR", None)
    try:
        if cwd:
            os.chdir(cwd)
        with redirect_stdout(вывод), unittest.mock.patch.object(sam_kompilyator, "включён", return_value=False):
            код = dan.main(list(аргументы))
    finally:
        os.chdir(было)
        if прежний is not None:
            os.environ["DAN_KOMPILYATOR"] = прежний
    return код, вывод.getvalue()


import unittest.mock  # noqa: E402

ЕСТЬ_ZIG = dan.найти_zig() is not None
ФАКТОРИАЛ = 'функ ф(н: цел) -> цел {\n    если н <= 1 { вернуть 1 }\n    вернуть н * ф(н - 1)\n}\nвывод(ф(10))\n'


class ТестыВстроенныхФункций(unittest.TestCase):
    """ввод_байтов, вывести_байты, путь_программы и соединения TCP на 127.0.0.1."""

    def test_байты_ввода_и_вывода_как_есть(self):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "эхо.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write('перем всего = 0\nпока истина {\n    пусть б = ввод_байтов(3)\n    если б.длина == 0 { прервать }\n'
                        '    всего += б.длина\n    вывести_байты(б)\n}\nвывести_байты(байты("|" + стр(всего) + "\\n"))\n'
                        'вывод_ошибки(путь_программы())\n')
            код, вывод_, ошибки = выполнить("собрать", путь)
            self.assertEqual(код, 0, вывод_ + ошибки)
            exe = os.path.join(папка, "эхо" + (".exe" if WINDOWS else ""))
            р = subprocess.run([exe], input=b"a\r\nb\nccc", capture_output=True, timeout=60)
            self.assertEqual(р.stdout, b"a\r\nb\nccc|8\n")
            self.assertEqual(os.path.realpath(р.stderr.decode("utf-8").strip()), os.path.realpath(exe))

    def test_соединение(self):
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "сервер.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write('пусть с = слушать_порт(0)\nвывод(порт_слушателя(с))\nпусть к = принять_соединение(с)\n'
                        'пусть б = прочитать_соединение(к, 100)\n'
                        'утвердить(записать_соединение(к, байты("ответ: ") + б))\nзакрыть_соединение(к)\n'
                        'закрыть_соединение(с)\nутвердить(порт_слушателя(с) == -1)\n'
                        'утвердить(не записать_соединение(к, байты("x")))\n')
            код, вывод_, ошибки = выполнить("собрать", путь)
            self.assertEqual(код, 0, вывод_ + ошибки)
            exe = os.path.join(папка, "сервер" + (".exe" if WINDOWS else ""))
            процесс = subprocess.Popen([exe], stdout=subprocess.PIPE)
            try:
                порт = int(процесс.stdout.readline())
                import socket
                with socket.create_connection(("127.0.0.1", порт), timeout=30) as с:
                    с.sendall("привет".encode("utf-8"))
                    данные = b""
                    while True:
                        часть = с.recv(100)
                        if not часть:
                            break
                        данные += часть
                self.assertEqual(данные.decode("utf-8"), "ответ: привет")
                self.assertEqual(процесс.wait(60), 0)
            finally:
                процесс.kill()
                процесс.stdout.close()


class ТестыСборкиДляДругих(unittest.TestCase):
    """«собрать --для=», «собрать --веб», «--сжать»: тот же вывод, что у компилятора на Python."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.папка = self.tmp.name
        with open(os.path.join(self.папка, "ф.dan"), "w", encoding="utf-8") as ф:
            ф.write(ФАКТОРИАЛ)

    def tearDown(self):
        self.tmp.cleanup()

    def сверить(self, *аргументы):
        код, вывод_, _ = выполнить(*аргументы, cwd=self.папка)
        код_п, вывод_п = python_dan(*аргументы, cwd=self.папка)
        self.assertEqual((код, вывод_), (код_п, вывод_п))
        return вывод_

    def test_неизвестная_система(self):
        вывод_ = self.сверить("собрать", "--для=амига", "ф.dan")
        self.assertIn("неизвестная система «амига»", вывод_)

    def test_ошибка_в_программе(self):
        with open(os.path.join(self.папка, "о.dan"), "w", encoding="utf-8") as ф:
            ф.write("вывод(нет_такой)\n")
        for параметр in ("--веб", "--для=linux"):
            if not ЕСТЬ_ZIG and параметр != "--веб":
                continue
            код, вывод_, _ = выполнить("собрать", параметр, "о.dan", cwd=self.папка)
            self.assertEqual(код, 1)
            self.assertIn("нет_такой", вывод_)

    @unittest.skipUnless(ЕСТЬ_ZIG, "нужен zig (pip install ziglang)")
    def test_для_других_систем(self):
        for система, окончание in (("linux", "-linux"), ("windows", "-windows.exe")):
            вывод_ = self.сверить("собрать", "--для=" + система, "ф.dan")
            self.assertIn("для системы " + система, вывод_)
            self.assertTrue(os.path.isfile(os.path.join(self.папка, "ф" + окончание)))

    @unittest.skipUnless(ЕСТЬ_ZIG, "нужен zig (pip install ziglang)")
    def test_веб_та_же_страница(self):
        код, вывод_, ошибки = выполнить("собрать", "--веб", "ф.dan", cwd=self.папка)
        self.assertEqual(код, 0, вывод_ + ошибки)
        with open(os.path.join(self.папка, "ф.html"), encoding="utf-8") as ф:
            страница = ф.read()
        self.сверить("собрать", "--веб", "ф.dan")
        with open(os.path.join(self.папка, "ф.html"), encoding="utf-8") as ф:
            self.assertEqual(страница, ф.read())
        self.assertIn("<title>ф</title>", страница)

    @unittest.skipUnless(ЕСТЬ_ZIG, "нужен zig (pip install ziglang)")
    def test_веб_без_задач(self):
        with open(os.path.join(self.папка, "з.dan"), "w", encoding="utf-8") as ф:
            ф.write("функ ф() -> цел { вернуть 1 }\nпусть з = запустить ф()\nвывод(з.дождаться())\n")
        вывод_ = self.сверить("собрать", "--веб", "з.dan")
        self.assertIn("параллельные задачи", вывод_)

    def test_сжать(self):
        вывод_ = self.сверить("собрать", "--сжать", "ф.dan")
        if shutil.which("upx"):
            self.assertIn("Сжато упаковщиком UPX", вывод_)
        else:
            self.assertIn("нужен упаковщик UPX", вывод_)


class ТестыПесочницы(unittest.TestCase):
    """«песочница»: сервер компилятора на Дане отвечает так же, как pesochnica.py."""

    @classmethod
    def setUpClass(cls):
        import re
        cls.процесс = subprocess.Popen([компилятор(), "песочница", "--порт=0", "--без-браузера"],
                                       stdout=subprocess.PIPE, env=среда())
        строка = cls.процесс.stdout.readline().decode("utf-8")
        cls.адрес = re.search(r"http://\S+", строка).group(0).rstrip("/")
        with urllib.request.urlopen(cls.адрес + "/", timeout=60) as о:
            cls.страница = о.read().decode("utf-8")
        cls.ключ = re.search(r'const КЛЮЧ = "([^"]+)"', cls.страница).group(1)

    @classmethod
    def tearDownClass(cls):
        cls.процесс.kill()
        cls.процесс.wait()
        cls.процесс.stdout.close()

    def запрос(self, путь, данные=None, ключ=None, заголовки=None):
        тело = None if данные is None else json.dumps(данные).encode("utf-8")
        з = urllib.request.Request(self.адрес + urllib.request.quote(путь), data=тело,
                                   headers={"Content-Type": "application/json",
                                            "X-Dan-Key": self.ключ if ключ is None else ключ, **(заголовки or {})})
        try:
            with urllib.request.urlopen(з, timeout=120) as о:
                return о.status, о.read().decode("utf-8")
        except urllib.error.HTTPError as о:
            return о.code, о.read().decode("utf-8")

    def test_та_же_страница(self):
        import pesochnica
        ожидается = (pesochnica.СТРАНИЦА.replace("__КЛЮЧ__", self.ключ)
                     .replace("__НАЧАЛО__", json.dumps(pesochnica.НАЧАЛО, ensure_ascii=False).replace("</", "<\\/")))
        self.assertEqual(self.страница, ожидается)
        self.assertEqual(len(self.ключ), 32)

    def test_чужой_запрос_отклоняется(self):
        код, _ = self.запрос("/выполнить", {"текст": "вывод(1)"}, ключ="foreign-key")
        self.assertEqual(код, 403)
        код, _ = self.запрос("/", заголовки={"Host": "evil.example:80"})
        self.assertEqual(код, 403)
        код, _ = self.запрос("/нет")
        self.assertEqual(код, 404)

    def test_выполнение_как_у_python(self):
        import pesochnica
        ошибки = (dan.ОшибкаЛексера, dan.ОшибкаРазбора, dan.ОшибкаМодуля, dan.ОшибкаПроверки, dan.ОшибкаСборки)
        for текст, ввод in (('пусть имя = ввод()\nвывод("Привет, " + имя)\nвывод_ошибки("ош")\n', "мир\n"),
                           ("вывод(нет_такой)\n", ""), ("пусть = 1\n", ""), ("завершить(3)\n", "")):
            код, ответ = self.запрос("/выполнить", {"текст": текст, "ввод": ввод})
            self.assertEqual(код, 200)
            ответ = json.loads(ответ)
            with unittest.mock.patch.object(sam_kompilyator, "включён", return_value=False):
                эталон = pesochnica.выполнить(текст, ввод, dan.собрать, ошибки)
            for о in (ответ, эталон):
                if "время" not in о:
                    о["состояние"] = о["состояние"].split(":")[0].split(" (")[0]
            self.assertEqual(ответ, эталон)

    def test_неверный_вывод_и_остановка(self):
        код, ответ = self.запрос("/выполнить", {"текст": "вывести_байты(байты([65, 255, 66]))\nпока истина { }\n",
                                               "ввод": ""})
        ответ = json.loads(ответ)
        self.assertEqual(ответ["вывод"], "A\ufffdB")
        self.assertIn("работала дольше 10 секунд", ответ["состояние"])

    def test_формат_и_примеры(self):
        код, ответ = self.запрос("/формат", {"текст": "пусть  а=1"})
        self.assertEqual(json.loads(ответ), {"текст": "пусть а = 1\n"})
        код, ответ = self.запрос("/формат", {"текст": "вывод(\"абв"})
        self.assertIn("текст не приведён к единому виду", json.loads(ответ)["ошибка"])
        import pesochnica
        код, ответ = self.запрос("/примеры", {})
        self.assertEqual(json.loads(ответ)["примеры"], pesochnica.список_примеров())
        код, ответ = self.запрос("/пример", {"имя": "факториал"})
        with open(os.path.join(ПАПКА, "примеры", "факториал.dan"), encoding="utf-8") as ф:
            self.assertEqual(json.loads(ответ)["текст"], ф.read())
        код, ответ = self.запрос("/пример", {"имя": "../dan"})
        self.assertEqual(json.loads(ответ), {"ошибка": "нет такого примера"})

    def test_справка_и_порт(self):
        import pesochnica
        код, вывод_, _ = выполнить("песочница", "--лишнее")
        self.assertEqual((код, вывод_.strip()), (2, pesochnica.__doc__.strip()))
        код, вывод_, _ = выполнить("песочница", "--порт=восемь")
        self.assertEqual((код, вывод_.strip()), (2, "Порт - число, например --порт=8080"))


import test_server  # noqa: E402


class ТестыСервераНаДане(test_server.ТестыСервера, unittest.TestCase):
    """Те же тесты, что у сервера на Python (test_server.py), с сервером «дан сервер»."""

    def setUp(self):
        self.команда = [компилятор(), "сервер"]
        self.среда = среда()
        super().setUp()

    def test_версия_сервера(self):
        клиент = test_server.Клиент(self.команда, self.среда)
        try:
            ответ = клиент.запрос("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
            self.assertEqual(ответ["serverInfo"], {"name": "Дан", "version": versiya.ВЕРСИЯ})
        finally:
            клиент.закрыть()




class ТестыСсылокСервераНаДане(unittest.TestCase):
    """textDocument/references, prepareRename и rename сервера на Дане: места имени с учётом местных имён функций."""

    ТЕКСТ = ("функ итог(дела: [цел]) -> цел {\n"          # 0
             "    перем сумма = 0\n"                      # 1
             "    для д в дела { сумма += д }\n"          # 2
             "    вернуть сумма\n"                        # 3
             "}\n"                                        # 4
             "функ другая(сумма: цел) -> цел {\n"         # 5
             "    вернуть сумма * 2\n"                    # 6
             "}\n"                                        # 7
             "пусть сумма = итог([1, 2])\n"               # 8
             "вывод(\"сумма\" + стр(сумма) + стр(другая(сумма)))  // сумма\n")   # 9

    def setUp(self):
        self.папка = tempfile.TemporaryDirectory()
        self.uri = test_server.uri_из_пути(os.path.join(self.папка.name, "главная.dan"))
        self.клиент = test_server.Клиент([компилятор(), "сервер"], среда())
        ответ = self.клиент.запрос("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
        self.assertTrue(ответ["capabilities"]["referencesProvider"])
        self.assertEqual(ответ["capabilities"]["renameProvider"], {"prepareProvider": True})
        self.клиент.уведомление("textDocument/didOpen", {"textDocument": {
            "uri": self.uri, "languageId": "dan", "version": 1, "text": self.ТЕКСТ}})
        self.клиент.ждать_ошибок(self.uri)

    def tearDown(self):
        self.клиент.закрыть()
        self.папка.cleanup()

    def места(self, строка, символ):
        ответ = self.клиент.запрос("textDocument/references", {"textDocument": {"uri": self.uri},
                                   "position": {"line": строка, "character": символ}, "context": {"includeDeclaration": True}})
        return sorted((м["range"]["start"]["line"], м["range"]["start"]["character"], м["range"]["end"]["character"])
                      for м in ответ)

    def test_ссылки(self):
        # местная «сумма» функции итог - только в ней
        self.assertEqual(self.места(3, 13), [(1, 10, 15), (2, 19, 24), (3, 12, 17)])
        # параметр «сумма» функции другая
        self.assertEqual(self.места(6, 12), [(5, 12, 17), (6, 12, 17)])
        # «сумма» верхнего уровня: не в функциях, где своя «сумма», не в строке и не в комментарии
        self.assertEqual(self.места(8, 7), [(8, 6, 11), (9, 20, 25), (9, 40, 45)])
        # функция: объявление и вызовы
        self.assertEqual(self.места(8, 16), [(0, 5, 9), (8, 14, 18)])
        self.assertEqual(self.места(4, 0), [])          # «}» - не имя

    def test_переименование(self):
        диапазон = self.клиент.запрос("textDocument/prepareRename", {"textDocument": {"uri": self.uri},
                                      "position": {"line": 1, "character": 12}})
        self.assertEqual(диапазон, {"start": {"line": 1, "character": 10}, "end": {"line": 1, "character": 15}})
        правка = self.клиент.запрос("textDocument/rename", {"textDocument": {"uri": self.uri},
                                    "position": {"line": 1, "character": 12}, "newName": "всего"})
        правки = правка["changes"][self.uri]
        self.assertEqual(sorted(п["range"]["start"]["line"] for п in правки), [1, 2, 3])
        self.assertTrue(all(п["newText"] == "всего" for п in правки))
        self.клиент.послать({"jsonrpc": "2.0", "id": 99, "method": "textDocument/rename", "params": {
            "textDocument": {"uri": self.uri}, "position": {"line": 1, "character": 12}, "newName": "если"}})
        ответ = self.клиент.получить()
        while ответ.get("id") != 99:
            ответ = self.клиент.получить()
        self.assertEqual(ответ["error"]["code"], -32602)
        self.assertIn("не может быть именем", ответ["error"]["message"])


class ТестыСверкиСервера(unittest.TestCase):
    """Сервер на Дане отвечает так же, как сервер на Python, и знает возможности версий 1.5 и новее."""

    def setUp(self):
        self.папка = tempfile.TemporaryDirectory()
        with open(os.path.join(self.папка.name, "расчёты.dan"), "w", encoding="utf-8") as ф:
            ф.write(test_server.РАСЧЁТЫ)
        self.путь = os.path.join(self.папка.name, "главная.dan")
        self.uri = test_server.uri_из_пути(self.путь)
        # эталон - сервер на Python (без DAN_KOMPILYATOR, иначе «python dan.py сервер» - тоже сервер на Дане)
        на_python = {к: з for к, з in os.environ.items() if к != "DAN_KOMPILYATOR"}
        на_python["DAN_SERVER_PYTHON"] = "1"
        self.клиенты = [test_server.Клиент(None, на_python), test_server.Клиент([компилятор(), "сервер"], среда())]
        for к in self.клиенты:
            к.запрос("initialize", {"processId": None, "rootUri": None, "capabilities": {}})

    def tearDown(self):
        for к in self.клиенты:
            к.закрыть()
        self.папка.cleanup()

    def открыть(self, клиент, текст, версия=1):
        метод = "textDocument/didOpen" if версия == 1 else "textDocument/didChange"
        клиент.уведомление(метод, {"textDocument": {"uri": self.uri, "languageId": "dan", "version": версия,
                                                    "text": текст}, "contentChanges": [{"text": текст}]})
        return клиент.ждать_ошибок(self.uri)

    def test_новые_возможности_без_ложных_ошибок(self):
        # программы поставки с плотными массивами, векторными ядрами, «параллельно для» и модулем «модели»:
        # сервер на Python их не понимает, сервер на Дане не находит в них ошибок
        на_python, на_дане = self.клиенты
        for версия, имя in enumerate(("массивы_и_ядра.dan", "параллельно.dan", "модель.dan"), 1):
            with open(os.path.join(ПАПКА, "поставка", "программы", имя), encoding="utf-8") as ф:
                текст = ф.read()
            with self.subTest(имя):
                self.assertEqual(self.открыть(на_дане, текст, версия), [])
                self.assertNotEqual(self.открыть(на_python, текст, версия), [])
        ошибки = self.открыть(на_дане, текст + "вывод(нет_такой)\n", 9)
        self.assertEqual(len(ошибки), 1)
        self.assertEqual(ошибки[0]["range"]["start"]["line"], текст.count("\n"))
        self.assertIn("нет_такой", ошибки[0]["message"])

    def test_те_же_ответы(self):
        тексты = [test_server.ПРОГРАММА, test_server.ПРОГРАММА.replace("вернуть всего", "вернуть \"всего\""),
                  "пусть а = (1\n", "пусть сумма = 1\nвывод(сумм)\n", "пусть а = 1\nвывод(\"😀\" + нет)\n",
                  'подключить "расчёты.dan" как р\nвывод(р.скрытая())\n', 'подключить "нет_файла"\n',
                  "функ ф() -> цел {\n    пусть х = 1\n    вернуть 2\n}\nвывод(ф())\n"]
        for версия, текст in enumerate(тексты, 1):
            ответы = []
            for к in self.клиенты:
                ошибки = self.открыть(к, текст, версия)
                строки = текст.split("\n")
                запросы = []
                for н, линия in enumerate(строки):
                    for символ in range(0, len(линия) + 1, 3):
                        место = {"textDocument": {"uri": self.uri}, "position": {"line": н, "character": символ}}
                        запросы.append(к.запрос("textDocument/hover", место))
                        запросы.append(к.запрос("textDocument/definition", место))
                    место = {"textDocument": {"uri": self.uri}, "position": {"line": н, "character": len(линия)}}
                    подсказки = к.запрос("textDocument/completion", место)["items"]
                    запросы.append(sorted((п["label"], п["kind"], п.get("detail"), п.get("documentation"))
                                          for п in подсказки))
                запросы.append(к.запрос("textDocument/documentSymbol", {"textDocument": {"uri": self.uri}}))
                запросы.append(к.запрос("textDocument/formatting", {"textDocument": {"uri": self.uri}}))
                ответы.append((ошибки, запросы))
            with self.subTest(текст=текст):
                self.assertEqual(ответы[1][0], ответы[0][0])
                for а, б in zip(ответы[1][1], ответы[0][1]):
                    self.assertEqual(а, б)

    def test_неизвестный_метод(self):
        for к in self.клиенты:
            with self.assertRaises(AssertionError) as о:
                к.запрос("textDocument/codeLens", {})
            self.assertIn("метод textDocument/codeLens не поддерживается", str(о.exception))


def адрес(путь):
    return pathlib.Path(путь).as_uri()


class ТестыОбновления(unittest.TestCase):
    """«обновить»: те же выпуски, что в test_obnovlenie.py, через curl и адреса file://."""

    @classmethod
    def setUpClass(cls):
        if not shutil.which("curl"):
            raise unittest.SkipTest("нужна программа curl")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name
        # «установленный» Дан: копия компилятора в папке установки внутри временной папки
        self.установка = os.path.join(self.п, "программы", "dan")
        os.makedirs(self.установка)
        self.exe = os.path.join(self.установка, "dan" + (".exe" if WINDOWS else ""))
        shutil.copy2(компилятор(), self.exe)
        self.env = среда(DAN_CONFIG=os.path.join(self.п, "настройки"))
        for имя in ("DAN_RELEASES_URL", "DAN_UPDATE_MODE", "DAN_UPDATE_DRY_RUN"):
            self.env.pop(имя, None)

    def tearDown(self):
        self.tmp.cleanup()

    def выпуск(self, версия, пробный=False, испортить=False):
        # латиницей: curl в Windows не раскодирует русские буквы в адресе file://
        папка = os.path.join(self.п, "vypusk_" + версия)
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
                ф.write(f"{'0' * 64 if испортить else сумма}  {os.path.basename(путь)}\n")
        файлы = [{"name": os.path.basename(п), "size": os.path.getsize(п), "browser_download_url": адрес(п)}
                 for п in (архив, установщик, суммы)]
        return {"tag_name": "v" + версия, "prerelease": пробный, "html_url": "https://example.org/" + версия,
                "assets": файлы}

    def опубликовать(self, *выпуски):
        путь = os.path.join(self.п, "releases.json")
        with open(путь, "w", encoding="utf-8") as ф:
            json.dump(list(выпуски), ф, ensure_ascii=False)
        self.env["DAN_RELEASES_URL"] = адрес(путь)

    def выполнить(self, *аргументы):
        код, вывод_, ошибки = выполнить("обновить", *аргументы, env=self.env, программа=self.exe)
        return код, вывод_ + ошибки

    def test_номера_и_каналы(self):
        список = [{"tag_name": "v1.3"}, {"tag_name": "v1.4"}, {"tag_name": "v1.5b1", "prerelease": True},
                  {"tag_name": "v1.6", "draft": True}, {"tag_name": "v1.5rc2"}, {"tag_name": "ночная"},
                  {"tag_name": "v1.10"}]
        self.опубликовать(*список[:6])
        self.env["DAN_UPDATE_MODE"] = "исходники"
        код, вывод_ = self.выполнить("--проверить", "--канал=пробный")
        self.assertEqual(код, 0, вывод_)
        if dan_версия_меньше("1.5rc2"):
            self.assertIn("Есть новая версия Дана: 1.5rc2", вывод_)
        else:
            self.assertIn("Установлена последняя версия", вывод_)

    def test_последняя_версия(self):
        self.опубликовать(self.выпуск(versiya.ВЕРСИЯ))
        код, вывод_ = self.выполнить("--проверить")
        self.assertEqual(код, 0, вывод_)
        self.assertIn("Установлена последняя версия", вывод_)

    def test_канал_запоминается(self):
        self.опубликовать(self.выпуск(versiya.ВЕРСИЯ), self.выпуск("99.0b1", пробный=True))
        код, вывод_ = self.выполнить("--проверить")
        self.assertIn("Установлена последняя версия", вывод_)
        код, вывод_ = self.выполнить("--проверить", "--канал=пробный")
        self.assertIn("Есть новая версия Дана: 99.0b1", вывод_)
        with open(os.path.join(self.п, "настройки", "канал.txt"), encoding="utf-8") as ф:
            self.assertEqual(ф.read().strip(), "пробный")
        код, вывод_ = self.выполнить("--проверить")
        self.assertIn("99.0b1", вывод_)
        self.assertEqual(self.выполнить("--канал=ночной")[0], 2)

    def test_архив_распаковывается_рядом(self):
        self.env["DAN_UPDATE_MODE"] = "архив"
        self.опубликовать(self.выпуск("99.0"))
        код, вывод_ = self.выполнить()
        self.assertEqual(код, 0, вывод_)
        новая = os.path.join(self.п, "программы", "dan-99.0", "dan")
        with open(os.path.join(новая, "dan.exe"), encoding="utf-8") as ф:
            self.assertEqual(ф.read(), "новая версия 99.0")
        self.assertIn(новая, вывод_)

    def test_поставка_узнаётся_по_папке(self):
        # рядом с исполняемым файлом папка sreda: это переносной архив, а не исходные тексты
        os.makedirs(os.path.join(self.установка, "sreda"))
        self.опубликовать(self.выпуск("99.0"))
        код, вывод_ = self.выполнить()
        self.assertEqual(код, 0, вывод_)
        self.assertIn("распакован в папку", вывод_)

    def test_неверная_сумма_ничего_не_меняет(self):
        self.env["DAN_UPDATE_MODE"] = "архив"
        self.опубликовать(self.выпуск("99.0", испортить=True))
        код, вывод_ = self.выполнить()
        self.assertEqual(код, 1)
        self.assertIn("не совпадает с контрольной суммой", вывод_)
        self.assertEqual(os.listdir(os.path.join(self.п, "программы")), ["dan"])

    def test_установщик_скачивается(self):
        self.env["DAN_UPDATE_MODE"] = "установщик"
        self.env["DAN_UPDATE_DRY_RUN"] = "1"
        self.опубликовать(self.выпуск("99.0"))
        код, вывод_ = self.выполнить()
        self.assertEqual(код, 0, вывод_)
        self.assertIn("Скачивание dan-99.0-setup.exe (1 МБ)...", вывод_)
        self.assertIn("Установщик готов:", вывод_)
        self.assertTrue(вывод_.strip().endswith("dan-99.0-setup.exe"))

    def test_из_исходников_только_совет(self):
        self.опубликовать(self.выпуск("99.0"))
        код, вывод_ = self.выполнить()
        self.assertEqual(код, 0)
        self.assertIn("git pull", вывод_)

    def test_нет_связи(self):
        self.env["DAN_RELEASES_URL"] = адрес(os.path.join(self.п, "нет.json"))
        код, вывод_ = self.выполнить("--проверить")
        self.assertEqual(код, 1)
        self.assertIn("Ошибка: не удалось связаться", вывод_)

    def test_справка(self):
        код, вывод_ = self.выполнить("--лишнее")
        self.assertEqual(код, 2)
        import obnovlenie
        self.assertEqual(вывод_.strip(), obnovlenie.__doc__.strip())


def dan_версия_меньше(версия):
    import obnovlenie
    return obnovlenie.номер(versiya.ВЕРСИЯ) < obnovlenie.номер(версия)


if __name__ == "__main__":
    unittest.main()
