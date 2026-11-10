# Записи JSON в свои типы: записать 200 000 записей своего типа в текст JSON, разобрать его в тот же тип и сложить суммы.
import dataclasses
import json


@dataclasses.dataclass
class Запись:
    номер: int
    имя: str
    сумма: int


записи = [Запись(к, "Клиент " + str(к), к * 37 % 1000) for к in range(1, 200001)]
текст = json.dumps([dataclasses.asdict(з) for з in записи], ensure_ascii=False, separators=(",", ":"))
данные = [Запись(**д) for д in json.loads(текст)]
print(len(текст))
print(sum(з.сумма for з in данные))
