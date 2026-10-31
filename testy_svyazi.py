"""Какие тесты проверяют какой файл: выбор тестов для `python testy.py --изменения`.

Правило выбора для изменённого файла (путь от корня репозитория, разделитель «/»):
1. Сам файл теста `test_*.py` - этот тест.
2. Явная таблица СВЯЗИ ниже (образцы путей в записи fnmatch).
3. Модуль Python: тесты, которые подключают его прямо (`import модуль`, `from модуль import`).
4. Файл в папке, которую тест называет в кавычках (`"каталог"`, `"примеры"`, ...): этот тест.
5. Компилятор на Дане (папка «сам») и модули библиотеки: классы test_sam, которые собирают
   программу, подключающую изменённый файл (прямо или через другие модули), и классы,
   сверяющие все программы репозитория.
Файл, для которого не нашлось ни одного правила, означает «запустить все тесты»:
лучше проверить лишнее, чем пропустить нужное. Полный набор всегда идёт в CI.
"""

import ast
import fnmatch
import functools
import glob
import os
import re

ПАПКА = os.path.dirname(os.path.abspath(__file__))

# Ядро компилятора на Python: от него зависят почти все тесты, поэтому выбирается
# широкий, но не полный набор; полный прогон делает CI.
ПЕРЕДНЯЯ_ЧАСТЬ = ["test_lexer", "test_parser", "test_proverka", "test_moduli", "test_obobshchenie", "test_chisla",
                  "test_katalog", "test_sovmestimost", "test_ustoychivost", "test_privychki", "test_kody",
                  "test_garantii", "test_raskladka", "test_formatirovanie", "test_objasnenie"]
# классы сверки с компилятором на Дане для проверки смысла (типы, ошибки, пометки для порождения)
САМ_ПРОВЕРКА = ["test_sam.ТестыПроверкиНаДане", "test_sam.ТестыТиповНаДане", "test_sam.ТестыПорожденияНаДане",
                "test_sam.ТестыСамосборки", "test_sam.ТестыРезультатаПрограмм"]
ПОРОЖДЕНИЕ = ["test_kodogen", "test_poryadok", "test_garantii", "test_pamyat", "test_zadachi", "test_sverka",
              "test_sverka_sborok", "test_sovmestimost", "test_katalog", "test_testy", "test_zamery", "test_profil",
              "test_otladka", "test_sam.ТестыПорожденияНаДане", "test_sam.ТестыТаблицСреды",
              "test_sam.ТестыСамосборки", "test_sam.ТестыРезультатаПрограмм"]
ТАБЛИЦЫ = ["test_sam.ТестыТаблицВстроенных", "test_sam.ТестыТаблицСреды", "test_llms", "test_katalog"]
ДАННЫЕ = ["test_dannye", "test_json", "test_excel", "test_xml", "test_word", "test_fazzing", "test_pamyat"]

