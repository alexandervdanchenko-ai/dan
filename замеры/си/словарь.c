/* Строки и словарь: подсчёт 5 000 000 слов из 1000 разных (своя хеш-таблица,
   как написал бы обычный программист на C: строки создаются и освобождаются). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { char *key; long count; } slot;
static slot table[4096];
static uint64_t hash(const char *s) { uint64_t h = 1469598103934665603ULL; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }
int main(void) {
    long distinct = 0;
    for (long k = 1; k <= 5000000; k++) {
        char num[24], *word;
        snprintf(num, sizeof num, "%ld", k * 7919 % 1000);
        word = malloc(strlen("слово") + strlen(num) + 1);
        strcpy(word, "слово"); strcat(word, num);
        uint64_t i = hash(word) & 4095;
        while (table[i].key && strcmp(table[i].key, word) != 0) i = (i + 1) & 4095;
        if (!table[i].key) { table[i].key = word; distinct++; } else free(word);
        table[i].count++;
    }
    uint64_t i = hash("слово0") & 4095;
    while (strcmp(table[i].key, "слово0") != 0) i = (i + 1) & 4095;
    printf("%ld\n%ld\n", distinct, table[i].count);
    return 0;
}
