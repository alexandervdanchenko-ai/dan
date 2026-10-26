"""Обновление Дана: dan обновить.

    python dan.py обновить                   - узнать о новой версии и поставить её
    python dan.py обновить --проверить       - только узнать, есть ли новая версия
    python dan.py обновить --канал=пробный   - получать и пробные выпуски (запоминается)
    python dan.py обновить --канал=стабильный - только стабильные выпуски (так по умолчанию)

Выпуски берутся со страницы выпусков репозитория Дана на GitHub - это
единственный официальный источник. Канал «стабильный» - обычные выпуски,
канал «пробный» - также пробные выпуски (их номер содержит букву: 1.4b1),
в которых новое можно попробовать до стабильного выпуска.

Скачанный файл сверяется с контрольной суммой из файла SHA256SUMS.txt
выпуска; без совпадения ничего не запускается и не распаковывается.
Дан, поставленный установщиком, обновляется установщиком новой версии
(он запускается сам после выхода из команды). Переносной архив
распаковывается в новую папку рядом с прежней: старая версия остаётся на
месте, пока не станет не нужна.
"""

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

import versiya

РЕПОЗИТОРИЙ = "alexandervdanchenko-ai/dan"
АДРЕС_ВЫПУСКОВ = f"https://api.github.com/repos/{РЕПОЗИТОРИЙ}/releases"
СТРАНИЦА_ВЫПУСКОВ = f"https://github.com/{РЕПОЗИТОРИЙ}/releases"
КАНАЛЫ = ("стабильный", "пробный")


class ОшибкаОбновления(Exception):
    pass


def номер(версия):
    """«1.4» -> (1, 4, 1, 0); «1.4b2» -> (1, 4, 0, 2): пробный выпуск раньше стабильного того же номера."""
    с = re.fullmatch(r"v?(\d+(?:\.\d+)*)(?:\s*-?\s*([a-zа-я]+)\.?(\d*))?", версия.strip().lower())
    if not с:
        return None
    числа = tuple(int(ч) for ч in с.group(1).split("."))
    числа = числа + (0,) * (3 - len(числа)) if len(числа) < 3 else числа
    if с.group(2):
        return числа + (0, int(с.group(3) or 0))
    return числа + (1, 0)


def пробный(версия):
    н = номер(версия)
    return н is not None and н[-2] == 0


def папка_настроек():
    if os.environ.get("DAN_CONFIG"):
        return os.environ["DAN_CONFIG"]
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "Dan")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "dan")


def канал():
    try:
        with open(os.path.join(папка_настроек(), "канал.txt"), encoding="utf-8") as ф:
            значение = ф.read().strip()
        return значение if значение in КАНАЛЫ else "стабильный"
    except OSError:
        return "стабильный"


def запомнить_канал(значение):
    os.makedirs(папка_настроек(), exist_ok=True)
    with open(os.path.join(папка_настроек(), "канал.txt"), "w", encoding="utf-8") as ф:
        ф.write(значение + "\n")


def _получить(адрес, двоичное=False):
    адрес = os.environ.get("DAN_RELEASES_URL", АДРЕС_ВЫПУСКОВ) if адрес is None else адрес
    запрос = urllib.request.Request(адрес, headers={"User-Agent": f"dan/{versiya.ВЕРСИЯ}",
                                                    "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(запрос, timeout=60) as ответ:
            данные = ответ.read()
    except OSError as о:
        raise ОшибкаОбновления(f"не удалось связаться с {адрес.split('/')[2] or адрес}: "
                               f"{getattr(о, 'reason', None) or о}. Проверьте подключение к интернету") from None
    return данные if двоичное else данные.decode("utf-8")


def выпуски():
    try:
        данные = json.loads(_получить(None))
    except ValueError:
        raise ОшибкаОбновления("сервер выпусков вернул непонятный ответ") from None
    if not isinstance(данные, list):
        raise ОшибкаОбновления(f"сервер выпусков отказал: {данные.get('message', данные) if isinstance(данные, dict) else данные}")
    return данные


def выбрать(список, канал_выпусков):
    """Самый новый выпуск канала: (версия, выпуск) или (None, None)."""
    лучший = (None, None)
    for в in список:
        if в.get("draft"):
            continue
        версия = (в.get("tag_name") or "").lstrip("v")
        н = номер(версия)
        if н is None:
            continue
        if канал_выпусков == "стабильный" and (в.get("prerelease") or пробный(версия)):
            continue
        if лучший[0] is None or н > номер(лучший[0]):
            лучший = (версия, в)
    return лучший


def файл_выпуска(выпуск, образец):
    for ф in выпуск.get("assets", []):
        if re.fullmatch(образец, ф.get("name", "")):
            return ф
    return None


def скачать_проверенный(выпуск, образец, папка):
    """Скачивает файл выпуска и сверяет его с SHA256SUMS.txt; возвращает путь."""
    файл = файл_выпуска(выпуск, образец)
    if файл is None:
        raise ОшибкаОбновления(f"в выпуске {выпуск.get('tag_name')} нет нужного файла; "
                               f"скачайте его вручную: {СТРАНИЦА_ВЫПУСКОВ}")
    суммы = файл_выпуска(выпуск, r"SHA256SUMS\.txt")
    if суммы is None:
        raise ОшибкаОбновления("в выпуске нет файла контрольных сумм SHA256SUMS.txt; обновление остановлено")
    ожидаемые = {}
    for с in _получить(суммы["browser_download_url"]).splitlines():
        части = с.split()
        if len(части) == 2:
            ожидаемые[части[1].lstrip("*")] = части[0].lower()
    if файл["name"] not in ожидаемые:
        raise ОшибкаОбновления(f"для файла {файл['name']} нет контрольной суммы; обновление остановлено")
    print(f"Скачивание {файл['name']} ({файл.get('size', 0) // 1024 // 1024 or 1} МБ)...")
    sys.stdout.flush()
    данные = _получить(файл["browser_download_url"], двоичное=True)
    if hashlib.sha256(данные).hexdigest() != ожидаемые[файл["name"]]:
        raise ОшибкаОбновления(f"файл {файл['name']} не совпадает с контрольной суммой выпуска: он повреждён "
                               f"или подменён. Ничего не изменено")
    путь = os.path.join(папка, файл["name"])
    with open(путь, "wb") as ф:
        ф.write(данные)
    return путь


def папка_установки():
    return os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))


