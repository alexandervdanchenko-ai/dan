"""Песочница: сервер на 127.0.0.1 собирает и выполняет программы со своей страницы."""

import json
import threading
import unittest
import urllib.error
import urllib.request

import dan
import pesochnica

ОШИБКИ = (dan.ОшибкаЛексера, dan.ОшибкаРазбора, dan.ОшибкаМодуля, dan.ОшибкаПроверки, dan.ОшибкаСборки)


class ТестыПесочницы(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.сервер = pesochnica.создать_сервер(0, dan.собрать, ОШИБКИ)
        cls.адрес = f"http://127.0.0.1:{cls.сервер.server_address[1]}"
        cls.поток = threading.Thread(target=cls.сервер.serve_forever, daemon=True)
        cls.поток.start()

    @classmethod
    def tearDownClass(cls):
        cls.сервер.shutdown()
        cls.сервер.server_close()

    def запрос(self, путь, данные=None, ключ=None, заголовки=None):
        тело = None if данные is None else json.dumps(данные).encode("utf-8")
        з = urllib.request.Request(self.адрес + urllib.request.quote(путь), data=тело,
                                   headers={"Content-Type": "application/json",
                                            "X-Dan-Key": self.сервер.ключ if ключ is None else ключ,
                                            **(заголовки or {})})
        try:
            with urllib.request.urlopen(з, timeout=120) as о:
                return о.status, о.read().decode("utf-8")
        except urllib.error.HTTPError as о:
            return о.code, о.read().decode("utf-8")

    def test_страница_содержит_ключ(self):
        код, текст = self.запрос("/")
        self.assertEqual(код, 200)
        self.assertIn(self.сервер.ключ, текст)
        self.assertIn("Песочница Дана", текст)

    def test_чужой_запрос_отклоняется(self):
        код, _ = self.запрос("/выполнить", {"текст": "вывод(1)"}, ключ="foreign-key")
        self.assertEqual(код, 403)
        # подмена имени сервера (DNS rebinding): чужое имя в заголовке Host
        код, _ = self.запрос("/", заголовки={"Host": "evil.example:80"})
        self.assertEqual(код, 403)

    def test_ошибка_в_программе(self):
        код, текст = self.запрос("/выполнить", {"текст": "вывод(нет_такой)", "ввод": ""})
        ответ = json.loads(текст)
        self.assertEqual(ответ["состояние"], "в программе ошибка")
        self.assertIn("нет_такой", ответ["ошибки"])

    def test_формат_и_примеры(self):
        код, текст = self.запрос("/формат", {"текст": "пусть  х=1\n"})
        self.assertEqual(json.loads(текст)["текст"], "пусть х = 1\n")
        _, текст = self.запрос("/примеры", {})
        примеры = json.loads(текст)["примеры"]
        self.assertIn("алгебра", примеры)
        _, текст = self.запрос("/пример", {"имя": "алгебра"})
        self.assertIn("вывод", json.loads(текст)["текст"])
        _, текст = self.запрос("/пример", {"имя": "../dan"})
        self.assertIn("ошибка", json.loads(текст))

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_выполнение_с_вводом(self):
        код, текст = self.запрос("/выполнить", {"текст": 'пусть имя = ввод()\nвывод("Привет, " + имя)\n', "ввод": "Аня\n"})
        ответ = json.loads(текст)
        self.assertEqual(ответ["вывод"], "Привет, Аня\n")
        self.assertTrue(ответ["состояние"].startswith("готово"), ответ)

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_начальная_программа_выполняется(self):
        _, текст = self.запрос("/выполнить", {"текст": pesochnica.НАЧАЛО})
        ответ = json.loads(текст)
        self.assertEqual(ответ["ошибки"], "")
        self.assertIn("5 в квадрате = 25", ответ["вывод"])

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_долгая_программа_останавливается(self):
        старое = pesochnica.ВРЕМЯ
        pesochnica.ВРЕМЯ = 1
        try:
            _, текст = self.запрос("/выполнить", {"текст": "перем н = 0\nпока истина { н = (н + 1) % 7 }\n"})
        finally:
            pesochnica.ВРЕМЯ = старое
        self.assertIn("остановлена", json.loads(текст)["состояние"])

    @unittest.skipUnless(dan.компилятор_есть(), "нет компилятора C")
    def test_ошибка_выполнения(self):
        _, текст = self.запрос("/выполнить", {"текст": "пусть н = 0\nвывод(5 % н)\n"})
        ответ = json.loads(текст)
        self.assertIn("остаток от деления на ноль", ответ["ошибки"])
        self.assertIn("завершилась с кодом", ответ["состояние"])


if __name__ == "__main__":
    unittest.main()
