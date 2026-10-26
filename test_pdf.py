"""Тесты модуля PDF (библиотека/pdf.dan, sreda/pdf.c).

Созданные Даном документы проверяются qpdf, pdftotext, pdffonts и pypdf;
документы reportlab, LibreOffice и qpdf (потоки объектов, перекрёстные ссылки
в виде потока, повреждённые таблицы ссылок, шифрование) читаются Даном. Без
этих программ соответствующие проверки пропускаются.
"""

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

from test_kodogen import КОМПИЛЯТОР_ЕСТЬ
from test_word import выполнить

ЕСТЬ_QPDF = shutil.which("qpdf") is not None
ЕСТЬ_POPPLER = shutil.which("pdftotext") is not None and shutil.which("pdffonts") is not None
ЕСТЬ_REPORTLAB = importlib.util.find_spec("reportlab") is not None
ЕСТЬ_OFFICE = shutil.which("soffice") is not None
ШРИФТ = next((п for п in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
                          "C:/Windows/Fonts/arial.ttf") if os.path.exists(п)), None)

СОЗДАНИЕ = r'''подключить "pdf"
перем блоки = [pdf.заголовок("Справка о доходах", 1), pdf.по_центру("г. Москва, 26.10.2026"),
    pdf.оформленный(pdf.Абзац("Настоящая справка выдана Иванову Ивану Ивановичу в том, что он действительно работает в организации с 1 марта 2020 года по настоящее время.", выравнивание = "по_ширине")),
    pdf.таблица([["Месяц", "Сумма, руб."], ["Сентябрь", "65 000"], ["Октябрь", "70 000"]]),
    pdf.жирный("Итого: 135 000 руб. & <знаки> «ёлки» №5"),
    pdf.разрыв_страницы(),
    pdf.абзац("Вторая страница.\nНовая строка абзаца.")]
если ошибка о = pdf.создать_с_оформлением("справка.pdf", блоки, pdf.Оформление(размер = 13.0, отступ_первой_строки = 1.25)) {
    вывод(о)
}
'''