def способ_установки():
    """«установщик», «архив» или «исходники»."""
    if not getattr(sys, "frozen", False) and not os.environ.get("DAN_UPDATE_MODE"):
        return "исходники"
    if os.environ.get("DAN_UPDATE_MODE"):
        return os.environ["DAN_UPDATE_MODE"]
    папка = папка_установки()
    if os.name == "nt" and any(re.fullmatch(r"unins\d+\.exe", и, re.I) for и in os.listdir(папка)):
        return "установщик"
    return "архив"


def распаковать_рядом(архив, версия):
    """Переносной архив -> новая папка рядом с нынешней установкой."""
    основа = os.path.dirname(папка_установки())
    цель = os.path.join(основа, f"dan-{версия}")
    н = 2
    while os.path.exists(цель):
        цель = os.path.join(основа, f"dan-{версия}-{н}")
        н += 1
    with zipfile.ZipFile(архив) as з:
        for имя in з.namelist():
            путь = os.path.normpath(os.path.join(цель, имя))
            if not путь.startswith(os.path.normpath(цель) + os.sep) and путь != os.path.normpath(цель):
                raise ОшибкаОбновления(f"в архиве недопустимое имя файла: {имя}")
        з.extractall(цель)
    # архив содержит одну папку верхнего уровня: она и есть Дан
    внутри = os.listdir(цель)
    if len(внутри) == 1 and os.path.isdir(os.path.join(цель, внутри[0])):
        return os.path.join(цель, внутри[0])
    return цель


def команда(аргументы):
    только_проверить = False
    выбранный = None
    for а in аргументы:
        if а == "--проверить":
            только_проверить = True
        elif а.startswith("--канал="):
            выбранный = а[len("--канал="):]
            if выбранный not in КАНАЛЫ:
                print(f"Канал может быть: {', '.join(КАНАЛЫ)}")
                return 2
        else:
            print(__doc__.strip())
            return 2
    try:
        if выбранный:
            запомнить_канал(выбранный)
        к = выбранный or канал()
        текущая = versiya.ВЕРСИЯ
        версия, выпуск = выбрать(выпуски(), к)
        if версия is None:
            print(f"В канале «{к}» выпусков нет")
            return 0
        if номер(версия) <= номер(текущая):
            print(f"Установлена последняя версия Дана ({текущая}, канал «{к}»)")
            return 0
        print(f"Есть новая версия Дана: {версия} (установлена {текущая}, канал «{к}»)")
        if выпуск.get("html_url"):
            print(f"Что нового: {выпуск['html_url']}")
        if только_проверить:
            return 0
        способ = способ_установки()
        if способ == "исходники":
            print("Дан запущен из исходных текстов: обновите их (git pull) или поставьте выпуск со страницы "
                  + СТРАНИЦА_ВЫПУСКОВ)
            return 0
        папка = tempfile.mkdtemp(prefix="dan_update_")
        if способ == "установщик":
            установщик = скачать_проверенный(выпуск, r"dan-.*-setup\.exe", папка)
            if os.environ.get("DAN_UPDATE_DRY_RUN"):
                print(f"Установщик готов: {установщик}")
                return 0
            # установщик заменит файлы, когда эта команда завершится
            subprocess.Popen([установщик, "/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS"],
                             creationflags=getattr(subprocess, "DETACHED_PROCESS", 0), close_fds=True)
            print(f"Запущен установщик Дана {версия}. Когда он закончит, откройте новое окно командной строки")
            return 0
        архив = скачать_проверенный(выпуск, r"dan-.*-windows\.zip", папка)
        новая = распаковать_рядом(архив, версия)
        print(f"Дан {версия} распакован в папку {новая}. Прежняя версия осталась в {папка_установки()}; "
              f"чтобы команда dan запускала новую, укажите новую папку в переменной PATH вместо прежней")
        return 0
    except ОшибкаОбновления as о:
        print(f"Ошибка: {о}")
        return 1
    except OSError as о:
        print(f"Ошибка: {о}")
        return 1
