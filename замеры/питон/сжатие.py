# Сжатие: сжать около 20 МБ текста в формат gzip и распаковать обратно.
import gzip
текст = "".join("Строка номер " + str(к) + ": договор, акт, счёт\n" for к in range(1, 400001)).encode("utf-8")
обратно = gzip.decompress(gzip.compress(текст))
print("истина" if обратно == текст else "ложь")
print(len(обратно))
