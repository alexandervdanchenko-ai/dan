"""Тесты библиотеки данных: файлы, байты, хеши, сжатие, шифрование, архивы.

Программы на Дан собираются и запускаются во временной папке. Результаты
сверяются с независимыми реализациями: модулями zlib, gzip, lzma, hashlib
и zipfile из Python, а также (если установлены) cryptography и pyzipper.
"""

import gzip
import hashlib
import lzma
import os
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib

import dan

КОМПИЛЯТОР_ЕСТЬ = dan.компилятор_есть()

try:
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
except ImportError:
    ChaCha20Poly1305 = None
try:
    import pyzipper
except ImportError:
    pyzipper = None


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыДанных(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.папка, ignore_errors=True)

    def путь(self, имя):
        return os.path.join(self.папка, имя)

    def запустить(self, текст):
        путь = self.путь("программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8",
                           timeout=120, cwd=self.папка)
        return р.returncode, р.stdout, р.stderr

    def вывод(self, текст, ожидается):
        код, вывод, ошибки = self.запустить(текст)
        self.assertEqual(код, 0, ошибки)
        self.assertEqual(вывод.splitlines(), ожидается)

    def ошибка(self, текст, фрагмент):
        код, _, ошибки = self.запустить(текст)
        self.assertEqual(код, 1)
        self.assertIn(фрагмент, ошибки)

    def записать(self, имя, данные):
        with open(self.путь(имя), "wb") as ф:
            ф.write(данные)

    def прочитать(self, имя):
        with open(self.путь(имя), "rb") as ф:
            return ф.read()

    # ---- байты ----

    def test_байты(self):
        self.вывод('пусть б = байты("Дан")\nвывод(б)\nвывод(б.длина)\nвывод(б[0])\nвывод(стр(б))\n'
                   'вывод(байты([104, 105]) + байты("!"))\nвывод(шестнадцатеричная(байты("A")))\n'
                   'вывод(из_шестнадцатеричной("d0 94") == байты("Д"))\nвывод(числа_байтов(байты("ab")))\n'
                   'вывод([байты("x")])',
                   ["байты(6): d0 94 d0 b0 d0 bd", "6", "208", "Дан", "байты(3): 68 69 21", "41",
                    "истина", "[97, 98]", "[байты(1): 78]"])
        self.вывод('вывод(байты("абв")[2..5])', ["байты(4): d0 b1 d0 b2"])
        self.ошибка("вывод(байты([256]))", "от 0 до 255")
        self.ошибка('вывод(байты("ab")[2])', "индекс 2")
        self.ошибка('вывод(из_шестнадцатеричной("zz"))', "шестнадцатеричной")

    def test_контрольные_суммы_и_хеш(self):
        self.вывод('вывод(контрольная_сумма("123456789"))\nвывод(хеш("abc"))\nвывод(хеш(байты("")))',
                   [str(zlib.crc32(b"123456789")), hashlib.sha256(b"abc").hexdigest(),
                    hashlib.sha256(b"").hexdigest()])

    # ---- файлы ----

    def test_файлы(self):
        self.вывод('записать("а.txt", "раз\\nдва\\n")\nдописать("а.txt", "три\\n")\nвывод(прочитать("а.txt"))\n'
                   'вывод(размер_файла("а.txt"))\nвывод(существует("а.txt"))\nвывод(существует("нет.txt"))\n'
                   'создать_папку("п/вложенная")\nзаписать("п/вложенная/б.bin", байты([1, 2]))\n'
                   'вывод(прочитать_байты("п/вложенная/б.bin"))\nвывод(список_файлов("п"))\n'
                   'удалить_файл("а.txt")\nвывод(существует("а.txt"))',
                   ["раз", "два", "три", "", str(len("раз\nдва\nтри\n".encode())), "истина", "ложь",
                    "байты(2): 01 02", '["вложенная/"]', "ложь"])

    def test_текущая_папка(self):
        # в Windows путь может быть записан короткими именами (RUNNER~1), поэтому сравнивается сама папка
        код, вывод_, ошибки = self.запустить('вывод(текущая_папка())\nзаписать(текущая_папка() + "/а.txt", "да")\n'
                                              'вывод(прочитать("а.txt"))')
        self.assertEqual(код, 0, ошибки)
        строки = вывод_.splitlines()
        self.assertTrue(os.path.isabs(строки[0]) and os.path.samefile(строки[0], self.папка), строки)
        self.assertEqual(строки[1], "да")

    def test_папки_ссылки_переименование(self):
        os.makedirs(self.путь("внешняя"))
        self.записать("внешняя/важное.txt", b"1")
        ссылка = True
        try:
            os.symlink(self.путь("внешняя"), self.путь("ссылка"), target_is_directory=True)
        except (OSError, NotImplementedError):
            ссылка = False          # в Windows без прав разработчика ссылки создавать нельзя
        программа = ('создать_папку("п/вложенная")\nзаписать("п/вложенная/а.txt", "а")\nзаписать("п/б.txt", "б")\n'
                     'вывод(это_папка("п"))\nвывод(это_папка("п/б.txt"))\nвывод(это_папка("нет"))\n'
                     'переименовать("п/б.txt", "п/в.txt")\nвывод(список_файлов("п"))\n'
                     'удалить_папку("п")\nвывод(существует("п"))\nудалить_папку("п")\n'
                     'пусть т = создать_временную_папку()\nвывод(это_папка(т) и список_файлов(т).длина == 0)\n'
                     'вывод(т != создать_временную_папку())\nудалить_папку(т)\n')
        if ссылка:
            программа += ('вывод(это_ссылка("ссылка"))\nвывод(это_ссылка("внешняя"))\n'
                          'создать_папку("с")\nпереименовать("ссылка", "с/ссылка")\nудалить_папку("с")\n'
                          'вывод(существует("внешняя/важное.txt"))\n')
        ожидается = ["истина", "ложь", "ложь", '["в.txt", "вложенная/"]', "ложь", "истина", "истина"]
        self.вывод(программа, ожидается + (["истина", "ложь", "истина"] if ссылка else []))
        if os.name == "nt":
            # git делает свои файлы «только для чтения»: удалить_папку справляется и с ними
            os.makedirs(self.путь("ч"))
            self.записать("ч/файл.txt", b"1")
            os.chmod(self.путь("ч/файл.txt"), 0o444)
            self.вывод('удалить_папку("ч")\nвывод(существует("ч"))', ["ложь"])

    def test_испорченный_текст_из_байтов(self):
        # байт, который не начинает целую букву UTF-8, считается одной буквой: длина, перебор,
        # срезы и поиск согласованы и не выходят за конец строки (находка фаззинга)
        self.вывод('пусть с = стр(байты([65, 248, 128, 208, 180, 226, 130, 208]))\nвывод(с.длина)\n'
                   'перем н = 0\nдля б в с { н += 1 }\nвывод(н)\nвывод(с[1..с.длина - 1].длина)\n'
                   'вывод(с.найти("д"))\nвывод(с.прописные().длина)\nвывод(стр(байты([248])).длина)',
                   ["7", "7", "6", "3", "7", "1"])

    def test_текст_с_bom_и_окончаниями_windows(self):
        self.записать("w.txt", "﻿строка 1\r\nстрока 2\r\n".encode("utf-8"))
        self.вывод('вывод(прочитать("w.txt") == "строка 1\\nстрока 2\\n")', ["истина"])

    def test_ошибки_файлов(self):
        self.ошибка('вывод(прочитать("нет.txt"))', "не найдены")
        self.ошибка('удалить_файл("нет.txt")', "не удалось удалить")

    # ---- сжатие ----

    def наборы_данных(self):
        random.seed(11)
        слова = "договор работник работодатель заработная плата суд the law 2026".split()
        return {
            "пусто": b"",
            "один": b"x",
            "текст": " ".join(random.choice(слова) for _ in range(20000)).encode(),
            "случайные": os.urandom(70000),
            "повторы": b"abc" * 50000,
            "смесь": os.urandom(3000) * 30 + bytes(random.randrange(3) for _ in range(30000)),
        }

    def test_сжатие_xz_и_gzip_совместимо_с_python(self):
        наборы = self.наборы_данных()
        for имя, данные in наборы.items():
            self.записать(имя, данные)
            self.записать(имя + ".pyxz", lzma.compress(данные, preset=6))
            self.записать(имя + ".pygz", gzip.compress(данные, 6))
        программа = "".join(
            f'записать("{и}.xz", сжать(прочитать_байты("{и}")))\n'
            f'записать("{и}.gz", сжать_gzip(прочитать_байты("{и}")))\n'
            f'записать("{и}.из_xz", распаковать(прочитать_байты("{и}.pyxz")))\n'
            f'записать("{и}.из_gz", распаковать(прочитать_байты("{и}.pygz")))\n' for и in наборы)
        self.вывод(программа + 'вывод("ок")', ["ок"])
        for имя, данные in наборы.items():
            with self.subTest(имя=имя):
                self.assertEqual(lzma.decompress(self.прочитать(имя + ".xz")), данные)
                self.assertEqual(gzip.decompress(self.прочитать(имя + ".gz")), данные)
                self.assertEqual(self.прочитать(имя + ".из_xz"), данные)
                self.assertEqual(self.прочитать(имя + ".из_gz"), данные)
        # степень сжатия: не хуже zip и близко к xz
        текст = наборы["текст"]
        xz_дан = len(self.прочитать("текст.xz"))
        self.assertLess(xz_дан, len(zlib.compress(текст, 9)))
        self.assertLess(xz_дан, len(lzma.compress(текст, preset=6)))
        self.assertLessEqual(len(self.прочитать("текст.gz")), len(gzip.compress(текст, 9)) + 20)

    def test_сжатие_строк_и_повреждения(self):
        self.вывод('пусть т = "Повтор. Повтор. Повтор. Повтор."\nпусть с = сжать(т)\n'
                   'вывод(стр(распаковать(с)) == т)\nвывод(с.длина < байты(т).длина + 64)\n'
                   'записать("с.xz", с)', ["истина", "истина"])
        данные = bytearray(self.прочитать("с.xz"))
        данные[len(данные) // 2] ^= 0x10
        self.записать("с.xz", bytes(данные))
        self.ошибка('вывод(распаковать(прочитать_байты("с.xz")))', "повреждены")
        self.ошибка('вывод(распаковать(байты("просто текст")))', "не являются сжатыми")

    # ---- шифрование ----

    def test_шифрование_и_сжатие_с_паролем(self):
        self.вывод('пусть з = зашифровать("тайна", "пароль")\nвывод(стр(расшифровать(з, "пароль")))\n'
                   'пусть с = сжать("тайна тайна тайна", "пароль")\nвывод(стр(распаковать(с, "пароль")))\n'
                   'вывод(зашифровать("тайна", "пароль") == з)',
                   ["тайна", "тайна тайна тайна", "ложь"])
        self.ошибка('вывод(расшифровать(зашифровать("а", "1"), "2"))', "неверный пароль")
        self.ошибка('вывод(распаковать(сжать("а", "1")))', "укажите пароль")

    @unittest.skipUnless(ChaCha20Poly1305, "нет пакета cryptography")
    def test_шифрование_совместимо_с_cryptography(self):
        self.вывод('записать("д.enc", зашифровать("Привет", "секрет"))', [])
        б = self.прочитать("д.enc")
        self.assertEqual(б[:5], b"DANC\x01")
        повторений = struct.unpack("<I", б[5:9])[0]
        соль, nonce = б[9:25], б[25:37]
        ключ = hashlib.pbkdf2_hmac("sha256", "секрет".encode(), соль, повторений, 32)
        self.assertEqual(ChaCha20Poly1305(ключ).decrypt(nonce, б[37:], б[:37]), "Привет".encode())
        # и в обратную сторону, с малым числом повторений
        соль, nonce = os.urandom(16), os.urandom(12)
        заголовок = b"DANC\x01" + struct.pack("<I", 1000) + соль + nonce
        ключ = hashlib.pbkdf2_hmac("sha256", "п".encode(), соль, 1000, 32)
        self.записать("py.enc", заголовок + ChaCha20Poly1305(ключ).encrypt(nonce, b"from python", заголовок))
        self.вывод('вывод(стр(расшифровать(прочитать_байты("py.enc"), "п")))', ["from python"])

    # ---- архивы ----

    def test_архивы_zip(self):
        self.записать("отчёт.txt", "Отчёт. ".encode() * 1000)
        self.записать("данные.bin", os.urandom(3000))
        self.вывод('создать_архив("а.zip", ["отчёт.txt", "данные.bin"])\nвывод(содержимое_архива("а.zip"))\n'
                   'вывод(извлечь_архив("а.zip", "распаковано"))',
                   ['["отчёт.txt", "данные.bin"]', '["отчёт.txt", "данные.bin"]'])
        with zipfile.ZipFile(self.путь("а.zip")) as з:
            self.assertIsNone(з.testzip())
            self.assertEqual(з.read("отчёт.txt"), self.прочитать("отчёт.txt"))
        self.assertEqual(self.прочитать("распаковано/данные.bin"), self.прочитать("данные.bin"))
        # архив, созданный Python, с папкой внутри
        with zipfile.ZipFile(self.путь("py.zip"), "w", zipfile.ZIP_DEFLATED) as з:
            з.writestr("папка/внутри.txt", "вложенный файл")
            з.writestr("корень.txt", "")
        self.вывод('вывод(извлечь_архив("py.zip", "из_py"))\nвывод(прочитать("из_py/папка/внутри.txt"))',
                   ['["папка/внутри.txt", "корень.txt"]', "вложенный файл"])

    def test_среда_данных_без_неявных_объявлений(self):
        # clang и gcc 14 не собирают вызов необъявленной функции (так было с lstat в Linux)
        код = dan.перевести('создать_архив("а.zip", ["а"], "б")\nвывод(извлечь_архив("а.zip", "в", "б"))')
        путь_c = self.путь("p.c")
        with open(путь_c, "w", encoding="utf-8") as ф:
            ф.write(код)
        сборка = subprocess.run([*dan.команда_компилятора(), *[ф for ф in dan.ФЛАГИ_ОБЩИЕ if ф != "-w"],
                                 "-Werror=implicit-function-declaration",
                                 "-c", путь_c, "-o", self.путь("p.o")],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(сборка.returncode, 0, сборка.stderr)

    def test_опасные_имена_в_архиве(self):
        with zipfile.ZipFile(self.путь("злой.zip"), "w") as з:
            з.writestr("../вне.txt", "плохо")
        self.ошибка('вывод(извлечь_архив("злой.zip", "сюда"))', "небезопасное имя")
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(self.папка), "вне.txt")))

    def test_архив_с_паролем(self):
        self.записать("секрет.txt", "Секретные сведения".encode() * 100)
        self.вывод('создать_архив("с.zip", ["секрет.txt"], "пароль")\n'
                   'вывод(извлечь_архив("с.zip", "р", "пароль"))', ['["секрет.txt"]'])
        self.assertEqual(self.прочитать("р/секрет.txt"), self.прочитать("секрет.txt"))
        self.ошибка('вывод(извлечь_архив("с.zip", "р2", "другой"))', "неверный пароль")
        self.ошибка('вывод(извлечь_архив("с.zip", "р3"))', "укажите пароль")

    @unittest.skipUnless(pyzipper, "нет пакета pyzipper")
    def test_архив_aes_совместим_с_pyzipper(self):
        self.записать("файл.txt", "AES-256 ".encode() * 300)
        self.вывод('создать_архив("aes.zip", ["файл.txt"], "tajna")', [])
        with pyzipper.AESZipFile(self.путь("aes.zip")) as з:
            з.setpassword(b"tajna")
            self.assertEqual(з.read("файл.txt"), self.прочитать("файл.txt"))
        with pyzipper.AESZipFile(self.путь("py_aes.zip"), "w", compression=pyzipper.ZIP_DEFLATED,
                                 encryption=pyzipper.WZ_AES) as з:
            з.setpassword(b"tajna")
            з.writestr("из_python.txt", "зашифровано в Python")
        self.вывод('вывод(извлечь_архив("py_aes.zip", "в", "tajna"))\nвывод(прочитать("в/из_python.txt"))',
                   ['["из_python.txt"]', "зашифровано в Python"])


