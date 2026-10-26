# Обмен данными JSON: записать 200 000 записей в текст JSON, разобрать его обратно и сложить суммы.
import json
записи = [{"номер": к, "имя": "Клиент " + str(к), "сумма": к * 37 % 1000} for к in range(1, 200001)]
текст = json.dumps(записи, ensure_ascii=False, separators=(",", ":"))
данные = json.loads(текст)
print(len(текст))
print(sum(з["сумма"] for з in данные))
