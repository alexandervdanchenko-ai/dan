"""Проверка выпуска Дана глазами пользователя Linux и macOS.

    python поставка/проверить_выпуск.py [метка] [--папка=рабочая_папка]

Метка по умолчанию - «v» и номер из versiya.py. На чистой машине проверка делает то же, что
пользователь, а не то, что видят тесты в репозитории:

1. Скачивает файлы со страницы выпуска (те же адреса, что у кнопок «скачать») и сверяет каждый
   с SHA256SUMS.txt выпуска; файл выпуска без строки в SHA256SUMS.txt - тоже ошибка.
2. Выполняет команды из README (раздел «Установка в Linux и macOS», первый блок команд) ровно
   как они написаны, только метку в «git clone --branch» заменяет проверяемой меткой.
3. «semya/dan --версия» должен назвать номер выпуска.
4. «semya/dan модели установить» (команда из того же раздела README) ставит библиотеку llama.cpp
   со страницы выпуска, «semya/dan модели проверить» её находит и выбирает вариант для процессора.
5. Из другой папки, полным путём к компилятору и с DAN_HOME (как сказано в README), собирает и
   выполняет программы поставка/программы выпуска: плотные массивы и векторные ядра,
   «параллельно для», модуль «модели» на крошечной модели GGUF (gguf_malaya.py).

Python нужен только самой проверке: Дан из семени собирается и работает без него.
Задание «Выпуск глазами пользователя» сценария .github/workflows/proverka_vypuska.yml выполняет
эту проверку на ubuntu-latest, ubuntu-24.04-arm и macos-latest после каждого выпуска.
"""

import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, КОРЕНЬ)   # versiya.py, gguf_malaya.py - в корне проекта
РЕПОЗИТОРИЙ = "alexandervdanchenko-ai/dan"
РАЗДЕЛ = "## Установка в Linux и macOS"
УСТАНОВИТЬ_МОДЕЛИ = "semya/dan модели установить"


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


def раздел_readme(текст):
    """Текст раздела README об установке в Linux и macOS (до следующего заголовка «## »)."""
    начало = текст.find(РАЗДЕЛ + "\n")
    if начало < 0:
        raise ValueError(f"в README нет раздела «{РАЗДЕЛ}»")
    тело = текст[начало + len(РАЗДЕЛ) + 1:]
    конец = re.search(r"^## ", тело, re.M)
    return тело[:конец.start()] if конец else тело


def команды_readme(текст, метка=None):
    """Команды первого блока ``` раздела «Установка в Linux и macOS».

    Первая команда должна загружать выпуск по метке («git clone --branch v<номер> ...»): иначе
    пользователь получает ветку main, а не выпуск. С меткой - метка в ней заменяется на эту."""
    раздел = раздел_readme(текст)
    блок = re.search(r"^```[^\n]*\n(.*?)^```", раздел, re.M | re.S)
    if not блок:
        raise ValueError(f"в разделе «{РАЗДЕЛ}» нет блока команд")
    команды = [с.strip() for с in блок.group(1).splitlines() if с.strip()]
    if not команды or not re.fullmatch(r"git clone --branch v\S+ .*https://github\.com/\S+\.git", команды[0]):
        raise ValueError("первая команда установки должна быть «git clone --branch v<номер> ... https://github.com/...git», "
                         f"а она: {команды[:1]}")
    if метка:
        команды[0] = re.sub(r"--branch v\S+", "--branch " + метка, команды[0])
    if УСТАНОВИТЬ_МОДЕЛИ not in раздел:
        raise ValueError(f"в разделе «{РАЗДЕЛ}» нет команды «{УСТАНОВИТЬ_МОДЕЛИ}»")
    return команды


def метка_readme(текст):
    """Метка выпуска, которую загружает «git clone --branch» в README."""
    return re.search(r"--branch (v\S+)", команды_readme(текст)[0]).group(1)


def суммы_из_файла(текст):
    """{имя файла: SHA256} из SHA256SUMS.txt."""
    итог = {}
    for строка in текст.splitlines():
        части = строка.split()
        if len(части) == 2:
            итог[части[1].lstrip("*")] = части[0].lower()
    return итог


def скачать(адрес, путь):
    запрос = urllib.request.Request(адрес, headers={"User-Agent": "dan-proverka-vypuska"})
    with urllib.request.urlopen(запрос, timeout=600) as ответ, open(путь, "wb") as ф:
        shutil.copyfileobj(ответ, ф)


def файлы_выпуска(метка):
    """Имена файлов на странице выпуска (REST API GitHub; GH_TOKEN, если задан)."""
    заголовки = {"User-Agent": "dan-proverka-vypuska", "Accept": "application/vnd.github+json"}
    токен = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if токен:
        заголовки["Authorization"] = "Bearer " + токен
    запрос = urllib.request.Request(f"https://api.github.com/repos/{РЕПОЗИТОРИЙ}/releases/tags/{метка}", headers=заголовки)
    with urllib.request.urlopen(запрос, timeout=120) as ответ:
        return sorted(ф["name"] for ф in json.load(ответ)["assets"])


