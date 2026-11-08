"""Проверка готовой поставки Дана: собранный dan.exe (или dan) без Python и без
компилятора C в системе собирает и выполняет программы каталога.

    python поставка/проверить.py путь\\к\\dan.exe
    python поставка/проверить.py путь\\к\\dan.exe --без-поставки

Поставка с версии 1.8 - без Python: dan - сам компилятор на Дане, рядом с ним папки sreda,
библиотека и zig; проверка следит, чтобы Python (PyInstaller) в неё не вернулся, и проверяет
команды, которые раньше выполнял Python: сервер языка (программа с «параллельно для» без ложных
ошибок), песочницу (выполнение программы) и сборку --для=linux компилятором zig из поставки.

Кроме программ каталога dan собирает и выполняет программы папки поставка/программы с
возможностями версий 1.5 и новее (плотные массивы и векторные ядра, «параллельно для», модуль
«модели» на крошечной модели GGUF из gguf_malaya.py) и выполняет «dan модели проверить»: так
проверка ловит поставку, в которой команды выполняет прежний компилятор на Python.

С --без-поставки проверяется Дан, установленный по инструкции (компилятор на Дане, собранный из
семени, и компилятор C системы): PATH не урезается, библиотека llama.cpp ищется там же, где её ищут
программы (DAN_LLAMA, папка данных пользователя), папку Дана задаёт DAN_HOME.

Для каждой проверки dan запускается с PATH, в котором есть только его папка и
системные папки: gcc, clang и Python из системы ему не видны, программы
собираются компилятором zig из поставки. Сама проверка написана на Python, но
dan.exe его не использует.

Рядом с dan.exe должна лежать библиотека llama.cpp для модуля «модели»: проверка
загружает llama.dll, варианты для процессора ggml-cpu-*.dll и печатает возможности
процессора (llama_print_system_info).
"""

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
КАТАЛОГ = os.path.join(КОРЕНЬ, "каталог")
sys.path.insert(0, КОРЕНЬ)   # modeli.py (версия llama.cpp) - в корне проекта


def прочитать(путь):
    with open(путь, encoding="utf-8") as ф:
        return ф.read()


def среда(dan, без_поставки=False):
    if без_поставки:
        return dict(os.environ)
    папки = [os.path.dirname(os.path.abspath(dan))]
    if os.name == "nt":
        windows = os.environ.get("SystemRoot", r"C:\Windows")
        папки += [os.path.join(windows, "System32"), windows]
    else:
        папки += ["/usr/bin", "/bin"]
    # без DAN_HOME: dan поставки находит свою папку сам (рядом со своим исполняемым файлом)
    с = {к: з for к, з in os.environ.items() if к.upper() not in ("PATH", "DAN_COMPILER", "PYTHONPATH", "DAN_HOME")}
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
    import modeli
    папка = os.path.dirname(os.path.abspath(dan))
    имя = "llama.dll" if os.name == "nt" else "libllama.so"
    if not os.path.isfile(os.path.join(папка, имя)):
        return f"рядом с {dan} нет библиотеки llama.cpp ({имя})"
    варианты = glob.glob(os.path.join(папка, "ggml-cpu-*.dll" if os.name == "nt" else "libggml-cpu-*.so"))
    if not варианты:
        return "рядом с dan нет вариантов для процессора ggml-cpu-*"
    with open(os.path.join(папка, "version.txt"), encoding="utf-8") as ф:
        метка = ф.readline().strip()
    if метка != modeli.ВЕРСИЯ_LLAMA:
        return f"библиотека llama.cpp {метка}, а нужна {modeli.ВЕРСИЯ_LLAMA}"
    р = subprocess.run([sys.executable, "-c", ПРОБА_LLAMA, папка], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=с, timeout=300)
    print(f"llama.cpp {метка}, вариантов для процессора: {len(варианты)}")
    print(р.stdout.strip())
    if р.returncode != 0:
        return f"библиотека llama.cpp не загрузилась (код {р.returncode}):\n{р.stdout}{р.stderr}"
    return None


