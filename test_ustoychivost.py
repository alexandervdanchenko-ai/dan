"""Компилятор не падает на случайно испорченных программах (см. ustoychivost.py)."""

import unittest

import ustoychivost


class ТестыУстойчивости(unittest.TestCase):

    def test_испорченные_программы(self):
        проблемы = ustoychivost.проверить(число=600, зерно=2026, собирать=20)
        self.assertEqual([(вид, т) for вид, т, _ in проблемы], [])


if __name__ == "__main__":
    unittest.main()
