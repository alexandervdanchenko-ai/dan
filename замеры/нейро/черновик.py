"""Замер спекулятивной генерации движка «нейро» (м.ответ_с_черновиком) на настоящих моделях.

    python замеры/нейро/черновик.py дан большая.gguf черновик.gguf [--потоков=N] [--токенов=N]
                                   [--к=2,3,4,5,6,8] [--llama=папка_libllama] [--llama-черновик=llama-speculative-simple]
                                   [--итог=файл.md]

дан - компилятор Дана (semya/dan). Для каждого запроса (английский, код, русский; шаблон чата ChatML):
  - скорость порождения без черновика и с черновиком при каждом к (жадная выборка), токенов в секунду
    (без чтения запроса), доля принятых предложений и совпадение текста с ответом без черновика;
  - то же с подстройкой числа предложений и с температурой 0,7 (к = 4);
  - если задана папка libllama (--llama) - скорость llama.cpp без черновика (замеры/нейро/сверка_llama.c);
    если задан llama-speculative-simple той же сборки (--llama-черновик) - его скорость и доля принятых при
    тех же к (жадная выборка, тот же запрос и число токенов).
Таблица печатается и дописывается в файл --итог (например $GITHUB_STEP_SUMMARY).
"""

import os
import re
import subprocess
import sys
import tempfile

ЗДЕСЬ = os.path.dirname(os.path.abspath(__file__))

ЗАПРОСЫ = [
    ("англ.", "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\n"
              "Explain in a few sentences how a CPU cache works.<|im_end|>\n<|im_start|>assistant\n"),
    ("код", "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\n"
            "Write a Python function that checks whether a number is prime. Add comments.<|im_end|>\n"
            "<|im_start|>assistant\n"),
    ("рус.", "<|im_start|>system\nТы - полезный помощник.<|im_end|>\n<|im_start|>user\n"
             "Напиши короткое стихотворение о море.<|im_end|>\n<|im_start|>assistant\n"),
]


def разобрать(текст):
    итог = {}
    for строка in текст.splitlines():
        ключ, _, значение = строка.partition(":")
        if _:
            итог.setdefault(ключ.strip(), []).append(значение.strip())
    return итог


def запустить(команда, окружение=None):
    р = subprocess.run(команда, capture_output=True, env={**os.environ, **(окружение or {})}, timeout=7200)
    if р.returncode != 0:
        sys.stderr.write(р.stdout.decode("utf-8", "replace")[-2000:] + р.stderr.decode("utf-8", "replace")[-3000:])
        raise SystemExit("не выполнилось: " + " ".join(команда))
    return разобрать(р.stdout.decode("utf-8", "replace"))


def llama_черновик(программа, большая, черновик, файл, токенов, потоков, к):
    """Скорость и доля принятых llama-speculative-simple (None, если не вышло)."""
    команда = [программа, "-m", большая, "-md", черновик, "-f", файл, "-n", str(токенов), "-t", str(потоков),
               "-td", str(потоков), "-c", "1024", "--draft-max", str(к), "--draft-min", "0", "--temp", "0",
               "--top-k", "1", "-ngl", "0", "-ngld", "0"]
    try:
        р = subprocess.run(команда, capture_output=True, timeout=3600)
    except subprocess.TimeoutExpired:
        return None
    вывод_ = р.stdout.decode("utf-8", "replace") + р.stderr.decode("utf-8", "replace")
    скорость = re.findall(r"decoded\s+(\d+) tokens in\s+([\d.]+) seconds, speed:\s+([\d.]+) t/s", вывод_)
    доля = re.findall(r"accept\s*=\s*([\d.]+)%", вывод_)
    if р.returncode != 0 or not скорость:
        sys.stderr.write("llama-speculative-simple (к=%d): код %d\n%s\n" % (к, р.returncode, вывод_[-1500:]))
        return None
    return float(скорость[-1][2]), (float(доля[-1]) / 100 if доля else None)


