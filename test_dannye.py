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

КОМПИЛЯТОР_ЕСТЬ = any(shutil.which(к) for к in dan.КОМПИЛЯТОРЫ)

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


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыОфициальныхПримеров(unittest.TestCase):
    """Сверка шифрования библиотеки выполнения с примерами из RFC, без сторонних библиотек.

    Функции библиотеки выполнения вызываются напрямую: к переведённой программе
    добавляется небольшая программа на C, печатающая результаты в шестнадцатеричном виде.
    """

    def test_примеры_rfc(self):
        код = dan.перевести('пусть з = зашифровать("а", "б")\nвывод(стр(расшифровать(з, "б")))')
        обвязка = ["#include <stdio.h>",
                   "static void печать(const uint8_t *д, size_t n) {",
                   "    for (size_t i = 0; i < n; i++) printf(\"%02x\", д[i]);",
                   "    printf(\"\\n\");",
                   "}"]
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
        текст_c = ("#define main dan_program_main\n" + код + "\n#undef main\n#include <string.h>\n"
                   + "\n".join(обвязка) + "\n")
        папка = tempfile.mkdtemp()
        try:
            путь_c = os.path.join(папка, "rfc.c")
            исполняемый = os.path.join(папка, "rfc.exe" if os.name == "nt" else "rfc")
            with open(путь_c, "w", encoding="utf-8") as ф:
                ф.write(текст_c)
            сборка = subprocess.run([dan.найти_компилятор(), *dan.ФЛАГИ_ОБЩИЕ, *dan.ФЛАГИ_СКОРОСТЬ,
                                     путь_c, "-o", исполняемый, *dan.БИБЛИОТЕКИ],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(сборка.returncode, 0, сборка.stderr)
            р = subprocess.run([исполняемый], capture_output=True, text=True, timeout=120)
            self.assertEqual(р.returncode, 0, р.stderr)
            строки = р.stdout.split()
        finally:
            shutil.rmtree(папка, ignore_errors=True)
        self.assertEqual(строки[0], RFC8439_ШИФР)
        self.assertEqual(строки[1], RFC8439_КОД)
        self.assertEqual(строки[2:], [ожидается for _, _, _, ожидается in RFC7914])


if __name__ == "__main__":
    unittest.main()
