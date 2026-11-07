"""Поставка библиотеки llama.cpp для модуля «модели»: версия в одном месте, команды компилятора
на Дане «дан модели установить» и «дан модели проверить».

Настоящая библиотека здесь не нужна: установка проверяется на поддельном архиве в папке,
которая заменяет страницу выпуска (DAN_VYPUSKI=file:///...), так же, как её скачивает curl из
выпуска на GitHub. Загрузку настоящей библиотеки на четырёх системах проверяет задание CI
«Модели - установка и проверка llama.cpp».
"""

import hashlib
import os
import platform
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile

import sam_kompilyator
import modeli
import versiya

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def платформа():
    система = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(platform.system())
    процессор = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "arm64": "arm64"}.get(platform.machine().lower())
    имя = f"{система}-{процессор}"
    return имя if имя in ("linux-x64", "linux-arm64", "macos-arm64", "windows-x64") else None


def имя_библиотеки():
    if os.name == "nt":
        return "llama.dll"
    return "libllama.dylib" if platform.system() == "Darwin" else "libllama.so"


class ТестыВерсии(unittest.TestCase):

    def test_метка_в_одном_месте(self):
        self.assertRegex(modeli.ВЕРСИЯ_LLAMA, r"^b\d+$")
        with open(os.path.join(ПАПКА, "сам", "среда.dan"), encoding="utf-8") as ф:
            self.assertIn(f'общий функ версия_llama() -> стр {{ вернуть "{modeli.ВЕРСИЯ_LLAMA}" }}', ф.read())
        # больше нигде метка не записана: сценарии и CI берут её из versiya.py
        for путь in (os.path.join(ПАПКА, "поставка", "собрать_llama.py"),
                     os.path.join(ПАПКА, ".github", "workflows", "llama.yml"),
                     os.path.join(ПАПКА, ".github", "workflows", "vypusk.yml"),
                     os.path.join(ПАПКА, "сам", "модели_поставка.dan")):
            with open(путь, encoding="utf-8") as ф:
                self.assertNotIn(modeli.ВЕРСИЯ_LLAMA, ф.read(), путь)

    def test_сборка_только_для_процессора(self):
        import importlib.util
        спец = importlib.util.spec_from_file_location("sobrat_llama", os.path.join(ПАПКА, "поставка", "собрать_llama.py"))
        м = importlib.util.module_from_spec(спец)
        спец.loader.exec_module(м)
        for система in ("linux", "macos", "windows"):
            п = м.параметры_cmake(система, "исходники")
            for нужно in ("-DGGML_BACKEND_DL=ON", "-DGGML_CPU_ALL_VARIANTS=ON", "-DGGML_NATIVE=OFF",
                          "-DGGML_METAL=OFF", "-DGGML_CUDA=OFF", "-DGGML_VULKAN=OFF", "-DBUILD_SHARED_LIBS=ON"):
                self.assertIn(нужно, п, система)
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", м.параметры_cmake("windows", "и"))


