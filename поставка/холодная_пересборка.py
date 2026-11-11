"""Холодная пересборка Дана из метки выпуска на чистой машине.

    python поставка/холодная_пересборка.py [метка] [--папка=рабочая_папка] [--здесь]

Проверка выпуска (поставка/проверить_выпуск.py) ставит Дан по README и собирает программы; эта
проверка идёт дальше и подтверждает, что выпуск собирается сам из себя без готовых файлов:

1. Загружает исходники выпуска по метке (git clone --depth 1 --branch <метка>) в пустую папку;
   с --здесь вместо этого берёт текущую папку проекта (для проверки до выпуска).
2. Собирает компилятор из семени semya/dan.c командой sh semya/sobrat.sh, с пустым кэшем Дана.
3. Неподвижная точка: собранный компилятор переводит сам себя (сам/дан.dan), код C совпадает
   с семенем; компилятор, собранный из этого кода, переводит себя ещё раз - и снова совпадает.
4. Тесты выпуска на компиляторе из семени (DAN_KOMPILYATOR=сам, DAN_SAM_EXE - этот компилятор,
   кэш - новая пустая папка): каталог, результаты программ, показательные программы, задачи
   учебника, команды компилятора на Дане.

Метка по умолчанию - «v» и номер из versiya.py. На GitHub - задание «Холодная пересборка из
метки» сценария .github/workflows/proverka_vypuska.yml (Linux x86-64, ARM, macOS).
"""

import os
import shutil
import subprocess
import sys
import tempfile

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, КОРЕНЬ)
РЕПОЗИТОРИЙ = "https://github.com/alexandervdanchenko-ai/dan.git"
ТЕСТЫ = ["test_katalog", "test_sam.ТестыРезультатаПрограмм", "test_pokazatelnye", "test_zadachi",
         "test_odin_kompilyator"]


def шаг(текст):
    print(f"\n==> {текст}", flush=True)


def выполнить(команда, cwd, env=None, вывод=True):
    р = subprocess.run(команда, cwd=cwd, env=env, capture_output=True)
    if р.returncode != 0:
        if вывод:
            print(р.stdout.decode("utf-8", "replace")[-4000:])
            print(р.stderr.decode("utf-8", "replace")[-4000:])
        raise SystemExit(f"команда завершилась с кодом {р.returncode}: {' '.join(команда)}")
    return р.stdout


def среда(кэш, **ещё):
    окружение = {к: з for к, з in os.environ.items() if not к.startswith("DAN_")}
    окружение["DAN_CACHE"] = кэш
    окружение.update(ещё)
    return окружение


def собрать_из_кода(код, папка, имя, cc):
    """Исполняемый файл компилятора из кода C (как semya/sobrat.sh)."""
    исходник = os.path.join(папка, имя + ".c")
    with open(исходник, "wb") as ф:
        ф.write(код)
    выход = os.path.join(папка, имя)
    выполнить([cc, "-std=c11", "-O2", "-fwrapv", "-ffp-contract=off", "-w", исходник, "-o", выход, "-lm", "-pthread"],
              папка)
    return выход


def неподвижная_точка(исходники, компилятор, кэш, работа):
    """Компилятор переводит сам себя дважды; оба раза код совпадает с семенем."""
    with open(os.path.join(исходники, "semya", "dan.c"), "rb") as ф:
        семя = ф.read()
    окружение = среда(кэш, DAN_HOME=исходники)
    первый = выполнить([компилятор, "си", os.path.join("сам", "дан.dan")], исходники, окружение)
    if первый != семя:
        raise SystemExit("компилятор из семени переводит себя не в семя: семя выпуска устарело")
    cc = os.environ.get("DAN_COMPILER") or "cc"
    второй_компилятор = собрать_из_кода(первый, работа, "dan2", cc)
    второй = выполнить([второй_компилятор, "си", os.path.join("сам", "дан.dan")], исходники, окружение)
    if второй != семя:
        raise SystemExit("второе поколение компилятора переводит себя иначе, чем первое")
    print(f"код C компилятора: {len(семя)} байт, совпадает в двух поколениях")


def main(аргументы):
    метка = None
    папка = None
    здесь = False
    for а in аргументы:
        if а.startswith("--папка="):
            папка = os.path.abspath(а[len("--папка="):])
        elif а == "--здесь":
            здесь = True
        elif а.startswith("v") and not а.startswith("--"):
            метка = а
        else:
            print(__doc__.strip())
            return 2
    if not метка and not здесь:
        import versiya
        метка = "v" + versiya.ВЕРСИЯ
    работа = папка or tempfile.mkdtemp(prefix="dan_holod_")
    shutil.rmtree(работа, ignore_errors=True)
    os.makedirs(работа)
    кэш = os.path.join(работа, "кэш")
    os.makedirs(кэш)
    if здесь:
        исходники = КОРЕНЬ
        шаг("исходники - текущая папка проекта")
    else:
        исходники = os.path.join(работа, "dan")
        шаг(f"исходники выпуска {метка}")
        выполнить(["git", "clone", "--quiet", "--depth", "1", "--branch", метка, РЕПОЗИТОРИЙ, исходники], работа)
    шаг("компилятор из семени (sh semya/sobrat.sh)")
    if здесь:
        # своя копия семени и сценария: сборка не трогает semya/dan текущей папки
        копия = os.path.join(работа, "semya")
        shutil.copytree(os.path.join(исходники, "semya"), копия)
        with open(os.path.join(копия, "dan.c"), "rb") as ф:
            компилятор = собрать_из_кода(ф.read(), работа, "dan", os.environ.get("DAN_COMPILER") or "cc")
    else:
        выполнить(["sh", os.path.join("semya", "sobrat.sh")], исходники, среда(кэш))
        компилятор = os.path.join(исходники, "semya", "dan")
    print(выполнить([компилятор, "--версия"], исходники, среда(кэш, DAN_HOME=исходники)).decode().strip())
    шаг("неподвижная точка: компилятор переводит сам себя")
    неподвижная_точка(исходники, компилятор, кэш, работа)
    шаг("тесты выпуска на компиляторе из семени, с пустым кэшем")
    кэш_тестов = os.path.join(работа, "кэш_тестов")
    os.makedirs(кэш_тестов)
    окружение = среда(кэш_тестов, DAN_KOMPILYATOR="сам", DAN_SAM_EXE=компилятор, PYTHONUTF8="1")
    р = subprocess.run([sys.executable, "testy.py", *ТЕСТЫ], cwd=исходники, env=окружение)
    if р.returncode != 0:
        print("тесты выпуска на компиляторе из семени не прошли")
        return 1
    шаг("готово: выпуск собирается сам из себя и проходит тесты")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
