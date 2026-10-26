"""Тесты сборки в WebAssembly (--веб) и для других систем (--для=...).

Нужен компилятор zig (pip install ziglang); программы WebAssembly
выполняет wasmtime (pip install wasmtime), страницу для браузера - Chromium
через Playwright (если они есть). Без них тесты пропускаются.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import dan
import test_katalog as каталог

ЕСТЬ_ZIG = dan.найти_zig() is not None
ЕСТЬ_WASMTIME = importlib.util.find_spec("wasmtime") is not None or shutil.which("wasmtime") is not None
ПАПКА = os.path.dirname(os.path.abspath(__file__))


def собрать_и_выполнить(текст, ввод_данных=""):
    with tempfile.TemporaryDirectory() as п:
        путь = os.path.join(п, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        путь_wasm, путь_html = dan.собрать_веб(путь, текст)
        р = subprocess.run([sys.executable, "-c", "import sys; sys.path.insert(0, sys.argv[2]); import dan; "
                                                  "sys.exit(dan.выполнить_wasm(sys.argv[1], []))", путь_wasm, ПАПКА],
                           input=ввод_данных, capture_output=True, text=True, encoding="utf-8", cwd=п, timeout=300)
        return р.returncode, р.stdout, р.stderr


@unittest.skipUnless(ЕСТЬ_ZIG and ЕСТЬ_WASMTIME, "нужны zig и wasmtime (pip install ziglang wasmtime)")
class ТестыWebAssembly(unittest.TestCase):

    def test_каталог(self):
        for путь in каталог.программы("программы"):
            основа = путь[:-4]
            текст = каталог.прочитать(путь)
            if "запустить" in текст or "внешняя" in текст:
                continue        # задачи и функции C в WebAssembly пока не поддерживаются
            ввод_данных = каталог.прочитать(основа + ".ввод") if os.path.exists(основа + ".ввод") else ""
            with self.subTest(путь=путь):
                код, вывод, ош = собрать_и_выполнить(текст, ввод_данных)
                self.assertEqual(код, 0, ош[-1000:])
                self.assertEqual(вывод.splitlines(), каталог.прочитать(основа + ".вывод").splitlines())

    def test_ошибки_выполнения(self):
        код, вывод, ош = собрать_и_выполнить("вывод(1)\nпусть с = [1, 2]\nвывод(с[5])")
        self.assertEqual(код, 1)
        self.assertEqual(вывод, "1\n")
        self.assertIn("Ошибка выполнения в строке 3: индекс 5 за пределами", ош)
        код, вывод, ош = собрать_и_выполнить("функ ф(н: цел) -> цел { вернуть ф(н + 1) + 1 }\nвывод(ф(0))")
        self.assertEqual(код, 1)
        self.assertIn("слишком глубокая рекурсия", ош)

    def test_стек_больше_допустимого(self):
        # Python в Windows не принимает стек потока 256 МБ: выполнение не должно падать.
        import threading
        from unittest import mock
        настоящий = threading.stack_size
        размеры = []

        def как_в_windows(размер=0):
            if размер > 128 * 1024 * 1024:
                raise ValueError(f"size not valid: {размер} bytes")
            размеры.append(размер)
            return настоящий(размер)
        with tempfile.TemporaryDirectory() as п:
            путь = os.path.join(п, "программа.dan")
            текст = "пусть х = 1\n"
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            путь_wasm, _ = dan.собрать_веб(путь, текст)
            with mock.patch.object(threading, "stack_size", side_effect=как_в_windows):
                self.assertEqual(dan.выполнить_wasm(путь_wasm, []), 0)
        self.assertIn(128 * 1024 * 1024, размеры)

    def test_чего_нет_в_webassembly(self):
        for текст, часть in [
                ("функ ф() -> цел { вернуть 1 }\nпусть з = запустить ф()\nвывод(з.дождаться())", "параллельные задачи"),
                ('внешняя "" {\n    функ дл(с: стр) -> цел = "strlen"\n}\nвывод(дл("а"))', "функции библиотек C"),
                ('подключить "окна"\nпусть о = окна.создать("о", 100, 100) иначе окна.Окно(0)', "только в программах для Windows")]:
            with self.subTest(часть=часть):
                with self.assertRaises(dan.ОшибкаСборки) as о:
                    собрать_и_выполнить(текст)
                self.assertIn(часть, str(о.exception))
        код, вывод, ош = собрать_и_выполнить('если ошибка о = вывод_команды("ls") { вывод(о) }\n'
                                            'подключить "http"\nесли ошибка о = http.получить("http://x") { вывод(о) }')
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), ["не удалось выполнить команду",
                                              "http://x: в программе для WebAssembly запросы HTTP недоступны"])

    @unittest.skipUnless(shutil.which("node") and subprocess.run(
        ["node", "-e", "require('playwright')"], capture_output=True,
        env=dict(os.environ, NODE_PATH=subprocess.run([shutil.which("npm") or "npm", "root", "-g"], capture_output=True, text=True).stdout.strip()
                 if shutil.which("npm") else "")).returncode == 0, "нет Node.js с Playwright")
    def test_страница_в_браузере(self):
        сценарий = r"""
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch(); const p = await b.newPage();
  const ответы = ["Анна", "17"];
  p.on('dialog', async d => { await d.accept(ответы.shift() || ""); });
  await p.goto('file://' + process.argv[2]);
  await p.waitForFunction(() => document.getElementById('состояние').textContent.startsWith('Программа завершена'),
                          null, { timeout: 60000 });
  console.log(await p.textContent('#состояние'));
  console.log(await p.textContent('#вывод'));
  await b.close();
})();
"""
        текст = ('пусть имя = ввод("Как вас зовут? ")\nвывод("Здравствуйте, " + имя + "!")\n'
                 'пусть лет = цел(ввод("Сколько вам лет? "))\nвывод(18 - лет)\nвывод(найти_все("а1 б22", "\\\\d+"))\n'
                 'вывод(1 / (лет - 17))\n')
        with tempfile.TemporaryDirectory() as п:
            путь = os.path.join(п, "программа.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            _, путь_html = dan.собрать_веб(путь, текст)
            with open(os.path.join(п, "браузер.js"), "w", encoding="utf-8") as ф:
                ф.write(сценарий)
            окружение = dict(os.environ, NODE_PATH=subprocess.run([shutil.which("npm") or "npm", "root", "-g"], capture_output=True,
                                                                  text=True).stdout.strip())
            р = subprocess.run(["node", os.path.join(п, "браузер.js"), путь_html], capture_output=True, text=True,
                               encoding="utf-8", timeout=120, env=окружение)
        self.assertEqual(р.returncode, 0, р.stderr)
        self.assertEqual(р.stdout.rstrip("\n").splitlines(), [
            "Программа завершена с кодом 1", "Как вас зовут? Анна", "Здравствуйте, Анна!", "Сколько вам лет? 17", "1",
            '["1", "22"]', "Ошибка выполнения в строке 6: деление на ноль"])


@unittest.skipUnless(ЕСТЬ_ZIG, "нужен zig (pip install ziglang)")
class ТестыДругихСистем(unittest.TestCase):

    def test_сборка_для_систем(self):
        текст = каталог.прочитать(os.path.join(ПАПКА, "каталог", "программы", "49_даты_и_сроки.dan"))
        with tempfile.TemporaryDirectory() as п:
            путь = os.path.join(п, "сроки.dan")
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            заголовки = {"windows": b"MZ", "linux": b"\x7fELF", "macos": b"\xcf\xfa\xed\xfe",
                         "macos-intel": b"\xcf\xfa\xed\xfe"}
            for система, начало in заголовки.items():
                with self.subTest(система=система):
                    исп = dan.собрать_для(путь, текст, система)
                    with open(исп, "rb") as ф:
                        self.assertEqual(ф.read(len(начало)), начало)
            if sys.platform == "linux":
                р = subprocess.run([os.path.join(п, "сроки-linux")], capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(р.stdout, каталог.прочитать(os.path.join(ПАПКА, "каталог", "программы",
                                                                         "49_даты_и_сроки.вывод")))
        with self.assertRaises(dan.ОшибкаСборки):
            dan.собрать_для(путь, текст, "amiga")

    def test_каталог_собирается_для_macos(self):
        """Запустить программы для macOS здесь нельзя, но сборка проверяет, что код подходит к системе."""
        for путь in каталог.программы("программы")[::3]:
            текст = каталог.прочитать(путь)
            with self.subTest(путь=путь), tempfile.TemporaryDirectory() as п:
                копия = os.path.join(п, "п.dan")
                with open(копия, "w", encoding="utf-8") as ф:
                    ф.write(текст)
                исп = dan.собрать_для(копия, текст, "macos")
                with open(исп, "rb") as ф:
                    self.assertEqual(ф.read(4), b"\xcf\xfa\xed\xfe")


if __name__ == "__main__":
    unittest.main()
