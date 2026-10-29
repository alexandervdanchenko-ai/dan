"""Тесты машинного вывода `dan проверить --json` и постоянных кодов сообщений (kody.py)."""

import glob
import json
import os
import subprocess
import sys
import tempfile
import unittest

import dan
import kody

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def проверить_json(текст, *дополнительные):
    """(код завершения, разобранный JSON) для программы «текст» в папке с файлами «дополнительные»."""
    with tempfile.TemporaryDirectory() as папка:
        for имя, содержимое in дополнительные:
            with open(os.path.join(папка, имя), "w", encoding="utf-8") as ф:
                ф.write(содержимое)
        with open(os.path.join(папка, "п.dan"), "w", encoding="utf-8") as ф:
            ф.write(текст)
        р = subprocess.run([sys.executable, os.path.join(ПАПКА, "dan.py"), "проверить", "--json", "п.dan"],
                           capture_output=True, timeout=120, cwd=папка)
        return р.returncode, json.loads(р.stdout.decode("utf-8"))


class ТестыОбобщения(unittest.TestCase):

    def test_шаблоны(self):
        for текст, шаблон in [
            ("переменная «сума» не объявлена; возможно, имелось в виду «сумма»", "переменная «*» не объявлена"),
            ("нельзя сложить стр и цел", "нельзя сложить Т и Т"),
            ("у типа Книга нет поля «автр»", "у типа Т нет поля «*»"),
            ("у значения типа json.Значение нет метода «х»", "у значения типа Т нет метода «*»"),
            ("ожидалось «)» после аргументов функции, а найдено конец файла", "ожидалось «*» после аргументов функции"),
            ("функция «ф» принимает 2 значения, а передано 3", "функция «*» принимает * значения"),
            ("неизвестная последовательность \\q", "неизвестная последовательность \\*"),
            ("недопустимый символ «x» (U+202E): такие символы", "недопустимый символ «*» (U+*)"),
            ("переменная «к» не объявлена (в «ф» при Т = цел)", "переменная «*» не объявлена"),
        ]:
            with self.subTest(текст=текст):
                self.assertEqual(kody.обобщить(текст), шаблон)

    def test_таблица(self):
        коды = [к for к, _ in kody.КОДЫ]
        шаблоны = [ш for _, ш in kody.КОДЫ]
        self.assertEqual(len(коды), len(set(коды)))
        self.assertEqual(len(шаблоны), len(set(шаблоны)))
        self.assertEqual(коды, ["Д%04d" % н for н in range(1, len(коды) + 1)])
        # шаблон таблицы - неподвижная точка обобщения: иначе его код никогда не выдаётся
        for _, ш in kody.КОДЫ:
            with self.subTest(шаблон=ш):
                self.assertEqual(kody.обобщить(ш), ш)

    def test_у_ошибок_каталога_есть_коды(self):
        for путь in sorted(glob.glob(os.path.join(ПАПКА, "каталог", "ошибки", "*.dan"))):
            if путь.endswith(".исправлено.dan"):
                continue
            with open(путь, encoding="utf-8") as ф:
                исходник = ф.read()
            for з in dan.замечания(путь, исходник):
                with self.subTest(файл=os.path.basename(путь), текст=з[4]):
                    self.assertNotEqual(kody.код(з[4]), "Д0000")

    def test_страница_кодов_не_устарела(self):
        with open(os.path.join(ПАПКА, kody.СТРАНИЦА), encoding="utf-8") as ф:
            self.assertEqual(ф.read(), kody.страница(), "запустите python kody.py")


class ТестыМашинногоВывода(unittest.TestCase):

    def test_ошибки_с_исправлением(self):
        код, д = проверить_json("пусть сумма = 1\nвывод(сума)\nвывод(сумма + \"а\")\n")
        self.assertEqual(код, 1)
        self.assertEqual((д["файл"], д["ошибок"], д["предупреждений"]), ("п.dan", 2, 0))
        первое = д["сообщения"][0]
        self.assertEqual({к: первое[к] for к in ("вид", "код", "файл", "строка", "позиция", "длина", "исправление")},
                         {"вид": "ошибка", "код": "Д0001", "файл": "п.dan", "строка": 2, "позиция": 7, "длина": 4,
                          "исправление": "сумма"})
        self.assertNotIn("исправление", д["сообщения"][1])

    def test_ошибка_записи(self):
        код, д = проверить_json("функ ф() {\nвывод(1\n")
        self.assertEqual((код, д["ошибок"]), (1, 1))
        self.assertTrue(д["сообщения"][0]["текст"].startswith("ожидалось «)» после аргументов функции"))

    def test_ошибка_в_подключённом_файле(self):
        код, д = проверить_json('подключить "мод.dan"\nвывод(мод.ф())\n',
                                ("мод.dan", "общий функ ф( -> цел {\n"))
        с = д["сообщения"][0]
        self.assertEqual((код, с["файл"], с["строка"], с["позиция"], с["длина"]), (1, "мод.dan", 1, 15, 2))

    def test_без_ошибок_и_предупреждения(self):
        self.assertEqual(проверить_json("вывод(1)\n"),
                         (0, {"файл": "п.dan", "ошибок": 0, "предупреждений": 0, "сообщения": []}))
        код, д = проверить_json("вывод(праздник(сегодня()))\n")
        self.assertEqual((код, д["ошибок"], д["предупреждений"], д["сообщения"][0]["вид"]),
                         (0, 0, 1, "предупреждение"))

    def test_исправить(self):
        текст = ("def квадрат(х: цел) -> цел {\n    return х * х\n}\nперем сумма = 0\nсумма++\nвывод(квадрат(сума))\n"
                 "перем с = [1]\nс.append(2)\nесли 1 > 0 && 2 > 1 {\n    print(len(с))\n}\n")
        with tempfile.TemporaryDirectory() as папка:
            путь = os.path.join(папка, "п.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            р = subprocess.run([sys.executable, os.path.join(ПАПКА, "dan.py"), "исправить", "п.dan"],
                               capture_output=True, timeout=120, cwd=папка)
            with open(путь, encoding="utf-8") as ф:
                итог = ф.read()
        вывод_ = р.stdout.decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(р.returncode, 1)          # len(с) готового исправления не имеет
        self.assertIn("  строка 1: «def» -> «функ»\n", вывод_)
        self.assertIn("п.dan: исправлено мест: 7\nп.dan: осталось ошибок: 1", вывод_)
        self.assertEqual(итог, "функ квадрат(х: цел) -> цел {\n    вернуть х * х\n}\nперем сумма = 0\nсумма+= 1\n"
                               "вывод(квадрат(сумма))\nперем с = [1]\nс.добавить(2)\nесли 1 > 0 и 2 > 1 {\n"
                               "    вывод(len(с))\n}\n")

    def test_исправить_без_исправлений(self):
        with tempfile.TemporaryDirectory() as папка:
            with open(os.path.join(папка, "п.dan"), "w", encoding="utf-8") as ф:
                ф.write("вывод(1)\n")
            р = subprocess.run([sys.executable, os.path.join(ПАПКА, "dan.py"), "исправить", "п.dan"],
                               capture_output=True, timeout=120, cwd=папка)
        self.assertEqual((р.returncode, р.stdout.decode("utf-8").replace("\r\n", "\n")),
                         (0, "п.dan: готовых исправлений нет\nп.dan: ошибок не найдено\n"))

    def test_строка_json_как_в_дане(self):
        self.assertEqual(kody.строка_json('а"б\\в\nг\tд\x01'), '"а\\"б\\\\в\\nг\\tд\\u0001"')


if __name__ == "__main__":
    unittest.main()