def прочитать_текст(путь):
    return subprocess.run(["pdftotext", "-layout", путь, "-"], capture_output=True, text=True).stdout


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыPDF(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def файл(self, имя):
        return os.path.join(self.п, имя)

    def прочитать_даном(self, имя):
        код, вывод, ош = выполнить(self.п, '''подключить "pdf"
если пусть с = pdf.страницы("''' + имя + '''") {
    для н в 0..с.длина - 1 { вывод("=== " + стр(н + 1) + "\\n" + с[н]) }
} иначе ошибка о {
    вывод("ошибка: " + о)
}
''')
        self.assertEqual(код, 0, ош)
        return вывод

    def создать(self):
        код, вывод, ош = выполнить(self.п, СОЗДАНИЕ)
        self.assertEqual(код, 0, ош + вывод)
        if "не найден шрифт" in вывод:
            self.skipTest("в системе нет шрифта TrueType с русскими буквами")
        self.assertEqual(вывод, "")

    def test_созданный_документ_читается_даном(self):
        self.создать()
        текст = self.прочитать_даном("справка.pdf")
        self.assertIn("=== 1\nСправка о доходах\nг. Москва, 26.10.2026\n", текст)
        self.assertIn("Месяц Сумма, руб.", текст)
        self.assertIn("Итого: 135 000 руб. & <знаки> «ёлки» №5", текст)
        self.assertIn("=== 2\nВторая страница.\nНовая строка абзаца.", текст)

    @unittest.skipUnless(ЕСТЬ_QPDF and ЕСТЬ_POPPLER, "нет qpdf и poppler-utils")
    def test_созданный_документ_верен(self):
        self.создать()
        р = subprocess.run(["qpdf", "--check", self.файл("справка.pdf")], capture_output=True, text=True)
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertIn("No syntax or stream encoding errors", р.stdout)
        текст = прочитать_текст(self.файл("справка.pdf"))
        for часть in ("Справка о доходах", "Сентябрь", "70 000", "«ёлки» №5", "Вторая страница."):
            self.assertIn(часть, текст)
        шрифты = subprocess.run(["pdffonts", self.файл("справка.pdf")], capture_output=True, text=True).stdout
        строки = [с for с in шрифты.splitlines()[2:] if с.strip()]
        self.assertGreaterEqual(len(строки), 1)
        for с in строки:
            self.assertIn("CID TrueType", с)
            self.assertRegex(с, r"yes\s+yes\s+yes")            # встроен, подмножество, ToUnicode
        # встраиваются только нужные знаки: документ небольшой
        self.assertLess(os.path.getsize(self.файл("справка.pdf")), 120000)

    @unittest.skipUnless(ЕСТЬ_REPORTLAB and ШРИФТ, "нет reportlab или шрифта")
    def test_чтение_документа_reportlab(self):
        from reportlab.pdfbase import pdfmetrics
        from reportlab.pdfbase.ttfonts import TTFont
        from reportlab.pdfgen import canvas
        pdfmetrics.registerFont(TTFont("Шрифт", ШРИФТ))
        к = canvas.Canvas(self.файл("rl.pdf"))
        к.setFont("Шрифт", 14)
        к.drawString(72, 750, "Привет, мир! Договор № 17")
        к.drawString(72, 730, "Вторая строка")
        к.drawString(300, 730, "справа")
        к.setFont("Helvetica", 12)
        к.drawString(72, 700, "Latin text (WinAnsi) - dash")
        к.showPage()
        к.setFont("Шрифт", 12)
        к.drawString(72, 750, "Страница два")
        к.save()
        self.assertEqual(self.прочитать_даном("rl.pdf").splitlines(), [
            "=== 1", "Привет, мир! Договор № 17", "Вторая строка справа", "Latin text (WinAnsi) - dash",
            "=== 2", "Страница два"])

    @unittest.skipUnless(ЕСТЬ_QPDF, "нет qpdf")
    def test_потоки_объектов_повреждения_и_шифрование(self):
        self.создать()
        исходный = self.файл("справка.pdf")
        subprocess.run(["qpdf", "--object-streams=generate", исходный, self.файл("потоки.pdf")], check=True)
        subprocess.run(["qpdf", "--qdf", "--object-streams=disable", исходный, self.файл("qdf.pdf")], check=True)
        subprocess.run(["qpdf", "--encrypt", "", "владелец", "256", "--", исходный, self.файл("шифр.pdf")], check=True)
        with open(исходный, "rb") as ф:
            данные = ф.read()
        # повреждённая таблица ссылок: объекты находятся просмотром файла
        with open(self.файл("повреждён.pdf"), "wb") as ф:
            ф.write(данные.replace(b"startxref\n", b"startxref\n9").replace(b"xref\n0 ", b"xref\n7 "))
        with open(self.файл("обрезан.pdf"), "wb") as ф:
            ф.write(данные[:данные.rindex(b"xref")])
        образец = self.прочитать_даном("справка.pdf")
        for имя in ("потоки.pdf", "qdf.pdf", "повреждён.pdf", "обрезан.pdf"):
            with self.subTest(имя=имя):
                self.assertEqual(self.прочитать_даном(имя), образец)
        self.assertIn("ошибка: документ PDF зашифрован", self.прочитать_даном("шифр.pdf"))

    @unittest.skipUnless(ЕСТЬ_OFFICE and ЕСТЬ_POPPLER, "нет LibreOffice")
    def test_чтение_документа_офиса(self):
        код, вывод, ош = выполнить(self.п, '''подключить "word"
если ошибка о = word.создать("документ.docx", [word.заголовок("Заголовок документа", 1),
    word.абзац("Первый абзац с русским текстом."), word.таблица([["А", "Б"], ["1", "2"]])]) { вывод(о) }
''')
        self.assertEqual(код, 0, ош)
        р = subprocess.run(["soffice", "--headless", "--convert-to", "pdf", "--outdir", self.п,
                            self.файл("документ.docx")], capture_output=True, text=True, timeout=180)
        self.assertEqual(р.returncode, 0, р.stderr)
        строки = self.прочитать_даном("документ.pdf").splitlines()
        self.assertEqual(строки[:3], ["=== 1", "Заголовок документа", "Первый абзац с русским текстом."])
        self.assertIn("А Б", строки)

    def test_повреждённые_документы_не_роняют_программу(self):
        import random
        self.создать()
        with open(self.файл("справка.pdf"), "rb") as ф:
            данные = ф.read()
        сл = random.Random(2026)
        for н in range(120):
            б = bytearray(данные)
            for _ in range(сл.randint(1, 25)):
                м = сл.randrange(len(б))
                вид = сл.randrange(3)
                if вид == 0:
                    б[м] = сл.randrange(256)
                elif вид == 1:
                    del б[м:м + сл.randint(1, 100)]
                else:
                    б[м:м] = bytes(сл.choice(b"0123456789 <>[]()/R\nobjstream") for _ in range(сл.randint(1, 20)))
            with open(self.файл(f"п{н}.pdf"), "wb") as ф:
                ф.write(bytes(б))
        код, вывод, ош = выполнить(self.п, """перем прочитано = 0
для н в 0..119 {
    если пусть с = текст_pdf(прочитать_байты("п" + стр(н) + ".pdf")) { прочитано += 1 }
}
вывод(прочитано > 0)
""")
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод, "истина\n")

    def test_ошибки(self):
        with open(self.файл("текст.pdf"), "w", encoding="utf-8") as ф:
            ф.write("это не pdf")
        код, вывод, ош = выполнить(self.п, '''подключить "pdf"
если ошибка о = pdf.страницы("нет.pdf") { вывод(о) }
если ошибка о = pdf.страницы("текст.pdf") { вывод(о) }
если ошибка о = pdf.создать_с_оформлением("д.pdf", [pdf.абзац("а")], pdf.Оформление(шрифт = "/нет/шрифта.ttf")) { вывод(о) }
если ошибка о = pdf.создать_с_оформлением("д.pdf", [pdf.абзац("а")], pdf.Оформление(шрифт = "текст.pdf")) { вывод(о) }
''')
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), [
            "файл «нет.pdf» не найден", "это не документ PDF", "шрифт «/нет/шрифта.ttf» не найден",
            "шрифт «текст.pdf» - не шрифт TrueType (.ttf)"])


if __name__ == "__main__":
    unittest.main()
