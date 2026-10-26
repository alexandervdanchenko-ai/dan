"""Документация из комментариев «///»: страницы HTML и проверка примеров.

Комментарий из трёх косых черт, стоящий в строках прямо над объявлением
(функцией, типом, интерфейсом, полем, вариантом, методом интерфейса,
переменной верхнего уровня), - описание этого объявления. Блок «///» в самом
начале файла, отделённый от кода пустой строкой, - описание модуля.
Обычный комментарий «//» в документацию не попадает.

Текст описания записывается упрощённой разметкой Markdown: абзацы через
пустую строку, заголовки «# Примеры», списки «* » и «- », `код`, блоки кода
между строками ``` или с отступом в четыре пробела, **жирный**, _курсив_,
ссылки [текст](адрес) и ссылки на объявления [имя] или [модуль.имя].
Строки с отступом в четыре пробела - код и тогда, когда идут сразу за
абзацем без пустой строки.

    python dan.py документация [файл|папка ...] [--в=папка] [--проверить]

Без --проверить создаётся папка «документация» (или указанная) со страницей
index.html: по странице на каждый файл и общий поиск по именам. С
--проверить страницы не создаются: примеры из описаний (блоки ``` без языка
или с языком dan) собираются и выполняются как тесты вместе с тестами файла,
а общие объявления без описания перечисляются.
"""

import html
import json
import os
import re
from dataclasses import dataclass, field

from lexer import ОшибкаЛексера
from parser import разобрать, ОшибкаРазбора
import uzly as у

ЯЗЫКИ_ДАНА = ("", "dan", "дан")


@dataclass
class Описание:
    вид: str                    # функция, метод, тип, интерфейс, вариант, поле, переменная
    имя: str                    # как в программе: «площадь», «Точка.длина», «Фигура.Круг»
    подпись: str                # строка объявления
    текст: str                  # текст описания без «///»
    строка: int
    общий: bool = False
    части: list = field(default_factory=list)      # поля, варианты, методы интерфейса

    @property
    def кратко(self):
        """Первый абзац описания одной строкой."""
        абзац = []
        for с in self.текст.split("\n"):
            if not с.strip() or с.startswith("    ") or с.lstrip().startswith(("#", "```")):
                break
            абзац.append(с.strip())
        return " ".join(абзац)

    @property
    def якорь(self):
        return self.имя


@dataclass
class Модуль:
    имя: str
    путь: str
    текст: str                  # описание модуля
    описания: list
    строка_описания: int = 0


# ---------- извлечение ----------

def _комментарий(строки, номер):
    """Строки «///» прямо над строкой номер (от 1); (текст, номер первой строки)."""
    собрано = []
    н = номер - 2
    while н >= 0 and строки[н].lstrip().startswith("///"):
        собрано.append(строки[н])
        н -= 1
    if not собрано:
        return "", 0
    собрано.reverse()
    return _без_черт(собрано), н + 2


def _без_черт(строки):
    итог = []
    for с in строки:
        с = с.lstrip()[3:]
        итог.append(с[1:] if с.startswith(" ") else с)
    return "\n".join(итог).strip("\n")


def _описание_модуля(строки):
    """Блок «///» в начале файла (после пустых строк и обычных комментариев), за которым пустая строка."""
    н = 0
    while н < len(строки) and (not строки[н].strip() or строки[н].lstrip().startswith("//")
                               and not строки[н].lstrip().startswith("///")):
        н += 1
    начало = н
    while н < len(строки) and строки[н].lstrip().startswith("///"):
        н += 1
    if н == начало or (н < len(строки) and строки[н].strip()):
        return "", 0
    return _без_черт(строки[начало:н]), начало + 1


def _параметры_типа(п):
    if not п:
        return ""
    return "<" + ", ".join(т.имя + (": " + " и ".join(т.ограничения) if т.ограничения else "") for т in п) + ">"


def _подпись_функции(ф, имя):
    # у метода «функ Тип.имя(...)» первый параметр «сам» добавлен разбором, в программе его не пишут
    параметры = ", ".join(f"{п.имя}: {п.тип}" for п in ф.параметры
                          if not (getattr(ф, "метод_типа", None) and п.имя == "сам"))
    результат = f" -> {ф.тип_результата}" if ф.тип_результата else ""
    return f"функ {имя}{_параметры_типа(getattr(ф, 'параметры_типа', []))}({параметры}){результат}"