class ТестыКоманд(unittest.TestCase):
    """Команды «дан модели» компилятора на Дане."""

    @classmethod
    def setUpClass(cls):
        cls.дан = sam_kompilyator.исполняемый()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="dan_llama_")
        self.п = self.tmp.name
        self.пользователь = os.path.join(self.п, "polzovatel")
        os.makedirs(self.пользователь)
        self.среда = {к: з for к, з in os.environ.items()
                      if к not in ("DAN_LLAMA", "DAN_HOME", "DAN_VYPUSKI", "XDG_DATA_HOME", "LD_LIBRARY_PATH")}
        self.среда["HOME"] = self.пользователь
        self.среда["LOCALAPPDATA"] = self.пользователь
        if os.name == "nt":
            self.папка_llama = os.path.join(self.пользователь, "Dan", "llama")
        else:
            self.папка_llama = os.path.join(self.пользователь, ".local", "share", "dan", "llama")

    def tearDown(self):
        self.tmp.cleanup()

    def дан_(self, *аргументы, **добавить):
        среда = dict(self.среда, **добавить)
        р = subprocess.run([self.дан, "модели", *аргументы], capture_output=True, env=среда, cwd=self.п, timeout=600)
        return р.returncode, (р.stdout + р.stderr).decode("utf-8", "replace").replace("\r\n", "\n")

    def выпуск(self, файлы, сумма=None):
        """Папка «выпуска» (имя латиницей: curl в Windows получает адрес в кодировке системы) с архивом llama-<метка>-<система>.zip и SHA256SUMS.txt; адрес file:///."""
        выпуск = os.path.join(self.п, "vypusk")
        os.makedirs(выпуск, exist_ok=True)
        имя = f"llama-{modeli.ВЕРСИЯ_LLAMA}-{платформа()}.zip"
        путь = os.path.join(выпуск, имя)
        with zipfile.ZipFile(путь, "w") as з:
            for и, содержимое in файлы.items():
                з.writestr(и, содержимое)
        with open(путь, "rb") as ф:
            с = сумма or hashlib.sha256(ф.read()).hexdigest()
        with open(os.path.join(выпуск, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as ф:
            ф.write(f"{'0' * 64}  dan-{versiya.ВЕРСИЯ}-setup.exe\n{с}  {имя}\n")
        return "file:///" + выпуск.replace("\\", "/").lstrip("/")

    def нужен_curl(self):
        if not платформа():
            self.skipTest("для этой системы нет архива llama.cpp")
        if not shutil.which("curl"):
            self.skipTest("нет программы curl")

    def test_без_действия_справка(self):
        код, вывод = self.дан_()
        self.assertEqual(код, 2)
        self.assertIn("дан модели установить", вывод)
        self.assertIn("DAN_LLAMA", вывод)
        self.assertIn(modeli.ВЕРСИЯ_LLAMA, вывод)

    def test_неверный_путь_в_DAN_LLAMA(self):
        код, вывод = self.дан_("проверить", DAN_LLAMA=os.path.join(self.п, "нет", имя_библиотеки()))
        self.assertEqual(код, 1)
        self.assertIn("Переменная среды DAN_LLAMA указывает на", вывод)

    def test_библиотеки_нет(self):
        код, вывод = self.дан_("проверить")
        if "Библиотека готова к работе" in вывод:
            self.skipTest("в системных путях есть настоящая библиотека llama.cpp")
        self.assertEqual(код, 1, вывод)
        self.assertIn("Библиотека llama.cpp не найдена.", вывод)
        self.assertIn(os.path.join(self.папка_llama, имя_библиотеки()), вывод)
        self.assertIn("Что сделать:", вывод)

    def test_установка_и_проверка_поддельной_библиотеки(self):
        self.нужен_curl()
        адрес = self.выпуск({имя_библиотеки(): b"not a library", "version.txt": f"{modeli.ВЕРСИЯ_LLAMA}\nabc\n",
                             "LICENSE-llama.cpp.txt": "MIT"})
        код, вывод = self.дан_("установить", DAN_VYPUSKI=адрес)
        self.assertEqual(код, 0, вывод)
        self.assertIn("установлена в " + self.папка_llama, вывод)
        self.assertEqual(sorted(os.listdir(self.папка_llama)),
                         sorted([имя_библиотеки(), "version.txt", "LICENSE-llama.cpp.txt"]))
        # библиотека найдена в папке пользователя, но поддельная не загружается
        код, вывод = self.дан_("проверить")
        self.assertEqual(код, 1, вывод)
        self.assertIn("Библиотека llama.cpp найдена (папка данных пользователя", вывод)
        self.assertIn("но не загружается", вывод)
        # повторная установка заменяет папку целиком
        адрес = self.выпуск({имя_библиотеки(): b"second", "version.txt": modeli.ВЕРСИЯ_LLAMA})
        код, вывод = self.дан_("установить", DAN_VYPUSKI=адрес)
        self.assertEqual(код, 0, вывод)
        self.assertEqual(sorted(os.listdir(self.папка_llama)), sorted([имя_библиотеки(), "version.txt"]))

    def test_неверная_контрольная_сумма(self):
        self.нужен_curl()
        адрес = self.выпуск({имя_библиотеки(): b"x"}, сумма="ab" * 32)
        код, вывод = self.дан_("установить", DAN_VYPUSKI=адрес)
        self.assertEqual(код, 1, вывод)
        self.assertIn("не совпадает с SHA256SUMS.txt", вывод)
        self.assertIn("ничего не установлено", вывод)
        self.assertFalse(os.path.exists(self.папка_llama))

    def test_нет_архива_в_выпуске(self):
        self.нужен_curl()
        адрес = self.выпуск({имя_библиотеки(): b"x"})
        with open(os.path.join(self.п, "vypusk", "SHA256SUMS.txt"), "w", encoding="utf-8") as ф:
            ф.write("0" * 64 + "  dan-setup.exe\n")
        код, вывод = self.дан_("установить", DAN_VYPUSKI=адрес)
        self.assertEqual(код, 1, вывод)
        self.assertIn(f"нет архива llama-{modeli.ВЕРСИЯ_LLAMA}-{платформа()}.zip", вывод)

    def test_архив_без_библиотеки(self):
        self.нужен_curl()
        адрес = self.выпуск({"README.txt": "x"})
        код, вывод = self.дан_("установить", DAN_VYPUSKI=адрес)
        self.assertEqual(код, 1, вывод)
        self.assertIn("ничего не установлено", вывод)
        self.assertFalse(os.path.exists(self.папка_llama))

    def test_выпуска_нет(self):
        self.нужен_curl()
        код, вывод = self.дан_("установить", DAN_VYPUSKI="file:///" + self.п.replace("\\", "/").lstrip("/") + "/net")
        self.assertEqual(код, 1, вывод)
        self.assertIn("Не удалось скачать список контрольных сумм", вывод)
        self.assertTrue(re.search(r"поставка/собрать_llama\.py", вывод))


if __name__ == "__main__":
    unittest.main()
