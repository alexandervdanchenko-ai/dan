"""Замер модуля «модели»: токены в секунду порождения через Дан и через llama-completion той же версии.

    python замеры/модели/замер.py папка_llama [модель.gguf]

папка_llama - папка с libllama и llama-completion одной сборки llama.cpp (версия - modeli.ВЕРСИЯ_LLAMA).
Без файла модели создаётся модель архитектуры llama на 75 млн весов в половинной точности
(gguf_malaya.py, нужен numpy) - по размеру как небольшая настоящая модель. Оба способа: жадная
выборка, 128 токенов, запрос «привет мир», одно и то же число потоков; запуски попеременно,
в таблицу - медиана.
"""

import os
import re
import statistics
import subprocess
import sys
import tempfile

ПАПКА = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ПАПКА)

import gguf_malaya  # noqa: E402
import sam_kompilyator  # noqa: E402

ТОКЕНОВ = 128
ЗАПУСКОВ = 5


def главная():
    llama = os.path.abspath(sys.argv[1])
    временная = tempfile.mkdtemp(prefix="dan_zamer_modeli_")
    if len(sys.argv) > 2:
        модель = os.path.abspath(sys.argv[2])
    else:
        import numpy
        модель = os.path.join(временная, "средняя.gguf")
        gguf_malaya.создать(модель, слоёв=12, ширина=768, голов=12, голов_kv=4, ширина_ffn=2048, контекст=2048,
                            f16=True, numpy_=numpy)
    программа = os.path.join(временная, "скорость.dan")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "скорость.dan"), encoding="utf-8") as ф:
        текст = ф.read()
    with open(программа, "w", encoding="utf-8") as ф:
        ф.write(текст)
    subprocess.run([sam_kompilyator.исполняемый(), "собрать", программа], check=True, env=sam_kompilyator.среда())
    exe = программа[:-4] + (".exe" if os.name == "nt" else "")
    completion = os.path.join(llama, "llama-completion" + (".exe" if os.name == "nt" else ""))
    print("модель:", модель, os.path.getsize(модель) // 1048576, "МБ")
    for потоков in sorted({1, os.cpu_count() or 1}):
        дан, их = [], []
        for _ in range(ЗАПУСКОВ):
            р = subprocess.run([exe, модель, str(ТОКЕНОВ)], capture_output=True, text=True, check=True,
                               env={**os.environ, "DAN_LLAMA": llama, "DAN_POTOKI": str(потоков)})
            дан.append(float(re.search(r"токенов_в_секунду=([\d.]+)", р.stdout).group(1)))
            р = subprocess.run([completion, "-m", модель, "-p", "привет мир", "-n", str(ТОКЕНОВ), "-t", str(потоков),
                                "--temp", "0", "--ignore-eos", "-c", "512", "-no-cnv"],
                               capture_output=True, text=True, check=True, stdin=subprocess.DEVNULL)
            их.append(float(re.search(r"eval time = .*?runs.*?([\d.]+) tokens per second",
                                      р.stderr + р.stdout).group(1)))
        д, л = statistics.median(дан), statistics.median(их)
        print(f"| {потоков} | {д:.1f} | {л:.1f} | {(л / д - 1) * 100:+.1f}% |  (все: Дан {дан}, llama {их})")


if __name__ == "__main__":
    главная()