def извлечь(исходник, путь="программа.dan"):
    """Описания объявлений файла. Ошибки записи программы передаются вызывающему."""
    программа = разобрать(исходник)
    строки = исходник.split("\n")
    текст_модуля, строка_модуля = _описание_модуля(строки)
    описания = []

    def док(узел):
        текст, первая = _комментарий(строки, узел.строка)
        # блок в начале файла с пустой строкой после него - описание модуля, а не объявления
        return "" if текст and первая == строка_модуля else текст

    for и in программа.инструкции:
        общий = getattr(и, "общий", False)
        if isinstance(и, у.Функция):
            if getattr(и, "тест", None):
                continue
            имя = f"{и.метод_типа}.{и.имя}" if и.метод_типа else и.имя
            описания.append(Описание("метод" if и.метод_типа else "функция", имя,
                                     _подпись_функции(и, имя), док(и), и.строка, общий))
        elif isinstance(и, у.ОбъявлениеТипа):
            имя = и.написание or и.имя
            части = [Описание("поле", f"{имя}.{п.написание or п.имя}", f"{п.написание or п.имя}: {п.тип}",
                              док(п), п.строка) for п in и.поля]
            описания.append(Описание("тип", имя, f"тип {имя}{_параметры_типа(и.параметры_типа)}",
                                     док(и), и.строка, общий, части))
        elif isinstance(и, у.ОбъявлениеВариантов):
            имя = и.написание or и.имя
            части = []
            for в in и.варианты:
                поля = "(" + ", ".join(f"{п.написание or п.имя}: {п.тип}" for п in в.поля) + ")" if в.поля else ""
                части.append(Описание("вариант", f"{имя}.{в.написание or в.имя}",
                                      f"{в.написание or в.имя}{поля}", док(в), в.строка))
            описания.append(Описание("тип", имя, f"тип {имя}{_параметры_типа(и.параметры_типа)}",
                                     док(и), и.строка, общий, части))
        elif isinstance(и, у.ОбъявлениеИнтерфейса):
            имя = и.написание or и.имя
            части = [Описание("метод", f"{имя}.{м.имя}", _подпись_функции(м, м.имя), док(м), м.строка)
                     for м in и.методы]
            описания.append(Описание("интерфейс", имя, f"интерфейс {имя}", док(и), и.строка, общий, части))
        elif isinstance(и, у.ОбъявлениеПеременной):
            тип = f": {и.тип}" if и.тип else ""
            описания.append(Описание("переменная", и.имя, f"{'перем' if и.изменяемая else 'пусть'} {и.имя}{тип}",
                                     док(и), и.строка))
    имя_модуля = os.path.splitext(os.path.basename(путь))[0]
    return Модуль(имя_модуля, путь, текст_модуля, описания, строка_модуля)


def показываемые(модуль):
    """Что попадает на страницу: общие объявления и объявления с описанием."""
    return [о for о in модуль.описания if о.общий or о.текст or any(ч.текст for ч in о.части)]


# ---------- разметка ----------

def _строчная(текст, ссылка):
    """Разметка внутри строки: `код`, **жирный**, _курсив_, [текст](адрес), [имя]."""
    итог = []
    for часть in re.split(r"(`[^`]+`)", текст):
        if часть.startswith("`") and часть.endswith("`") and len(часть) > 1:
            итог.append(f"<code>{html.escape(часть[1:-1])}</code>")
            continue
        ч = html.escape(часть, quote=False)
        ч = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", ч)
        ч = re.sub(r"(?<![\w])_(.+?)_(?![\w])", r"<i>\1</i>", ч)

        def адрес(с):
            а = с.group(2)
            if not re.match(r"(https?://|mailto:|#|[\w./-]+\.html)", а):
                return с.group(0)
            return f'<a href="{html.escape(а)}">{с.group(1)}</a>'
        ч = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", адрес, ч)

        def имя(с):
            цель = ссылка(с.group(1))
            return f'<a href="{цель}"><code>{с.group(1)}</code></a>' if цель else с.group(0)
        ч = re.sub(r"\[([^\W\d][\w.]*)\](?!\()", имя, ч)
        итог.append(ч)
    return "".join(итог)


