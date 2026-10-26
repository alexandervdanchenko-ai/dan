# Рекурсивные вызовы функций: число Фибоначчи 38 наивной рекурсией.
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(38))