def проверить_новое(dan, запустить, папка):
    """Ошибки программ с возможностями версий 1.5 и новее и команды «dan модели проверить»."""
    import gguf_malaya
    ошибки = []
    модель = os.path.join(папка, "малая.gguf")
    gguf_malaya.создать(модель)
    for путь in sorted(glob.glob(os.path.join(КОРЕНЬ, "поставка", "программы", "*.dan"))):
        имя = os.path.basename(путь)
        копия = os.path.join(папка, имя)
        shutil.copy(путь, копия)
        р = запустить(dan, "собрать", копия)
        if р.returncode != 0:
            ошибки.append(f"{имя}: не собралась:\n{р.stdout}{р.stderr}")
            continue
        exe = копия[:-4] + (".exe" if os.name == "nt" else "")
        р = запустить(exe, *([модель] if имя == "модель.dan" else []))
        if р.returncode != 0 or р.stdout.splitlines() != прочитать(путь[:-4] + ".вывод").splitlines():
            ошибки.append(f"{имя}: неверный результат (код {р.returncode}):\n{р.stdout[-1500:]}{р.stderr[-1500:]}")
        else:
            print(f"{имя}: собрана и выполнена")
    р = запустить(dan, "модели", "проверить")
    print(р.stdout.strip())
    if р.returncode != 0 or "Вариант для процессора: " not in р.stdout:
        ошибки.append(f"dan модели проверить (код {р.returncode}):\n{р.stdout}{р.stderr}")
    return ошибки


def проверить_без_python(dan):
    """Ошибки устройства поставки: в ней нет Python, рядом с dan - папки среды и zig."""
    ошибки = []
    папка = os.path.dirname(os.path.abspath(dan))
    лишнее = [и for и in os.listdir(папка) if и.lower().startswith("python") or и == "_internal"]
    if лишнее:
        ошибки.append("в поставке остался Python (PyInstaller): " + ", ".join(лишнее))
    for нужное in ("sreda", "библиотека", "zig"):
        if not os.path.isdir(os.path.join(папка, нужное)):
            ошибки.append(f"рядом с {dan} нет папки {нужное}")
    return ошибки


def сообщение_lsp(данные):
    тело = json.dumps(данные, ensure_ascii=False).encode("utf-8")
    return f"Content-Length: {len(тело)}\r\n\r\n".encode("ascii") + тело


def ответы_lsp(вывод_):
    """Сообщения JSON из вывода сервера LSP."""
    итог = []
    while вывод_:
        голова, _, остаток = вывод_.partition(b"\r\n\r\n")
        длина = int(голова.decode("ascii").split(":")[1])
        итог.append(json.loads(остаток[:длина].decode("utf-8")))
        вывод_ = остаток[длина:]
    return итог


