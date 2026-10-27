"""Тесты сервера языка (server.py): обмен сообщениями протокола LSP с запущенным сервером."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

from server import uri_из_пути

ПАПКА = os.path.dirname(os.path.abspath(__file__))


class Клиент:
    """Простейший клиент LSP: запускает «python dan.py сервер» и обменивается с ним сообщениями."""

    def __init__(self):
        self.процесс = subprocess.Popen([sys.executable, os.path.join(ПАПКА, "dan.py"), "сервер"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.номер = 0
        self.уведомления = []

    def послать(self, сообщение):
        данные = json.dumps(сообщение, ensure_ascii=False).encode("utf-8")
        self.процесс.stdin.write(f"Content-Length: {len(данные)}\r\n\r\n".encode() + данные)
        self.процесс.stdin.flush()

    def получить(self):
        заголовки = {}
        while True:
            линия = self.процесс.stdout.readline().decode().strip()
            if not линия:
                break
            имя, _, значение = линия.partition(":")
            заголовки[имя.lower()] = значение.strip()
        return json.loads(self.процесс.stdout.read(int(заголовки["content-length"])).decode("utf-8"))

    def запрос(self, метод, параметры):
        self.номер += 1
        self.послать({"jsonrpc": "2.0", "id": self.номер, "method": метод, "params": параметры})
        while True:
            ответ = self.получить()
            if ответ.get("id") == self.номер:
                if "error" in ответ:
                    raise AssertionError(ответ["error"]["message"])
                return ответ["result"]
            self.уведомления.append(ответ)

    def уведомление(self, метод, параметры):
        self.послать({"jsonrpc": "2.0", "method": метод, "params": параметры})

    def ждать_ошибок(self, uri):
        while True:
            for у_ in self.уведомления:
                if у_.get("method") == "textDocument/publishDiagnostics" and у_["params"]["uri"] == uri:
                    self.уведомления.remove(у_)
                    return у_["params"]["diagnostics"]
            self.уведомления.append(self.получить())

    def закрыть(self):
        self.запрос("shutdown", None)
        self.уведомление("exit", None)
        self.процесс.wait(timeout=10)
        self.процесс.stdout.close()
        self.процесс.stdin.close()
        self.процесс.stderr.close()


ПРОГРАММА = """подключить "json"
подключить "расчёты.dan" как р

тип Дело { номер: стр, сумма: цел }

функ итог(дела: [Дело]) -> цел {
    перем всего = 0
    для д в дела { всего += д.сумма }
    вернуть всего
}