# (образцы путей, тесты). Тест - файл («test_json») или класс («test_sam.ТестыСамосборки»).
СВЯЗИ = [
    (["lexer.py", "parser.py", "uzly.py"], ПЕРЕДНЯЯ_ЧАСТЬ + ["test_sam"]),
    (["moduli.py"], ПЕРЕДНЯЯ_ЧАСТЬ + САМ_ПРОВЕРКА + ["test_sam.ТестыМодулейНаДане"]),
    (["proverka.py", "obobshchenie.py"], ПЕРЕДНЯЯ_ЧАСТЬ + САМ_ПРОВЕРКА),
    (["privychki.py", "kody.py"], ПЕРЕДНЯЯ_ЧАСТЬ + САМ_ПРОВЕРКА + ["test_sam.ТестыТаблицВстроенных"]),
    (["kodogen.py"], ПОРОЖДЕНИЕ),
    (["dan.py"], ["test_kesh", "test_testy", "test_zamery", "test_profil", "test_otladka", "test_kody", "test_moduli",
                  "test_mingw", "test_veb", "test_katalog", "test_sam.ТестыСамосборки", "test_poryadok",
                  "test_sam_kompilyator"]),
    # проверка тестами через компилятор на Дане (DAN_KOMPILYATOR=сам)
    (["sam_kompilyator.py"], ["test_sam_kompilyator", "test_proverka", "test_kodogen", "test_sovmestimost",
                              "test_sam.ТестыРезультатаПрограмм"]),
    (["matematika.py", "vysshaya.py"], ТАБЛИЦЫ + ["test_chisla", "test_vysshaya", "test_sverka"]),
    (["dannye.py"], ТАБЛИЦЫ + ДАННЫЕ),
    (["sistema.py"], ТАБЛИЦЫ + ["test_sistema", "test_http", "test_pochta", "test_sqlite", "test_okna", "test_paketi"]),
    (["versiya.py"], ["test_sovmestimost", "test_obnovlenie", "test_sam.ТестыТаблицСреды", "test_sam.ТестыСамосборки"]),
    (["sreda/dannye.c"], ДАННЫЕ + ["test_katalog", "test_sam.ТестыТаблицСреды"]),
    (["sreda/sistema.c"], ["test_sistema", "test_paketi", "test_katalog", "test_zadachi", "test_sam.ТестыТаблицСреды",
                           "test_sam.ТестыСамосборки"]),
    (["sreda/set.c"], ["test_http", "test_pochta", "test_sam.ТестыТаблицСреды"]),
    (["sreda/baza.c"], ["test_sqlite", "test_sam.ТестыТаблицСреды"]),
    (["sreda/okna.c", "sreda/okna_veb.c"], ["test_okna", "test_sam.ТестыТаблицСреды"]),
    (["sreda/pdf.c"], ["test_pdf", "test_sam.ТестыТаблицСреды"]),
    (["formatirovanie.py"], ["test_formatirovanie", "test_raskladka", "test_ustoychivost",
                             "test_sam.ТестыФорматаНаДане"]),
    (["dokumentaciya.py"], ["test_dokumentaciya", "test_opisaniya", "test_sam.ТестыДокументацииНаДане"]),
    (["objasnenie.py"], ["test_objasnenie", "test_server", "test_sam.ТестыОбъясненияНаДане"]),
    (["server.py"], ["test_server", "test_opisaniya"]),
    (["paketi.py"], ["test_paketi", "test_sam.ТестыСамосборки"]),
    (["zadachnik.py", "uchebnik.py", "учебник/*"], ["test_uchebnik", "test_sam.ТестыСамосборки"]),
    (["fazzing.py", "фаззинг/*"], ["test_fazzing", "test_sverka_sborok"]),
    (["sverka_sborok.py"], ["test_sverka_sborok"]),
    (["sverka_vychisleniy.py"], ["test_sverka"]),
    (["ustoychivost.py"], ["test_ustoychivost", "test_formatirovanie"]),
    (["zamery.py", "замеры/*"], ["test_zamery"]),
    (["veb.py"], ["test_veb"]),
    (["pesochnica.py"], ["test_pesochnica"]),
    (["obnovlenie.py", "выпуски/*"], ["test_obnovlenie"]),
    (["создать_llms.py", "llms.txt", "llms-full.txt"], ["test_llms"]),
    (["сам/создать_*.py", "пересоздать.py"], ["test_sam.ТестыТаблицВстроенных", "test_sam.ТестыТаблицСреды",
                                               "test_llms", "test_testy_svyazi"]),
    (["каталог/*"], ["test_katalog", "test_kody", "test_garantii"]),
    (["совместимость/*"], ["test_sovmestimost"]),
    (["гарантии.md"], ["test_garantii"]),
    (["spec.md", "справочник.md"], ["test_dokumentaciya", "test_soglasovannost", "test_llms", "test_opisaniya"]),
    (["*.md"], ["test_soglasovannost"]),
    (["testy.py", "testy_svyazi.py", "testy_vremya.json"], ["test_testy_svyazi"]),
    (["semya/*", ".gitattributes"], ["test_semya", "test_testy_svyazi"]),
    (["библиотека/*"], ["test_katalog", "test_okna"]),
    # файлы, которые тесты не проверяют (проверяют задания CI «Установка» и «Поставка»)
    (["LICENSE", ".gitignore", ".github/*", "поставка/*", "редактор/*", "установить_windows.ps1", "исследование.md",
      "принципы.md"], []),
]

# модули библиотеки, у которых есть свой файл тестов
ТЕСТЫ_МОДУЛЕЙ = {"json": "test_json", "csv": "test_excel", "excel": "test_excel", "xml": "test_xml",
                 "word": "test_word", "pdf": "test_pdf", "http": "test_http", "почта": "test_pochta",
                 "sqlite": "test_sqlite", "окна": "test_okna"}


@functools.lru_cache(maxsize=None)
def _прочитать(путь):
    with open(os.path.join(ПАПКА, путь), encoding="utf-8") as ф:
        return ф.read()


@functools.lru_cache(maxsize=None)
def _файлы_тестов():
    return sorted(os.path.basename(п)[:-3] for п in glob.glob(os.path.join(ПАПКА, "test_*.py")))


