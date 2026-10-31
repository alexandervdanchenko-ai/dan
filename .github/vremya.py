"""Время тестов этого прогона - в замечания GitHub (их видно через API, без журнала задания).

По ним обновляется testy_vremya.json: время классов и тестов долгих классов, по которому
testy.py делит долгие классы на части, а CI - тесты на несколько машин.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import testy  # noqa: E402

время = testy.прочитать_время()
try:
    with open(testy.файл_времени(), encoding="utf-8") as ф:
        время = json.load(ф)          # только время этой машины, без времени из репозитория
except (OSError, ValueError):
    pass
# только классы, которые шли в этом прогоне (вывод testy.py - первый параметр)
with open(sys.argv[1], encoding="utf-8", errors="replace") as ф:
    шли = set(re.findall(r"^(?:ок|ОШИБКА)\s+[\d,]+ с\s+(\S+)", ф.read(), flags=re.M))
классы = {к: в for к, в in время.items() if к.count(".") == 1 and к in шли}
долгие = {к for к, в in классы.items() if в > 20}
тесты = {к: в for к, в in время.items() if к.count(".") == 2 and ".".join(к.split(".")[:2]) in долгие}
текст = json.dumps(dict(sorted({**классы, **тесты}.items())), ensure_ascii=False, separators=(",", ":"))
система = os.environ.get("RUNNER_OS", sys.platform)
часть = os.environ.get("DAN_CHAST", "")
кусков = (len(текст) + 29999) // 30000
for н in range(кусков):
    кусок = текст[н * 30000:(н + 1) * 30000].replace("%", "%25").replace("\r", "").replace("\n", "%0A")
    print(f"::notice title=Время тестов {система} {часть} {н + 1} из {кусков}::{кусок}")