def в_html(текст, ссылка=lambda имя: None):
    """Упрощённый Markdown -> HTML."""
    строки = текст.split("\n")
    итог, абзац, список = [], [], []

    def закрыть():
        if абзац:
            итог.append("<p>" + _строчная(" ".join(с.strip() for с in абзац), ссылка) + "</p>")
            абзац.clear()
        if список:
            итог.append("<ul>" + "".join(f"<li>{_строчная(п, ссылка)}</li>" for п in список) + "</ul>")
            список.clear()

    н = 0
    while н < len(строки):
        с = строки[н]
        if с.strip().startswith("```"):
            закрыть()
            код = []
            н += 1
            while н < len(строки) and not строки[н].strip().startswith("```"):
                код.append(строки[н])
                н += 1
            итог.append("<pre><code>" + html.escape("\n".join(код)) + "</code></pre>")
        elif с.startswith("    ") and not список:
            # строки с отступом в четыре пробела - код, даже сразу после абзаца
            закрыть()
            код = []
            while н < len(строки) and (строки[н].startswith("    ") or not строки[н].strip()):
                код.append(строки[н][4:])
                н += 1
            while код and not код[-1].strip():
                код.pop()
            итог.append("<pre><code>" + html.escape("\n".join(код)) + "</code></pre>")
            continue
        elif not с.strip():
            закрыть()
        elif re.match(r"#{1,4} ", с):
            закрыть()
            итог.append(f"<h4>{_строчная(с.lstrip('#').strip(), ссылка)}</h4>")
        elif re.match(r"\s*[*-] ", с):
            if абзац:
                закрыть()
            список.append(с.strip()[2:].strip())
        elif список and с.startswith("  "):
            список[-1] += " " + с.strip()
        else:
            if список:
                закрыть()
            абзац.append(с)
        н += 1
    закрыть()
    return "\n".join(итог)


def примеры(текст):
    """Блоки кода на Дане в описании: (номер строки внутри описания, код)."""
    итог = []
    строки = текст.split("\n")
    н = 0
    while н < len(строки):
        с = строки[н].strip()
        if с.startswith("```"):
            язык = с[3:].strip().lower()
            начало = н
            код = []
            н += 1
            while н < len(строки) and not строки[н].strip().startswith("```"):
                код.append(строки[н])
                н += 1
            if язык in ЯЗЫКИ_ДАНА:
                итог.append((начало, "\n".join(код)))
        н += 1
    return итог


# ---------- страницы ----------

СТИЛЬ = """
:root { --фон: #fbfaf7; --текст: #1d1d1f; --тихий: #6b6b70; --рамка: #e3e1da; --код: #f1efe8; --ссылка: #1f5fbf; }
@media (prefers-color-scheme: dark) {
  :root { --фон: #18181a; --текст: #e8e6e1; --тихий: #a0a0a6; --рамка: #34343a; --код: #232327; --ссылка: #7fb0ff; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--фон); color: var(--текст); font: 16px/1.55 system-ui, sans-serif; }
main { max-width: 860px; margin: 0 auto; padding: 24px 16px 64px; }
a { color: var(--ссылка); text-decoration: none; } a:hover { text-decoration: underline; }
h1 { font-size: 28px; margin: 8px 0 4px; } h2 { font-size: 20px; margin: 32px 0 8px; }
h4 { font-size: 15px; margin: 16px 0 4px; }
code, pre { font: 14px/1.45 ui-monospace, Consolas, monospace; background: var(--код); border-radius: 4px; }
code { padding: 1px 4px; } pre { padding: 10px 12px; overflow-x: auto; } pre code { padding: 0; }
.путь { color: var(--тихий); font-size: 14px; }
.объявление { border-top: 1px solid var(--рамка); padding: 12px 0; }
.подпись { font-weight: 600; background: none; padding: 0; font-size: 15px; word-break: break-word; }
.вид { color: var(--тихий); font-size: 13px; margin-left: 6px; }
.части { margin: 8px 0 0 16px; } .части .подпись { font-weight: 500; }
.нет { color: var(--тихий); font-style: italic; }
#поиск { width: 100%; padding: 8px 10px; font-size: 16px; border: 1px solid var(--рамка); border-radius: 6px;
         background: var(--фон); color: var(--текст); margin: 12px 0; }
ul.список { list-style: none; padding: 0; } ul.список li { padding: 4px 0; border-bottom: 1px solid var(--рамка); }
"""

ПОИСК = """
const поле = document.getElementById("поиск"), итоги = document.getElementById("найдено");
поле.addEventListener("input", () => {
  const ч = поле.value.trim().toLowerCase().replaceAll("ё", "е");
  итоги.innerHTML = "";
  if (!ч) { итоги.hidden = true; document.getElementById("модули").hidden = false; return; }
  итоги.hidden = false; document.getElementById("модули").hidden = true;
  for (const з of ИНДЕКС.filter(з => (з.имя + " " + з.кратко).toLowerCase().replaceAll("ё", "е").includes(ч)).slice(0, 200)) {
    const li = document.createElement("li"), a = document.createElement("a");
    a.href = з.адрес; a.textContent = з.модуль + "." + з.имя;
    li.append(a, document.createTextNode(з.кратко ? " - " + з.кратко : ""));
    итоги.append(li);
  }
});
"""


