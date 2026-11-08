"""Сборка библиотеки llama.cpp закреплённой версии для модуля «модели» - только для процессора.

    python поставка/собрать_llama.py [--исходники=папка] [--в=папка] [--llama-simple]

Версия llama.cpp - метка выпуска modeli.ВЕРСИЯ_LLAMA (одно место для всего Дана). Сценарий
загружает исходники этой метки (git clone), собирает их CMake и складывает в папку
«поставка/готово» (или в папку --в=...):

    llama-<метка>-<система>/          - готовые файлы библиотеки;
    llama-<метка>-<система>.zip       - те же файлы одним архивом (без папки внутри).

Система: linux-x64, linux-arm64, macos-arm64 или windows-x64 - та, на которой запущен сценарий.

Что собирается: llama (llama.dll, libllama.so, libllama.dylib), ggml и ggml-base, а также
варианты вычислений на процессоре ggml-cpu-*: по одному на набор команд (SSE4.2, AVX2,
AVX-512 и т.д.; для ARM - armv8.0, armv8.2 с dotprod и т.д.). Видеокарты (CUDA, Metal,
Vulkan) выключены. Сборка не подстраивается под процессор машины сборки (GGML_NATIVE=OFF):
вариант для процессора выбирается при выполнении программы, когда она загружает библиотеку
(GGML_BACKEND_DL и GGML_CPU_ALL_VARIANTS). Рядом кладутся лицензия llama.cpp и файл
version.txt (первая строка - метка, вторая - коммит).

С --llama-simple рядом кладётся и пример llama.cpp llama-simple-<метка>-<система>[.exe]: тесты
модуля «модели» сверяют с ним ответ Дана (в архив выпуска он не входит).

Нужны: git, CMake 3.21 и новее, компилятор C++ (Linux - gcc или clang, macOS - Xcode,
Windows - Visual Studio 2022 с компонентом «Сборка C++» и clang; окружение Visual Studio
сценарий находит сам через vswhere). Ninja необязателен, в Windows ставится сам (pip).
"""

import glob
import os
import platform
import shutil
import subprocess
import sys
import zipfile

КОРЕНЬ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, КОРЕНЬ)
import modeli  # noqa: E402

АДРЕС = "https://github.com/ggml-org/llama.cpp"

# Главная библиотека (её загружает модуль «модели») и библиотеки, от которых она зависит;
# варианты для процессора ggml-cpu-*.so (в Windows ggml-cpu-*.dll) лежат рядом и загружаются
# функцией ggml_backend_load_all_from_path(папка). В macOS варианты тоже называются *.so.
ФАЙЛЫ = {
    "linux": (["libllama.so", "libggml.so", "libggml-base.so"], "libggml-cpu-*.so"),
    "macos": (["libllama.dylib", "libggml.dylib", "libggml-base.dylib"], "libggml-cpu-*.so"),
    "windows": (["llama.dll", "ggml.dll", "ggml-base.dll"], "ggml-cpu-*.dll"),
}


def шаг(текст):
    print(f"\n==> {текст}", flush=True)


def система():
    с = platform.system()
    if с == "Linux":
        return "linux"
    if с == "Darwin":
        return "macos"
    if с == "Windows":
        return "windows"
    raise SystemExit(f"Сборка библиотеки llama.cpp для системы {с} не поддерживается")


def процессор():
    п = platform.machine().lower()
    if п in ("x86_64", "amd64", "x64"):
        return "x64"
    if п in ("aarch64", "arm64"):
        return "arm64"
    raise SystemExit(f"Сборка библиотеки llama.cpp для процессора {п} не поддерживается")


def платформа():
    return f"{система()}-{процессор()}"


def выполнить(команда, **параметры):
    print("$ " + " ".join(команда), flush=True)
    subprocess.run(команда, check=True, **параметры)


def среда_visual_studio():
    """Переменные среды Visual Studio (как после vcvarsall.bat x64) или None."""
    if os.environ.get("VCINSTALLDIR"):
        return dict(os.environ)
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    р = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                       capture_output=True, text=True)
    папка = р.stdout.strip().splitlines()[0] if р.stdout.strip() else ""
    vcvars = os.path.join(папка, "VC", "Auxiliary", "Build", "vcvarsall.bat")
    if not os.path.isfile(vcvars):
        return None
    р = subprocess.run(f'cmd /s /c ""{vcvars}" x64 >nul && set"', capture_output=True, text=True, shell=True)
    if р.returncode != 0:
        return None
    среда = {}
    for строка in р.stdout.splitlines():
        if "=" in строка:
            имя, значение = строка.split("=", 1)
            среда[имя] = значение
    return среда


