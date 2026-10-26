/* Сортировка: упорядочить 5 000 000 псевдослучайных целых чисел (qsort). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static int cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}
int main(void) {
    const int n = 5000000;
    int64_t *v = malloc(sizeof *v * n), x = 1;
    for (int k = 0; k < n; k++) { x = (x * 1103515245 + 12345) % 2147483648LL; v[k] = x; }
    qsort(v, n, sizeof *v, cmp);
    printf("%lld\n%lld\n%lld\n", (long long)v[0], (long long)v[2500000], (long long)v[4999999]);
    free(v);
    return 0;
}