def _страница(заголовок, тело, скрипт=""):
    return ("<!doctype html>\n<html lang=\"ru\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            f"<title>{html.escape(заголовок)}</title><link rel=\"stylesheet\" href=\"стиль.css\"></head>"
            f"<body><main>{тело}</main>{скрипт}</body></html>\n")


def _файл_страницы(модуль):
    return модуль.имя + ".html"


def создать(модули, папка):
    """Записать страницы в папку; вернуть список созданных файлов."""
    os.makedirs(папка, exist_ok=True)
    имена = {}                                     # «модуль.имя» и «имя» -> адрес
    for м in модули:
        for о in показываемые(м):
            for з in [о, *о.части]:
                имена[f"{м.имя}.{з.имя}"] = f"{_файл_страницы(м)}#{з.якорь}"

    файлы = []
    индекс = []
    for м in модули:
        свои = {о.имя: f"#{о.якорь}" for о in показываемые(м)}
        свои.update({ч.имя: f"#{ч.якорь}" for о in показываемые(м) for ч in о.части})

        def ссылка(имя, свои=свои):
            return свои.get(имя) or имена.get(имя)

        тело = [f'<p class="путь"><a href="index.html">Все модули</a></p><h1>{html.escape(м.имя)}</h1>',
                f'<p class="путь">{html.escape(м.путь)}</p>']
        if м.текст:
            тело.append(в_html(м.текст, ссылка))
        показ = показываемые(м)
        if not показ:
            тело.append('<p class="нет">В файле нет общих объявлений и описаний.</p>')
        for о in показ:
            тело.append(_объявление(о, ссылка))
            индекс.append({"модуль": м.имя, "имя": о.имя, "кратко": о.кратко,
                           "адрес": f"{_файл_страницы(м)}#{о.якорь}"})
        путь = os.path.join(папка, _файл_страницы(м))
        with open(путь, "w", encoding="utf-8", newline="\n") as ф:
            ф.write(_страница(f"{м.имя} - документация", "\n".join(тело)))
        файлы.append(путь)

    строки = "".join(f'<li><a href="{html.escape(_файл_страницы(м))}">{html.escape(м.имя)}</a>'
                     + (f" - {html.escape(_кратко(м.текст))}" if м.текст else "") + "</li>" for м in модули)
    тело = ('<h1>Документация</h1><input id="поиск" type="search" placeholder="Найти функцию или тип" autofocus>'
            f'<ul class="список" id="найдено" hidden></ul><ul class="список" id="модули">{строки}</ul>')
    данные = json.dumps(индекс, ensure_ascii=False).replace("</", "<\\/")
    скрипт = f"<script>const ИНДЕКС = {данные};{ПОИСК}</script>"
    путь = os.path.join(папка, "index.html")
    with open(путь, "w", encoding="utf-8", newline="\n") as ф:
        ф.write(_страница("Документация", тело, скрипт))
    with open(os.path.join(папка, "стиль.css"), "w", encoding="utf-8", newline="\n") as ф:
        ф.write(СТИЛЬ.lstrip())
    return [путь, *файлы]


def _кратко(текст):
    return Описание("", "", "", текст, 0).кратко


def _объявление(о, ссылка):
    вид = f'<span class="вид">{о.вид}</span>' if о.вид in ("тип", "интерфейс", "переменная") else ""
    части = [f'<div class="объявление" id="{html.escape(о.якорь)}">',
             f'<code class="подпись">{html.escape(о.подпись)}</code>{вид}']
    if о.текст:
        части.append(в_html(о.текст, ссылка))
    if о.части:
        части.append('<div class="части">')
        for ч in о.части:
            части.append(f'<div id="{html.escape(ч.якорь)}"><code class="подпись">{html.escape(ч.подпись)}</code>'
                         + (в_html(ч.текст, ссылка) if ч.текст else "") + "</div>")
        части.append("</div>")
    части.append("</div>")
    return "\n".join(части)


# ---------- проверка ----------