пусть з = json.разобрать("{}")!
вывод(итог([Дело("А", 1)]) + р.пени(1))
"""

РАСЧЁТЫ = "// модуль расчётов\nобщий функ пени(сумма: цел) -> цел {\n    вернуть сумма * 2\n}\nфунк скрытая() -> цел { вернуть 1 }\n"


class ТестыСервера(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.TemporaryDirectory()
        self.путь = os.path.join(self.папка.name, "главная.dan")
        with open(os.path.join(self.папка.name, "расчёты.dan"), "w", encoding="utf-8") as ф:
            ф.write(РАСЧЁТЫ)
        self.uri = uri_из_пути(self.путь)
        self.клиент = Клиент()
        ответ = self.клиент.запрос("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
        self.assertTrue(ответ["capabilities"]["hoverProvider"])
        self.клиент.уведомление("initialized", {})

    def tearDown(self):
        self.клиент.закрыть()
        self.папка.cleanup()

    def открыть(self, текст):
        self.клиент.уведомление("textDocument/didOpen", {"textDocument": {
            "uri": self.uri, "languageId": "dan", "version": 1, "text": текст}})
        return self.клиент.ждать_ошибок(self.uri)

    def изменить(self, текст):
        self.клиент.уведомление("textDocument/didChange", {"textDocument": {"uri": self.uri, "version": 2},
                                                           "contentChanges": [{"text": текст}]})
        return self.клиент.ждать_ошибок(self.uri)

    def test_ошибки_в_тексте(self):
        self.assertEqual(self.открыть(ПРОГРАММА), [])
        ошибки = self.изменить(ПРОГРАММА.replace("вернуть всего", "вернуть \"всего\""))
        self.assertEqual(len(ошибки), 1)
        self.assertIn("функция «итог» должна вернуть цел", ошибки[0]["message"])
        self.assertEqual(ошибки[0]["range"]["start"]["line"], 8)
        ошибки = self.изменить("пусть а = (1\n")
        self.assertEqual(ошибки[0]["range"]["start"]["line"], 1)
        ошибки = self.изменить("пусть сумма = 1\nвывод(сумм)\n")
        self.assertIn("возможно, имелось в виду «сумма»", ошибки[0]["message"])
        self.assertEqual(ошибки[0]["range"], {"start": {"line": 1, "character": 6}, "end": {"line": 1, "character": 10}})
        ошибки = self.изменить('подключить "расчёты.dan" как р\nвывод(р.скрытая())\n')
        self.assertIn("не помечено словом «общий»", ошибки[0]["message"])

    def test_подсказки(self):
        self.открыть(ПРОГРАММА)
        def подсказки(строка, символ):
            return {в["label"]: в for в in self.клиент.запрос("textDocument/completion", {
                "textDocument": {"uri": self.uri}, "position": {"line": строка, "character": символ}})["items"]}
        все = подсказки(12, 0)
        for имя in ("функ", "вывод", "итог", "Дело", "корень", "сегодня", "р", "json"):
            self.assertIn(имя, все)
        self.assertIn("корень", все["корень"]["documentation"])
        модуль = подсказки(12, len("вывод(итог([Дело(\"А\", 1)]) + р."))
        self.assertEqual(set(модуль), {"пени"})
        json_ = подсказки(11, len("пусть з = json."))
        self.assertIn("разобрать", json_)
        self.assertIn("записать_красиво", json_)
        self.assertNotIn("строка_в_текст", json_)

    def test_описание_и_переход(self):
        self.открыть(ПРОГРАММА)
        описание = self.клиент.запрос("textDocument/hover", {"textDocument": {"uri": self.uri},
                                                              "position": {"line": 12, "character": 8}})
        self.assertIn("функ итог(дела: [Дело]) -> цел", описание["contents"]["value"])
        описание = self.клиент.запрос("textDocument/hover", {"textDocument": {"uri": self.uri},
                                                              "position": {"line": 12, "character": 31}})
        self.assertIn("функ пени(сумма: цел) -> цел", описание["contents"]["value"])
        # ключевое слово в начале строки: подсказка объясняет и всю строку
        описание = self.клиент.запрос("textDocument/hover", {"textDocument": {"uri": self.uri},
                                                              "position": {"line": 7, "character": 5}})
        self.assertIn("**Эта строка.** Цикл «для»: переменная «д» по очереди принимает элементы «дела»",
                      описание["contents"]["value"])
        место = self.клиент.запрос("textDocument/definition", {"textDocument": {"uri": self.uri},
                                                                "position": {"line": 12, "character": 31}})
        self.assertTrue(место["uri"].endswith(uri_из_пути(os.path.join(self.папка.name, "расчёты.dan"))[-30:]))
        self.assertEqual(место["range"]["start"]["line"], 1)
        место = self.клиент.запрос("textDocument/definition", {"textDocument": {"uri": self.uri},
                                                                "position": {"line": 12, "character": 13}})
        self.assertEqual(место["range"]["start"]["line"], 3)
        символы = self.клиент.запрос("textDocument/documentSymbol", {"textDocument": {"uri": self.uri}})
        self.assertEqual({с["name"] for с in символы}, {"Дело", "итог", "з"})

    def test_форматирование(self):
        self.открыть("пусть а=1\nвывод( а+2 )\n")
        правки = self.клиент.запрос("textDocument/formatting", {"textDocument": {"uri": self.uri},
                                                                 "options": {"tabSize": 4, "insertSpaces": True}})
        self.assertEqual(правки[0]["newText"], "пусть а = 1\nвывод(а + 2)\n")


if __name__ == "__main__":
    unittest.main()
