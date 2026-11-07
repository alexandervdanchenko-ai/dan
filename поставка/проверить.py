"""Проверка готовой поставки Дана: собранный dan.exe (или dan) без Python и без
компилятора C в системе собирает и выполняет программы каталога.

    python поставка/проверить.py путь\\к\\dan.exe

Для каждой проверки dan запускается с PATH, в котором есть только его папка и
системные папки: gcc, clang и Python из системы ему не видны, программы
собираются компилятором zig из поставки. Сама проверка написана на Python, но
dan.exe его не использует.

Рядом с dan.exe должна лежать библиотека llama.cpp для модуля «модели»: проверка
загружает llama.dll, варианты для процессора ggml-cpu-*.dll и печатает возможности
процессора (llama_print_system_info).
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


# Загрузка библиотеки llama.cpp в отдельном процессе: сбой загрузки не останавливает проверку.
ПРОБА_LLAMA = r"""
import ctypes, os, sys
папка = sys.argv[1]
llama = ctypes.CDLL(os.path.join(папка, "llama.dll"))
ggml = ctypes.CDLL(os.path.join(папка, "ggml.dll"))
ggml.ggml_backend_load_all_from_path.argtypes = [ctypes.c_char_p]
ggml.ggml_backend_load_all_from_path(папка.encode("utf-8"))
ggml.ggml_backend_dev_count.restype = ctypes.c_size_t
llama.llama_print_system_info.restype = ctypes.c_char_p
print("устройств:", ggml.ggml_backend_dev_count())
print(llama.llama_print_system_info().decode("utf-8", "replace"))
sys.exit(0 if ggml.ggml_backend_dev_count() > 0 else 1)
"""


def проверить_llama(dan, с):
    """Сообщение об ошибке или None: библиотека llama.cpp рядом с dan загружается."""
    import versiya
    папка = os.path.dirname(os.path.abspath(dan))
    имя = "llama.dll" if os.name == "nt" else "libllama.so"
    if not os.path.isfile(os.path.join(папка, имя)):
        return f"рядом с {dan} нет библиотеки llama.cpp ({имя})"
    варианты = glob.glob(os.path.join(папка, "ggml-cpu-*.dll" if os.name == "nt" else "libggml-cpu-*.so"))
    if not варианты:
        return "рядом с dan нет вариантов для процессора ggml-cpu-*"
    with open(os.path.join(папка, "version.txt"), encoding="utf-8") as ф:
        метка = ф.readline().strip()
    if метка != versiya.LLAMA_CPP:
        return f"библиотека llama.cpp {метка}, а нужна {versiya.LLAMA_CPP}"
    р = subprocess.run([sys.executable, "-c", ПРОБА_LLAMA, папка], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=с, timeout=300)
    print(f"llama.cpp {метка}, вариантов для процессора: {len(варианты)}")
    print(р.stdout.strip())
    if р.returncode != 0:
        return f"библиотека llama.cpp не загрузилась (код {р.returncode}):\n{р.stdout}{р.stderr}"
    return None


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
    if os.name == "nt":
        о = проверить_llama(dan, с)
        if о:
            ошибки.append(о)
        проверено += 1
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