def текст_проверки(исходник, модуль):
    """Исходник файла с тестами из примеров описаний и соответствие строк:
    (текст, {строка в тексте: строка примера в файле}); (None, {}), если примеров нет."""
    строки_файла = исходник.split("\n")
    итог = исходник.rstrip("\n").split("\n")
    соответствие = {}
    for о in модуль.описания:
        for з in [о, *о.части]:
            if not з.текст:
                continue
            _, первая = _комментарий(строки_файла, з.строка)
            сдвиг = 0
            while _без_черт([строки_файла[первая - 1 + сдвиг]]) == "":
                сдвиг += 1
            for номер, код in примеры(з.текст):
                if not код.strip():
                    continue
                название = f"пример к {з.имя}".replace('"', "'").replace("\\", "/")
                итог += ["", f'тест "{название}" {{']
                for н, с in enumerate(код.split("\n")):
                    итог.append("    " + с)
                    соответствие[len(итог)] = первая + сдвиг + номер + 1 + н
                итог.append("}")
    if not соответствие:
        return None, {}
    return "\n".join(итог) + "\n", соответствие


def переписать_строки(текст, соответствие):
    """Номера строк проверочного текста в сообщениях -> номера строк исходного файла."""
    def замена(с):
        н = int(с.group(2))
        return f"{с.group(1)} {соответствие[н]}" if н in соответствие else с.group(0)
    return re.sub(r"([Сс]трока) (\d+)", замена, текст)


def без_описания(модуль):
    """Общие объявления без описания: строки «строка N: подпись»."""
    return [f"строка {о.строка}: {о.подпись}" for о in модуль.описания if о.общий and not о.текст]


def файлы_целей(цели):
    файлы = []
    for цель in цели or ["."]:
        if os.path.isdir(цель):
            файлы += sorted(os.path.join(цель, и) for и in os.listdir(цель) if и.endswith(".dan"))
        else:
            файлы.append(цель)
    return файлы


def команда(аргументы, собрать=None, ошибки=()):
    """dan документация [файл|папка ...] [--в=папка] [--проверить]."""
    import subprocess
    import sys
    папка = "документация"
    проверить = False
    цели = []
    for а in аргументы:
        if а.startswith("--в="):
            папка = а[len("--в="):]
        elif а == "--проверить":
            проверить = True
        elif а.startswith("--"):
            print(f"Неизвестный параметр: {а}")
            return 2
        else:
            цели.append(а)
    модули, исходники, плохо = [], {}, 0
    for путь in файлы_целей(цели):
        try:
            with open(путь, encoding="utf-8") as ф:
                исходник = ф.read()
            модуль = извлечь(исходник, путь)
        except (OSError, UnicodeDecodeError) as о:
            print(f"Не удалось открыть файл «{путь}»: {о}")
            плохо += 1
            continue
        except (ОшибкаЛексера, ОшибкаРазбора) as о:
            print(f"{путь}: {о}")
            плохо += 1
            continue
        модули.append(модуль)
        исходники[путь] = исходник
    if not модули:
        print("Файлов программ не найдено")
        return 1
    if not проверить:
        файлы = создать(модули, папка)
        print(f"Готово: {файлы[0]} (страниц: {len(файлы)})")
        return 1 if плохо else 0

    примеров = 0
    for м in модули:
        for строка in без_описания(м):
            print(f"{м.путь}: нет описания: {строка}")
        текст, соответствие = текст_проверки(исходники[м.путь], м)
        if текст is None:
            continue
        print(f"== {м.путь}: примеры")
        sys.stdout.flush()
        # отдельный файл рядом с исходным: подключённые модули находятся так же
        временный = os.path.join(os.path.dirname(os.path.abspath(м.путь)), f".{м.имя}_примеры.dan")
        try:
            with open(временный, "w", encoding="utf-8", newline="\n") as ф:
                ф.write(текст)
            try:
                исполняемый = собрать(временный, текст, тесты=True)
            except ошибки as о:
                print(f"{м.путь}: пример не собирается: {переписать_строки(str(о), соответствие)}")
                плохо += 1
                основа = os.path.splitext(временный)[0] + "_тесты"
                for лишний in (основа + ".c", основа + ".exe"):
                    if os.path.exists(лишний):
                        os.remove(лишний)
                continue
            выполнение = subprocess.run([os.path.abspath(исполняемый)], capture_output=True, text=True,
                                        encoding="utf-8", errors="replace",
                                        cwd=os.path.dirname(os.path.abspath(м.путь)))
            print(переписать_строки(выполнение.stdout, соответствие), end="")
            print(выполнение.stderr, end="", file=sys.stderr)
            код = выполнение.returncode
            примеров += 1
            if код != 0:
                плохо += 1
            for лишний in (исполняемый, os.path.splitext(исполняемый)[0] + ".c"):
                try:
                    os.remove(лишний)
                except OSError:
                    pass
        finally:
            try:
                os.remove(временный)
            except OSError:
                pass
    if not примеров and not плохо:
        print("Примеров кода в описаниях не найдено")
    return 1 if плохо else 0

