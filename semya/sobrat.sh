#!/bin/sh
# Сборка компилятора Дана из семени (semya/dan.c) без Python:
#
#     sh semya/sobrat.sh            - компилятор C из DAN_COMPILER, иначе cc
#     sh semya/sobrat.sh clang      - указанный компилятор C
#
# Готовый компилятор - semya/dan; запускать из корня репозитория (там папки sreda и библиотека)
# или указать корень переменной DAN_HOME.
set -e
cd "$(dirname "$0")/.."
CC="${1:-${DAN_COMPILER:-cc}}"
"$CC" -std=c11 -O2 -fwrapv -ffp-contract=off -w semya/dan.c -o semya/dan -lm
echo "Готово: semya/dan"
