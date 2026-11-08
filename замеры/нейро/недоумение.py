"""Недоумение (perplexity) моделей: движок «нейро» (м.недоумение, библиотека/нейро.dan) против
llama-perplexity той же версии llama.cpp на одном и том же тексте и в тех же окнах.

    python замеры/нейро/недоумение.py папка_llama дан текст.txt модель.gguf [модель.gguf ...]
                                      [--окно=512] [--окон=16] [--потоков=4] [--итог=файл.md] [--шапка]

папка_llama - папка с llama-perplexity и её библиотеками (собрана с -DLLAMA_BUILD_TOOLS=ON). Обоим
движкам - один файл текста, окно (-c) и число окон (--chunks), кэш f16 (Дан - дроб16, по умолчанию),
одно число потоков. Для каждой модели - строка таблицы: недоумение и погрешность Дана и llama.cpp,
относительная разница, время. «--шапка» - сначала шапка таблицы (сценарий вызывает скрипт для каждой
модели по очереди: файл модели скачивается, меряется и удаляется, чтобы не держать на диске все).
"""

import os
import re
import subprocess
import sys
import tempfile
import time

ЗДЕСЬ = os.path.dirname(os.path.abspath(__file__))


def llama_perplexity(папка, модель, текст, окно, окон, потоков):
    окружение = {**os.environ, "LD_LIBRARY_PATH": папка}
    начало = time.time()
    р = subprocess.run([os.path.join(папка, "llama-perplexity"), "-m", модель, "-f", текст, "-c", str(окно),
                        "--chunks", str(окон), "-t", str(потоков), "-b", str(окно)], capture_output=True, timeout=4 * 3600,
                       env=окружение)
    секунд = time.time() - начало
    весь = р.stdout.decode("utf-8", "replace") + р.stderr.decode("utf-8", "replace")
    итог = re.search(r"Final estimate: PPL = ([0-9.]+) \+/- ([0-9.]+)", весь)
    окна = re.findall(r"\[(\d+)\]([0-9.]+),", весь)
    if not итог:
        sys.stderr.write(весь[-3000:])
        return None
    return {"значение": float(итог.group(1)), "погрешность": float(итог.group(2)), "секунд": секунд,
            "по_окнам": [float(з) for _, з in окна]}


def дан_недоумение(программа, модель, текст, окно, окон, потоков):
    окружение = {**os.environ, "DAN_POTOKI": str(потоков)}
    начало = time.time()
    р = subprocess.run([программа, модель, текст, str(окно), str(окон)], capture_output=True, timeout=6 * 3600,
                       env=окружение)
    секунд = time.time() - начало
    вывод = р.stdout.decode("utf-8", "replace")
    итог = {}
    for строка in вывод.splitlines():
        ключ, _, значение = строка.partition(": ")
        if _:
            итог[ключ] = значение
    if "недоумение" not in итог:
        sys.stderr.write(вывод[-2000:] + р.stderr.decode("utf-8", "replace")[-2000:])
        return None
    значение, погрешность = (float(х) for х in итог["недоумение"].split())
    return {"значение": значение, "погрешность": погрешность, "секунд": секунд,
            "счёт": float(итог.get("секунд", "0")), "по_окнам": [float(х) for х in итог["по_окнам"].split()]}


def главная():
    арг = [а for а in sys.argv[1:] if not а.startswith("--")]
    ключи = dict(а[2:].split("=", 1) for а in sys.argv[1:] if а.startswith("--") and "=" in а)
    llama, дан, текст = os.path.abspath(арг[0]), арг[1], os.path.abspath(арг[2])
    модели = [os.path.abspath(м) for м in арг[3:]]
    окно = int(ключи.get("окно", "512"))
    окон = int(ключи.get("окон", "16"))
    потоков = int(ключи.get("потоков", str(os.cpu_count() or 4)))
    папка = tempfile.mkdtemp(prefix="dan_nedoumenie_")
    программа = os.path.join(папка, "nedoumenie.dan")
    with open(os.path.join(ЗДЕСЬ, "недоумение.dan"), encoding="utf-8") as ф, open(программа, "w", encoding="utf-8") as в:
        в.write(ф.read())
    subprocess.run([дан, "собрать", программа], check=True)
    программа = программа[:-4]
    строки = []
    if "--шапка" in sys.argv:
        строки += [f"Окно {окно} токенов, окон {окон}, потоков {потоков}, кэш f16 (Дан - дроб16), текст "
                   f"{os.path.basename(текст)} ({os.path.getsize(текст)} байт)", "",
                   "| Модель | Файл, МБ | Дан: недоумение | llama.cpp: недоумение | Дан / llama.cpp - 1 | "
                   "Наибольшая разница по окнам | Время, с: Дан / llama.cpp |",
                   "|---|---|---|---|---|---|---|"]
    подробно = []
    for модель in модели:
        имя = os.path.basename(модель)
        размер = os.path.getsize(модель) / 1048576
        print("==", имя, int(размер), "МБ", flush=True)
        их = llama_perplexity(llama, модель, текст, окно, окон, потоков)
        наше = дан_недоумение(программа, модель, текст, окно, окон, потоков)
        подробно.append(f"{имя}: Дан {наше}, llama.cpp {их}")
        def ячейка(р):
            return "%.4f ± %.4f" % (р["значение"], р["погрешность"]) if р else "-"
        разница = "-"
        по_окнам = "-"
        if наше and их:
            разница = "%+.2e" % (наше["значение"] / их["значение"] - 1)
            пары = list(zip(наше["по_окнам"], их["по_окнам"]))
            if пары:
                по_окнам = "%.2e" % max(abs(а / б - 1) for а, б in пары)
        время = "%s / %s" % ("%.0f" % наше["секунд"] if наше else "-", "%.0f" % их["секунд"] if их else "-")
        строки.append(f"| {имя} | {размер:.0f} | {ячейка(наше)} | {ячейка(их)} | {разница} | {по_окнам} | {время} |")
    итог = "\n".join(строки) + "\n"
    print(итог)
    print("\n".join(подробно))
    if "итог" in ключи:
        with open(ключи["итог"], "a", encoding="utf-8") as ф:
            if "--шапка" in sys.argv:
                ф.write("## Недоумение: Дан и llama-perplexity\n\n")
            ф.write(итог)


if __name__ == "__main__":
    главная()
