"""Документация из комментариев «///»: извлечение, разметка, страницы, проверка примеров."""

import os
import subprocess
import sys
import tempfile
import unittest

import dan
import dokumentaciya as д
from server import описание_из_комментария

ПАПКА_ДАНА = os.path.dirname(os.path.abspath(__file__))
КОМПИЛЯТОР_ЕСТЬ = dan.компилятор_есть()

ГЕОМЕТРИЯ = '''/// Геометрия на плоскости.
///
/// Главное - [расстояние] и тип [Точка].

/// Точка на плоскости.
общий тип Точка {
    /// абсцисса
    х: дроб
    у: дроб
}

/// Расстояние между точками.
///
/// # Примеры
///
/// ```
/// пусть а = Точка(х = 0.0, у = 0.0)
/// утвердить_равно(расстояние(а, Точка(х = 3.0, у = 4.0)), 5.0)
/// ```
общий функ расстояние(а: Точка, б: Точка) -> дроб {
    вернуть корень((а.х - б.х)^2 + (а.у - б.у)^2)
}

общий функ без_описания() -> цел { вернуть 1 }

// обычный комментарий не описание
функ скрытая() -> цел { вернуть 2 }

/// Фигура
общий тип Фигура {
    /// круг с радиусом
    Круг(радиус: дроб)
    Точечная
}

/// Площадное
общий интерфейс Площадное {
    /// площадь фигуры
    функ площадь() -> дроб
}

тест "свой тест файла" { утвердить(истина) }
'''


class ТестыИзвлечения(unittest.TestCase):

    def setUp(self):
        self.м = д.извлечь(ГЕОМЕТРИЯ, "папка/геометрия.dan")
        self.по_имени = {о.имя: о for о in self.м.описания}

    def test_описание_модуля(self):
        self.assertEqual(self.м.имя, "геометрия")
        self.assertTrue(self.м.текст.startswith("Геометрия на плоскости."))
        # первое объявление после пустой строки своего описания не получает
        self.assertEqual(self.по_имени["Точка"].текст, "Точка на плоскости.")

    def test_объявления_и_части(self):
        о = self.по_имени["расстояние"]
        self.assertEqual(о.подпись, "функ расстояние(а: Точка, б: Точка) -> дроб")
        self.assertEqual(о.кратко, "Расстояние между точками.")
        self.assertTrue(о.общий)
        поля = {ч.имя: ч for ч in self.по_имени["Точка"].части}
        self.assertEqual(поля["Точка.х"].текст, "абсцисса")
        self.assertEqual(поля["Точка.у"].текст, "")
        варианты = {ч.имя: ч.текст for ч in self.по_имени["Фигура"].части}
        self.assertEqual(варианты, {"Фигура.Круг": "круг с радиусом", "Фигура.Точечная": ""})
        методы = self.по_имени["Площадное"].части
        self.assertEqual((методы[0].подпись, методы[0].текст), ("функ площадь() -> дроб", "площадь фигуры"))

    def test_обычный_комментарий_и_тест_не_попадают(self):
        self.assertEqual(self.по_имени["скрытая"].текст, "")
        показ = [о.имя for о in д.показываемые(self.м)]
        self.assertNotIn("скрытая", показ)
        self.assertIn("без_описания", показ)          # общий - показывается и без описания
        self.assertFalse(any(о.имя.startswith("тест") for о in self.м.описания))

    def test_без_описания(self):
        self.assertEqual(д.без_описания(self.м), ["строка 24: функ без_описания() -> цел"])

    def test_примеры(self):
        код = д.примеры(self.по_имени["расстояние"].текст)
        self.assertEqual(len(код), 1)
        self.assertIn("утвердить_равно", код[0][1])
        self.assertEqual(д.примеры("```текст\nне код\n```\n```dan\nвывод(1)\n```"), [(3, "вывод(1)")])

    def test_соответствие_строк_примера(self):
        текст, соответствие = д.текст_проверки(ГЕОМЕТРИЯ, self.м)
        строки = текст.split("\n")
        for н, исходная in соответствие.items():
            self.assertEqual(строки[н - 1].strip(), ГЕОМЕТРИЯ.split("\n")[исходная - 1].lstrip("/ ").strip())
        self.assertEqual(д.переписать_строки("ПРОВАЛЕН: x (строка 99)", {99: 18}), "ПРОВАЛЕН: x (строка 18)")

    def test_подсказка_редактора(self):
        self.assertEqual(описание_из_комментария(ГЕОМЕТРИЯ, 20), "Расстояние между точками.\n\n# Примеры\n\n```\n"
                         "пусть а = Точка(х = 0.0, у = 0.0)\n"
                         "утвердить_равно(расстояние(а, Точка(х = 3.0, у = 4.0)), 5.0)\n```")
        self.assertEqual(описание_из_комментария(ГЕОМЕТРИЯ, 27), "")


