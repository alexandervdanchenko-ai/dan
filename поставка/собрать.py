"""Сборка поставки Дана для Windows без Python: dan.exe - компилятор на Дане, собранный из семени,
компилятор zig рядом, библиотека llama.cpp для модуля «модели», установщик и переносной архив.

dan.exe - сам компилятор на Дане (сам/дан.dan), собранный из семени semya/dan.c тем же zig, что
лежит в поставке; с версии 1.8 он выполняет все команды, включая песочницу, сервер языка,
обновление и параметры --веб, --для=, --сжать, поэтому Python (прежний dan.exe из PyInstaller) в
поставке не нужен. Папку Дана (sreda, библиотека, учебник/задачи, zig) компилятор находит рядом со
своим исполняемым файлом, если не задана переменная DAN_HOME. Компилятор на Дане вызывает компилятор C
одной программой, поэтому рядом с zig.exe лежит обёртка zig-cc.exe (поставка/zig_cc.c), которая
запускает «zig cc»; её dan.exe берёт, если в системе нет gcc, clang и cc.

Запускается в Windows (на GitHub - задачей «Поставка»):

    python поставка/собрать.py [--llama=llama-<метка>-windows-x64.zip]

Архив библиотеки llama.cpp собирает сценарий поставка/собрать_llama.py (на GitHub - задание
«Библиотека llama.cpp»); без --llama=... он запускается отсюда. Её файлы (llama.dll, ggml*.dll,
version.txt, лицензия) кладутся рядом с dan.exe: установщик добавляет эту папку в PATH, и
программы на Дане находят llama.dll системным поиском библиотек.

Результат в папке «поставка/готово»:
    dan/                              - готовая папка Дана (dan.exe, zig, библиотека, примеры, справочник);
    dan-<версия>-setup.exe            - установщик (нужен Inno Setup 6);
    dan-<версия>-windows.zip          - та же папка одним архивом, без установки.

Нужны: Python 3 с пакетами ziglang и markdown (ставятся сами; Python нужен только для сборки),
Inno Setup 6 (если его нет, установщик не собирается, архив собирается).
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ЗНАЧКИ = os.path.join(КОРЕНЬ, "поставка", "значки")
ГОТОВО = os.path.join(КОРЕНЬ, "поставка", "готово")
ПАПКА = os.path.join(ГОТОВО, "dan")
sys.path.insert(0, КОРЕНЬ)

# Заголовки C для систем, под которые Дан не собирает программы: в поставку не входят.
ЛИШНЕЕ_В_ZIG = ["freebsd", "netbsd", "openbsd", "dragonfly", "haiku", "solaris", "illumos", "serenity"]


def шаг(текст):
    print(f"\n==> {текст}", flush=True)


def поставить(*пакеты):
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--upgrade", *пакеты], check=True)


def добавить_zig():
    шаг("компилятор zig")
    поставить("ziglang")
    import importlib
    import ziglang
    importlib.reload(ziglang)
    источник = os.path.dirname(ziglang.__file__)
    цель = os.path.join(ПАПКА, "zig")
    shutil.rmtree(цель, ignore_errors=True)
    shutil.copytree(источник, цель, ignore=shutil.ignore_patterns("__pycache__", "*.py", "README.md"))
    заголовки = os.path.join(цель, "lib", "libc", "include")
    for п in glob.glob(os.path.join(заголовки, "*")) + glob.glob(os.path.join(цель, "lib", "libc", "*")):
        if any(лишнее in os.path.basename(п) for лишнее in ЛИШНЕЕ_В_ZIG):
            shutil.rmtree(п, ignore_errors=True)
    for лишнее in ("docs", "libtsan"):
        shutil.rmtree(os.path.join(цель, "lib", лишнее), ignore_errors=True)


def собрать_компилятор_на_дане():
    шаг("dan.exe - компилятор на Дане (из семени semya/dan.c), обёртка zig-cc и папки среды")
    windows = os.name == "nt"
    exe = ".exe" if windows else ""
    zig = os.path.join(ПАПКА, "zig", "zig" + exe)
    # цель указана явно: без неё zig собирает под процессор этой машины, а компилятор должен
    # работать на любом процессоре x86-64
    цель = ["-target", "x86_64-windows-gnu"] if windows else []
    # сборка в папке с латинским именем: исходники и выход не зависят от русских букв в пути
    with tempfile.TemporaryDirectory(prefix="dan_sam_") as работа:
        семя = os.path.join(работа, "dan.c")
        shutil.copy(os.path.join(КОРЕНЬ, "semya", "dan.c"), семя)
        выход = os.path.join(работа, "dan" + exe)
        иконка = []
        if windows:
            # значок dan.exe: ресурс из dan.ico (zig собирает файл .rc сам)
            with open(os.path.join(работа, "dan.rc"), "w", encoding="ascii") as ф:
                ф.write('1 ICON "dan.ico"\n')
            shutil.copy(os.path.join(ЗНАЧКИ, "dan.ico"), os.path.join(работа, "dan.ico"))
            иконка = [os.path.join(работа, "dan.rc")]
        subprocess.run([zig, "cc", *цель, "-std=c11", "-O2", "-fwrapv", "-ffp-contract=off", "-w", "-s", семя,
                        *иконка, "-o", выход, "-lm", *(["-Wl,--stack,67108864"] if windows else ["-pthread"])], check=True)
        shutil.copy(выход, os.path.join(ПАПКА, "dan" + exe))
        if windows:
            обёртка = os.path.join(работа, "zig_cc.c")
            shutil.copy(os.path.join(КОРЕНЬ, "поставка", "zig_cc.c"), обёртка)
            готовая = os.path.join(работа, "zig-cc.exe")
            subprocess.run([zig, "cc", *цель, "-O2", "-s", обёртка, "-o", готовая, "-lshell32"], check=True)
            shutil.copy(готовая, os.path.join(ПАПКА, "zig", "zig-cc.exe"))
    # среда выполнения, стандартная библиотека и задачник - рядом с dan.exe (папка Дана)
    for папка in ("sreda", "библиотека", os.path.join("учебник", "задачи")):
        shutil.copytree(os.path.join(КОРЕНЬ, папка), os.path.join(ПАПКА, папка),
                        ignore=shutil.ignore_patterns("*.o", "*.exe", "__pycache__"))
    среда = {к: з for к, з in os.environ.items() if к != "DAN_HOME"}
    р = subprocess.run([os.path.join(ПАПКА, "dan" + exe), "--версия"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=среда)
    print(р.stdout.strip() or р.stderr.strip())
    if р.returncode != 0:
        raise SystemExit("собранный компилятор на Дане не запускается")


def в_html(путь_md, путь_html, заголовок):
    import markdown
    with open(путь_md, encoding="utf-8") as ф:
        тело = markdown.markdown(ф.read(), extensions=["tables", "fenced_code", "toc"])
    стиль = ("body{font-family:Segoe UI,Arial,sans-serif;max-width:960px;margin:2em auto;padding:0 1em;"
             "line-height:1.55;color:#1d1d1f}pre,code{font-family:Consolas,monospace;background:#f4f4f6}"
             "pre{padding:.8em;overflow:auto}table{border-collapse:collapse}td,th{border:1px solid #ccc;"
             "padding:.3em .6em;vertical-align:top}h1,h2,h3{line-height:1.25}")
    with open(путь_html, "w", encoding="utf-8") as ф:
        ф.write(f'<!doctype html><html lang="ru"><head><meta charset="utf-8"><title>{заголовок}</title>'
                f"<style>{стиль}</style></head><body>{тело}</body></html>")


def добавить_файлы():
    шаг("примеры и справочник")
    поставить("markdown")
    примеры = os.path.join(ПАПКА, "примеры")
    os.makedirs(os.path.join(примеры, "модули"), exist_ok=True)
    for путь in glob.glob(os.path.join(КОРЕНЬ, "примеры", "*.dan")):
        shutil.copy(путь, примеры)
    for путь in glob.glob(os.path.join(КОРЕНЬ, "примеры", "модули", "*.dan")):
        shutil.copy(путь, os.path.join(примеры, "модули"))
    # llms-full.txt - правила языка для агентов (инструмент spravka сервера «dan mcp», AGENTS.md «dan новый»)
    for имя in ("LICENSE", "README.md", "llms-full.txt"):
        shutil.copy(os.path.join(КОРЕНЬ, имя), ПАПКА)
    # значки программы и файлов .dan (для «Пуска» и проводника)
    for имя in ("dan.ico", "dan-fail.ico"):
        shutil.copy(os.path.join(ЗНАЧКИ, имя), ПАПКА)
    в_html(os.path.join(КОРЕНЬ, "справочник.md"), os.path.join(ПАПКА, "справочник.html"), "Справочник языка Дан")
    в_html(os.path.join(КОРЕНЬ, "spec.md"), os.path.join(ПАПКА, "описание_языка.html"), "Описание языка Дан")
    # страницы о модулях стандартной библиотеки из их описаний «///» - командой «документация» dan.exe
    # поставки: модули, написанные после заморозки (нейро, модели), разбирает только компилятор на Дане
    dan = os.path.join(ПАПКА, "dan.exe" if os.name == "nt" else "dan")
    среда = {к: з for к, з in os.environ.items() if к != "DAN_HOME"}
    subprocess.run([dan, "документация", "библиотека", "--в=" + os.path.join(ПАПКА, "документация_библиотеки")],
                   check=True, cwd=КОРЕНЬ, env=среда)


def добавить_учебник_и_редактор():
    шаг("учебник и редактор")
    import uchebnik
    uchebnik.собрать_html(os.path.join(ПАПКА, "учебник"))
    # редактор собирается самим Даном из поставки
    dan = os.path.join(ПАПКА, "dan.exe" if os.name == "nt" else "dan")
    with tempfile.TemporaryDirectory() as папка:
        исходник = os.path.join(папка, "редактор.dan")
        shutil.copy(os.path.join(КОРЕНЬ, "поставка", "редактор", "редактор.dan"), исходник)
        # dan.exe поставки собирает редактор тем же zig, что в поставке
        среда = {к: з for к, з in os.environ.items() if к != "DAN_HOME"}
        if os.name == "nt":
            среда["DAN_COMPILER"] = os.path.join(ПАПКА, "zig", "zig-cc.exe")
        subprocess.run([dan, "собрать", исходник, "--малый"], check=True, env=среда)
        готовый = исходник[:-4] + (".exe" if os.name == "nt" else "")
        shutil.copy(готовый, os.path.join(ПАПКА, os.path.basename(готовый)))


def добавить_llama(архив):
    шаг("библиотека llama.cpp")
    import modeli
    import zipfile
    if архив is None:
        import importlib.util
        спец = importlib.util.spec_from_file_location("sobrat_llama", os.path.join(КОРЕНЬ, "поставка", "собрать_llama.py"))
        сборка = importlib.util.module_from_spec(спец)
        спец.loader.exec_module(сборка)
        работа = tempfile.mkdtemp(prefix="dan_llama_")
        if сборка.main(["--в=" + работа]) != 0:
            raise SystemExit("не удалось собрать библиотеку llama.cpp")
        архив = os.path.join(работа, f"llama-{modeli.ВЕРСИЯ_LLAMA}-{сборка.платформа()}.zip")
    имя = os.path.basename(архив)
    if not имя.startswith(f"llama-{modeli.ВЕРСИЯ_LLAMA}-windows-"):
        raise SystemExit(f"архив {имя} - не llama.cpp {modeli.ВЕРСИЯ_LLAMA} для Windows")
    with zipfile.ZipFile(архив) as з:
        имена = з.namelist()
        if "llama.dll" not in имена or any("/" in и or "\\" in и or и.startswith(".") for и in имена):
            raise SystemExit(f"в архиве {имя} нет llama.dll или есть вложенные папки")
        з.extractall(ПАПКА)
    print(", ".join(sorted(имена)))


def найти_iscc():
    for путь in (shutil.which("iscc"), r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
                 r"C:\Program Files\Inno Setup 6\ISCC.exe"):
        if путь and os.path.isfile(путь):
            return путь
    return None


def собрать_установщик(версия):
    шаг("установщик (Inno Setup)")
    iscc = найти_iscc()
    if not iscc and shutil.which("choco"):
        subprocess.run(["choco", "install", "innosetup", "-y", "--no-progress"], check=False)
        iscc = найти_iscc()
    if not iscc:
        print("Inno Setup 6 не найден: установщик не собран, собран только архив")
        return None
    subprocess.run([iscc, "/Q", f"/DAppVer={версия}", os.path.join(КОРЕНЬ, "поставка", "dan.iss")], check=True)
    return os.path.join(ГОТОВО, f"dan-{версия}-setup.exe")


def main(аргументы):
    import versiya
    версия = versiya.ВЕРСИЯ
    архив_llama = None
    for а in аргументы:
        if а.startswith("--llama="):
            архив_llama = os.path.abspath(а[len("--llama="):])
        else:
            print(__doc__.strip())
            return 2
    shutil.rmtree(ГОТОВО, ignore_errors=True)
    os.makedirs(ПАПКА)
    добавить_llama(архив_llama)
    добавить_zig()
    собрать_компилятор_на_дане()
    добавить_файлы()
    добавить_учебник_и_редактор()
    шаг("переносной архив")
    архив = shutil.make_archive(os.path.join(ГОТОВО, f"dan-{версия}-windows"), "zip", ГОТОВО, "dan")
    установщик = собрать_установщик(версия) if os.name == "nt" else None
    шаг("готово")
    for путь in filter(None, (установщик, архив)):
        print(f"{путь}: {os.path.getsize(путь) // (1024 * 1024)} МБ")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
