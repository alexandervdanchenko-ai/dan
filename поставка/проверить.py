"""Проверка готовой поставки Дана: собранный dan.exe (или dan) без Python и без
компилятора C в системе собирает и выполняет программы каталога.

    python поставка/проверить.py путь\\к\\dan.exe

Для каждой проверки dan запускается с PATH, в котором есть только его папка и
системные папки: gcc, clang и Python из системы ему не видны, программы
собираются компилятором zig из поставки. Сама проверка написана на Python, но
dan.exe его не использует.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
КАТАЛОГ = os.path.join(КОРЕНЬ, "каталог")


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


def среда(dan):
    папки = [os.path.dirname(os.path.abspath(dan))]
    if os.name == "nt":
        windows = os.environ.get("SystemRoot", r"C:\Windows")
        папки += [os.path.join(windows, "System32"), windows]
    else:
        папки += ["/usr/bin", "/bin"]
    с = {к: з for к, з in os.environ.items() if к.upper() not in ("PATH", "DAN_COMPILER", "PYTHONPATH")}
    с["PATH"] = os.pathsep.join(папки)
    return с


def main(аргументы):
    if len(аргументы) != 1:
        print(__doc__.strip())
        return 2
    dan = os.path.abspath(аргументы[0])
    с = среда(dan)
    if os.name == "nt":
        for лишний in ("gcc", "clang", "python"):
            if shutil.which(лишний, path=с["PATH"]):
                print(f"в PATH проверки остался {лишний}: проверка не будет честной")
                return 2
    ошибки = []
    проверено = 0
    with tempfile.TemporaryDirectory(prefix="dan_postavka_") as папка:
        def запустить(*команда, ввод="", cwd=папка):
            return subprocess.run(list(команда), capture_output=True, text=True, encoding="utf-8",
                                  errors="replace", input=ввод, env=с, cwd=cwd, timeout=600)

        р = запустить(dan, "--версия")
        print(р.stdout.strip() or р.stderr.strip())
        if р.returncode != 0:
            print("dan --версия не работает:\n" + р.stdout + р.stderr)
            return 1
        for путь in sorted(glob.glob(os.path.join(КАТАЛОГ, "программы", "*.dan"))):
            имя = os.path.basename(путь)
            копия = os.path.join(папка, имя)
            shutil.copy(путь, копия)
            основа = путь[:-4]
            ввод = прочитать(основа + ".ввод") if os.path.exists(основа + ".ввод") else ""
            р = запустить(dan, "собрать", копия)
            if р.returncode != 0:
                ошибки.append(f"{имя}: не собралась:\n{р.stdout}{р.stderr}")
                continue
            exe = копия[:-4] + (".exe" if os.name == "nt" else "")
            р = запустить(exe, ввод=ввод)
            if р.returncode != 0 or р.stdout.splitlines() != прочитать(основа + ".вывод").splitlines():
                ошибки.append(f"{имя}: неверный результат (код {р.returncode}):\n{р.stdout[-1500:]}{р.stderr[-1500:]}")
            проверено += 1
        for путь in sorted(glob.glob(os.path.join(КАТАЛОГ, "ошибки", "*.dan"))):
            if путь.endswith(".исправлено.dan"):
                continue
            ожидается = прочитать(путь[:-4] + ".сообщение").strip()
            if ожидается.startswith("Ошибка выполнения"):
                continue
            р = запустить(dan, "проверить", путь)
            if р.returncode == 0 or ожидается not in р.stdout + р.stderr:
                ошибки.append(f"{os.path.basename(путь)}: нет ожидаемого сообщения «{ожидается}»:\n{р.stdout}{р.stderr}")
            проверено += 1
        # модули библиотеки и окна: сборка (окна не открываются)
        for текст, имя in (('подключить "http"\nподключить "почта"\nподключить "окна"\nподключить "word"\n'
                            'подключить "pdf"\nвывод(делители(12))\n'
                            'если ошибка о = http.получить("http://x") { вывод(о) }\n'
                            'если пусть с = текст_pdf(байты("x")) { вывод(с) }\n'
                            'вывод(почта.разобрать(байты("x")).тема)\n'
                            'если ошибка о = файлы_архива(байты("x")) { вывод(о) }\n', "модули.dan"),):
            путь = os.path.join(папка, имя)
            with open(путь, "w", encoding="utf-8") as ф:
                ф.write(текст)
            р = запустить(dan, "собрать", путь)
            if р.returncode != 0:
                ошибки.append(f"{имя}: не собралась:\n{р.stdout}{р.stderr}")
            проверено += 1
    for о in ошибки:
        print("=" * 60)
        print(о)
    print(f"Проверено: {проверено}; ошибок: {len(ошибки)}")
    return 1 if ошибки else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