class ТестыРазметки(unittest.TestCase):

    def test_строчная_разметка(self):
        html = д.в_html("Есть `код <b>`, **жирный**, _курсив_, [сайт](https://example.org) и [ф].",
                        lambda имя: "#ф" if имя == "ф" else None)
        self.assertIn("<code>код &lt;b&gt;</code>", html)
        self.assertIn("<b>жирный</b>", html)
        self.assertIn("<i>курсив</i>", html)
        self.assertIn('<a href="https://example.org">сайт</a>', html)
        self.assertIn('<a href="#ф"><code>ф</code></a>', html)

    def test_неизвестная_ссылка_и_опасный_адрес_остаются_текстом(self):
        html = д.в_html("[нет] и [плохо](javascript:alert(1))")
        self.assertNotIn("<a", html)
        self.assertNotIn("<script", д.в_html("<script>alert(1)</script>"))

    def test_блоки(self):
        html = д.в_html("Абзац\nпродолжение\n\n# Раздел\n\n* один\n* два\n\n    код\n    ещё\n\nконец")
        self.assertIn("<p>Абзац продолжение</p>", html)
        self.assertIn("<h4>Раздел</h4>", html)
        self.assertIn("<ul><li>один</li><li>два</li></ul>", html)
        self.assertIn("<pre><code>код\nещё</code></pre>", html)
        self.assertTrue(html.endswith("<p>конец</p>"))


class ТестыКоманды(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.п = self.tmp.name
        with open(os.path.join(self.п, "геометрия.dan"), "w", encoding="utf-8") as ф:
            ф.write(ГЕОМЕТРИЯ)

    def tearDown(self):
        self.tmp.cleanup()

    def dan(self, *аргументы):
        return subprocess.run([sys.executable, os.path.join(ПАПКА_ДАНА, "dan.py"), *аргументы],
                              capture_output=True, text=True, encoding="utf-8", cwd=self.п, timeout=300)

    def test_страницы(self):
        р = self.dan("документация", "--в=док")
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertEqual(sorted(os.listdir(os.path.join(self.п, "док"))), ["index.html", "геометрия.html", "стиль.css"])
        with open(os.path.join(self.п, "док", "геометрия.html"), encoding="utf-8") as ф:
            страница = ф.read()
        self.assertIn('id="расстояние"', страница)
        self.assertIn('<a href="#Точка"><code>Точка</code></a>', страница)
        self.assertNotIn("скрытая", страница)
        with open(os.path.join(self.п, "док", "index.html"), encoding="utf-8") as ф:
            оглавление = ф.read()
        self.assertIn('"адрес": "геометрия.html#расстояние"', оглавление)

    def test_ошибка_в_файле(self):
        with open(os.path.join(self.п, "плохой.dan"), "w", encoding="utf-8") as ф:
            ф.write("функ (")
        р = self.dan("документация", "плохой.dan")
        self.assertEqual(р.returncode, 1)
        self.assertIn("плохой.dan: Строка 1", р.stdout)

    @unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
    def test_проверка_примеров(self):
        р = self.dan("документация", "--проверить", "геометрия.dan")
        self.assertEqual(р.returncode, 0, р.stdout + р.stderr)
        self.assertIn("нет описания: строка 24: функ без_описания() -> цел", р.stdout)
        self.assertIn("пройден: пример к расстояние", р.stdout)
        self.assertIn("пройден: свой тест файла", р.stdout)
        self.assertEqual(sorted(os.listdir(self.п)), ["геометрия.dan"])     # временные файлы удалены

    @unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
    def test_неверный_пример_с_номером_строки_файла(self):
        with open(os.path.join(self.п, "геометрия.dan"), "w", encoding="utf-8") as ф:
            ф.write(ГЕОМЕТРИЯ.replace("4.0)), 5.0)", "4.0)), 6.0)"))
        р = self.dan("документация", "--проверить", "геометрия.dan")
        self.assertEqual(р.returncode, 1)
        self.assertIn("ПРОВАЛЕН: пример к расстояние (строка 18): ожидалось 6.0, получено 5.0", р.stdout)
        with open(os.path.join(self.п, "геометрия.dan"), "w", encoding="utf-8") as ф:
            ф.write(ГЕОМЕТРИЯ.replace("утвердить_равно(расстояние(", "утвердить_равно(расстоян("))
        р = self.dan("документация", "--проверить", "геометрия.dan")
        self.assertEqual(р.returncode, 1)
        self.assertIn("пример не собирается: Строка 18", р.stdout)
        self.assertEqual(sorted(os.listdir(self.п)), ["геометрия.dan"])


if __name__ == "__main__":
    unittest.main()
