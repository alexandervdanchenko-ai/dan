"""Тесты модуля настройки (библиотека/настройки.dan): TOML, YAML, JSON, поиск по пути.

Разбор примеров TOML и YAML сверяется с tomllib и PyYAML из Python, если они есть: ключи
с русскими буквами для tomllib берутся в кавычки (простые ключи TOML 1.0 - только латиница,
Дан принимает и буквы, как TOML 1.1), а для PyYAML (YAML 1.1) пропускаются расхождения,
в которых Дан следует YAML 1.2. Ошибки и крайние случаи - программа
тесты_данные/настройки/проверка.dan с ожидаемым выводом проверка.вывод.
"""

import datetime
import json
import math
import os
import re
import shutil
import subprocess
import tempfile
import unittest

import dan
from test_kodogen import КОМПИЛЯТОР_ЕСТЬ

try:
    import tomllib
except ImportError:          # Python до 3.11
    tomllib = None
try:
    import yaml
except ImportError:
    yaml = None

ДАННЫЕ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "тесты_данные", "настройки")


def прочитать(имя):
    with open(os.path.join(ДАННЫЕ, имя), encoding="utf-8") as ф:
        return ф.read()


def в_ключи_tomllib(текст):
    """Русские простые ключи TOML - в кавычки (для TOML 1.0)."""
    def часть(ч):
        return f'"{ч}"' if re.search(r"[^\x00-\x7f]", ч) and not ч.startswith('"') else ч

    итог = []
    for строка in текст.split("\n"):
        if строка.startswith("["):
            строка = re.sub(r'[^\[\]."]+', lambda м: часть(м.group(0)), строка)
        else:
            м = re.match(r'^([^\s="\'#]+)( = .*)$', строка)
            if м:
                строка = ".".join(часть(ч) for ч in м.group(1).split(".")) + м.group(2)
        итог.append(строка)
    return "\n".join(итог).replace("{ имя =", '{ "имя" =').replace("адрес.город", '"адрес"."город"')


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыНастроек(unittest.TestCase):

    def выполнить(self, имя):
        with tempfile.TemporaryDirectory() as папка:
            for ф in os.listdir(ДАННЫЕ):
                shutil.copy(os.path.join(ДАННЫЕ, ф), папка)
            путь = os.path.join(папка, имя)
            исполняемый = dan.собрать(путь, прочитать(имя))
            р = subprocess.run([исполняемый], capture_output=True, cwd=папка, timeout=60)
            self.assertEqual(р.returncode, 0, р.stderr)
            return р.stdout.decode("utf-8").replace("\r\n", "\n")

    def сверить(self, эталон, дан, путь=""):
        if isinstance(эталон, (datetime.datetime, datetime.date, datetime.time)):
            self.assertIsInstance(дан, str, путь)
        elif isinstance(эталон, dict):
            self.assertEqual(sorted(эталон), sorted(дан), путь)
            for к in эталон:
                self.сверить(эталон[к], дан[к], путь + "." + к)
        elif isinstance(эталон, list):
            self.assertEqual(len(эталон), len(дан), путь)
            for н, (а, б) in enumerate(zip(эталон, дан)):
                self.сверить(а, б, f"{путь}[{н}]")
        elif isinstance(эталон, float) and (math.isinf(эталон) or math.isnan(эталон)):
            self.assertIsNone(дан, путь)            # в JSON нет бесконечности: null
        else:
            self.assertEqual(эталон, дан, путь)
            self.assertEqual(type(эталон), type(дан), путь)

    def test_примеры(self):
        строки = self.выполнить("примеры.dan").splitlines()
        self.assertEqual(len(строки), 2, строки)
        дан_toml, дан_yaml = json.loads(строки[0]), json.loads(строки[1])
        self.assertEqual(дан_toml["многострочная"], 'Роза мимо\n"кавычки" ""внутри"" ')
        self.assertEqual(дан_toml["пользователи"][1], {"имя": "Пётр", "ключи": [{"вид": "ssh"}]})
        self.assertEqual(дан_toml["дата2"], "1979-05-27T07:32:00")
        if tomllib is not None:
            self.сверить(tomllib.loads(в_ключи_tomllib(прочитать("пример.toml"))), дан_toml)
        self.assertEqual(дан_yaml["числа"], [31, 15, -5, 3, 0.5, 1.0, None, None])
        self.assertEqual(дан_yaml["строки_похожие"], ["yes", "no", "on", "012abc", "123"])
        self.assertEqual(дан_yaml["блок"], "первая строка\n  с отступом\nтретья\n")
        self.assertEqual(дан_yaml["свёрнутый"], "слова одного абзаца\nновый абзац")
        if yaml is not None:
            эталон = yaml.safe_load(прочитать("пример.yaml"))
            # YAML 1.1 у PyYAML: yes/no/on - логические, 1.5e3 - строка, 0o17 - строка
            эталон["строки_похожие"] = ["yes", "no", "on", "012abc", "123"]
            эталон["сервер"]["предел"] = 1500.0
            эталон["числа"][1] = 15
            self.сверить(эталон, дан_yaml)

    def test_ошибки_и_крайние_случаи(self):
        self.assertEqual(self.выполнить("проверка.dan"), прочитать("проверка.вывод"))


if __name__ == "__main__":
    unittest.main()