def проверить_файлы(метка, папка):
    """Ошибки: каждый файл выпуска скачивается и совпадает с SHA256SUMS.txt."""
    ошибки = []
    адрес = f"https://github.com/{РЕПОЗИТОРИЙ}/releases/download/{метка}/"
    скачать(адрес + "SHA256SUMS.txt", os.path.join(папка, "SHA256SUMS.txt"))
    суммы = суммы_из_файла(прочитать(os.path.join(папка, "SHA256SUMS.txt")))
    имена = файлы_выпуска(метка)
    for имя in имена:
        if имя != "SHA256SUMS.txt" and имя not in суммы:
            ошибки.append(f"файл выпуска {имя} не записан в SHA256SUMS.txt")
    for имя, ожидается in sorted(суммы.items()):
        if имя not in имена:
            ошибки.append(f"SHA256SUMS.txt называет {имя}, а на странице выпуска его нет")
            continue
        путь = os.path.join(папка, имя)
        скачать(адрес + имя, путь)
        with open(путь, "rb") as ф:
            сумма = hashlib.sha256(ф.read()).hexdigest()
        if сумма != ожидается:
            ошибки.append(f"{имя}: SHA256 {сумма}, а в SHA256SUMS.txt {ожидается}")
        else:
            print(f"{имя}: SHA256 совпадает")
        os.remove(путь)
    return ошибки


def выполнить(команда, cwd, env=None, ввод=""):
    р = subprocess.run(команда, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=cwd,
                       env=env, input=ввод, timeout=1800, shell=isinstance(команда, str))
    return р


def main(аргументы):
    import versiya
    import gguf_malaya
    папка = None
    метки = []
    for а in аргументы:
        if а.startswith("--папка="):
            папка = os.path.abspath(а.split("=", 1)[1])
        elif а.startswith("-"):
            print(__doc__.strip())
            return 2
        else:
            метки.append(а)
    if len(метки) > 1:
        print(__doc__.strip())
        return 2
    метка = метки[0] if метки else "v" + versiya.ВЕРСИЯ
    if not re.fullmatch(r"v\d+(\.\d+)*([a-z]+\d*)?", метка):
        print(f"метка выпуска должна быть вида v1.6.1, а не «{метка}»")
        return 2
    версия = метка[1:]
    if папка is None:
        папка = tempfile.mkdtemp(prefix="dan_vypusk_")
    os.makedirs(папка, exist_ok=True)
    ошибки = []

    print(f"== Файлы выпуска {метка} и SHA256SUMS.txt")
    загрузки = os.path.join(папка, "загрузки")
    os.makedirs(загрузки, exist_ok=True)
    ошибки += проверить_файлы(метка, загрузки)

    print("== Установка по README")
    команды = команды_readme(прочитать(os.path.join(КОРЕНЬ, "README.md")), метка)
    дом = os.path.join(папка, "пользователь")
    os.makedirs(дом, exist_ok=True)
    for к in команды:
        print("$ " + к)
    р = выполнить("set -e\n" + "\n".join(команды) + "\n", дом)
    print(р.stdout[-3000:] + р.stderr[-3000:])
    if р.returncode != 0:
        ошибки.append(f"команды установки из README завершились с кодом {р.returncode}")
        return итог(ошибки)
    проект = os.path.join(дом, "dan")
    dan = os.path.join(проект, "semya", "dan")
    if not os.path.isfile(dan):
        ошибки.append(f"после команд README нет компилятора {dan}")
        return итог(ошибки)

    р = выполнить([dan, "--версия"], проект)
    print(р.stdout.strip() or р.stderr.strip())
    if р.returncode != 0 or not р.stdout.startswith(f"Дан {версия};"):
        ошибки.append(f"semya/dan --версия должен назвать версию {версия}, а вывел:\n{р.stdout}{р.stderr}")

    print("== " + УСТАНОВИТЬ_МОДЕЛИ)
    р = выполнить(УСТАНОВИТЬ_МОДЕЛИ.split(), проект)
    print(р.stdout.strip() + р.stderr.strip())
    if р.returncode != 0:
        ошибки.append(f"{УСТАНОВИТЬ_МОДЕЛИ}: код {р.returncode}")
    р = выполнить(["semya/dan", "модели", "проверить"], проект)
    print(р.stdout.strip() + р.stderr.strip())
    if р.returncode != 0 or "Вариант для процессора: " not in р.stdout:
        ошибки.append(f"semya/dan модели проверить: код {р.returncode}")

    print("== Программы с возможностями версий 1.5 и новее (из другой папки, с DAN_HOME)")
    программы = os.path.join(проект, "поставка", "программы")
    if not os.path.isdir(программы):
        программы = os.path.join(КОРЕНЬ, "поставка", "программы")
        print(f"в выпуске {метка} нет папки поставка/программы: программы берутся из {программы}")
    проба = os.path.join(папка, "программы")
    os.makedirs(проба, exist_ok=True)
    модель = os.path.join(проба, "малая.gguf")
    gguf_malaya.создать(модель)
    с = dict(os.environ, DAN_HOME=проект)
    for путь in sorted(glob.glob(os.path.join(программы, "*.dan"))):
        имя = os.path.basename(путь)
        копия = os.path.join(проба, имя)
        shutil.copy(путь, копия)
        р = выполнить([dan, "собрать", копия], проба, с)
        if р.returncode != 0:
            ошибки.append(f"{имя}: не собралась:\n{р.stdout}{р.stderr}")
            continue
        р = выполнить([копия[:-4], *([модель] if имя == "модель.dan" else [])], проба, с)
        if р.returncode != 0 or р.stdout.splitlines() != прочитать(путь[:-4] + ".вывод").splitlines():
            ошибки.append(f"{имя}: неверный результат (код {р.returncode}):\n{р.stdout[-1500:]}{р.stderr[-1500:]}")
        else:
            print(f"{имя}: собрана и выполнена, вывод совпадает")
    return итог(ошибки)


def итог(ошибки):
    for о in ошибки:
        print("=" * 60)
        print(о)
    print(f"Ошибок: {len(ошибки)}")
    return 1 if ошибки else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
