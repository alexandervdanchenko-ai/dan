# Таблицы CSV: записать таблицу из 300 000 строк в текст CSV, разобрать обратно и сложить столбец.
import csv
import io
строки = [["Номер", "Клиент", "Сумма"]] + [[str(к), "Клиент \"" + str(к % 100) + "\", Москва", str(к * 37 % 1000)]
                                          for к in range(1, 300001)]
буфер = io.StringIO()
csv.writer(буфер).writerows(строки)
таблица = list(csv.reader(io.StringIO(буфер.getvalue())))
print(len(таблица))
print(sum(int(с[2]) for с in таблица[1:]))