# Контрольный пример RFC 8439, раздел 2.8.2 (ChaCha20-Poly1305 с дополнительными данными).
RFC8439_ТЕКСТ = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
                 b"for the future, sunscreen would be it.")
RFC8439_ШИФР = ("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca96712"
                "82fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58"
                "fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116")
RFC8439_КОД = "1ae10b594f09e26a7e902ecbd0600691"
# Контрольные примеры RFC 7914, раздел 11 (PBKDF2-HMAC-SHA256).
RFC7914 = [
    (b"passwd", b"salt", 1,
     "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
     "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"),
    (b"Password", b"NaCl", 80000,
     "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
     "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"),
]


def массив_c(имя, данные):
    return f"static const uint8_t {имя}[{max(len(данные), 1)}] = {{{', '.join(str(б) for б in данные) or '0'}}};\n"


ПЕЧАТЬ_C = ["#include <stdio.h>",
            "static void печать(const uint8_t *д, size_t n) {",
            "    for (size_t i = 0; i < n; i++) printf(\"%02x\", д[i]);",
            "    printf(\"\\n\");",
            "}"]


def собрать_обвязку(папка, обвязка):
    """Собирает переведённую программу вместе с программой на C (строки обвязки), которая
    вызывает функции библиотеки выполнения напрямую; возвращает путь к исполняемому файлу."""
    код = dan.перевести('пусть з = зашифровать("а", "б")\nвывод(стр(расшифровать(з, "б")))\n'
                        'создать_архив("а.zip", ["а"], "б")\nвывод(извлечь_архив("а.zip", "в", "б"))')
    текст_c = ("#define main dan_program_main\n" + код + "\n#undef main\n#include <string.h>\n"
               + "\n".join(обвязка) + "\n")
    путь_c = os.path.join(папка, "obvyazka.c")
    исполняемый = os.path.join(папка, "obvyazka.exe" if os.name == "nt" else "obvyazka")
    with open(путь_c, "w", encoding="utf-8") as ф:
        ф.write(текст_c)
    сборка = subprocess.run([*dan.команда_компилятора(), *dan.ФЛАГИ_ОБЩИЕ, *dan.ФЛАГИ_СКОРОСТЬ,
                             путь_c, "-o", исполняемый, *dan.БИБЛИОТЕКИ],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if сборка.returncode != 0:
        raise AssertionError(сборка.stderr)
    return исполняемый


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыОфициальныхПримеров(unittest.TestCase):
    """Сверка шифрования библиотеки выполнения с примерами из RFC, без сторонних библиотек.

    Функции библиотеки выполнения вызываются напрямую: к переведённой программе
    добавляется небольшая программа на C, печатающая результаты в шестнадцатеричном виде.
    """

    def test_примеры_rfc(self):
        обвязка = list(ПЕЧАТЬ_C)
        обвязка.append(массив_c("ключ", bytes(range(0x80, 0xa0))))
        обвязка.append(массив_c("одноразовое", bytes.fromhex("070000004041424344454647")))
        обвязка.append(массив_c("доп", bytes.fromhex("50515253c0c1c2c3c4c5c6c7")))
        обвязка.append(массив_c("текст", RFC8439_ТЕКСТ))
        for номер, (пароль, соль, _, _) in enumerate(RFC7914):
            обвязка.append(массив_c(f"пароль{номер}", пароль))
            обвязка.append(массив_c(f"соль{номер}", соль))
        обвязка += ["int main(void) {",
                    f"    uint8_t буфер[{len(RFC8439_ТЕКСТ)}], код[16], вывод[64];",
                    "    memcpy(буфер, текст, sizeof буфер);",
                    "    dan_chacha20_xor(ключ, 1, одноразовое, буфер, sizeof буфер);",
                    "    dan_aead_tag(ключ, одноразовое, доп, sizeof доп, буфер, sizeof буфер, код);",
                    "    печать(буфер, sizeof буфер);",
                    "    печать(код, 16);"]
        for номер, (пароль, соль, повторы, _) in enumerate(RFC7914):
            обвязка.append(f"    dan_pbkdf2_sha256(пароль{номер}, {len(пароль)}, соль{номер}, {len(соль)}, "
                           f"{повторы}, вывод, 64);")
            обвязка.append("    печать(вывод, 64);")
        обвязка += ["    return 0;", "}"]
        папка = tempfile.mkdtemp()
        try:
            р = subprocess.run([собрать_обвязку(папка, обвязка)], capture_output=True, text=True, timeout=120)
            self.assertEqual(р.returncode, 0, р.stderr)
            строки = р.stdout.split()
        finally:
            shutil.rmtree(папка, ignore_errors=True)
        self.assertEqual(строки[0], RFC8439_ШИФР)
        self.assertEqual(строки[1], RFC8439_КОД)
        self.assertEqual(строки[2:], [ожидается for _, _, _, ожидается in RFC7914])


def таблица_aes():
    """S-блок AES из определения (FIPS-197, раздел 5.1.1): обратный элемент в GF(2^8)
    и аффинное преобразование. Независим от кода среды, где таблицы нет."""
    def умножить(а, б):
        р = 0
        while б:
            if б & 1:
                р ^= а
            а = ((а << 1) ^ 0x11b) if а & 0x80 else а << 1
            б >>= 1
        return р
    таблица = []
    for x in range(256):
        обратный = next((у for у in range(1, 256) if умножить(x, у) == 1), 0)
        с = 0x63
        for i in range(8):
            бит = ((обратный >> i) ^ (обратный >> ((i + 4) % 8)) ^ (обратный >> ((i + 5) % 8))
                   ^ (обратный >> ((i + 6) % 8)) ^ (обратный >> ((i + 7) % 8))) & 1
            с ^= бит << i
        таблица.append(с)
    return таблица


# Образцовый AES для сверки: обычная запись с таблицей (только в тесте: в среде таблица
# по секретному номеру дала бы утечку через кэш).
ОБРАЗЦОВЫЙ_AES_C = """
typedef struct { uint8_t rk[240]; int rounds; } обр_aes;
static uint8_t обр_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static void обр_init(обр_aes *a, const uint8_t *key, int keylen) {
    int nk = keylen / 4, total;
    uint8_t rcon = 1;
    a->rounds = nk + 6;
    total = 4 * (a->rounds + 1);
    memcpy(a->rk, key, (size_t)keylen);
    for (int i = nk; i < total; i++) {
        uint8_t t[4], x;
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            x = t[0];
            t[0] = обр_sbox[t[1]] ^ rcon; t[1] = обр_sbox[t[2]]; t[2] = обр_sbox[t[3]]; t[3] = обр_sbox[x];
            rcon = обр_xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = обр_sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) a->rk[4 * i + j] = a->rk[4 * (i - nk) + j] ^ t[j];
    }
}
static void обр_block(const обр_aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= a->rounds; r++) {
        for (int i = 0; i < 16; i++) t[i] = обр_sbox[s[i]];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) s[4 * c + row] = t[4 * ((c + row) % 4) + row];
        if (r != a->rounds)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = s + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3], all = a0 ^ a1 ^ a2 ^ a3;
                q[0] ^= all ^ обр_xt(a0 ^ a1); q[1] ^= all ^ обр_xt(a1 ^ a2);
                q[2] ^= all ^ обр_xt(a2 ^ a3); q[3] ^= all ^ обр_xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}
static uint64_t обр_сост = 0x9E3779B97F4A7C15ull;
static uint8_t обр_случ(void) {
    обр_сост ^= обр_сост << 13; обр_сост ^= обр_сост >> 7; обр_сост ^= обр_сост << 17;
    return (uint8_t)(обр_сост >> 24);
}
"""

# FIPS-197, приложение C: ключ 00 01 02 ..., текст 00 11 22 ... ff
FIPS197 = {128: "69c4e0d86a7b0430d8cdb78070b4c55a", 192: "dda97ca4864cdfe06eaf70a0ec0d7191",
           256: "8ea2b7ca516745bfeafc49904b496089"}
# хеши SHA-256 файлов архива тесты_данные/winzip_aes.zip (создан pyzipper 0.4.0, пароль Dan-WinZip-AES)
WINZIP_AES = {"aes128.bin": "a6f26eaf54947c5f31af02ad5477f33e58ffc6a8ceecb2c94912ede3963a4d02",
              "aes192.bin": "e270c22bcc9baa4c2d5eacca6a733b585d466954b7cd2a723797ad01cff96815",
              "aes256.bin": "7ed0a020badb8e5e998a9df8c1faec433d9446823af3831a28e17ffbe7ecad9d"}
ПАПКА_ДАННЫХ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "тесты_данные")