def найти_ninja(среда):
    ninja = shutil.which("ninja", path=среда.get("PATH") or среда.get("Path"))
    if ninja or система() != "windows":
        return ninja
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "ninja"], check=True)
    import ninja as модуль
    путь = os.path.join(модуль.BIN_DIR, "ninja.exe")
    return путь if os.path.isfile(путь) else None


def загрузить_исходники(метка, папка):
    if os.path.isdir(os.path.join(папка, ".git")):
        return
    шаг(f"исходники llama.cpp {метка}")
    выполнить(["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--branch", метка,
               АДРЕС, папка])


def параметры_cmake(сис, исходники):
    п = [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=ON",
        # только процессор: варианты для разных наборов команд, выбор при загрузке
        "-DGGML_BACKEND_DL=ON",
        "-DGGML_CPU_ALL_VARIANTS=ON",
        "-DGGML_NATIVE=OFF",
        "-DGGML_METAL=OFF",
        "-DGGML_CUDA=OFF",
        "-DGGML_VULKAN=OFF",
        "-DGGML_RPC=OFF",
        "-DGGML_BLAS=OFF",
        # OpenMP потребовал бы отдельную библиотеку (libgomp, vcomp) на машине пользователя;
        # у ggml свой пул потоков
        "-DGGML_OPENMP=OFF",
        # только библиотека: без программ, примеров, тестов, сервера и HTTPS
        "-DLLAMA_BUILD_COMMON=OFF",
        "-DLLAMA_BUILD_TESTS=OFF",
        "-DLLAMA_BUILD_TOOLS=OFF",
        "-DLLAMA_BUILD_EXAMPLES=OFF",
        "-DLLAMA_BUILD_SERVER=OFF",
        "-DLLAMA_BUILD_APP=OFF",
        "-DLLAMA_OPENSSL=OFF",
        # имена файлов без номера версии (libllama.so, а не libllama.so.0.0.N): в архиве нет ссылок
        "-DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON",
    ]
    if сис == "linux":
        п += ["-DCMAKE_INSTALL_RPATH=$ORIGIN", "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"]
    elif сис == "macos":
        # Accelerate (векторные функции Apple) остаётся включённым, Metal выключен выше
        п += ["-DCMAKE_INSTALL_RPATH=@loader_path", "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON",
              "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3", "-DGGML_ACCELERATE=ON"]
    elif сис == "windows":
        # одна общая среда C (CRT) для всех DLL: llama.dll открывает файл модели функцией ggml-base.dll и
        # работает с полученным FILE*, а при CRT, встроенной в каждую DLL (MultiThreaded), FILE* одной DLL
        # чужой для другой - загрузка модели останавливалась (0xC0000409). Библиотеки среды C++
        # (vcruntime140*.dll, msvcp140*.dll) кладутся в архив рядом с llama.dll (сложить), поэтому
        # Visual C++ Redistributable на машине пользователя по-прежнему не нужен
        п += ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"]
        # clang даёт больше вариантов для процессора (у MSVC нет части наборов команд) и более
        # быстрый код ggml; так собирает и сам llama.cpp
        if shutil.which("clang"):
            п += ["-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(исходники, "cmake", "x64-windows-llvm.cmake")]
    return п


def собрать(исходники, сборка):
    сис = система()
    среда = dict(os.environ)
    if сис == "windows":
        найдено = среда_visual_studio()
        if найдено is None:
            raise SystemExit("Не найдена Visual Studio с компонентом «Сборка C++» (vswhere); запустите сценарий "
                             "в «Developer PowerShell for VS» или установите Visual Studio Build Tools")
        среда = найдено
    параметры = параметры_cmake(сис, исходники)
    ninja = найти_ninja(среда)
    if ninja:
        параметры = ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja] + параметры
    шаг("настройка CMake")
    выполнить(["cmake", "-S", исходники, "-B", сборка] + параметры, env=среда)
    шаг("сборка")
    выполнить(["cmake", "--build", сборка, "--config", "Release", "-j", str(os.cpu_count() or 2)], env=среда)


def собрать_пример(исходники, сборка):
    """Пример llama-simple той же сборкой: настройки дополняются примерами, библиотеки не пересобираются."""
    сис = система()
    среда = среда_visual_studio() if сис == "windows" else dict(os.environ)
    шаг("пример llama-simple")
    выполнить(["cmake", "-S", исходники, "-B", сборка, "-DLLAMA_BUILD_COMMON=ON", "-DLLAMA_BUILD_EXAMPLES=ON"], env=среда)
    выполнить(["cmake", "--build", сборка, "--config", "Release", "-j", str(os.cpu_count() or 2),
               "--target", "llama-simple"], env=среда)
    имя = "llama-simple" + (".exe" if сис == "windows" else "")
    путь = os.path.join(папка_результата(сборка), имя)
    if not os.path.isfile(путь):
        raise SystemExit(f"Сборка не создала {имя}")
    return путь


