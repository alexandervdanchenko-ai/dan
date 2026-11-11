"""Тесты набора «оценка_моделей»: задачи, по которым мерится, насколько верно языковые модели пишут
программы на Дане с первой попытки и за три (оценка_моделей/оценить.py).

Все эталонные решения проходят свои проверки - так задачи остаются решаемыми при изменениях языка
и библиотеки. Программы собирает компилятор на Дане (sam_kompilyator) в любом режиме тестов: задачи
пользуются возможностями, которых нет у замороженного компилятора на Python (json.разобрать_в,
параллельно для). Сценарий оценки проверяется с поддельными исполнителями; сеть наружу не нужна:
исполнитель HTTP API обращается к местному серверу на 127.0.0.1.
"""

import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

import sam_kompilyator
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ПАПКА = os.path.dirname(os.path.abspath(__file__))
НАБОР = os.path.join(ПАПКА, "оценка_моделей")
sys.path.insert(0, НАБОР)
import оценить  # noqa: E402

НАИМЕНЬШЕЕ_ЧИСЛО_ЗАДАЧ = 60
БЕЗ_ПРОКСИ = {"NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost"}


def компилятор():
    return оценить.Компилятор(sam_kompilyator.исполняемый())


def задача(номер):
    return оценить.загрузить_задачи(выбор=[str(номер)])[0]


def ответ(программа):
    return "Вот программа:\n\n```dan\n" + программа + "```\n\nОна читает ввод и печатает итог."


