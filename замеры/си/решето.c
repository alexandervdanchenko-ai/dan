/* Списки и циклы: количество простых чисел до 30 000 000 решетом Эратосфена. */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
int main(void) {
    const long n = 30000000;
    bool *s = malloc((size_t)n + 1);
    for (long k = 0; k <= n; k++) s[k] = true;
    for (long i = 2; i * i <= n; i++)
        if (s[i]) for (long m = i * i; m <= n; m += i) s[m] = false;
    long c = 0;
    for (long k = 2; k <= n; k++) if (s[k]) c++;
    printf("%ld\n", c);
    free(s);
    return 0;
}
