"""Заранее собранная среда выполнения (gotovaya_sreda.py, сам/готовая_среда.dan).

Программа, собранная из своей части и готовых частей среды, ведёт себя так же, как
собранная целиком; готовые части берутся из кэша; сборки, для которых готовая среда
не годится, собираются как раньше.
"""

import glob
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import dan
import gotovaya_sreda
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def _перевести_образец(исходник):
    return dan.перевести(исходник, "образец.dan")


ПРОГРАММЫ = {
    # данные: сжатие, контрольные суммы, байты, ошибка выполнения внутри библиотеки
    "данные": 'пусть с = сжать("Повтор. Повтор. Повтор.")\nвывод(распаковать(с))\nвывод(контрольная_сумма("123456789"))\n'
              'вывод(хеш("abc"))\nвывод(байты("да"))\n',
    "ошибка_в_библиотеке": 'вывод(распаковать(байты([1, 2, 3])))\n',
    "математика": "вывод(синус(пи / 6))\nвывод(корни(многочлен([1, -3, 2])))\nвывод(прописью(1234))\n",
    "система": "вывод(сегодня() >= разобрать_дату(\"01.01.2020\")!)\nвывод(совпадает(\"абв\", \"а.в\"))\n",
    "несколько": 'вывод(хеш("а").длина)\nвывод(корень(2.0))\nвывод(сегодня() > разобрать_дату("01.01.2000")!)\n',
}


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыГотовойСреды(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.mkdtemp()
        self.кэш = os.path.join(self.папка, "кэш")
        self.среда = mock.patch.dict(os.environ, {"DAN_CACHE": self.кэш})
        self.среда.start()
        os.environ.pop("DAN_NO_CACHE", None)
        os.environ.pop("DAN_GOTOVAYA_SREDA", None)

    def tearDown(self):
        self.среда.stop()
        shutil.rmtree(self.папка, ignore_errors=True)

    def собрать_и_выполнить(self, имя, текст, готовая):
        путь = os.path.join(self.папка, имя + ("_г" if готовая else "_ц") + ".dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        with mock.patch.dict(os.environ, {"DAN_GOTOVAYA_SREDA": "1" if готовая else "0",
                                          "DAN_CACHE": self.кэш if готовая else os.path.join(self.папка, "без")}):
            исполняемый = dan.собрать(путь, текст)
        р = subprocess.run([исполняемый], capture_output=True, timeout=60, cwd=self.папка)
        ошибки = р.stderr.decode("utf-8", "replace").replace(os.path.basename(путь), "программа.dan")
        return р.returncode, р.stdout, ошибки

    def test_то_же_поведение(self):
        for имя, текст in ПРОГРАММЫ.items():
            with self.subTest(программа=имя):
                код_c = dan.перевести(текст, "программа.dan")
                разделение = gotovaya_sreda.разделить(код_c, _перевести_образец)
                self.assertIsNotNone(разделение)
                self.assertTrue(разделение[1])
                self.assertEqual(self.собрать_и_выполнить(имя, текст, True),
                                 self.собрать_и_выполнить(имя, текст, False))
        готовые = os.listdir(os.path.join(self.кэш, "среда"))
        self.assertTrue(готовые and all(и.endswith(".o") for и in готовые), готовые)

    def test_программы_каталога(self):
        # программы каталога, которые пользуются большими библиотеками среды
        for путь in sorted(glob.glob(os.path.join(ПАПКА, "каталог", "программы", "*.dan"))):
            with open(путь, encoding="utf-8") as ф:
                текст = ф.read()
            код_c = dan.перевести(текст, путь)
            разделение = gotovaya_sreda.разделить(код_c, _перевести_образец)
            if разделение is None or "ввод(" in текст:
                continue
            with self.subTest(программа=os.path.basename(путь)):
                копия = os.path.join(self.папка, os.path.basename(путь))
                shutil.copyfile(путь, копия)
                итоги = []
                for готовая in ("1", "0"):
                    with mock.patch.dict(os.environ, {"DAN_GOTOVAYA_SREDA": готовая}):
                        исполняемый = dan.собрать(копия, текст)
                    р = subprocess.run([исполняемый], capture_output=True, timeout=60, cwd=os.path.dirname(путь))
                    итоги.append((р.returncode, р.stdout))
                    os.remove(исполняемый)
                    for ключ in os.listdir(self.кэш):          # без готовых программ из кэша
                        if ключ != "среда":
                            os.remove(os.path.join(self.кэш, ключ))
                self.assertEqual(итоги[0], итоги[1])

    def test_не_для(self):
        # отладка, задачи, окна - без готовой среды
        for текст in ("параллельно { }\nвывод(хеш(\"а\"))\n",):
            код_c = dan.перевести(текст, "программа.dan")
            self.assertIsNone(gotovaya_sreda.разделить(код_c, _перевести_образец))
        код_c = dan.перевести('вывод(хеш("а"))\n', "программа.dan", отладка=True)
        self.assertIsNone(gotovaya_sreda.разделить(код_c, _перевести_образец))
        # программа без больших библиотек
        self.assertIsNone(gotovaya_sreda.разделить(dan.перевести("вывод(1)\n", "программа.dan"), _перевести_образец))

    def test_отключение(self):
        текст = 'вывод(хеш("а"))\n'
        путь = os.path.join(self.папка, "п.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        with mock.patch.dict(os.environ, {"DAN_GOTOVAYA_SREDA": "0"}):
            dan.собрать(путь, текст)
        self.assertFalse(os.path.isdir(os.path.join(self.кэш, "среда")))
        with mock.patch.dict(os.environ, {"DAN_NO_CACHE": "1"}):
            dan.собрать(путь, текст)
        self.assertFalse(os.path.isdir(os.path.join(self.кэш, "среда")))

    def test_код_си_не_меняется(self):
        # «dan си» печатает самодостаточный код: разделение - только при сборке
        код_c = dan.перевести('вывод(хеш("а"))\n', "программа.dan")
        self.assertIn(gotovaya_sreda._текст_группы("данные"), код_c)


if __name__ == "__main__":
    unittest.main()
