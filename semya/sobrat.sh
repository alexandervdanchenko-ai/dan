#!/bin/sh
# Сборка компилятора Дана из семени (semya/dan.c) без Python:
#
#     sh semya/sobrat.sh            - компилятор C из DAN_COMPILER, иначе cc
#     sh semya/sobrat.sh clang      - указанный компилятор C
#
# Готовый компилятор - semya/dan; папку проекта (с папками sreda и библиотека) он находит сам -
# это папка выше semya; если исполняемый файл перенесён, корень указывает переменная DAN_HOME.
set -e
cd "$(dirname "$0")/.."
CC="${1:-${DAN_COMPILER:-cc}}"
"$CC" -std=c11 -O2 -fwrapv -ffp-contract=off -w semya/dan.c -o semya/dan -lm -pthread
echo "Готово: semya/dan"
