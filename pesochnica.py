"""Песочница: страница в браузере, где можно написать программу на Дане и сразу выполнить.

    python dan.py песочница [--порт=8080] [--без-браузера]

Запускается небольшой сервер только для этого компьютера (адрес 127.0.0.1);
страница открывается в браузере. Программа собирается и выполняется так же,
как командой «dan запустить», в отдельной временной папке; на выполнение
даётся 10 секунд, вывод ограничен мегабайтом. Значения для ввод() задаются в
поле «Ввод» (по строке на каждый вызов).

Защита: сервер отвечает только на адрес 127.0.0.1 и только запросам со
своей страницы. Страница получает случайный ключ, без которого сервер не
выполняет программы, поэтому сайт, открытый в том же браузере, не может
заставить песочницу выполнить свою программу. Программа в песочнице - обычная
программа на компьютере: она может читать и писать файлы, как любая другая.
"""

import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ВРЕМЯ = 10                 # секунд на выполнение программы
НАИБОЛЬШИЙ_ВЫВОД = 1 << 20
НАИБОЛЬШИЙ_ТЕКСТ = 1 << 20

НАЧАЛО = '''// Программа на Дане. Ctrl+Enter - выполнить.
пусть имя = "мир"
вывод("Привет, " + имя + "!")

для н в 1..5 {
    вывод(стр(н) + " в квадрате = " + стр(н * н))
}
'''

СТРАНИЦА = r"""<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Песочница Дана</title>
<style>
:root { --фон: #fbfaf7; --текст: #1d1d1f; --тихий: #6b6b70; --рамка: #d9d6cc; --поле: #ffffff; --кнопка: #1f5fbf; --ошибка: #b3261e; }
@media (prefers-color-scheme: dark) {
  :root { --фон: #18181a; --текст: #e8e6e1; --тихий: #a0a0a6; --рамка: #3a3a40; --поле: #222226; --кнопка: #5d95ef; --ошибка: #ff8a80; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--фон); color: var(--текст); font: 15px/1.5 system-ui, sans-serif; }
header { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; padding: 10px 16px; border-bottom: 1px solid var(--рамка); }
header b { font-size: 17px; margin-right: 8px; }
button, select { font: inherit; padding: 5px 12px; border-radius: 6px; border: 1px solid var(--рамка); background: var(--поле); color: var(--текст); cursor: pointer; }
button.главная { background: var(--кнопка); color: #fff; border-color: var(--кнопка); }
button:disabled { opacity: .6; cursor: default; }
main { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; padding: 12px 16px; height: calc(100vh - 58px); }
@media (max-width: 800px) { main { grid-template-columns: 1fr; height: auto; } textarea#код { min-height: 50vh; } }
.столбец { display: flex; flex-direction: column; gap: 8px; min-height: 0; }
textarea { width: 100%; font: 14px/1.5 ui-monospace, Consolas, monospace; background: var(--поле); color: var(--текст);
           border: 1px solid var(--рамка); border-radius: 6px; padding: 10px; resize: none; tab-size: 4; }
#код { flex: 1; white-space: pre; overflow-wrap: normal; overflow: auto; }
#ввод { height: 70px; }
pre#вывод { flex: 1; margin: 0; overflow: auto; white-space: pre-wrap; word-break: break-word; background: var(--поле);
            border: 1px solid var(--рамка); border-radius: 6px; padding: 10px; font: 14px/1.5 ui-monospace, Consolas, monospace; }
.ошибка { color: var(--ошибка); }
.тихо { color: var(--тихий); font-size: 13px; }
</style></head>
<body>
<header>
  <b>Песочница Дана</b>
  <button class="главная" id="выполнить" title="Ctrl+Enter">Выполнить</button>
  <button id="формат">Единый вид</button>
  <select id="примеры"><option value="">Примеры...</option></select>
  <span class="тихо" id="состояние"></span>
</header>
<main>
  <div class="столбец">
    <textarea id="код" spellcheck="false" aria-label="Текст программы"></textarea>
    <label class="тихо" for="ввод">Ввод (по строке на каждый вызов ввод())</label>
    <textarea id="ввод" spellcheck="false"></textarea>
  </div>
  <div class="столбец">
    <pre id="вывод" aria-live="polite"></pre>
  </div>
</main>
<script>
"use strict";
const КЛЮЧ = "__КЛЮЧ__";
const код = document.getElementById("код"), ввод = document.getElementById("ввод"), вывод = document.getElementById("вывод");
const состояние = document.getElementById("состояние"), кнопка = document.getElementById("выполнить");
let сохранено = null;
try { сохранено = localStorage.getItem("песочница-код"); } catch (e) {}
код.value = сохранено || __НАЧАЛО__;

async function запрос(путь, данные) {
  const ответ = await fetch(путь, { method: "POST", headers: { "Content-Type": "application/json", "X-Dan-Key": КЛЮЧ },
                                    body: JSON.stringify(данные) });
  return ответ.json();
}

async function выполнить() {
  кнопка.disabled = true; состояние.textContent = "сборка и выполнение...";
  вывод.textContent = "";
  try { localStorage.setItem("песочница-код", код.value); } catch (e) {}
  try {
    const р = await запрос("/выполнить", { текст: код.value, ввод: ввод.value });
    вывод.textContent = р.вывод;
    if (р.ошибки) { const с = document.createElement("span"); с.className = "ошибка"; с.textContent = р.ошибки; вывод.append(с); }
    состояние.textContent = р.состояние;
  } catch (e) {
    состояние.textContent = "песочница не отвечает: возможно, её окно командной строки закрыто";
  }
  кнопка.disabled = false;
}

кнопка.onclick = выполнить;
document.getElementById("формат").onclick = async () => {
  const р = await запрос("/формат", { текст: код.value });
  if (р.текст !== undefined) код.value = р.текст; else состояние.textContent = р.ошибка;
};
код.addEventListener("keydown", (с) => {
  if (с.key === "Enter" && (с.ctrlKey || с.metaKey)) { с.preventDefault(); выполнить(); }
  else if (с.key === "Tab") {
    с.preventDefault();
    const н = код.selectionStart;
    код.setRangeText("    ", н, код.selectionEnd, "end");
  }
});
const выбор = document.getElementById("примеры");
запрос("/примеры", {}).then((р) => {
  for (const имя of р.примеры) { const в = document.createElement("option"); в.value = в.textContent = имя; выбор.append(в); }
});
выбор.onchange = async () => {
  if (!выбор.value) return;
  const р = await запрос("/пример", { имя: выбор.value });
  if (р.текст !== undefined) { код.value = р.текст; вывод.textContent = ""; состояние.textContent = выбор.value; }
  выбор.value = "";
};
</script>
</body></html>
"""


