"""Тесты сервера MCP («дан mcp», сам/mcp.dan) и команды «дан новый» (сам/проект.dan).

Сервер запускается компилятором на Дане; сообщения JSON-RPC 2.0 идут построчно через стандартный ввод и вывод.
"""

import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest

import sam_kompilyator
import versiya

ПАПКА = os.path.dirname(os.path.abspath(__file__))
КОМПИЛЯТОР_ЕСТЬ = bool(os.environ.get("DAN_COMPILER") or shutil.which("gcc") or shutil.which("clang")
                       or shutil.which("cc"))
ИНСТРУМЕНТЫ = ["proverit", "ispravit", "zapustit", "testirovat", "formatirovat", "spravka"]


def среда():
    return dict(os.environ, DAN_HOME=ПАПКА)


class Клиент:
    """Простейший клиент MCP: запускает «дан mcp» и обменивается с ним строками JSON."""

    def __init__(self, cwd=None):
        self.процесс = subprocess.Popen([sam_kompilyator.исполняемый(), "mcp"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=среда(), cwd=cwd)
        self.номер = 0

    def послать_строку(self, строка):
        self.процесс.stdin.write(строка.encode("utf-8") + b"\n")
        self.процесс.stdin.flush()

    def получить(self):
        строка = self.процесс.stdout.readline()
        if not строка:
            raise AssertionError("сервер закрыл вывод")
        self.проверить_строку(строка)
        return json.loads(строка.decode("utf-8"))

    @staticmethod
    def проверить_строку(строка):
        if not строка.endswith(b"\n") or b"\n" in строка[:-1]:
            raise AssertionError("сообщение не на одной строке: " + repr(строка[:200]))

    def запрос(self, метод, параметры=None):
        self.номер += 1
        сообщение = {"jsonrpc": "2.0", "id": self.номер, "method": метод}
        if параметры is not None:
            сообщение["params"] = параметры
        self.послать_строку(json.dumps(сообщение, ensure_ascii=False))
        ответ = self.получить()
        if ответ.get("id") != self.номер:
            raise AssertionError(f"ответ не на тот запрос: {ответ}")
        return ответ

    def уведомление(self, метод, параметры=None):
        сообщение = {"jsonrpc": "2.0", "method": метод}
        if параметры is not None:
            сообщение["params"] = параметры
        self.послать_строку(json.dumps(сообщение, ensure_ascii=False))

    def вызвать(self, имя, параметры):
        ответ = self.запрос("tools/call", {"name": имя, "arguments": параметры})
        if "error" in ответ:
            raise AssertionError(ответ["error"])
        return ответ["result"]

    def закрыть(self):
        self.процесс.stdin.close()
        self.процесс.wait(timeout=30)
        self.процесс.stdout.close()


def текст(итог):
    return "".join(ч["text"] for ч in итог["content"] if ч["type"] == "text")


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыСервераMCP(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.папка = tempfile.mkdtemp(prefix="dan_mcp_")
        cls.клиент = Клиент(cwd=cls.папка)
        cls.начало = cls.клиент.запрос("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                                      "clientInfo": {"name": "тест", "version": "1"}})
        cls.клиент.уведомление("notifications/initialized")

    @classmethod
    def tearDownClass(cls):
        cls.клиент.закрыть()
        shutil.rmtree(cls.папка, ignore_errors=True)

    def test_инициализация(self):
        итог = self.начало["result"]
        self.assertEqual(итог["protocolVersion"], "2025-06-18")
        self.assertIn("tools", итог["capabilities"])
        self.assertEqual(итог["serverInfo"]["name"], "dan")
        self.assertEqual(итог["serverInfo"]["version"], versiya.ВЕРСИЯ)
        self.assertIn("spravka", итог["instructions"])

    def test_другая_версия_протокола(self):
        клиент = Клиент()
        try:
            ответ = клиент.запрос("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
            self.assertEqual(ответ["result"]["protocolVersion"], "2024-11-05")
            ответ = клиент.запрос("initialize", {"protocolVersion": "1999-01-01", "capabilities": {}})
            self.assertEqual(ответ["result"]["protocolVersion"], "2025-06-18")
        finally:
            клиент.закрыть()

    def test_ping_и_уведомления(self):
        # уведомление без номера не получает ответа: следующий ответ - на ping
        self.клиент.уведомление("notifications/cancelled", {"requestId": 99})
        self.assertEqual(self.клиент.запрос("ping")["result"], {})

    def test_список_инструментов(self):
        инструменты = self.клиент.запрос("tools/list")["result"]["tools"]
        self.assertEqual([и["name"] for и in инструменты], ИНСТРУМЕНТЫ)
        for и in инструменты:
            self.assertRegex(и["name"], r"^[a-z_]+$")
            self.assertTrue(и["description"])
            схема = и["inputSchema"]
            self.assertEqual(схема["type"], "object")
            for имя, свойство in схема["properties"].items():
                # имена параметров - латиницей (так требуют клиенты), описания - по-русски
                self.assertRegex(имя, r"^[a-z_]+$")
                self.assertIn(свойство["type"], ("string", "integer", "boolean", "array"))
                self.assertRegex(свойство["description"], "[а-яё]")
        по_имени = {и["name"]: set(и["inputSchema"]["properties"]) for и in инструменты}
        self.assertEqual(по_имени["proverit"], {"tekst", "put"})
        self.assertEqual(по_имени["zapustit"], {"tekst", "put", "vvod", "argumenty", "vremya", "pamyat_mb"})
        self.assertEqual(по_имени["testirovat"], {"tekst", "put", "vremya", "pamyat_mb"})
        self.assertEqual(по_имени["spravka"], {"imya", "vsyo"})

    def test_ошибки_протокола(self):
        ответ = self.клиент.запрос("нет/такого")
        self.assertEqual(ответ["error"]["code"], -32601)
        ответ = self.клиент.запрос("tools/call", {"name": "нет_такого", "arguments": {}})
        self.assertEqual(ответ["error"]["code"], -32602)
        self.клиент.послать_строку("{не json")
        ответ = self.клиент.получить()
        self.assertEqual(ответ["error"]["code"], -32700)
        self.assertIsNone(ответ["id"])
        self.клиент.послать_строку("[1, 2]")
        self.assertEqual(self.клиент.получить()["error"]["code"], -32600)
        # после ошибок сервер работает дальше
        self.assertEqual(self.клиент.запрос("ping")["result"], {})

    def test_проверить(self):
        итог = self.клиент.вызвать("proverit", {"tekst": "пусть х = 5\nвывод(х)\n"})
        self.assertFalse(итог["isError"])
        self.assertEqual(итог["structuredContent"]["ошибок"], 0)
        self.assertEqual(json.loads(текст(итог))["ошибок"], 0)
        итог = self.клиент.вызвать("proverit", {"tekst": "пусть х = 5\nвывд(х)\n"})
        данные = итог["structuredContent"]
        self.assertEqual(данные["ошибок"], 1)
        сообщение = данные["сообщения"][0]
        self.assertEqual((сообщение["строка"], сообщение["позиция"], сообщение["длина"]), (2, 1, 4))
        self.assertRegex(сообщение["код"], r"^Д\d{4}$")
        self.assertEqual(сообщение["исправление"], "вывод")
        self.assertEqual(json.loads(текст(итог)), данные)

    def test_проверить_файл(self):
        путь = os.path.join(self.папка, "файл.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write("вывод(1 +)\n")
        итог = self.клиент.вызвать("proverit", {"put": "файл.dan"})
        self.assertEqual(итог["structuredContent"]["ошибок"], 1)
        self.assertEqual(итог["structuredContent"]["файл"], "файл.dan")
        итог = self.клиент.вызвать("proverit", {"put": "нет.dan"})
        self.assertTrue(итог["isError"])
        self.assertIn("не найден", текст(итог))
        итог = self.клиент.вызвать("proverit", {})
        self.assertTrue(итог["isError"])

    def test_исправить(self):
        итог = self.клиент.вызвать("ispravit", {"tekst": "пусть х = 5\nвывд(х)\n"})
        данные = итог["structuredContent"]
        self.assertEqual(данные["текст"], "пусть х = 5\nвывод(х)\n")
        self.assertEqual(данные["правки"], ["строка 2: «вывд» -> «вывод»"])
        self.assertEqual(данные["осталось"]["ошибок"], 0)
        итог = self.клиент.вызвать("ispravit", {"tekst": "вывод(1 +)\n"})
        self.assertEqual(итог["structuredContent"]["правки"], [])
        self.assertEqual(итог["structuredContent"]["осталось"]["ошибок"], 1)

    def test_форматировать(self):
        итог = self.клиент.вызвать("formatirovat", {"tekst": "пусть х=5\nвывод( х )\n"})
        self.assertFalse(итог["isError"])
        self.assertEqual(текст(итог), "пусть х = 5\nвывод(х)\n")
        итог = self.клиент.вызвать("formatirovat", {"tekst": "вывод(\"\n"})
        self.assertTrue(итог["isError"])

    def test_запустить(self):
        итог = self.клиент.вызвать("zapustit", {"tekst": "пусть с = ввод()\nвывод(\"привет, \" + с)\n"
                                                         "вывод(аргументы())\n",
                                                "vvod": "мир\n", "argumenty": ["а б", "в"]})
        self.assertFalse(итог["isError"], итог)
        данные = итог["structuredContent"]
        self.assertEqual(данные["код"], 0)
        self.assertEqual(данные["вывод"], "привет, мир\n[\"а б\", \"в\"]\n")
        self.assertIn("привет, мир", текст(итог))
        # временная папка удалена, в папке сервера ничего не появилось
        self.assertNotIn("программа", " ".join(os.listdir(self.папка)))

    def test_запустить_неверную(self):
        итог = self.клиент.вызвать("zapustit", {"tekst": "вывод(1)\nпусть х = вывд(2)\n"})
        self.assertTrue(итог["isError"])
        self.assertEqual(итог["structuredContent"]["состояние"], "в программе ошибка")
        self.assertIn("программа.dan: найдено ошибок: 1", текст(итог))
        self.assertIn("«вывод»", текст(итог))
        self.assertNotIn("dan_", текст(итог))         # без пути временной папки

    def test_ошибка_выполнения(self):
        итог = self.клиент.вызвать("zapustit", {"tekst": "пусть с = [1, 2]\nвывод(с[5])\n"})
        self.assertTrue(итог["isError"])
        данные = итог["structuredContent"]
        self.assertNotEqual(данные["код"], 0)
        self.assertIn("в строке 2", данные["ошибки"])

    def test_ограничение_времени(self):
        начало = time.time()
        итог = self.клиент.вызвать("zapustit", {"tekst": "перем к = 0\nпока истина { к += 1 }\n", "vremya": 1})
        self.assertLess(time.time() - начало, 60)
        self.assertTrue(итог["isError"])
        self.assertEqual(итог["structuredContent"]["код"], -1)
        self.assertIn("остановлена", итог["structuredContent"]["состояние"])
        # сервер работает дальше
        итог = self.клиент.вызвать("zapustit", {"tekst": "вывод(2 + 2)\n"})
        self.assertEqual(итог["structuredContent"]["вывод"], "4\n")

    @unittest.skipIf(os.name == "nt" or os.uname().sysname == "Darwin", "предел памяти - в Linux")
    def test_ограничение_памяти(self):
        программа = ("перем с: [цел] = []\nдля к в 1..100_000_000 { с.добавить(к) }\nвывод(с.длина)\n")
        итог = self.клиент.вызвать("zapustit", {"tekst": программа, "pamyat_mb": 64, "vremya": 60})
        self.assertTrue(итог["isError"])
        self.assertNotEqual(итог["structuredContent"]["код"], 0)
        self.assertNotEqual(итог["structuredContent"]["код"], -1)
        self.assertIn("памяти", итог["structuredContent"]["ошибки"])
        # программа с задачами (потоками) работает и с пределом по умолчанию
        итог = self.клиент.вызвать("zapustit", {"tekst": "функ кв(х: цел) -> цел { вернуть х * х }\n"
                                                         "пусть з = запустить кв(5)\nвывод(з.дождаться())\n"})
        self.assertEqual(итог["structuredContent"]["вывод"], "25\n", итог)

    def test_тестировать(self):
        программа = ("функ ф() -> цел { вернуть 2 }\n"
                     "тест \"верный\" { утвердить_равно(ф(), 2) }\n"
                     "тест \"неверный\" { утвердить_равно(ф(), 3) }\n")
        итог = self.клиент.вызвать("testirovat", {"tekst": программа})
        self.assertTrue(итог["isError"])
        вывод_ = итог["structuredContent"]["вывод"]
        self.assertIn("пройден: верный", вывод_)
        self.assertIn("ПРОВАЛЕН: неверный", вывод_)
        итог = self.клиент.вызвать("testirovat", {"tekst": программа.replace("ф(), 3", "ф(), 2")})
        self.assertFalse(итог["isError"], итог)
        self.assertEqual(итог["structuredContent"]["код"], 0)
        итог = self.клиент.вызвать("testirovat", {"tekst": "вывод(1)\n"})
        self.assertTrue(итог["isError"])
        self.assertIn("нет тестов", текст(итог))
        итог = self.клиент.вызвать("testirovat", {"tekst": "тест \"а\" { утвердить(вывд()) }\n"})
        self.assertTrue(итог["isError"])
        self.assertIn("вывд", текст(итог))

    def test_справка(self):
        итог = self.клиент.вызвать("spravka", {"imya": "частное"})
        self.assertFalse(итог["isError"])
        self.assertIn("частное(цел, цел) -> цел", текст(итог))
        итог = self.клиент.вызвать("spravka", {"imya": "если"})
        self.assertIn("если условие", текст(итог))
        итог = self.клиент.вызвать("spravka", {"imya": "JSON"})
        self.assertTrue(текст(итог).startswith("### json"))
        итог = self.клиент.вызвать("spravka", {"imya": "Python - Дан"})
        self.assertIn("утвердить", текст(итог))
        итог = self.клиент.вызвать("spravka", {})
        self.assertIn("- Модули библиотеки", текст(итог))
        итог = self.клиент.вызвать("spravka", {"imya": "простые"})
        self.assertTrue(итог["isError"])
        self.assertIn("простые_множители", текст(итог))
        итог = self.клиент.вызвать("spravka", {"vsyo": True})
        with open(os.path.join(ПАПКА, "llms-full.txt"), encoding="utf-8") as ф:
            self.assertEqual(текст(итог), ф.read())


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыНовогоПроекта(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.mkdtemp(prefix="dan_novyy_")

    def tearDown(self):
        shutil.rmtree(self.папка, ignore_errors=True)

    def выполнить(self, *аргументы, cwd=None):
        р = subprocess.run([sam_kompilyator.исполняемый(), *аргументы], capture_output=True, env=среда(),
                           cwd=cwd or self.папка, timeout=600)
        return р.returncode, р.stdout.decode("utf-8", "replace")

    def test_создаёт_проект(self):
        код, вывод_ = self.выполнить("новый", "мой_бот")
        self.assertEqual(код, 0, вывод_)
        проект = os.path.join(self.папка, "мой_бот")
        self.assertEqual(sorted(os.listdir(проект)), sorted(["главная.dan", "AGENTS.md", "CLAUDE.md", ".gitignore"]))
        with open(os.path.join(проект, "AGENTS.md"), encoding="utf-8") as ф:
            инструкция = ф.read()
        self.assertIn(f"/v{versiya.ВЕРСИЯ}/llms-full.txt", инструкция)
        self.assertIn(os.path.join(ПАПКА, "llms-full.txt"), инструкция)
        for команда in ("dan проверить главная.dan", "dan запустить главная.dan", "dan тестировать главная.dan",
                        "claude mcp add dan -- dan mcp"):
            self.assertIn(команда, инструкция)
        self.assertNotRegex(инструкция, r"[\u2013\u2014]")
        with open(os.path.join(проект, "CLAUDE.md"), encoding="utf-8") as ф:
            self.assertEqual(ф.read(), "@AGENTS.md\n")
        # программа проекта верна, её тест проходит и она уже в едином виде
        код, вывод_ = self.выполнить("проверить", "главная.dan", cwd=проект)
        self.assertEqual(код, 0, вывод_)
        код, вывод_ = self.выполнить("тестировать", "главная.dan", cwd=проект)
        self.assertEqual(код, 0, вывод_)
        self.assertIn("пройден: приветствие", вывод_)
        код, вывод_ = self.выполнить("формат", "главная.dan", "--проверить", cwd=проект)
        self.assertEqual(код, 0, вывод_)

    def test_непустая_папка(self):
        проект = os.path.join(self.папка, "есть")
        os.mkdir(проект)
        with open(os.path.join(проект, "файл.txt"), "w") as ф:
            ф.write("1")
        код, вывод_ = self.выполнить("новый", "есть")
        self.assertEqual(код, 1)
        self.assertIn("не пуста", вывод_)
        self.assertEqual(os.listdir(проект), ["файл.txt"])
        код, вывод_ = self.выполнить("новый")
        self.assertEqual(код, 2)
        self.assertIn("дан новый имя", вывод_)


if __name__ == "__main__":
    unittest.main()
