# Списки и циклы: количество простых чисел до 30 000 000 решетом Эратосфена.
n = 30000000
s = [True] * (n + 1)
i = 2
while i * i <= n:
    if s[i]:
        for j in range(i * i, n + 1, i):
            s[j] = False
    i += 1
print(sum(1 for k in range(2, n + 1) if s[k]))
