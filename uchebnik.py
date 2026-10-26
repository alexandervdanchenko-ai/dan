"""Учебник «Дан с нуля»: проверка программ и сборка книги.

Главы лежат в «учебник/главы/NN_название.md». Программы из глав - отдельными
файлами в «учебник/листинги/NN/имя.dan», в тексте главы на их месте стоит
строка «{{листинг NN/имя}}». Рядом с программой лежат:
    имя.вывод          - что она печатает (книга показывает его под программой);
    имя.ввод           - что вводит человек (если программа спрашивает);
    имя.только_сборка  - программа ждёт человека или сети: только собирается;
    имя.модуль         - модуль для подключения: только проверяется.

    python uchebnik.py вывод          - выполнить все программы и записать .вывод
    python uchebnik.py html папка     - собрать книгу в HTML в указанную папку

Тест test_uchebnik.py проверяет, что каждая программа книги собирается и
печатает ровно то, что показано в книге.
"""

import glob
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile

import dan

КОРЕНЬ = os.path.dirname(os.path.abspath(__file__))
УЧЕБНИК = os.path.join(КОРЕНЬ, "учебник")
ГЛАВЫ = os.path.join(УЧЕБНИК, "главы")
ЛИСТИНГИ = os.path.join(УЧЕБНИК, "листинги")
МЕТКА = re.compile(r"^\{\{листинг (\S+)\}\}$", re.M)


def листинги():
    """Имена вида «NN/имя» всех программ учебника."""
    return sorted(os.path.relpath(п, ЛИСТИНГИ)[:-4].replace(os.sep, "/")
                  for п in glob.glob(os.path.join(ЛИСТИНГИ, "*", "*.dan")))


def путь(имя, окончание=".dan"):
    return os.path.join(ЛИСТИНГИ, *имя.split("/")) + окончание


def вид(имя):
    for в in ("модуль", "только_сборка"):
        if os.path.exists(путь(имя, "." + в)):
            return в
    return "выполнить"


def прочитать(п):
    with open(п, encoding="utf-8") as ф:
        return ф.read()


def выполнить(имя):
    """(код завершения, вывод, ошибки). Программа собирается в копии папки главы,
    чтобы подключаемые модули и создаваемые файлы были рядом."""
    with tempfile.TemporaryDirectory() as папка:
        глава = os.path.dirname(путь(имя))
        for п in glob.glob(os.path.join(глава, "*.dan")):
            shutil.copy(п, папка)
        копия = os.path.join(папка, os.path.basename(путь(имя)))
        текст = прочитать(копия)
        if вид(имя) == "модуль":
            dan.проверить(dan.загрузить(текст, копия))
            return 0, "", ""
        if вид(имя) == "только_сборка":
            dan.перевести(текст, копия)
            return 0, "", ""
        исполняемый = os.path.abspath(dan.собрать(копия, текст))
        ввод = прочитать(путь(имя, ".ввод")) if os.path.exists(путь(имя, ".ввод")) else ""
        р = subprocess.run([исполняемый], input=ввод, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=120, cwd=папка)
        return р.returncode, р.stdout, р.stderr


def обновить_выводы():
    for имя in листинги():
        if вид(имя) != "выполнить":
            continue
        код, вывод, ошибки = выполнить(имя)
        if код != 0:
            raise RuntimeError(f"{имя}: программа завершилась с кодом {код}:\n{ошибки}")
        with open(путь(имя, ".вывод"), "w", encoding="utf-8", newline="\n") as ф:
            ф.write(вывод)
        print(f"{имя}: {len(вывод.splitlines())} строк вывода")


def главы():
    return sorted(glob.glob(os.path.join(ГЛАВЫ, "*.md")))


def заголовок(текст):
    первая = текст.splitlines()[0]
    return первая.lstrip("# ").strip()


def подставить(текст):
    """Текст главы в Markdown с подставленными программами и их выводом."""
    def замена(м):
        имя = м.group(1)
        if not os.path.exists(путь(имя)):
            raise FileNotFoundError(f"нет программы {имя}")
        код = прочитать(путь(имя)).rstrip("\n")
        части = [f"Файл `{os.path.basename(путь(имя))}`:", "", "```dan", код, "```"]
        if os.path.exists(путь(имя, ".ввод")):
            части += ["", "Человек вводит:", "", "```", прочитать(путь(имя, ".ввод")).rstrip("\n"), "```"]
        if os.path.exists(путь(имя, ".вывод")):
            части += ["", "Программа печатает:", "", "```", прочитать(путь(имя, ".вывод")).rstrip("\n"), "```"]
        return "\n".join(части)
    return МЕТКА.sub(замена, текст)