def папка_результата(сборка):
    for п in (os.path.join(сборка, "bin", "Release"), os.path.join(сборка, "bin")):
        if os.path.isdir(п):
            return п
    raise SystemExit("Сборка llama.cpp не создала папку bin")


# Библиотеки среды C++ Visual Studio, которые нужны llama.dll и ggml*.dll (сборка с MultiThreadedDLL);
# распространять их вместе с программой разрешено (Visual C++ redistributable, app-local).
CRT = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")


def библиотеки_crt():
    """Пути к библиотекам CRT из папки redist Visual Studio (переменная VCToolsRedistDir)."""
    среда = среда_visual_studio() or {}
    redist = среда.get("VCToolsRedistDir") or os.environ.get("VCToolsRedistDir") or ""
    папки = sorted(glob.glob(os.path.join(redist, "x64", "Microsoft.VC*.CRT")))
    if not папки:
        raise SystemExit(f"Не найдена папка библиотек среды C++ x64\\Microsoft.VC*.CRT в «{redist}» (VCToolsRedistDir)")
    пути = []
    for имя in CRT:
        путь = os.path.join(папки[-1], имя)
        if not os.path.isfile(путь):
            raise SystemExit(f"Нет {имя} в {папки[-1]}")
        пути.append(путь)
    return пути


def сложить(исходники, сборка, цель, метка):
    сис = система()
    главные, образец = ФАЙЛЫ[сис]
    откуда = папка_результата(сборка)
    shutil.rmtree(цель, ignore_errors=True)
    os.makedirs(цель)
    варианты = sorted(os.path.basename(п) for п in glob.glob(os.path.join(откуда, образец)))
    if not варианты:
        raise SystemExit(f"Сборка не создала вариантов для процессора ({образец}) в {откуда}")
    for имя in главные + варианты:
        путь = os.path.join(откуда, имя)
        if not os.path.isfile(путь):
            raise SystemExit(f"Сборка не создала файл {имя} в {откуда}")
        shutil.copyfile(путь, os.path.join(цель, имя))
    if сис == "windows":
        for путь in библиотеки_crt():
            shutil.copyfile(путь, os.path.join(цель, os.path.basename(путь)))
    shutil.copyfile(os.path.join(исходники, "LICENSE"), os.path.join(цель, "LICENSE-llama.cpp.txt"))
    р = subprocess.run(["git", "-C", исходники, "rev-parse", "HEAD"], capture_output=True, text=True)
    with open(os.path.join(цель, "version.txt"), "w", encoding="utf-8", newline="\n") as ф:
        ф.write(f"{метка}\n{р.stdout.strip()}\n")
    return варианты


def упаковать(папка, архив):
    if os.path.exists(архив):
        os.remove(архив)
    with zipfile.ZipFile(архив, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as з:
        for имя in sorted(os.listdir(папка)):
            з.write(os.path.join(папка, имя), имя)


def main(аргументы):
    метка = modeli.ВЕРСИЯ_LLAMA
    в = os.path.join(КОРЕНЬ, "поставка", "готово")
    исходники = None
    пример = False
    for а in аргументы:
        if а == "--llama-simple":
            пример = True
            continue
        if а.startswith("--в="):
            в = os.path.abspath(а[len("--в="):])
        elif а.startswith("--исходники="):
            исходники = os.path.abspath(а[len("--исходники="):])
        else:
            print(__doc__.strip())
            return 2
    плат = платформа()
    работа = os.path.join(в, "_llama")
    if система() == "windows" and not работа.isascii():
        # инструменты сборки в Windows (rc.exe, компоновщик) не всегда понимают русские буквы в путях
        работа = os.path.join(os.environ.get("SystemDrive", "C:") + os.sep, "dan_llama")
    os.makedirs(работа, exist_ok=True)
    if исходники is None:
        исходники = os.path.join(работа, "llama.cpp-" + метка)
        загрузить_исходники(метка, исходники)
    сборка = os.path.join(работа, "build-" + плат)
    собрать(исходники, сборка)
    имя = f"llama-{метка}-{плат}"
    цель = os.path.join(в, имя)
    варианты = сложить(исходники, сборка, цель, метка)
    архив = os.path.join(в, имя + ".zip")
    упаковать(цель, архив)
    if пример:
        simple = собрать_пример(исходники, сборка)
        shutil.copy2(simple, os.path.join(в, f"llama-simple-{метка}-{плат}" + os.path.splitext(simple)[1]))
    шаг("готово")
    print(f"варианты для процессора: {', '.join(варианты)}")
    print(f"{архив}: {os.path.getsize(архив) / (1024 * 1024):.1f} МБ")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
