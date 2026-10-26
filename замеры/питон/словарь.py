# Строки и словарь: подсчёт 5 000 000 слов из 1000 разных.
count = {}
for k in range(1, 5000001):
    w = "слово" + str(k * 7919 % 1000)
    count[w] = count.get(w, 0) + 1
print(len(count))
print(count["слово0"])
