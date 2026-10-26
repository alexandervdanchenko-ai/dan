/* Целочисленная арифметика: сумма остатков к·к при делении на 7 для к от 1 до 500 000 000. */
#include <stdio.h>
#include <stdint.h>
int main(void) {
    int64_t s = 0;
    for (int64_t k = 1; k <= 500000000; k++) s += k * k % 7;
    printf("%lld\n", (long long)s);
    return 0;
}
