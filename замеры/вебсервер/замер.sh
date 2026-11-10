#!/bin/sh
# Замер сервера HTTP: 20000 соединений по одному запросу GET от клиента на C в 8 потоков.
# Запуск из корня репозитория: sh замеры/вебсервер/замер.sh (нужны собранный semya/dan и python3)
set -e
T=$(mktemp -d)
cc -O2 -pthread замеры/вебсервер/нагрузка.c -o "$T/нагрузка"
cp замеры/вебсервер/сервер.dan "$T/сервер.dan"
semya/dan собрать "$T/сервер.dan" > /dev/null
for i in 1 2 3; do
    "$T/сервер" > "$T/порт.txt" &
    sleep 0.5
    printf "Дан: "; "$T/нагрузка" "$(head -1 "$T/порт.txt")" 20000 8
    wait
done
for i in 1 2 3; do
    python3 замеры/вебсервер/сервер.py > "$T/порт.txt" &
    PY=$!
    sleep 1
    printf "Python: "; "$T/нагрузка" "$(head -1 "$T/порт.txt")" 20000 8
    kill $PY
done
rm -rf "$T"