def замер_aes():
    """Скорость AES-256 CTR каждого варианта (замеры/результаты.md, раздел «Шифрование AES»)."""
    ТестыAES.setUpClass()
    try:
        print(subprocess.run([ТестыAES.программа, "замер"], capture_output=True, text=True,
                             encoding="utf-8").stdout, end="")
    finally:
        ТестыAES.tearDownClass()


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыAES(unittest.TestCase):
    """AES среды без таблиц по секретному номеру: вариант на C (схема Бояра - Перальты),
    AES-NI и ARMv8 дают одни и те же байты."""

    @classmethod
    def setUpClass(cls):
        cls.папка = tempfile.mkdtemp()
        обвязка = list(ПЕЧАТЬ_C)
        обвязка.append("static const uint8_t обр_sbox[256] = {" + ", ".join(map(str, таблица_aes())) + "};")
        обвязка.append(ОБРАЗЦОВЫЙ_AES_C)
        обвязка.append("""
#include <time.h>
/* замер: AES-256 в режиме CTR WinZip, МБ/с каждого варианта и образца с таблицей */
static int замер(void) {
    static uint8_t к[32] = { 1, 2, 3 };
    dan_aes a;
    обр_aes о;
    dan_aes_init(&a, к, 32);
    обр_init(&о, к, 32);
    for (int v = 0; v < 4; v++) {
        if (v < 3 && !dan_aes_supported(v)) continue;
        size_t n = (size_t)(v == 1 || v == 2 ? 256 : 32) << 20;
        uint8_t *д = calloc(n, 1), сч[16] = { 0 }, ks[16];
        double лучшее = 1e9;
        for (int повтор = 0; повтор < 3; повтор++) {
            clock_t начало = clock();
            if (v < 3) dan_aes_ctr_winzip_v(&a, v, д, n);
            else
                for (size_t off = 0; off < n; off += 16) {
                    for (int i = 0; i < 16; i++) if (++сч[i] != 0) break;
                    обр_block(&о, сч, ks);
                    for (int i = 0; i < 16; i++) д[off + i] ^= ks[i];
                }
            double с = (double)(clock() - начало) / CLOCKS_PER_SEC;
            if (с < лучшее) лучшее = с;
        }
        printf("замер %s %.1f МБ/с (%u)\\n", v < 3 ? dan_aes_names[v] : "таблица", n / 1048576.0 / лучшее, (unsigned)д[n - 1]);
        free(д);
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) return замер();
    uint8_t ключ[32], текст[16], выход[16];
    for (int i = 0; i < 32; i++) ключ[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++) текст[i] = (uint8_t)(0x11 * i);
    printf("выбран %s\\n", dan_aes_names[dan_aes_variant()]);
    for (int v = 0; v < 3; v++) {
        if (!dan_aes_supported(v)) { printf("нет %s\\n", dan_aes_names[v]); continue; }
        for (int kl = 16; kl <= 32; kl += 8) {
            dan_aes a;
            dan_aes_init(&a, ключ, kl);
            dan_aes_blocks(&a, v, текст, выход, 1);
            printf("fips %s %d ", dan_aes_names[v], kl * 8);
            печать(выход, 16);
        }
        /* случайные ключи, длины и данные: режим CTR WinZip и отдельные блоки */
        int расхождений = 0;
        static uint8_t данные[1100], образец[1100], поток[1100], блоки[1100];
        for (int повтор = 0; повтор < 1500; повтор++) {
            uint8_t к[32];
            int kl = 16 + 8 * (повтор % 3);
            size_t n = (size_t)(обр_случ() * 4 + обр_случ() % 4);
            for (int i = 0; i < 32; i++) к[i] = обр_случ();
            for (size_t i = 0; i < n; i++) данные[i] = обр_случ();
            dan_aes a;
            обр_aes о;
            dan_aes_init(&a, к, kl);
            обр_init(&о, к, kl);
            if (memcmp(a.rk, о.rk, (size_t)(16 * (о.rounds + 1))) != 0 || a.rounds != о.rounds) расхождений++;
            uint8_t сч[16] = { 0 }, ks[16];
            memcpy(образец, данные, n);
            for (size_t off = 0; off < n; off += 16) {
                for (int i = 0; i < 16; i++) if (++сч[i] != 0) break;
                обр_block(&о, сч, ks);
                for (size_t i = 0; i < 16 && off + i < n; i++) образец[off + i] ^= ks[i];
            }
            memcpy(поток, данные, n);
            dan_aes_ctr_winzip_v(&a, v, поток, n);
            if (memcmp(поток, образец, n) != 0) расхождений++;
            size_t б = n / 16;
            dan_aes_blocks(&a, v, данные, блоки, б);
            for (size_t i = 0; i < б; i++) {
                обр_block(&о, данные + 16 * i, ks);
                if (memcmp(ks, блоки + 16 * i, 16) != 0) расхождений++;
            }
        }
        printf("расхождений %s %d\\n", dan_aes_names[v], расхождений);
    }
    return 0;
}""")
        cls.программа = собрать_обвязку(cls.папка, обвязка)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.папка, ignore_errors=True)

    def запуск(self, вариант=None):
        окружение = dict(os.environ)
        окружение.pop("DAN_AES", None)
        if вариант:
            окружение["DAN_AES"] = вариант
        р = subprocess.run([self.программа], capture_output=True, text=True, encoding="utf-8",
                           timeout=300, env=окружение)
        self.assertEqual(р.returncode, 0, р.stderr)
        return р.stdout.splitlines()

    def test_s_блок_из_определения(self):
        # проверка самого образца: известные значения S-блока FIPS-197
        т = таблица_aes()
        self.assertEqual((т[0x00], т[0x01], т[0x53], т[0xff]), (0x63, 0x7c, 0xed, 0x16))

    def test_варианты_совпадают_с_образцом(self):
        строки = self.запуск()
        варианты = [с.split()[1] for с in строки if с.startswith("расхождений")]
        self.assertIn("c", варианты)
        for с in строки:
            if с.startswith("fips"):
                _, вариант, биты, шифр = с.split()
                self.assertEqual(шифр, FIPS197[int(биты)], с)
            if с.startswith("расхождений"):
                self.assertEqual(с.split()[2], "0", с)
        self.assertEqual(sum(с.startswith("fips") for с in строки), 3 * len(варианты))
        # на x86-64 и ARM64 (кроме старых процессоров) есть аппаратный вариант, он выбирается сам
        if "aesni" in варианты or "armv8" in варианты:
            self.assertNotEqual(строки[0], "выбран c")

    def test_выбор_варианта(self):
        self.assertEqual(self.запуск("c")[0], "выбран c")
        self.assertIn(self.запуск("нет-такого")[0], ["выбран c", "выбран aesni", "выбран armv8"])

    def test_архив_winzip_каждым_вариантом(self):
        # архив создан другой программой (pyzipper) с ключами AES-128, AES-192 и AES-256
        текст = ('пусть файлы = извлечь_архив("winzip_aes.zip", "в", "Dan-WinZip-AES")\n'
                 'для имя в файлы { вывод(имя + " " + хеш(прочитать_байты("в/" + имя))) }')
        for вариант in ("c", "aesni", "armv8"):
            with self.subTest(вариант=вариант):
                папка = tempfile.mkdtemp()
                try:
                    shutil.copy(os.path.join(ПАПКА_ДАННЫХ, "winzip_aes.zip"), папка)
                    путь = os.path.join(папка, "п.dan")
                    with open(путь, "w", encoding="utf-8") as ф:
                        ф.write(текст)
                    исполняемый = dan.собрать(путь, текст)
                    окружение = dict(os.environ, DAN_AES=вариант)
                    р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8",
                                       timeout=120, cwd=папка, env=окружение)
                    self.assertEqual(р.returncode, 0, р.stderr)
                    self.assertEqual(р.stdout.splitlines(), [f"{и} {х}" for и, х in WINZIP_AES.items()])
                finally:
                    shutil.rmtree(папка, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
