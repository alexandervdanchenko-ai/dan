"""Упавшие тесты - в замечания GitHub (их видно на странице проверки и через API)."""
import re
import sys

текст = open(sys.argv[1], encoding="utf-8", errors="replace").read()
блоки = re.split(r"^={50,}\n", текст, flags=re.M)[1:]
упавшие = []
for блок in блоки:
    заголовок, _, тело = блок.partition("\n")
    тело = тело.split("\n" + "-" * 70 + "\nRan ")[0]
    упавшие.append((заголовок.strip(), тело.strip()))


def экранировать(с):
    return с.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def экранировать_заголовок(с):
    return экранировать(с).replace(":", "%3A").replace(",", "%2C")


for заголовок, тело in упавшие[:9]:
    print(f"::error title={экранировать_заголовок(заголовок)[:200]}::{экранировать(тело[-6000:])}")
if упавшие:
    print("::error title=Всего упало {}::{}".format(len(упавшие), экранировать("\n".join(з for з, _ in упавшие))[:20000]))
else:
    print("::error title=Тесты не запустились::" + экранировать(текст[-6000:]))
итог = re.findall(r"^(Ran .*|OK.*|FAILED.*)$", текст, flags=re.M)
print("\n".join(итог))
