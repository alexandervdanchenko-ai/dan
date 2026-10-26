"""Сборка поставки Дана для Windows: dan.exe без Python, компилятор zig рядом,
установщик и переносной архив.

Запускается в Windows (на GitHub - задачей «Поставка»):

    python поставка/собрать.py

Результат в папке «поставка/готово»:
    dan/                              - готовая папка Дана (dan.exe, zig, примеры, справочник);
    dan-<версия>-setup.exe            - установщик (нужен Inno Setup 6);
    dan-<версия>-windows.zip          - та же папка одним архивом, без установки.

Нужны: Python 3 с пакетами pyinstaller, ziglang и markdown (ставятся сами),
Inno Setup 6 (если его нет, установщик не собирается, архив собирается).
"""

import glob
import os
import shutil
import subprocess
import sys

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ГОТОВО = os.path.join(КОРЕНЬ, "поставка", "готово")
ПАПКА = os.path.join(ГОТОВО, "dan")
sys.path.insert(0, КОРЕНЬ)

# Заголовки C для систем, под которые Дан не собирает программы: в поставку не входят.
ЛИШНЕЕ_В_ZIG = ["freebsd", "netbsd", "openbsd", "dragonfly", "haiku", "solaris", "illumos", "serenity"]


def шаг(текст):
    print(f"\n==> {текст}", flush=True)


def поставить(*пакеты):
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--upgrade", *пакеты], check=True)


def собрать_dan_exe():
    шаг("dan.exe (PyInstaller)")
    поставить("pyinstaller")
    работа = os.path.join(ГОТОВО, "_сборка")
    разделитель = ";" if os.name == "nt" else ":"
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--console",
                    "--name", "dan", "--distpath", ГОТОВО, "--workpath", работа, "--specpath", работа,
                    "--add-data", os.path.join(КОРЕНЬ, "sreda") + разделитель + "sreda",
                    "--add-data", os.path.join(КОРЕНЬ, "библиотека") + разделитель + "библиотека",
                    os.path.join(КОРЕНЬ, "dan.py")], check=True, cwd=КОРЕНЬ)
    shutil.rmtree(работа, ignore_errors=True)


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
    for имя in ("LICENSE", "README.md"):
        shutil.copy(os.path.join(КОРЕНЬ, имя), ПАПКА)
    в_html(os.path.join(КОРЕНЬ, "справочник.md"), os.path.join(ПАПКА, "справочник.html"), "Справочник языка Дан")
    в_html(os.path.join(КОРЕНЬ, "spec.md"), os.path.join(ПАПКА, "описание_языка.html"), "Описание языка Дан")


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


def main():
    import versiya
    версия = versiya.ВЕРСИЯ
    shutil.rmtree(ГОТОВО, ignore_errors=True)
    os.makedirs(ГОТОВО)
    собрать_dan_exe()
    добавить_zig()
    добавить_файлы()
    шаг("переносной архив")
    архив = shutil.make_archive(os.path.join(ГОТОВО, f"dan-{версия}-windows"), "zip", ГОТОВО, "dan")
    установщик = собрать_установщик(версия) if os.name == "nt" else None
    шаг("готово")
    for путь in filter(None, (установщик, архив)):
        print(f"{путь}: {os.path.getsize(путь) // (1024 * 1024)} МБ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
