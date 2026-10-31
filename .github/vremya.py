"""Время тестов этого прогона - в замечания GitHub (их видно через API, без журнала задания).

По ним обновляется testy_vremya.json: время классов и тестов долгих классов, по которому
testy.py делит долгие классы на части, а CI - тесты на несколько машин.
"""
import base64
import json
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import testy  # noqa: E402

if sys.argv[1:2] == ["--прочитать"]:
    # время из замечаний заданий проверки: python .github/vremya.py --прочитать <номер прогона> -> JSON в вывод
    import subprocess

    def gh(путь):
        return json.loads(subprocess.run(["gh", "api", путь], capture_output=True, text=True, check=True).stdout)
    прогон = sys.argv[2]
    итог = {}
    задания = gh(f"repos/{{owner}}/{{repo}}/actions/runs/{прогон}/jobs?per_page=100")["jobs"]
    for з in задания:
        куски = {}
        for з2 in gh(f"repos/{{owner}}/{{repo}}/check-runs/{з['id']}/annotations?per_page=100"):
            м = re.fullmatch(r"Время тестов (.*) (\d+) из (\d+)", з2["title"] or "")
            if м:
                куски.setdefault(м.group(1), {})[int(м.group(2))] = з2["message"]
        for машина, части in куски.items():
            данные = zlib.decompress(base64.b64decode("".join(части[н] for н in sorted(части))))
            итог[машина] = json.loads(данные.decode("utf-8"))
    print(json.dumps(итог, ensure_ascii=False, indent=1, sort_keys=True))
    sys.exit(0)

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
долгие = {к for к, в in классы.items() if в > testy.ЧАСТЬ}
тесты = {к: в for к, в in время.items() if к.count(".") == 2 and ".".join(к.split(".")[:2]) in долгие}
текст = json.dumps(dict(sorted({**классы, **тесты}.items())), ensure_ascii=False, separators=(",", ":"))
система = os.environ.get("RUNNER_OS", sys.platform)
часть = os.environ.get("DAN_CHAST", "")
# GitHub обрезает текст замечания до 4096 байт и показывает не больше 10 замечаний шага: время
# сжимается (zlib, Base64) и идёт несколькими замечаниями; прочитать - .github/vremya.py --прочитать
сжатое = base64.b64encode(zlib.compress(текст.encode("utf-8"), 9)).decode("ascii")
ДЛИНА = 4000
кусков = (len(сжатое) + ДЛИНА - 1) // ДЛИНА
for н in range(кусков):
    print(f"::notice title=Время тестов {система} {часть} {н + 1} из {кусков}::{сжатое[н * ДЛИНА:(н + 1) * ДЛИНА]}")
