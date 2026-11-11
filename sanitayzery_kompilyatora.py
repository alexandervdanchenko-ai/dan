"""Компилятор на Дане под санитайзерами (ASan, LSan, UBSan).

Компилятор собирается из семени `semya/dan.c` с -fsanitize=address,undefined, переводит сам себя
(код C обязан совпасть с семенем) и все программы каталога, примеров, совместимости, показательных
и задач учебника: команда «си» для каждой и «проверить --json» для программ с ошибками. Вывод и код
завершения сверяются с компилятором, собранным обычно, а любое сообщение санитайзера - ошибка.

    python sanitayzery_kompilyatora.py              # все программы
    python sanitayzery_kompilyatora.py 40           # первые 40 (для быстрой проверки)

Санитайзеры увеличивают кадры стека, поэтому стек основного потока поднимается до 256 МБ:
защита от переполнения стека в среде Дана считает предел по размеру стека.
"""

import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile

ПАПКА = os.path.dirname(os.path.abspath(__file__))
ПАПКИ = ["каталог", "примеры", "совместимость", "показательные", os.path.join("учебник", "задачи")]
ПРИЗНАКИ = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:", "SUMMARY: UndefinedBehaviorSanitizer")
СТЕК = 256 * 1024 * 1024


def компилятор_c():
    return os.environ.get("DAN_COMPILER") or "cc"


def собрать(выход, санитайзеры):
    флаги = ["-std=c11", "-fwrapv", "-ffp-contract=off", "-w"]
    if санитайзеры:
        флаги += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer"]
    else:
        флаги += ["-O2"]
    subprocess.run([компилятор_c(), *флаги, os.path.join(ПАПКА, "semya", "dan.c"), "-o", выход, "-lm"], check=True)


def большой_стек():
    """Поднимает предел стека до СТЕК, насколько разрешает система (в macOS жёсткий предел - 64 МБ)."""
    import resource                     # только Linux и macOS
    try:
        мягкий, жёсткий = resource.getrlimit(resource.RLIMIT_STACK)
        нужно = СТЕК if жёсткий == resource.RLIM_INFINITY else min(СТЕК, жёсткий)
        if мягкий != resource.RLIM_INFINITY and мягкий < нужно:
            resource.setrlimit(resource.RLIMIT_STACK, (нужно, жёсткий))
    except (ValueError, OSError):
        pass


def выполнить(компилятор, аргументы):
    окружение = dict(os.environ, DAN_HOME=ПАПКА, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0",
                     UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    р = subprocess.run([компилятор, *аргументы], cwd=ПАПКА, capture_output=True, env=окружение,
                       preexec_fn=большой_стек, timeout=1800)
    return р.returncode, р.stdout, р.stderr.decode("utf-8", "replace")


def программы(предел=None):
    файлы = []
    for п in ПАПКИ:
        файлы += sorted(glob.glob(os.path.join(ПАПКА, п, "**", "*.dan"), recursive=True))
    файлы = [os.path.relpath(ф, ПАПКА) for ф in файлы]
    return файлы[:предел] if предел else файлы


def сверить(обычный, проверяемый, аргументы):
    """Пустая строка, если всё сходится, иначе описание расхождения."""
    код_о, вывод_о, ош_о = выполнить(обычный, аргументы)
    код_с, вывод_с, ош_с = выполнить(проверяемый, аргументы)
    for признак in ПРИЗНАКИ:
        if признак in ош_с:
            return "санитайзер: " + ош_с[ош_с.find(признак) - 200 if ош_с.find(признак) > 200 else 0:][:3000]
    if (код_о, вывод_о) != (код_с, вывод_с):
        return f"вывод разный: код {код_о} и {код_с}\n{ош_с[:2000]}"
    return ""


def главная(аргументы):
    предел = int(аргументы[0]) if аргументы else None
    папка = tempfile.mkdtemp(prefix="dan-san-")
    обычный = os.path.join(папка, "dan")
    проверяемый = os.path.join(папка, "dan-san")
    print("сборка компилятора из семени (обычная и с санитайзерами)", flush=True)
    with concurrent.futures.ThreadPoolExecutor(2) as п:
        for з in [п.submit(собрать, обычный, False), п.submit(собрать, проверяемый, True)]:
            з.result()
    ошибки = []
    # 1. компилятор переводит сам себя: код C совпадает с семенем
    код, вывод, ош = выполнить(проверяемый, ["си", os.path.join("сам", "дан.dan")])
    with open(os.path.join(ПАПКА, "semya", "dan.c"), "rb") as ф:
        семя = ф.read()
    if код != 0 or вывод != семя or any(п in ош for п in ПРИЗНАКИ):
        ошибки.append(("сам/дан.dan", f"самоперевод: код {код}, совпадает с семенем: {вывод == семя}\n{ош[:3000]}"))
    print("самоперевод:", "ошибка" if ошибки else "ок", flush=True)
    # 2. программы: «си» и «проверить --json»
    задания = []
    for ф in программы(предел):
        задания.append((ф, ["си", ф]))
        задания.append((ф, ["проверить", "--json", ф]))
    потоков = max(1, (os.cpu_count() or 2))
    with concurrent.futures.ThreadPoolExecutor(потоков) as п:
        итоги = list(п.map(lambda з: (з[0], " ".join(з[1][:-1]), сверить(обычный, проверяемый, з[1])), задания))
    for ф, команда, итог in итоги:
        if итог:
            ошибки.append((ф + " (" + команда + ")", итог))
    print(f"программ: {len(задания) // 2}, команд: {len(задания)}, расхождений и находок: {len(ошибки)}")
    for где, что in ошибки:
        print("\n==== " + где + "\n" + что)
    return 1 if ошибки else 0


if __name__ == "__main__":
    sys.exit(главная(sys.argv[1:]))
