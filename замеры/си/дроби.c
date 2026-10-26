/* Дробная арифметика: сумма 1/к² для к от 1 до 200 000 000. */
#include <stdio.h>
#include <math.h>
int main(void) {
    double s = 0.0;
    for (long k = 1; k <= 200000000; k++) { double d = (double)k; s += 1.0 / (d * d); }
    printf("%.9f\n", round(s * 1e9) / 1e9);
    return 0;
}
