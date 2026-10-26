"""Тесты архивов в памяти (разобрать_архив, собрать_архив) и модуля XML
(библиотека/xml.dan).

Архивы сверяются с модулем zipfile языка Python в обе стороны. Документы XML
со случайной структурой создаются Python, разбираются и записываются обратно
программой на Дане, а результат снова читается Python и сравнивается с
исходным деревом.
"""

import io
import os
import random
import unittest
import xml.etree.ElementTree as ET
import zipfile

from test_json import выполнить_с_файлами
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ


def выполнить(текст, файлы=None, байты=None):
    import tempfile, subprocess, dan
    with tempfile.TemporaryDirectory() as п:
        for имя, содержимое in (файлы or {}).items():
            with open(os.path.join(п, имя), "w", encoding="utf-8", newline="") as ф:
                ф.write(содержимое)
        for имя, содержимое in (байты or {}).items():
            with open(os.path.join(п, имя), "wb") as ф:
                ф.write(содержимое)
        путь = os.path.join(п, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        р = subprocess.run([dan.собрать(путь, текст)], capture_output=True, text=True, encoding="utf-8",
                           timeout=120, cwd=п)
        результаты = {}
        for имя in os.listdir(п):
            if имя.startswith("итог"):
                with open(os.path.join(п, имя), "rb") as ф:
                    результаты[имя] = ф.read()
        return р.returncode, р.stdout, р.stderr, результаты


def случайное_дерево(сл, глубина=0):
    имя = сл.choice(["книга", "w_p", "w-r", "wt", "item", "Строка", "a.b-c_d"])
    эл = ET.Element(имя)
    for _ in range(сл.randint(0, 3)):
        эл.set(сл.choice(["год", "id", "xml:space", "w_val", "имя"]),
               "".join(сл.choice("абв xyz<>&\"'1\t\n") for _ in range(сл.randint(0, 8))))
    if сл.random() < 0.5:
        эл.text = "".join(сл.choice("Привет мир <>&\"' \n12") for _ in range(сл.randint(0, 12)))
    if глубина < 4:
        for _ in range(сл.randint(0, 3)):
            ребёнок = случайное_дерево(сл, глубина + 1)
            if сл.random() < 0.3:
                ребёнок.tail = "".join(сл.choice("хвост &<") for _ in range(сл.randint(0, 5)))
            эл.append(ребёнок)
    return эл


def как_кортеж(эл):
    return (эл.tag, sorted(эл.attrib.items()), эл.text or "", [как_кортеж(р) for р in эл],
            эл.tail or "")


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыАрхивовВПамяти(unittest.TestCase):

    def test_архив_дана_читает_python(self):
        текст = ('пусть а = собрать_архив([байты("привет.txt"), байты("Здравствуйте!"), '
                 'байты("папка/данные.bin"), байты([0, 1, 2, 255]), байты("пусто.txt"), байты("")])\n'
                 'записать("итог.zip", а)\n')
        код, вывод, ош, файлы = выполнить(текст)
        self.assertEqual(код, 0, ош)
        with zipfile.ZipFile(io.BytesIO(файлы["итог.zip"])) as з:
            self.assertIsNone(з.testzip())
            self.assertEqual(з.namelist(), ["привет.txt", "папка/данные.bin", "пусто.txt"])
            self.assertEqual(з.read("привет.txt").decode(), "Здравствуйте!")
            self.assertEqual(з.read("папка/данные.bin"), bytes([0, 1, 2, 255]))
            self.assertEqual(з.read("пусто.txt"), b"")

    def test_архив_python_читает_дан(self):
        данные = io.BytesIO()
        сл = random.Random(5)
        большой = bytes(сл.randrange(256) for _ in range(100000)) + "повтор".encode() * 20000
        with zipfile.ZipFile(данные, "w") as з:
            з.writestr("а.txt", "Текст файла", compress_type=zipfile.ZIP_DEFLATED)
            з.writestr("папка/", "")
            з.writestr("папка/б.bin", большой, compress_type=zipfile.ZIP_STORED)
            з.writestr("в.bin", большой, compress_type=zipfile.ZIP_DEFLATED)
        текст = ('пусть ф = разобрать_архив(прочитать_байты("вход.zip"))\n'
                 'вывод(ф.длина)\n'
                 'для к в 0..ф.длина / 2 - 1 { вывод(стр(ф[2 * к]) + " " + стр(ф[2 * к + 1].длина)) }\n'
                 'вывод(стр(ф[1]))\n'
                 'вывод(ф[3] == ф[5])\n')
        код, вывод, ош, _ = выполнить(текст, байты={"вход.zip": данные.getvalue()})
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), ["6", "а.txt 21", f"папка/б.bin {len(большой)}",
                                              f"в.bin {len(большой)}", "Текст файла", "истина"])

    def test_неверные_пары_и_повреждённый_архив(self):
        код, _, ош, _ = выполнить('пусть а = собрать_архив([байты("имя")])\n')
        self.assertEqual(код, 1)
        self.assertIn("список пар", ош)
        код, _, ош, _ = выполнить('пусть ф = разобрать_архив(байты("не архив вовсе, просто текст"))\n')
        self.assertEqual(код, 1)
        self.assertIn("не является zip-архивом", ош)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыXML(unittest.TestCase):

    def test_случайные_документы_туда_и_обратно(self):
        сл = random.Random(2026)
        файлы, деревья = {}, []
        for н in range(40):
            текст = ET.tostring(случайное_дерево(сл), encoding="unicode")
            деревья.append(ET.fromstring(текст))       # дерево так, как его прочтёт Python
            if н % 3 == 0:
                текст = '<?xml version="1.0" encoding="UTF-8"?>\n<!-- комментарий -->\n' + текст + "\n"
            файлы[f"д{н}.xml"] = текст
        программа = ('подключить "xml"\n'
                     'для н в 0..39 {\n'
                     '    пусть д = xml.разобрать(прочитать("д" + стр(н) + ".xml")) иначе xml.текст("ошибка")\n'
                     '    записать("итог" + стр(н) + ".xml", xml.записать(д))\n'
                     '}\n')
        код, _, ош, итог = выполнить(программа, файлы)
        self.assertEqual(код, 0, ош)
        for н, дерево in enumerate(деревья):
            with self.subTest(н=н):
                прочитано = ET.fromstring(итог[f"итог{н}.xml"].decode())
                self.assertEqual(как_кортеж(прочитано), как_кортеж(дерево))

    def test_доступ_к_узлам(self):
        программа = r'''подключить "xml"
пусть д = xml.разобрать("<книга год=\"2026\"><название>Дан &amp; Си</название><автор>А</автор><глава><автор>Б</автор></глава><пусто/></книга>") иначе xml.текст("")
вывод(д.атрибут("год") иначе "нет")
вывод(д.атрибут("месяц") иначе "нет")
вывод(д.значение("название"))
вывод(д.значение("нет такого") == "")
вывод(д.все("автор").длина)
вывод(д.найти_все("автор").длина)
вывод(д.найти_все("*").длина)
вывод(д.весь_текст())
пусть н = xml.элемент("а", {"x": "1 < 2"}, [xml.элемент_с_текстом("б", "в & г"), xml.текст("д")])
вывод(xml.записать(н))
вывод(xml.записать_документ(xml.элемент_с_текстом("к", "")))
'''
        код, вывод, ош, _ = выполнить(программа)
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), [
            "2026", "нет", "Дан & Си", "истина", "1", "2", "5", "Дан & СиАБ",
            '<а x="1 &lt; 2"><б>в &amp; г</б>д</а>',
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>', "<к></к>"])

    def test_ошибки_с_местом(self):
        случаи = {
            "<а>\n  <б></в></а>": "XML, строка 2, позиция 6: закрывающий тег </в> не соответствует открытому <б>",
            "<а>&nbsp;</а>": "XML, строка 1, позиция 4: неизвестная или незакрытая ссылка",
            "<а/><б/>": "второй корневой элемент",
            "<а>": "элемент <а> не закрыт",
            "": "в тексте нет ни одного элемента",
            "текст<а/>": "текст вне корневого элемента",
            "<а б=1/>": "неверная запись атрибута",
            "<а б=\"<\"/>": "знак «<» внутри значения атрибута",
        }
        файлы = {f"о{н}.xml": т for н, т in enumerate(случаи)}
        программа = ('подключить "xml"\n'
                     f'для н в 0..{len(случаи) - 1} {{\n'
                     '    если ошибка о = xml.разобрать(прочитать("о" + стр(н) + ".xml")) { вывод(о) } иначе { вывод("нет ошибки") }\n'
                     '}\n')
        код, вывод, ош, _ = выполнить(программа, файлы)
        self.assertEqual(код, 0, ош)
        for строка, ожидается in zip(вывод.splitlines(), случаи.values()):
            self.assertIn(ожидается, строка)

    def test_большой_документ_быстро(self):
        текст = ("<w:document><w:body>" + "".join(
            f'<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">Абзац {н} с текстом</w:t></w:r></w:p>'
            for н in range(30000)) + "</w:body></w:document>")
        программа = ('подключить "xml"\n'
                     'пусть т = прочитать("д.xml")\n'
                     'пусть д = xml.разобрать(т) иначе xml.текст("")\n'
                     'вывод(д.найти_все("w:t").длина)\n'
                     'вывод(xml.записать(д).длина == т.длина)\n')
        код, вывод, ош, длительность = выполнить_с_файлами(программа, {"д.xml": текст})
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), ["30000", "истина"])
        self.assertLess(длительность, 10)


if __name__ == "__main__":
    unittest.main()