@functools.lru_cache(maxsize=None)
def _подключения(модуль_dan):
    """Модули, которые подключает файл сам/модуль.dan или библиотека/модуль.dan (сам файл тоже)."""
    итог = set()
    очередь = [модуль_dan]
    while очередь:
        имя = очередь.pop()
        if имя in итог:
            continue
        итог.add(имя)
        for папка in ("сам", "библиотека"):
            путь = os.path.join(ПАПКА, папка, имя + ".dan")
            if os.path.isfile(путь):
                with open(путь, encoding="utf-8") as ф:
                    очередь += re.findall(r'^\s*(?:общий\s+)?подключить\s+"([^"/]+)"', ф.read(), re.M)
                break
    return итог


@functools.lru_cache(maxsize=None)
def _классы_test_sam():
    """{класс: (программы сам/*.dan, сверяет ли все программы репозитория, файлы тестов, которые читает)}."""
    текст = _прочитать("test_sam.py")
    дерево = ast.parse(текст)
    строки = текст.split("\n")
    читаемые = re.search(r"def тексты_из_тестов\(\):.*?for имя in \((.*?)\):", текст, re.S)
    файлы_текстов = re.findall(r'"(test_\w+)\.py"', читаемые.group(1)) if читаемые else []
    итог = {}
    for узел in дерево.body:
        if isinstance(узел, ast.ClassDef):
            тело = "\n".join(строки[узел.lineno - 1:узел.end_lineno])
            программы = set(re.findall(r'САМ, "([^"]+)\.dan"', тело))
            итог[узел.name] = (программы, "файлы_репозитория()" in тело,
                               файлы_текстов if "тексты_из_тестов()" in тело else [])
    return итог


def _тесты_dan(путь):
    """Классы test_sam (и тесты модуля библиотеки) для изменённого файла .dan."""
    имя = os.path.basename(путь)[:-4]
    итог = set()
    # классы, сверяющие все программы репозитория, нужны для правок компилятора на Дане и каталога;
    # правку примеров и библиотеки на всех программах проверяет полный прогон в CI
    сверка_всех = путь.startswith("сам/") or путь.startswith("каталог/")
    for класс, (программы, весь_репозиторий, _) in _классы_test_sam().items():
        if (весь_репозиторий and сверка_всех) or any(имя in _подключения(п) for п in программы):
            итог.add("test_sam." + класс)
    if сверка_всех:
        # сверка результата программ собирает весь компилятор на Дане (sam_kompilyator.py)
        итог.add("test_sam.ТестыРезультатаПрограмм")
    if путь.startswith("библиотека/"):
        if имя in ТЕСТЫ_МОДУЛЕЙ:
            итог.add(ТЕСТЫ_МОДУЛЕЙ[имя])
    return итог


def тесты_для_файла(путь):
    """Множество тестов для одного изменённого файла или None (правил нет - нужны все тесты)."""
    путь = путь.replace("\\", "/")
    итог = set()
    найдено = False
    имя = os.path.basename(путь)
    if re.fullmatch(r"test_\w+\.py", путь):
        итог.add(путь[:-3])
        найдено = True
        for класс, (_, _, файлы) in _классы_test_sam().items():
            if путь[:-3] in файлы:
                итог.add("test_sam." + класс)
    for образцы, тесты in СВЯЗИ:
        if any(fnmatch.fnmatchcase(путь, о) for о in образцы):
            итог |= set(тесты)
            найдено = True
    if путь.endswith(".dan") and (путь.startswith("сам/") or путь.startswith("библиотека/")
                                 or путь.count("/") >= 1):
        тесты = _тесты_dan(путь)
        if тесты:
            итог |= тесты
            найдено = True
    if путь.endswith(".py") and "/" not in путь:
        модуль = путь[:-3]
        for тест in _файлы_тестов():
            # test_sam подключает почти все модули; его классы выбирают явные правила выше
            if тест != "test_sam" and re.search(rf"^\s*(?:import|from)\s+{re.escape(модуль)}\b",
                                               _прочитать(тест + ".py"), re.M):
                итог.add(тест)
                найдено = True
    папка = путь.split("/")[0] if "/" in путь else None
    if папка:
        for тест in _файлы_тестов():
            if f'"{папка}"' in _прочитать(тест + ".py") and тест != "test_sam":
                итог.add(тест)
                найдено = True
    return итог if найдено else None


def тесты_для(пути):
    """Тесты для списка изменённых файлов; None - нужен полный набор."""
    итог = set()
    for путь in пути:
        тесты = тесты_для_файла(путь)
        if тесты is None:
            return None
        итог |= тесты
    # класс внутри уже выбранного целиком файла не нужен отдельно
    целиком = {т for т in итог if "." not in т}
    return {т for т in итог if "." not in т or т.split(".")[0] not in целиком}
