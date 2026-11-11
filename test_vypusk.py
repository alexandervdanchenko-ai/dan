"""Сценарий выпуска: привязка файлов выпуска к коммиту (attestations GitHub, решение 0014)."""

import os
import unittest

ПАПКА = os.path.dirname(os.path.abspath(__file__))


def прочитать(*путь):
    with open(os.path.join(ПАПКА, *путь), encoding="utf-8") as ф:
        return ф.read()


class ТестыВыпуска(unittest.TestCase):
    def test_привязка_к_коммиту(self):
        текст = прочитать(".github", "workflows", "vypusk.yml")
        права = текст.split("permissions:", 1)[1].split("jobs:", 1)[0]
        self.assertIn("id-token: write", права)
        self.assertIn("attestations: write", права)
        self.assertIn("uses: actions/attest-build-provenance@", текст)
        # привязываются ровно выложенные файлы, включая SHA256SUMS.txt (он в списке «файлы»)
        шаг = текст.split("# те же файлы, что выложены", 1)[1]
        self.assertIn("for ф in файлы:", шаг)
        self.assertIn("privyazat/*", шаг)
        self.assertLess(текст.index('файлы.append(путь_сумм)'), текст.index("# те же файлы, что выложены"))

    def test_подпись_сумм(self):
        текст = прочитать(".github", "workflows", "vypusk.yml")
        self.assertIn("environment: выпуск", текст)
        self.assertIn("secrets.DAN_KLYUCH_PODPISI", текст)
        шаг = текст.split("файлы.append(путь_сумм)\n", 1)[1]
        self.assertIn('файлы.append(путь_сумм + ".sig")', шаг)
        # привязка к коммиту охватывает и подпись: она добавлена в «файлы» раньше
        self.assertLess(текст.index('файлы.append(путь_сумм + ".sig")'), текст.index("# те же файлы, что выложены"))
        # ключ, которым проверяет «dan обновить», один - в сам/обновление.dan
        import re
        ключи = re.findall(r'вернуть "([0-9a-f]{64})"', прочитать("сам", "обновление.dan"))
        self.assertEqual(len(ключи), 1)

    def test_проверка_описана_в_readme(self):
        self.assertIn("gh attestation verify", прочитать("README.md"))


if __name__ == "__main__":
    unittest.main()