def папка_примеров():
    свои = os.path.join(os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False)
                                                          else __file__)), "примеры")
    return свои if os.path.isdir(свои) else None


def список_примеров():
    папка = папка_примеров()
    if not папка:
        return []
    return sorted(и[:-4] for и in os.listdir(папка) if и.endswith(".dan"))


def выполнить(текст, ввод, собрать, ошибки_сборки):
    """Собирает и выполняет программу во временной папке: словарь для страницы."""
    папка = tempfile.mkdtemp(prefix="dan_sandbox_")
    try:
        путь = os.path.join(папка, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        начало = time.monotonic()
        try:
            исполняемый = собрать(путь, текст)
        except ошибки_сборки as о:
            return {"вывод": "", "ошибки": str(о), "состояние": "в программе ошибка"}
        собрана = time.monotonic() - начало
        процесс = subprocess.Popen([os.path.abspath(исполняемый)], cwd=папка, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        результат = {}

        def читать(поток, ключ):
            данные = поток.read(НАИБОЛЬШИЙ_ВЫВОД + 1)
            результат[ключ] = данные
            поток.read()                          # остальное отбрасывается, чтобы программа не встала

        потоки = [threading.Thread(target=читать, args=(процесс.stdout, "вывод"), daemon=True),
                  threading.Thread(target=читать, args=(процесс.stderr, "ошибки"), daemon=True)]
        for п in потоки:
            п.start()
        try:
            процесс.stdin.write(ввод.encode("utf-8"))
            процесс.stdin.close()
        except OSError:
            pass
        начало = time.monotonic()
        try:
            код = процесс.wait(timeout=ВРЕМЯ)
            прервана = False
        except subprocess.TimeoutExpired:
            процесс.kill()
            код = процесс.wait()
            прервана = True
        for п in потоки:
            п.join(5)
        прошло = time.monotonic() - начало
        вывод = результат.get("вывод", b"")
        ошибки = результат.get("ошибки", b"")
        обрезан = len(вывод) > НАИБОЛЬШИЙ_ВЫВОД
        # в Windows программа переводит строку как «\r\n»; странице нужен «\n»
        вывод = вывод[:НАИБОЛЬШИЙ_ВЫВОД].decode("utf-8", errors="replace").replace("\r\n", "\n")
        ошибки = ошибки[:НАИБОЛЬШИЙ_ВЫВОД].decode("utf-8", errors="replace").replace("\r\n", "\n")
        if обрезан:
            вывод += "\n... вывод длиннее мегабайта и показан не полностью\n"
        if прервана:
            состояние = f"программа остановлена: она работала дольше {ВРЕМЯ} секунд"
        elif код:
            состояние = f"программа завершилась с кодом {код} ({прошло:.2f} с)".replace(".", ",", 1)
        else:
            состояние = f"готово: сборка {собрана:.2f} с, выполнение {прошло:.2f} с".replace(".", ",")
        return {"вывод": вывод, "ошибки": ошибки, "состояние": состояние, "код": код}
    finally:
        shutil.rmtree(папка, ignore_errors=True)


def создать_сервер(порт, собрать, ошибки_сборки):
    ключ = secrets.token_urlsafe(24)
    страница = (СТРАНИЦА.replace("__КЛЮЧ__", ключ)
                .replace("__НАЧАЛО__", json.dumps(НАЧАЛО, ensure_ascii=False).replace("</", "<\\/")))

    class Обработчик(BaseHTTPRequestHandler):
        def log_message(self, *аргументы):
            pass

        def _свой_адрес(self):
            # защита от подмены имени сервера (DNS rebinding): только 127.0.0.1 и localhost
            хост = (self.headers.get("Host") or "").rsplit(":", 1)[0]
            return хост in ("127.0.0.1", "localhost")

        def _ответ(self, код, тип, тело):
            данные = тело.encode("utf-8")
            self.send_response(код)
            self.send_header("Content-Type", тип)
            self.send_header("Content-Length", str(len(данные)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; "
                                                        "style-src 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(данные)

        def do_GET(self):
            if not self._свой_адрес():
                return self._ответ(403, "text/plain; charset=utf-8", "доступ только с этого компьютера")
            if self.path in ("/", "/index.html"):
                return self._ответ(200, "text/html; charset=utf-8", страница)
            return self._ответ(404, "text/plain; charset=utf-8", "нет такой страницы")

        def do_POST(self):
            if not self._свой_адрес() or not secrets.compare_digest(self.headers.get("X-Dan-Key", ""), ключ):
                return self._ответ(403, "application/json", json.dumps({"ошибка": "нет доступа"}, ensure_ascii=False))
            try:
                длина = int(self.headers.get("Content-Length") or 0)
                if длина > НАИБОЛЬШИЙ_ТЕКСТ * 3:
                    raise ValueError("слишком длинный запрос")
                данные = json.loads(self.rfile.read(длина).decode("utf-8") or "{}")
            except (ValueError, UnicodeDecodeError) as о:
                return self._ответ(400, "application/json", json.dumps({"ошибка": str(о)}, ensure_ascii=False))
            путь = urllib.parse.unquote(self.path.split("?")[0])      # браузер кодирует русские буквы адреса
            if путь == "/выполнить":
                итог = выполнить(str(данные.get("текст", "")), str(данные.get("ввод", "")), собрать, ошибки_сборки)
            elif путь == "/формат":
                from formatirovanie import отформатировать, ОшибкаФорматирования
                try:
                    итог = {"текст": отформатировать(str(данные.get("текст", "")))}
                except ОшибкаФорматирования as о:
                    итог = {"ошибка": f"текст не приведён к единому виду: {о}"}
            elif путь == "/примеры":
                итог = {"примеры": список_примеров()}
            elif путь == "/пример":
                имя = str(данные.get("имя", ""))
                папка = папка_примеров()
                if папка and имя in список_примеров():
                    with open(os.path.join(папка, имя + ".dan"), encoding="utf-8") as ф:
                        итог = {"текст": ф.read()}
                else:
                    итог = {"ошибка": "нет такого примера"}
            else:
                return self._ответ(404, "application/json", json.dumps({"ошибка": "нет такого запроса"},
                                                                       ensure_ascii=False))
            self._ответ(200, "application/json; charset=utf-8", json.dumps(итог, ensure_ascii=False))

    сервер = ThreadingHTTPServer(("127.0.0.1", порт), Обработчик)
    сервер.daemon_threads = True
    сервер.ключ = ключ
    return сервер


def команда(аргументы, собрать, ошибки_сборки):
    порт = 8080
    браузер = True
    for а in аргументы:
        if а.startswith("--порт="):
            try:
                порт = int(а[len("--порт="):])
            except ValueError:
                print("Порт - число, например --порт=8080")
                return 2
        elif а == "--без-браузера":
            браузер = False
        else:
            print(__doc__.strip())
            return 2
    try:
        сервер = создать_сервер(порт, собрать, ошибки_сборки)
    except OSError:
        if порт == 8080:
            сервер = создать_сервер(0, собрать, ошибки_сборки)     # порт занят: любой свободный
        else:
            print(f"Порт {порт} занят; укажите другой: --порт=8081")
            return 1
    адрес = f"http://127.0.0.1:{сервер.server_address[1]}/"
    print(f"Песочница Дана: {адрес}\nЧтобы остановить её, закройте это окно или нажмите Ctrl+C")
    sys.stdout.flush()
    if браузер:
        import webbrowser
        webbrowser.open(адрес)
    try:
        сервер.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        сервер.server_close()
    return 0