СТИЛЬ = """
:root{--фон:#fbfbf8;--текст:#1d1d1f;--тихий:#6b6b70;--рамка:#dcdcd6;--код:#f1f0ea;--акцент:#1f5fa8}
@media (prefers-color-scheme:dark){:root{--фон:#18181a;--текст:#e8e8e6;--тихий:#a0a0a6;--рамка:#3a3a3e;
--код:#232326;--акцент:#7fb0ef}}
body{background:var(--фон);color:var(--текст);font:17px/1.6 "Segoe UI",Arial,sans-serif;max-width:46em;
margin:0 auto;padding:1.5em 1em 4em}
a{color:var(--акцент)}h1{font-size:1.7em;line-height:1.25}h2{margin-top:1.8em}
pre{background:var(--код);border:1px solid var(--рамка);border-radius:6px;padding:.8em 1em;overflow-x:auto;
font:15px/1.45 Consolas,"Cascadia Mono",monospace}
code{font-family:Consolas,"Cascadia Mono",monospace;background:var(--код);padding:0 .2em;border-radius:3px}
pre code{background:none;padding:0}
nav{display:flex;justify-content:space-between;gap:1em;border-top:1px solid var(--рамка);margin-top:3em;
padding-top:1em;font-size:.95em}
.оглавление li{margin:.25em 0}.путь{color:var(--тихий);font-size:.9em}
"""


def страница(заголовок_, тело, назад=None, вперёд=None):
    навигация = ""
    if назад or вперёд:
        л = f'<a href="{назад[0]}">← {html.escape(назад[1])}</a>' if назад else "<span></span>"
        п = f'<a href="{вперёд[0]}">{html.escape(вперёд[1])} →</a>' if вперёд else "<span></span>"
        навигация = f"<nav>{л}{п}</nav>"
    return (f'<!doctype html><html lang="ru"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width, initial-scale=1">'
            f"<title>{html.escape(заголовок_)}</title><style>{СТИЛЬ}</style></head><body>"
            f'<p class="путь"><a href="index.html">Дан с нуля</a></p>{тело}{навигация}</body></html>')


def собрать_html(папка):
    import markdown
    os.makedirs(папка, exist_ok=True)
    список = [(os.path.basename(г)[:-3] + ".html", прочитать(г)) for г in главы()]
    оглавление = []
    for н, (файл, текст) in enumerate(список):
        тело = markdown.markdown(подставить(текст), extensions=["fenced_code", "tables"])
        назад = (список[н - 1][0], заголовок(список[н - 1][1])) if н > 0 else None
        вперёд = (список[н + 1][0], заголовок(список[н + 1][1])) if н + 1 < len(список) else None
        with open(os.path.join(папка, файл), "w", encoding="utf-8") as ф:
            ф.write(страница(заголовок(текст), тело, назад, вперёд))
        оглавление.append(f'<li><a href="{файл}">{html.escape(заголовок(текст))}</a></li>')
    тело = ("<h1>Дан с нуля</h1><p>Учебник программирования на языке Дан для тех, кто никогда не программировал. "
            "Каждая программа из книги проверена: она собирается и печатает ровно то, что показано в тексте.</p>"
            "<p>После глав решайте задачи задачника: команда <code>dan задачи</code> покажет их список, "
            "<code>dan задача 1</code> проверит решение первой.</p>"
            f'<ol class="оглавление">{"".join(оглавление)}</ol>')
    with open(os.path.join(папка, "index.html"), "w", encoding="utf-8") as ф:
        ф.write(страница("Дан с нуля", тело, вперёд=(список[0][0], заголовок(список[0][1]))))
    return os.path.join(папка, "index.html")


if __name__ == "__main__":
    if sys.argv[1:] == ["вывод"]:
        обновить_выводы()
    elif len(sys.argv) == 3 and sys.argv[1] == "html":
        print(собрать_html(sys.argv[2]))
    else:
        print(__doc__.strip())
        sys.exit(2)