class ТестыУстройства(unittest.TestCase):
    """Файлы задач: условие, области, проверки, эталон."""

    def test_задачи_описаны(self):
        задачи = оценить.загрузить_задачи()
        self.assertGreaterEqual(len(задачи), НАИМЕНЬШЕЕ_ЧИСЛО_ЗАДАЧ)
        self.assertEqual(len({з.номер for з in задачи}), len(задачи), "номера задач различны")
        for з in задачи:
            with self.subTest(задача=з.имя):
                self.assertIn(з.область, оценить.ОБЛАСТИ)
                self.assertIn(з.трудность, (1, 2, 3))
                self.assertGreater(len(з.условие), 40)
                self.assertGreaterEqual(len(з.проверки), 3, "проверок не меньше трёх")
                self.assertTrue(os.path.isfile(з.путь_эталона))
                for слово in з.обязательно:
                    self.assertIn(слово, з.эталон())
        for область in оценить.ОБЛАСТИ:
            self.assertGreaterEqual(sum(з.область == область for з in задачи), 3, область)
        # каждый файл папки задач - часть какой-нибудь задачи
        имена = {з.имя for з in задачи}
        for имя in os.listdir(оценить.ЗАДАЧИ):
            self.assertIn(имя.rsplit(".", 1)[0], имена, имя)

    def test_без_длинных_тире(self):
        for корень, _, файлы in os.walk(НАБОР):
            for имя in файлы:
                if имя.endswith((".задача", ".dan", ".py", ".md")):
                    with open(os.path.join(корень, имя), encoding="utf-8") as ф:
                        текст = ф.read()
                    self.assertNotIn("\u2014", текст, имя)
                    self.assertNotIn("\u2013", текст, имя)

    def test_разбор_задачи(self):
        з = оценить.разобрать_задачу(
            "область: файлы\nтрудность: 2\nобязательно: канал; запустить\n\nУсловие\nв две строки.\n\n"
            "=== проверка первая\n=== аргументы\nа.txt\nдва слова\n=== файл а.txt\nстрока\n\\ нет перевода строки\n"
            "=== файл папка/\n=== ввод\n1\n2\n\n=== вывод\n3\n=== код 1\n=== файл после итог.txt\nок\n"
            "=== проверка\n=== ввод\n=== вывод\n", "001_проба", "")
        self.assertEqual((з.номер, з.область, з.трудность), (1, "файлы", 2))
        self.assertEqual(з.обязательно, ["канал", "запустить"])
        self.assertEqual(з.условие, "Условие\nв две строки.")
        п = з.проверки[0]
        self.assertEqual(п.имя, "первая")
        self.assertEqual(п.аргументы, ["а.txt", "два слова"])
        self.assertEqual(п.файлы, {"а.txt": "строка", "папка/": ""})
        self.assertEqual((п.ввод, п.вывод, п.код), ("1\n2\n", "3\n", 1))
        self.assertEqual(п.файлы_после, {"итог.txt": "ок\n"})
        self.assertEqual((з.проверки[1].ввод, з.проверки[1].вывод, з.проверки[1].код), ("", "", 0))
        with self.assertRaises(ValueError):
            оценить.разобрать_задачу("область: строки\n\nУсловие\n=== ввод\n1\n", "002_ошибка", "")
        with self.assertRaises(ValueError):
            оценить.разобрать_задачу("язык: Дан\n\nУсловие\n", "003_ошибка", "")

    def test_выбор_задач(self):
        self.assertEqual([з.номер for з in оценить.загрузить_задачи(выбор=["3", "10-12"])], [3, 10, 11, 12])
        self.assertTrue(all("json" in з.имя for з in оценить.загрузить_задачи(выбор=["json"])))
        даты = оценить.загрузить_задачи(области=["даты"])
        self.assertTrue(даты and all(з.область == "даты" for з in даты))

    def test_извлечь_программу(self):
        извлечь = оценить.извлечь_программу
        self.assertEqual(извлечь("Текст\n```dan\nвывод(1)\n```\nещё"), "вывод(1)\n")
        self.assertEqual(извлечь("```python\nprint(1)\n```\n```дан\nвывод(2)\n```"), "вывод(2)\n")
        self.assertEqual(извлечь("```\nвывод(3)\n```"), "вывод(3)\n")
        self.assertEqual(извлечь("```dan\nвывод(1)\n```\nИсправленная:\n```dan\nвывод(4)\n```"), "вывод(4)\n")
        self.assertEqual(извлечь("вывод(5)"), "вывод(5)\n")
        self.assertEqual(извлечь(""), "\n")

    def test_запрос(self):
        з = задача(61)
        сообщения = оценить.первый_запрос(з, "ОПИСАНИЕ ЯЗЫКА")
        self.assertEqual([с["role"] for с in сообщения], ["system", "user"])
        self.assertEqual(сообщения[0]["content"], "ОПИСАНИЕ ЯЗЫКА")
        текст = сообщения[1]["content"]
        self.assertIn(з.условие, текст)
        self.assertIn("«параллельно для»", текст)
        # первая проверка - пример в условии
        self.assertIn("Пример.", текст)
        self.assertIn(з.проверки[0].вывод.strip(), текст)
        self.assertEqual([с["role"] for с in оценить.первый_запрос(з, "")], ["user"])
        диалог = оценить.диалог_в_текст(сообщения + [{"role": "assistant", "content": "ОТВЕТ"}])
        self.assertTrue(диалог.startswith("ОПИСАНИЕ ЯЗЫКА"))
        self.assertIn("## Твой прошлый ответ\n\nОТВЕТ", диалог)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыЭталонов(unittest.TestCase):
    """Эталонные решения проходят все свои проверки с первой попытки и записаны в едином виде."""

    def test_эталоны_решают_задачи(self):
        задачи = оценить.загрузить_задачи()
        итог = оценить.оценить(задачи, оценить.исполнитель_эталон, компилятор(), попыток=1,
                               потоков=min(4, os.cpu_count() or 1), модель="эталон")
        нерешённые = {р["имя"]: р["попытки"][0]["сообщение"] for р in итог["задачи"] if not р["решена"]}
        self.assertEqual(нерешённые, {})
        self.assertEqual((итог["задач"], итог["с_первой"], итог["решено"]), (len(задачи),) * 3)

    def test_единый_вид(self):
        for з in оценить.загрузить_задачи():
            with self.subTest(задача=з.имя):
                р = subprocess.run([sam_kompilyator.исполняемый(), "формат", "--проверить", з.путь_эталона],
                                   capture_output=True, text=True, encoding="utf-8", timeout=120,
                                   env=sam_kompilyator.среда())
                self.assertEqual(р.returncode, 0, р.stdout + р.stderr)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыОценки(unittest.TestCase):
    """Подсчёт попыток с поддельным исполнителем, исполнители-команда и HTTP API, командная строка."""

    def test_первая_и_третья_попытки(self):
        задачи = [задача(1), задача(9), задача(11), задача(13), задача(61)]
        запросы = {}

        def исполнитель(сообщения, з):
            запросы.setdefault(з.имя, []).append(сообщения)
            номер = len(запросы[з.имя])
            if з.номер == 1:
                return оценить.исполнитель_эталон(сообщения, з)
            if з.номер == 9:
                # первая попытка не проходит проверку компилятора, вторая - эталон
                return ответ("print(1)\n") if номер == 1 else оценить.исполнитель_эталон(сообщения, з)
            if з.номер == 11:
                # всегда неверный вывод
                return ответ("вывод(\"нет\")\n")
            if з.номер == 13:
                raise RuntimeError("модель недоступна")
            # 61: верный итог без «параллельно для», затем - на третьей попытке - эталон
            if номер < 3:
                return ответ("пусть н = цел(ввод())\nвывод(частное(н * (н + 1) * (2 * н + 1), 6))\n")
            return оценить.исполнитель_эталон(сообщения, з)

        итог = оценить.оценить(задачи, исполнитель, компилятор(), подсказка="ОПИСАНИЕ", попыток=3)
        по_имени = {р["имя"]: р for р in итог["задачи"]}
        р1, р9, р11, р13, р61 = (по_имени[з.имя] for з in задачи)
        self.assertEqual((р1["решена_с_попытки"], р1["попыток"]), (1, 1))
        self.assertEqual((р9["решена_с_попытки"], р9["попыток"]), (2, 2))
        self.assertEqual(р9["попытки"][0]["этап"], "проверка")
        self.assertEqual((р11["решена"], р11["попыток"]), (False, 3))
        self.assertEqual([п["этап"] for п in р11["попытки"]], ["вывод"] * 3)
        self.assertEqual((р13["решена"], р13["попыток"], р13["попытки"][0]["этап"]), (False, 1, "ответ"))
        self.assertIn("модель недоступна", р13["попытки"][0]["сообщение"])
        self.assertEqual((р61["решена_с_попытки"], [п["этап"] for п in р61["попытки"]]), (3, ["запись", "запись", "успех"]))
        self.assertEqual((итог["задач"], итог["с_первой"], итог["решено"]), (5, 1, 3))
        self.assertEqual(итог["первые_ошибки"], {"проверка": 1, "вывод": 1, "ответ": 1, "запись": 1})
        self.assertEqual(итог["по_областям"]["числа"], {"задач": 3, "с_первой": 0, "решено": 1})
        self.assertEqual(list(итог["по_областям"]), ["строки", "числа", "параллельность"])
        # следующий запрос - весь прошлый диалог, ответ модели и сообщения компилятора
        второй = запросы[задачи[1].имя][1]
        self.assertEqual([с["role"] for с in второй], ["system", "user", "assistant", "user"])
        self.assertIn("print(1)", второй[2]["content"])
        self.assertIn("dan проверить", второй[3]["content"])
        self.assertIn("print", второй[3]["content"])
        третий = запросы[задачи[2].имя][2]
        self.assertEqual(len(третий), 6)
        self.assertIn("Ожидалось:", третий[5]["content"])
        self.assertIn("Получено:", третий[5]["content"])
        таблица = оценить.таблица(итог)
        self.assertIn("всего", таблица)
        self.assertIn("1/5 (20%)", таблица)
        self.assertIn("3/5 (60%)", таблица)
        self.assertIn("Не решены: " + задачи[2].имя + ", " + задачи[3].имя, таблица)
        json.dumps(итог, ensure_ascii=False)

    def test_исполнитель_команда(self):
        з = задача(2)
        with tempfile.TemporaryDirectory() as папка:
            сценарий = os.path.join(папка, "модель.py")
            with open(сценарий, "w", encoding="utf-8") as ф:
                ф.write(
                    "import json, os, sys\n"
                    "запрос = sys.stdin.buffer.read().decode('utf-8')\n"
                    "диалог = json.load(open(os.environ['DAN_OCENKA_DIALOG'], encoding='utf-8'))\n"
                    "assert 'Задача:' in запрос and диалог[-1]['role'] == 'user', запрос\n"
                    f"эталон = open({з.путь_эталона!r}, encoding='utf-8').read()\n"
                    "assert os.environ['DAN_OCENKA_ZADACHA'] == " + repr(з.имя) + "\n"
                    "sys.stdout.buffer.write(('```dan\\n' + эталон + '```\\n').encode('utf-8'))\n")
            исполнитель = оценить.исполнитель_команда([sys.executable, сценарий])
            р = оценить.решить(з, исполнитель, компилятор(), "ОПИСАНИЕ")
            self.assertEqual(р["решена_с_попытки"], 1, р["попытки"][0]["сообщение"])
            сбой = оценить.решить(з, оценить.исполнитель_команда([sys.executable, "-c", "import sys; sys.exit(3)"]),
                                  компилятор(), "")
            self.assertEqual(сбой["попытки"][0]["этап"], "ответ")
            self.assertIn("кодом 3", сбой["попытки"][0]["сообщение"])

    def test_исполнитель_api(self):
        з = задача(9)
        полученные = []

        class Обработчик(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                тело = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8"))
                полученные.append((self.path, self.headers.get("Authorization"), тело))
                ответ_ = {"choices": [{"message": {"role": "assistant", "content": оценить.исполнитель_эталон([], з)}}]}
                данные = json.dumps(ответ_, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(данные)))
                self.end_headers()
                self.wfile.write(данные)

            def log_message(self, *_):
                pass

        сервер = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Обработчик)
        поток = threading.Thread(target=сервер.serve_forever, daemon=True)
        поток.start()
        try:
            адрес = f"http://127.0.0.1:{сервер.server_address[1]}/v1"
            with mock.patch.dict(os.environ, БЕЗ_ПРОКСИ):
                исполнитель = оценить.исполнитель_api(адрес, "sk-local", "проба", температура=0.0)
                р = оценить.решить(з, исполнитель, компилятор(), "ОПИСАНИЕ")
        finally:
            сервер.shutdown()
            сервер.server_close()
        self.assertEqual(р["решена_с_попытки"], 1, р["попытки"][0]["сообщение"])
        путь, ключ, тело = полученные[0]
        self.assertEqual((путь, ключ), ("/v1/chat/completions", "Bearer sk-local"))
        self.assertEqual((тело["model"], тело["temperature"]), ("проба", 0.0))
        self.assertEqual([с["role"] for с in тело["messages"]], ["system", "user"])

    def test_командная_строка(self):
        with tempfile.TemporaryDirectory() as папка:
            путь_json = os.path.join(папка, "итог.json")
            среда = dict(sam_kompilyator.среда(), DAN_SAM_EXE=sam_kompilyator.исполняемый(), PYTHONIOENCODING="utf-8")
            р = subprocess.run([sys.executable, os.path.join(НАБОР, "оценить.py"), "--эталон", "--задачи=1,56",
                                "--json=" + путь_json, "--тихо"], capture_output=True, text=True, encoding="utf-8",
                               timeout=600, env=среда)
            self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
            self.assertIn("всего", р.stdout)
            with open(путь_json, encoding="utf-8") as ф:
                итог = json.load(ф)
        self.assertEqual((итог["модель"], итог["задач"], итог["с_первой"], итог["попыток"]), ("эталон", 2, 2, 1))
        self.assertEqual(итог["подсказка"], "llms-full.txt")
        self.assertEqual(итог["задачи"][0]["попытки"][0]["этап"], "успех")


if __name__ == "__main__":
    unittest.main()