def главная():
    арг = [а for а in sys.argv[1:] if not а.startswith("--")]
    ключи = dict(а[2:].split("=", 1) for а in sys.argv[1:] if а.startswith("--") and "=" in а)
    дан, большая, черновик = арг[0], os.path.abspath(арг[1]), os.path.abspath(арг[2])
    потоков = int(ключи.get("потоков", os.cpu_count() or 1))
    токенов = int(ключи.get("токенов", "128"))
    к_ = [int(к) for к in ключи.get("к", "2,3,4,5,6,8").split(",")]
    папка = tempfile.mkdtemp(prefix="dan_chernovik_")
    программа = os.path.join(папка, "chernovik.dan")
    with open(os.path.join(ЗДЕСЬ, "черновик.dan"), encoding="utf-8") as ф, open(программа, "w", encoding="utf-8") as в:
        в.write(ф.read())
    subprocess.run([дан, "собрать", программа], check=True)
    движок = программа[:-4]
    сверка = None
    if ключи.get("llama"):
        сверка = os.path.join(папка, "sverka_llama")
        subprocess.run(["gcc", "-std=c11", "-O2", os.path.join(ЗДЕСЬ, "сверка_llama.c"), "-o", сверка, "-ldl"], check=True)
    спек = ключи.get("llama-черновик")
    if спек and not os.path.exists(спек):
        спек = None
    имя = os.path.basename(большая) + " + " + os.path.basename(черновик)
    шапка = ("| Запрос | Без черновика | " + " | ".join("к=%d" % к for к in к_) + " | Подстройка (от 4) | "
             "Температура 0,7 (к=4) | Текст как без черновика |")
    таблица = [шапка, "|" + "---|" * (шапка.count("|") - 1)]
    llama_таблица = ["| Запрос | llama.cpp без черновика | " + " | ".join("к=%d" % к for к in к_) + " |",
                     "|" + "---|" * (len(к_) + 2)]
    подробно = []
    окр = {"DAN_POTOKI": str(потоков)}
    for метка, запрос in ЗАПРОСЫ:
        файл = os.path.join(папка, "zapros_%s.txt" % len(подробно))
        with open(файл, "w", encoding="utf-8", newline="\n") as ф:
            ф.write(запрос)
        print("==", имя, метка, flush=True)
        о = запустить([движок, большая, черновик, файл, str(токенов), ",".join(map(str, к_))], окр)
        обычный = float(о["обычный"][0])
        ячейки, совпали = [], True
        for к in к_:
            скорость, предложено, принято, проверок, совпал = о["к=%d" % к][0].split()
            доля = int(принято) / max(1, int(предложено))
            совпали = совпали and совпал == "истина"
            ячейки.append("%.2f (x%.2f, %.0f%%)" % (float(скорость), float(скорость) / обычный, доля * 100))
        скорость, предложено, принято, проверок, совпал = о["подстройка"][0].split()
        совпали = совпали and совпал == "истина"
        ячейки.append("%.2f (x%.2f, %.0f%%)" % (float(скорость), float(скорость) / обычный,
                                              int(принято) / max(1, int(предложено)) * 100))
        т_скорость, т_доля = о["температура"][0].split()
        ячейки.append("%.2f (x%.2f, %.0f%%)" % (float(т_скорость), float(т_скорость) / обычный, float(т_доля) * 100))
        таблица.append("| %s | %.2f | %s | %s |" % (метка, обычный, " | ".join(ячейки), "да" if совпали else "НЕТ"))
        подробно.append("%s, %s: загрузка %s с; %s" % (имя, метка, о["загрузка"][0], о["текст"][0][:300]))
        if сверка:
            их = запустить([сверка, os.path.join(ключи["llama"], "libllama.so"), большая, файл, str(токенов),
                            str(потоков), "f16"], {"LD_LIBRARY_PATH": ключи["llama"]})
            л_ячейки = []
            for к in к_:
                р = llama_черновик(спек, большая, черновик, файл, токенов, потоков, к) if спек else None
                if р:
                    л_ячейки.append("%.2f (%s)" % (р[0], "-" if р[1] is None else "%.0f%%" % (р[1] * 100)))
                else:
                    л_ячейки.append("-")
            llama_таблица.append("| %s | %s | %s |" % (метка, их["скорость"][0], " | ".join(л_ячейки)))
    итог = ("Пара: %s, потоков: %d, токенов ответа: %d. В ячейке - токенов в секунду порождения (без чтения "
            "запроса), ускорение относительно движка без черновика и доля принятых предложений.\n\n"
            % (имя, потоков, токенов) + "\n".join(таблица) + "\n")
    if сверка:
        итог += ("\nllama.cpp той же версии (кэш f16), токенов в секунду; с черновиком - llama-speculative-simple "
                 "(в скобках - доля принятых; «-» - не собран или не выполнился):\n\n" + "\n".join(llama_таблица) + "\n")
    итог += "\n```\n" + "\n".join(подробно) + "\n```\n"
    print(итог)
    if "итог" in ключи:
        with open(ключи["итог"], "a", encoding="utf-8") as ф:
            ф.write("## Спекулятивная генерация движка «нейро»\n\n" + итог)


if __name__ == "__main__":
    главная()
