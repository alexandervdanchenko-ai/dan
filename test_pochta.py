"""Тесты модуля почты (библиотека/почта.dan) и разбора писем (MIME).

Письма Дана разбираются модулем email языка Python; письма Python (с разными
кодировками, вложениями с русскими именами, вложенными частями) разбираются
Даном. Отправка и чтение проверяются с локальными серверами SMTP и IMAP,
написанными здесь же (без шифрования: защита = "нет").
"""

import email
import email.policy
import os
import socket
import socketserver
import tempfile
import threading
import unittest
from email.message import EmailMessage
from email.mime.application import MIMEApplication
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.header import Header

from test_kodogen import КОМПИЛЯТОР_ЕСТЬ
from test_word import выполнить


def библиотека_curl_есть():
    import ctypes.util
    return ctypes.util.find_library("curl") is not None


# ---------- серверы для проверки ----------

class СерверSMTP(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        self.письма, self.команды = [], []
        super().__init__(("127.0.0.1", 0), ОбработчикSMTP)


class ОбработчикSMTP(socketserver.StreamRequestHandler):
    def ответ(self, текст):
        self.wfile.write((текст + "\r\n").encode())

    def handle(self):
        с = self.server
        self.ответ("220 тест ESMTP".encode("utf-8").decode("latin-1"))
        отправитель, получатели = None, []
        while True:
            строка = self.rfile.readline()
            if not строка:
                return
            команда = строка.decode("utf-8", "replace").rstrip("\r\n")
            с.команды.append(команда)
            вверх = команда.upper()
            if вверх.startswith("EHLO") or вверх.startswith("HELO"):
                self.ответ("250-тест".encode("utf-8").decode("latin-1"))
                self.ответ("250-AUTH PLAIN LOGIN")
                self.ответ("250 8BITMIME")
            elif вверх.startswith("AUTH PLAIN"):
                части = команда.split(" ")
                if len(части) < 3:
                    self.ответ("334 ")
                    данные = self.rfile.readline().decode().strip()
                else:
                    данные = части[2]
                import base64
                _, имя, пароль = base64.b64decode(данные).decode().split("\0")
                if пароль == "верный":
                    self.ответ("235 OK")
                else:
                    self.ответ("535 неверный пароль".encode("utf-8").decode("latin-1"))
            elif вверх.startswith("MAIL FROM:"):
                отправитель, получатели = команда[10:].strip(), []
                self.ответ("250 OK")
            elif вверх.startswith("RCPT TO:"):
                получатели.append(команда[8:].strip())
                self.ответ("250 OK")
            elif вверх == "DATA":
                self.ответ("354 продолжайте".encode("utf-8").decode("latin-1"))
                данные = b""
                while True:
                    л = self.rfile.readline()
                    if л in (b".\r\n", b".\n", b""):
                        break
                    if л.startswith(b".."):
                        л = л[1:]
                    данные += л
                с.письма.append((отправитель, получатели, данные))
                self.ответ("250 принято".encode("utf-8").decode("latin-1"))
            elif вверх == "QUIT":
                self.ответ("221 до свидания".encode("utf-8").decode("latin-1"))
                return
            else:
                self.ответ("250 OK")


class СерверIMAP(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, письма):
        self.письма = письма        # номер (UID) -> (байты письма, прочитано)
        self.команды = []
        super().__init__(("127.0.0.1", 0), ОбработчикIMAP)


class ОбработчикIMAP(socketserver.StreamRequestHandler):
    def ответ(self, данные):
        self.wfile.write(данные if isinstance(данные, bytes) else (данные + "\r\n").encode())

    def handle(self):
        с = self.server
        self.ответ("* OK IMAP4rev1 тест".encode("utf-8").decode("latin-1"))
        while True:
            строка = self.rfile.readline()
            if not строка:
                return
            команда = строка.decode("utf-8", "replace").rstrip("\r\n")
            с.команды.append(команда)
            метка, _, остаток = команда.partition(" ")
            вверх = остаток.upper()
            if вверх.startswith("CAPABILITY"):
                self.ответ("* CAPABILITY IMAP4rev1")
                self.ответ(f"{метка} OK")
            elif вверх.startswith("LOGIN"):
                if остаток.split(" ")[-1].strip('"') == "верный":
                    self.ответ(f"{метка} OK вход".encode("utf-8").decode("latin-1"))
                else:
                    self.ответ(f"{метка} NO [AUTHENTICATIONFAILED] неверно".encode("utf-8").decode("latin-1"))
            elif вверх.startswith("SELECT") or вверх.startswith("EXAMINE"):
                папка = остаток.split(" ", 1)[1].strip('"')
                if папка not in ("INBOX", "&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-"):
                    self.ответ(f"{метка} NO нет папки".encode("utf-8").decode("latin-1"))
                    continue
                self.ответ(f"* {len(с.письма)} EXISTS")
                self.ответ("* OK [UIDVALIDITY 1]")
                self.ответ(f"{метка} OK [READ-WRITE]")
            elif вверх.startswith("UID SEARCH"):
                условие = вверх.split("UTF-8 ")[-1]
                номера = [н for н, (_, прочитано) in с.письма.items() if условие == "ALL" or not прочитано]
                self.ответ("* SEARCH " + " ".join(map(str, номера)))
                self.ответ(f"{метка} OK")
            elif вверх.startswith("UID FETCH"):
                номер = int(остаток.split(" ")[2])
                if номер not in с.письма:
                    self.ответ(f"{метка} OK")
                    continue
                данные, _ = с.письма[номер]
                с.письма[номер] = (данные, True)
                self.ответ(f"* 1 FETCH (UID {номер} BODY[] {{{len(данные)}}}\r\n".encode() + данные + b")\r\n")
                self.ответ(f"{метка} OK")
            elif вверх.startswith("LIST"):
                self.ответ('* LIST (\\HasNoChildren) "/" "INBOX"')
                self.ответ('* LIST (\\HasNoChildren) "/" "&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-"')
                self.ответ('* LIST (\\HasNoChildren) "/" Archive')
                self.ответ(f"{метка} OK")
            elif вверх.startswith("LOGOUT"):
                self.ответ("* BYE")
                self.ответ(f"{метка} OK")
                return
            else:
                self.ответ(f"{метка} OK")


def запустить(сервер):
    поток = threading.Thread(target=сервер.serve_forever, daemon=True)
    поток.start()
    return сервер.server_address[1]


# ---------- письма для разбора ----------

def письма_python():
    итог = {}
    п = EmailMessage()
    п["From"] = "Иван Иванов <ivan@example.ru>"
    п["To"] = "petrov@example.ru"
    п["Subject"] = "Отчёт за октябрь"
    п.set_content("Добрый день!\nОтчёт во вложении.")
    п.add_attachment(b"%PDF-1.4 test", maintype="application", subtype="pdf", filename="Отчёт за октябрь.pdf")
    итог["utf8"] = п.as_bytes()
    м = MIMEMultipart()
    м["From"] = str(Header("Пётр Петров", "windows-1251")) + " <petr@example.ru>"
    м["Subject"] = Header("Счёт 17 от 26.10.2026", "koi8-r")
    м.attach(MIMEText("Текст в кодировке windows-1251.", "plain", "windows-1251"))
    вл = MIMEApplication("Таблица".encode("cp1251"), Name="таблица.csv")
    вл.add_header("Content-Disposition", "attachment", filename=("windows-1251", "", "таблица.csv"))
    м.attach(вл)
    итог["1251"] = м.as_bytes()
    а = MIMEMultipart("alternative")
    а["Subject"] = "=?UTF-8?Q?=D0=9F=D1=80=D0=B8=D0=B2=D0=B5=D1=82?="
    а.attach(MIMEText("Простой текст КОИ-8", "plain", "koi8-r"))
    а.attach(MIMEText("<p>Текст <b>html</b></p>", "html", "utf-8"))
    итог["koi"] = а.as_bytes()
    return итог


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыРазбораПисем(unittest.TestCase):

    def test_письма_python_разбираются_даном(self):
        with tempfile.TemporaryDirectory() as п:
            for имя, данные in письма_python().items():
                with open(os.path.join(п, имя + ".eml"), "wb") as ф:
                    ф.write(данные)
            код, вывод, ош = выполнить(п, r'''подключить "почта"
для имя в ["utf8", "1251", "koi"] {
    пусть п = почта.разобрать(прочитать_байты(имя + ".eml"))
    вывод(имя + " | " + п.от + " | " + п.тема + " | " + п.текст.обрезать() + " | " + п.html.обрезать())
    для вл в п.вложения { вывод("  " + вл.имя + " " + вл.вид + " " + стр(вл.данные.длина)) }
}
пусть сохранено = почта.разобрать(прочитать_байты("utf8.eml")).сохранить_вложения("вложения") иначе ["ошибка"]
вывод(сохранено)
вывод(стр(прочитать_байты(сохранено[0])))
''')
        self.assertEqual(код, 0, ош)
        self.assertEqual(вывод.splitlines(), [
            "utf8 | Иван Иванов <ivan@example.ru> | Отчёт за октябрь | Добрый день!",
            "Отчёт во вложении. | ",
            "  Отчёт за октябрь.pdf application/pdf 13",
            "1251 | Пётр Петров <petr@example.ru> | Счёт 17 от 26.10.2026 | Текст в кодировке windows-1251. | ",
            "  таблица.csv application/octet-stream 7",
            "koi |  | Привет | Простой текст КОИ-8 | <p>Текст <b>html</b></p>",
            '["вложения/Отчёт за октябрь.pdf"]',
            "%PDF-1.4 test"])

    def test_письмо_дана_разбирается_python(self):
        with tempfile.TemporaryDirectory() as п:
            with open(os.path.join(п, "акт.pdf"), "wb") as ф:
                ф.write(bytes(range(256)) * 10)
            код, вывод, ош = выполнить(п, r'''подключить "почта"
пусть я = почта.Ящик("ivanov@yandex.ru", "пароль", имя = "Иван Иванов")
пусть д = почта.собрать(я, почта.Письмо(["Пётр Петров <petrov@mail.ru>", "sidorov@mail.ru"],
    "Акт сверки за третий квартал 2026 года с очень длинной темой письма, которая не помещается в одно слово",
    "Добрый день!\nАкт во вложении.", копия = ["buh@mail.ru"], вложения = ["акт.pdf"], html = "<p>Добрый день!</p>")) иначе байты("")
записать("итог.eml", д)
''')
            self.assertEqual(код, 0, ош)
            with open(os.path.join(п, "итог.eml"), "rb") as ф:
                данные = ф.read()
        for строка in данные.split(b"\r\n"):
            self.assertLessEqual(len(строка), 998)
        п = email.message_from_bytes(данные, policy=email.policy.default)
        self.assertEqual(п["From"], "Иван Иванов <ivanov@yandex.ru>")
        self.assertEqual(п["To"], "Пётр Петров <petrov@mail.ru>, sidorov@mail.ru")
        self.assertEqual(п["Cc"], "buh@mail.ru")
        self.assertEqual(п["Subject"], "Акт сверки за третий квартал 2026 года с очень длинной темой письма, "
                                       "которая не помещается в одно слово")
        self.assertTrue(п["Message-ID"].endswith("@yandex.ru>"))
        self.assertIsNotNone(п["Date"])
        self.assertEqual(п.get_body(("plain",)).get_content(), "Добрый день!\nАкт во вложении.")
        self.assertEqual(п.get_body(("html",)).get_content(), "<p>Добрый день!</p>")
        вложения = list(п.iter_attachments())
        self.assertEqual(len(вложения), 1)
        self.assertEqual(вложения[0].get_filename(), "акт.pdf")
        self.assertEqual(вложения[0].get_content_type(), "application/pdf")
        self.assertEqual(вложения[0].get_content(), bytes(range(256)) * 10)


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ and библиотека_curl_есть(), "нет компилятора C или libcurl")
class ТестыСерверов(unittest.TestCase):

    def test_отправка(self):
        сервер = СерверSMTP()
        порт = запустить(сервер)
        try:
            with tempfile.TemporaryDirectory() as п:
                with open(os.path.join(п, "отчёт.txt"), "w", encoding="utf-8") as ф:
                    ф.write("Строка отчёта\n")
                код, вывод, ош = выполнить(п, f'''подключить "почта"
пусть я = почта.Ящик("ivanov@example.ru", "верный", smtp = "127.0.0.1", порт_smtp = {порт}, защита = "нет")
если ошибка о = почта.отправить(я, почта.Письмо(["petrov@example.ru"], "Отчёт", "Текст письма\\n.точка в начале строки",
        копия = ["Бух <buh@example.ru>"], скрытая_копия = ["boss@example.ru"], вложения = ["отчёт.txt"])) {{
    вывод(о)
}}
пусть плохой = почта.Ящик("ivanov@example.ru", "неверный", smtp = "127.0.0.1", порт_smtp = {порт}, защита = "нет")
если ошибка о = почта.отправить(плохой, почта.Письмо(["petrov@example.ru"], "Т", "Т")) {{ вывод(о) }}
пусть нет_сервера = почта.Ящик("ivanov@example.ru", "в", smtp = "127.0.0.1", порт_smtp = 1, защита = "нет")
если ошибка о = почта.отправить(нет_сервера, почта.Письмо(["petrov@example.ru"], "Т", "Т")) {{ вывод(о) }}
если ошибка о = почта.отправить(я, почта.Письмо(["petrov@example.ru"], "Т", "Т", вложения = ["нет.pdf"])) {{ вывод(о) }}
''')
            self.assertEqual(код, 0, ош)
            строки = вывод.splitlines()
            self.assertIn("не принял имя пользователя или пароль", строки[0])
            self.assertIn("не удалось подключиться к почтовому серверу", строки[1])
            self.assertEqual(строки[2], "вложение «нет.pdf» не найдено")
            self.assertEqual(len(сервер.письма), 1)
            отправитель, получатели, данные = сервер.письма[0]
            self.assertEqual(отправитель, "<ivanov@example.ru>")
            self.assertEqual(получатели, ["<petrov@example.ru>", "<buh@example.ru>", "<boss@example.ru>"])
            п = email.message_from_bytes(данные, policy=email.policy.default)
            self.assertNotIn("Bcc", п)                   # скрытая копия не видна получателям
            self.assertEqual(п.get_body(("plain",)).get_content(), "Текст письма\n.точка в начале строки")
            self.assertEqual(next(п.iter_attachments()).get_content(), "Строка отчёта\n")
        finally:
            сервер.shutdown()
            сервер.server_close()

    def test_чтение(self):
        письма = {н + 3: (данные, False) for н, данные in enumerate(письма_python().values())}
        сервер = СерверIMAP(письма)
        порт = запустить(сервер)
        try:
            with tempfile.TemporaryDirectory() as п:
                код, вывод, ош = выполнить(п, f'''подключить "почта"
пусть я = почта.Ящик("ivanov@example.ru", "верный", imap = "127.0.0.1", порт_imap = {порт}, защита = "нет")
вывод(почта.папки(я) иначе ["ошибка"])
пусть новые = почта.новые(я) иначе []
вывод(новые)
для номер в новые {{
    если пусть п = почта.получить(я, "INBOX", номер) {{
        вывод(стр(п.номер) + " " + п.тема + " " + стр(п.вложения.длина))
    }}
}}
вывод(почта.новые(я) иначе [-1])
вывод(почта.найти(я, "Отправленные", "ALL") иначе [-1])
если ошибка о = почта.найти(я, "Нет такой", "ALL") {{ вывод(о) }}
пусть плохой = почта.Ящик("ivanov@example.ru", "неверный", imap = "127.0.0.1", порт_imap = {порт}, защита = "нет")
если ошибка о = почта.новые(плохой) {{ вывод(о) }}
''')
            self.assertEqual(код, 0, ош)
            строки = вывод.splitlines()
            self.assertEqual(строки[:7], [
                '["INBOX", "Отправленные", "Archive"]', "[3, 4, 5]",
                "3 Отчёт за октябрь 1", "4 Счёт 17 от 26.10.2026 1", "5 Привет 0", "[]", "[3, 4, 5]"])
            self.assertIn("нет такой папки", строки[7])
            self.assertIn("не принял имя пользователя или пароль", строки[8])
        finally:
            сервер.shutdown()
            сервер.server_close()


if __name__ == "__main__":
    unittest.main()
