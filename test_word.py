"""Тесты модуля Word (библиотека/word.dan).

Созданные Даном документы читаются библиотекой python-docx и открываются
LibreOffice (если они установлены); документы, созданные python-docx, pandoc
и LibreOffice, читаются и заполняются Даном. Без этих программ проверяется
только то, что можно проверить средствами Дана.
"""

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

import dan
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ЕСТЬ_DOCX = importlib.util.find_spec("docx") is not None
ЕСТЬ_OFFICE = shutil.which("soffice") is not None
ЕСТЬ_PANDOC = shutil.which("pandoc") is not None


def выполнить(папка, текст):
    путь = os.path.join(папка, "программа.dan")
    with open(путь, "w", encoding="utf-8") as ф:
        ф.write(текст)
    р = subprocess.run([dan.собрать(путь, текст)], capture_output=True, text=True, encoding="utf-8",
                       timeout=120, cwd=папка)
    return р.returncode, р.stdout, р.stderr


СОЗДАНИЕ = r'''подключить "word"
если ошибка о = word.создать_с_оформлением("документ.docx", [
    word.заголовок("Отчёт за октябрь", 1),
    word.заголовок("Раздел второго уровня", 2),
    word.абзац("Первый абзац & <знаки> \"в кавычках\"."),
    word.оформленный(word.Абзац("Строка один\nСтрока два\tс табуляцией", жирный = истина, курсив = истина,
                                 размер = 16, выравнивание = "по_центру")),
    word.таблица([["Товар", "Цена"], ["Книга «Дан»", "350"], ["Тетрадь"]]),
    word.разрыв_страницы(),
    word.по_центру("Последняя страница")
], word.Оформление(шрифт = "Arial", размер = 14, интервал = 1.5, отступ_первой_строки = 1.25)) {
    вывод(о)
}
'''


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыWord(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def файл(self, имя):
        return os.path.join(self.п, имя)

    def test_созданный_документ_читается_даном(self):
        код, вывод, ош = выполнить(self.п, СОЗДАНИЕ + '''если пусть д = word.открыть("документ.docx") {
    для а в д.абзацы() { вывод("[" + а + "]") }
    вывод(д.таблицы())
} иначе ошибка о {
    вывод(о)
}
''')
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), [
            "[Отчёт за октябрь]", "[Раздел второго уровня]", '[Первый абзац & <знаки> "в кавычках".]',
            "[Строка один]", "[Строка два\tс табуляцией]", "[]", "[Последняя страница]",
            '[[["Товар", "Цена"], ["Книга «Дан»", "350"], ["Тетрадь", ""]]]'])

    @unittest.skipUnless(ЕСТЬ_DOCX, "нет python-docx (pip install python-docx)")
    def test_созданный_документ_читается_python_docx(self):
        import docx
        код, _, ош = выполнить(self.п, СОЗДАНИЕ)
        self.assertEqual(код, 0, ош)
        д = docx.Document(self.файл("документ.docx"))
        абзацы = д.paragraphs
        self.assertEqual(абзацы[0].text, "Отчёт за октябрь")
        self.assertEqual(абзацы[0].style.name, "Heading 1")
        self.assertEqual(абзацы[1].style.name, "Heading 2")
        self.assertEqual(абзацы[2].text, 'Первый абзац & <знаки> "в кавычках".')
        прогон = абзацы[3].runs[0]
        self.assertTrue(прогон.bold)
        self.assertTrue(прогон.italic)
        self.assertEqual(прогон.font.size.pt, 16)
        self.assertEqual(абзацы[4].text, "Строка два\tс табуляцией")
        self.assertEqual([[я.text for я in с.cells] for с in д.tables[0].rows],
                         [["Товар", "Цена"], ["Книга «Дан»", "350"], ["Тетрадь", ""]])
        self.assertTrue(д.tables[0].rows[0].cells[0].paragraphs[0].runs[0].bold)
        стиль = д.styles["Normal"]
        self.assertIsNone(стиль.font.name)          # шрифт задан для всего документа (docDefaults)
        self.assertEqual(round(д.sections[0].left_margin.mm), 30)
        self.assertEqual(round(д.sections[0].page_width.mm), 210)

    @unittest.skipUnless(ЕСТЬ_OFFICE, "нет LibreOffice")
    def test_созданный_документ_открывается_офисом(self):
        код, _, ош = выполнить(self.п, СОЗДАНИЕ)
        self.assertEqual(код, 0, ош)
        р = subprocess.run(["soffice", "--headless", "--convert-to", "txt:Text", "--outdir", self.п,
                            self.файл("документ.docx")], capture_output=True, text=True, timeout=180)
        self.assertEqual(р.returncode, 0, р.stderr)
        with open(self.файл("документ.txt"), encoding="utf-8-sig") as ф:
            текст = ф.read()
        for часть in ("Отчёт за октябрь", "Книга «Дан»", "Последняя страница", "Строка два\tс табуляцией"):
            self.assertIn(часть, текст)

    @unittest.skipUnless(ЕСТЬ_DOCX, "нет python-docx (pip install python-docx)")
    def test_шаблон_с_разорванными_полями(self):
        import docx
        from docx.shared import Pt
        д = docx.Document()
        абзац = д.add_paragraph("Уважаемый ")
        # Word часто разрывает поле на части с разным оформлением
        абзац.add_run("{{").bold = True
        абзац.add_run("им").italic = True
        абзац.add_run("я}}, ваш заказ ")
        абзац.add_run("{{номер}}").font.size = Pt(16)
        абзац.add_run(" готов. Поле {{неизвестное}} остаётся.")
        д.add_paragraph("Без полей {не поле} и {{ имя }} с пробелами.")
        таблица = д.add_table(rows=1, cols=2)
        таблица.cell(0, 0).text = "Сумма: {{сумма}}"
        таблица.cell(0, 1).text = "{{пусто}}"
        д.sections[0].header.paragraphs[0].text = "Колонтитул: {{номер}}"
        д.sections[0].footer.paragraphs[0].text = "Подвал {{дата}}"
        д.save(self.файл("шаблон.docx"))
        код, вывод, ош = выполнить(self.п, '''подключить "word"
пусть пропущено = word.заполнить("шаблон.docx", "готово.docx", {"имя": "Иван & Ко", "номер": "17",
    "сумма": "1 000 руб.", "пусто": "", "дата": "26.10.2026"}) иначе ["ошибка"]
вывод(пропущено)
если пусть д = word.открыть("готово.docx") { вывод(д.текст()) }
''')
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines()[0], '["неизвестное"]')
        г = docx.Document(self.файл("готово.docx"))
        self.assertEqual(г.paragraphs[0].text, "Уважаемый Иван & Ко, ваш заказ 17 готов. Поле {{неизвестное}} остаётся.")
        self.assertTrue(г.paragraphs[0].runs[1].bold)                 # значение - с оформлением начала поля
        self.assertEqual(г.paragraphs[0].runs[4].font.size.pt, 16)
        self.assertEqual(г.paragraphs[1].text, "Без полей {не поле} и Иван & Ко с пробелами.")
        self.assertEqual(г.tables[0].cell(0, 0).text, "Сумма: 1 000 руб.")
        self.assertEqual(г.tables[0].cell(0, 1).text, "")
        self.assertEqual(г.sections[0].header.paragraphs[0].text, "Колонтитул: 17")
        self.assertEqual(г.sections[0].footer.paragraphs[0].text, "Подвал 26.10.2026")

    @unittest.skipUnless(ЕСТЬ_PANDOC, "нет pandoc")
    def test_чтение_документа_другой_программы(self):
        with open(self.файл("исходный.md"), "w", encoding="utf-8") as ф:
            ф.write("# Заголовок\n\nАбзац с **жирным** и *курсивом* текстом.\n\n"
                    "| А | Б |\n|---|---|\n| 1 | 2 |\n\n- пункт списка\n- второй пункт\n")
        subprocess.run(["pandoc", self.файл("исходный.md"), "-o", self.файл("из_pandoc.docx")], check=True)
        код, вывод, ош = выполнить(self.п, '''подключить "word"
если пусть д = word.открыть("из_pandoc.docx") {
    для а в д.абзацы() { вывод(а) }
    вывод(д.таблицы())
}
''')
        self.assertEqual(код, 0, ош)
        строки = вывод.splitlines()
        self.assertIn("Заголовок", строки)
        self.assertIn("Абзац с жирным и курсивом текстом.", строки)
        self.assertIn("пункт списка", строки)
        self.assertIn('[[["А", "Б"], ["1", "2"]]]', строки)

    def test_ошибки(self):
        with open(self.файл("текст.docx"), "w", encoding="utf-8") as ф:
            ф.write("это не документ")
        код, вывод, ош = выполнить(self.п, '''подключить "word"
если ошибка о = word.открыть("нет.docx") { вывод(о) }
если ошибка о = word.открыть("текст.docx") { вывод(о) }
если ошибка о = word.заполнить("нет.docx", "итог.docx", {"а": "б"}) { вывод(о) }
''')
        self.assertEqual(код, 0, ош)
        строки = вывод.splitlines()
        self.assertIn("не найден", строки[0])
        self.assertIn("это не документ Word в формате .docx", строки[1])
        self.assertIn("не найден", строки[2])

    def test_повреждённый_архив_даёт_ошибку(self):
        # файл начинается как архив, но внутри повреждён: ошибка значением, программа продолжает работу
        код, вывод, ош = выполнить(self.п, '''подключить "word"
подключить "excel"
пусть д = word.документ([word.абзац("Текст документа для проверки")], word.Оформление())
перем б: [цел] = []
для н в 0..д.длина - 1 { б.добавить(д[н]) }
для н в 40..60 { б[н] = 0 }
пусть испорчен = байты(б)
если ошибка о = word.из_байтов(испорчен) { вывод("word: " + о) }
если ошибка о = excel.из_байтов(испорчен) { вывод("excel: " + о) }
если ошибка о = файлы_архива(байты("PK" + "x".повторить(30))) { вывод("архив: " + о) }
вывод("продолжение")
''')
        self.assertEqual(код, 0, ош)
        строки = вывод.splitlines()
        self.assertEqual([с.split(":")[0] for с in строки], ["word", "excel", "архив", "продолжение"], вывод)


if __name__ == "__main__":
    unittest.main()