def проверить_команды(dan, с, папка, поставка):
    """Ошибки команд, которые до версии 1.8 выполнял Python: сервер языка, песочница, --для=."""
    ошибки = []
    # Дан по инструкции загружается из ветки main: в версиях до 1.8 этих команд у компилятора на Дане нет
    р = subprocess.run([dan, "сервер"], input=b"", capture_output=True, env=с, cwd=папка, timeout=300)
    if р.returncode != 0 and not поставка:
        print("dan сервер, песочница: в этой версии компилятора на Дане их нет - не проверяются")
        return []
    # сервер языка: программа с «параллельно для» и массивами - без ложных ошибок
    with open(os.path.join(КОРЕНЬ, "поставка", "программы", "параллельно.dan"), encoding="utf-8") as ф:
        текст = ф.read()
    uri = "file:///" + os.path.join(папка, "п.dan").replace("\\", "/").lstrip("/")
    ввод_ = b"".join(сообщение_lsp(с_) for с_ in (
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri, "languageId": "dan", "version": 1, "text": текст}}},
        {"jsonrpc": "2.0", "method": "textDocument/didChange",
         "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": текст + "вывод(нет)\n"}]}},
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown"}, {"jsonrpc": "2.0", "method": "exit"}))
    р = subprocess.run([dan, "сервер"], input=ввод_, capture_output=True, env=с, cwd=папка, timeout=300)
    try:
        диагностики = [о["params"]["diagnostics"] for о in ответы_lsp(р.stdout)
                       if о.get("method") == "textDocument/publishDiagnostics"]
        if len(диагностики) != 2 or диагностики[0] != [] or len(диагностики[1]) != 1:
            ошибки.append(f"dan сервер: неверные диагностики {диагностики}")
        else:
            print("dan сервер: программа с «параллельно для» без ошибок, ошибка в добавленной строке найдена")
    except (ValueError, IndexError, KeyError) as о:
        ошибки.append(f"dan сервер (код {р.returncode}): {о}\n{р.stdout[-1500:]!r}{р.stderr[-1500:]!r}")
    # песочница: страница, ключ и выполнение программы компилятором из поставки
    процесс = subprocess.Popen([dan, "песочница", "--порт=0", "--без-браузера"], stdout=subprocess.PIPE, env=с,
                               cwd=папка)
    try:
        строка = процесс.stdout.readline().decode("utf-8", "replace")
        адрес = re.search(r"http://\S+", строка).group(0).rstrip("/")
        with urllib.request.urlopen(адрес + "/", timeout=60) as о:
            ключ = re.search(r'const КЛЮЧ = "([^"]+)"', о.read().decode("utf-8")).group(1)
        запрос = urllib.request.Request(адрес + urllib.request.quote("/выполнить"), headers={"X-Dan-Key": ключ},
                                        data=json.dumps({"текст": "вывод(ввод() + \"!\")", "ввод": "песок\n"}).encode())
        with urllib.request.urlopen(запрос, timeout=600) as о:
            ответ = json.loads(о.read().decode("utf-8"))
        if ответ.get("вывод") != "песок!\n":
            ошибки.append(f"dan песочница: неверный ответ {ответ}")
        else:
            print("dan песочница: программа собрана и выполнена,", ответ["состояние"])
    except (OSError, AttributeError, ValueError) as о:
        ошибки.append(f"dan песочница: {о}")
    finally:
        процесс.kill()
        процесс.wait()
        процесс.stdout.close()
    if поставка:
        # сборка для другой системы - компилятором zig из поставки
        путь = os.path.join(папка, "для.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write("вывод(1)\n")
        р = subprocess.run([dan, "собрать", "--для=linux", путь], capture_output=True, text=True, encoding="utf-8",
                           errors="replace", env=с, cwd=папка, timeout=600)
        if р.returncode != 0 or not os.path.isfile(путь[:-4] + "-linux"):
            ошибки.append(f"dan собрать --для=linux (код {р.returncode}):\n{р.stdout}{р.stderr}")
        else:
            print(р.stdout.strip())
    return ошибки


def main(аргументы):
    без_поставки = "--без-поставки" in аргументы
    аргументы = [а for а in аргументы if а != "--без-поставки"]
    if len(аргументы) != 1:
        print(__doc__.strip())
        return 2
    import versiya
    dan = os.path.abspath(аргументы[0])
    с = среда(dan, без_поставки)
    if os.name == "nt" and not без_поставки:
        for лишний in ("gcc", "clang", "python"):
            if shutil.which(лишний, path=с["PATH"]):
                print(f"в PATH проверки остался {лишний}: проверка не будет честной")
                return 2
    ошибки = []
    проверено = 0
    if os.name == "nt" and not без_поставки:
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
        # Дан по инструкции загружается из GitHub (ветка main), и его версия может быть другой
        if not без_поставки and f"Дан {versiya.ВЕРСИЯ};" not in р.stdout:
            ошибки.append(f"dan --версия не называет версию {versiya.ВЕРСИЯ}:\n{р.stdout}")
        if not без_поставки and "компилятор на Дане" not in р.stdout:
            ошибки.append("dan --версия не говорит, что команды выполняет компилятор на Дане: dan в поставке - "
                          "не компилятор на Дане или рядом с ним нет папки sreda")
        ошибки += проверить_новое(dan, запустить, папка)
        проверено += 4
        if not без_поставки:
            ошибки += проверить_без_python(dan)
            проверено += 1
        ошибки += проверить_команды(dan, с, папка, not без_поставки)
        проверено += 3
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
