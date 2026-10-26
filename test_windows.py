"""Проверка программ, собранных для Windows (запускается по желанию).

Каталог (папка «каталог») собирается компилятором MinGW в исполняемые файлы
Windows и запускается через Wine; результаты сравниваются с ожидаемыми, как
в test_katalog.py. Так проверяется поведение именно в той системе, для
которой Дан создаётся, на компьютере с Linux.

Запуск: DAN_WINDOWS=1 python -m unittest test_windows
Нужны x86_64-w64-mingw32-gcc и wine64 (в Ubuntu: пакеты gcc-mingw-w64-x86-64 и wine64).
"""

import os
import shutil
import subprocess
import tempfile
import unittest

import dan
import test_katalog as каталог

MINGW = shutil.which("x86_64-w64-mingw32-gcc")
WINE = shutil.which("wine64") or ("/usr/lib/wine/wine64" if os.path.exists("/usr/lib/wine/wine64") else None)
ВКЛЮЧЕНО = os.environ.get("DAN_WINDOWS") == "1" and MINGW and WINE


def выполнить_в_windows(текст, ввод_данных=""):
    # Папка с латинским именем: Wine не всегда открывает рабочую папку с кириллицей в пути.
    with tempfile.TemporaryDirectory(prefix="dan_win_") as папка:
        путь_c = os.path.join(папка, "p.c")
        exe = os.path.join(папка, "p.exe")
        код_c = dan.перевести(текст)
        with open(путь_c, "w", encoding="utf-8") as ф:
            ф.write(код_c)
        окна = ["-mwindows"] if "#define DAN_GUI 1" in код_c else []
        сборка = subprocess.run([MINGW, *dan.ФЛАГИ_ОБЩИЕ, *dan.ФЛАГИ_СКОРОСТЬ, путь_c, "-o", exe,
                                 "-lm", "-Wl,--stack,67108864", *окна], capture_output=True, text=True)
        if сборка.returncode:
            raise AssertionError("сборка для Windows не удалась:\n" + сборка.stderr)
        среда = dict(os.environ, WINEDEBUG="-all",
                     WINEPREFIX=os.environ.get("WINEPREFIX", os.path.expanduser("~/.wine-dan")))
        р = subprocess.run([WINE, exe], input=ввод_данных.encode("utf-8"), capture_output=True,
                           timeout=300, cwd=папка, env=среда)
        return (р.returncode, р.stdout.decode("utf-8", "replace").replace("\r\n", "\n"),
                р.stderr.decode("utf-8", "replace").replace("\r\n", "\n"))


@unittest.skipUnless(ВКЛЮЧЕНО, "проверка Windows включается переменной DAN_WINDOWS=1 при наличии MinGW и Wine")
class ТестыWindows(unittest.TestCase):

    def test_каталог(self):
        for путь in каталог.программы("программы"):
            основа = путь[:-4]
            ввод_данных = каталог.прочитать(основа + ".ввод") if os.path.exists(основа + ".ввод") else ""
            with self.subTest(путь=путь):
                код, вывод, ошибки = выполнить_в_windows(каталог.прочитать(путь), ввод_данных)
                self.assertEqual(код, 0, ошибки)
                self.assertEqual(вывод.splitlines(), каталог.прочитать(основа + ".вывод").splitlines())

    def test_ошибки_выполнения_и_исправления(self):
        for путь in каталог.программы("ошибки"):
            основа = путь[:-4]
            сообщение = каталог.прочитать(основа + ".сообщение").strip()
            with self.subTest(путь=путь):
                if сообщение.startswith("Ошибка выполнения"):
                    код, _, ошибки = выполнить_в_windows(каталог.прочитать(путь))
                    self.assertEqual(код, 1)
                    self.assertIn(сообщение, ошибки)
                код, вывод, ошибки = выполнить_в_windows(каталог.прочитать(основа + ".исправлено.dan"))
                self.assertEqual(код, 0, ошибки)
                self.assertEqual(вывод.splitlines(),
                                 каталог.прочитать(основа + ".исправлено.вывод").splitlines())

    def test_файлы_с_русскими_именами(self):
        текст = ('создать_папку("Дела/2026 год")\n'
                 'записать("Дела/2026 год/иск.txt", "Прошу суд\\nвзыскать")\n'
                 'вывод(прочитать("Дела/2026 год/иск.txt"))\n'
                 'вывод(список_файлов("Дела"))\n'
                 'создать_архив("архив дела.zip", ["Дела/2026 год/иск.txt"], "пароль")\n'
                 'вывод(извлечь_архив("архив дела.zip", "Извлечено", "пароль"))\n'
                 'вывод(существует("Дела\\\\2026 год\\\\иск.txt"))')
        код, вывод, ошибки = выполнить_в_windows(текст)
        self.assertEqual(код, 0, ошибки)
        self.assertEqual(вывод.splitlines(), ["Прошу суд", "взыскать", '["2026 год/"]', '["иск.txt"]', "истина"])

    def test_задачи_и_каналы(self):
        import test_zadachi
        код, вывод, ошибки = выполнить_в_windows(test_zadachi.КОНВЕЙЕР)
        self.assertEqual(код, 0, ошибки)
        self.assertEqual(вывод.splitlines(), ["180300"])
        код, _, ошибки = выполнить_в_windows("пусть к: канал<цел> = канал()\nвывод(к.получить())")
        self.assertEqual(код, 1)
        self.assertIn("взаимная блокировка", ошибки)

    def test_глубокая_рекурсия(self):
        текст = ("функ глубина(н: цел) -> цел {\n если н == 0 { вернуть 0 }\n вернуть 1 + глубина(н - 1)\n}\n"
                 "вывод(глубина(300000))\nвывод(глубина(1000000000))")
        код, вывод, ошибки = выполнить_в_windows(текст)
        self.assertEqual(код, 1)
        self.assertEqual(вывод.splitlines(), ["300000"])
        self.assertIn("слишком глубокая рекурсия", ошибки)


if __name__ == "__main__":
    unittest.main()
