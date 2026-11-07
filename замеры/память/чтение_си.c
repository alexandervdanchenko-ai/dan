/* Простое чтение памяти на C для сверки с эталоном на Дане (память.dan):
   тот же буфер (выровнен по 64 байтам, заполнен до замера), те же потоки
   (DAN_POTOKI, по умолчанию все ядра; буфер делится на равные подряд идущие
   части по строкам в 16 КБ, как пул ядер делит строки матрицы), каждый поток
   складывает свою часть 64-битными словами в 8 независимых сумм (gcc -O3
   делает из этого векторные команды). Печатает ГБ/с (медиана проходов).

   gcc -std=c11 -O3 замеры/память/чтение_си.c -o чтение_си -pthread
   ./чтение_си [ГБ буфера] [ГБ весов] [проходов]        (в Windows без -pthread)

   Файл назван не «память.c»: «dan собрать память.dan» пишет рядом код C
   программы под этим именем. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define MAX_POTOKOV 256
#define MAX_PROHODOV 1000
#define STROKA (4096 * 4)   /* байтов в строке матрицы весов */

static double seychas(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

static int yader(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return c > 0 ? (int)c : 1;
#endif
}

typedef struct { const uint64_t *p; size_t n; uint64_t s; } chast;

#ifdef _WIN32
static DWORD WINAPI chitat(void *arg)
#else
static void *chitat(void *arg)
#endif
{
    chast *c = (chast *)arg;
    const uint64_t *p = c->p;
    uint64_t s[8] = {0};
    size_t i = 0;
    for (; i + 8 <= c->n; i += 8)
        for (int k = 0; k < 8; k++) s[k] += p[i + k];
    for (; i < c->n; i++) s[0] += p[i];
    c->s = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
    return 0;
}

static int sravnit(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    double gb = argc > 1 ? atof(argv[1]) : 4.0;
    double gb_vesov = argc > 2 ? atof(argv[2]) : 4.1;
    int prohodov = argc > 3 ? atoi(argv[3]) : 9;
    const char *e = getenv("DAN_POTOKI");
    int t = e && atoi(e) > 0 ? atoi(e) : yader();
    if (t > MAX_POTOKOV) t = MAX_POTOKOV;
    size_t strok = gb > 0 ? (size_t)(gb * 1e9) / STROKA : 0;
    if (strok < 1 || prohodov < 1 || prohodov > MAX_PROHODOV) {
        fprintf(stderr, "нужны буфер не меньше 16 КБ и от 1 до %d проходов\n", MAX_PROHODOV);
        return 2;
    }
    size_t bajtov = strok * STROKA;
#ifdef _WIN32
    uint64_t *buf = _aligned_malloc(bajtov, 64);
#else
    uint64_t *buf = aligned_alloc(64, bajtov);
#endif
    if (!buf) { fprintf(stderr, "не хватает памяти на %.2f ГБ\n", bajtov / 1e9); return 1; }
    /* те же числа, что в память.dan: каждая страница занята до замера */
    float chislo = 0.001f;
    for (size_t i = 0; i < bajtov / 4; i++) memcpy((char *)buf + i * 4, &chislo, 4);
    printf("буфер: %.2f ГБ, потоков: %d\n", bajtov / 1e9, t);
    chast ch[MAX_POTOKOV];
    double skorosti[MAX_PROHODOV];
    uint64_t kontrol = 0;
    for (int p = 0; p <= prohodov; p++) {   /* проход 0 - прогрев */
        double t0 = seychas();
#ifdef _WIN32
        HANDLE h[MAX_POTOKOV];
#else
        pthread_t h[MAX_POTOKOV];
#endif
        for (int k = 0; k < t; k++) {
            /* части - как у пула ядер: по строкам, остаток - первым частям */
            size_t uk = (size_t)k, ost = strok % (size_t)t;
            size_t b = strok / t * uk + (uk < ost ? uk : ost);
            size_t n = strok / t + (uk < ost ? 1 : 0);
            ch[k].p = buf + b * (STROKA / 8);
            ch[k].n = n * (STROKA / 8);
            if (k == 0) continue;
#ifdef _WIN32
            h[k] = CreateThread(NULL, 0, chitat, &ch[k], 0, NULL);
#else
            pthread_create(&h[k], NULL, chitat, &ch[k]);
#endif
        }
        chitat(&ch[0]);
        for (int k = 1; k < t; k++) {
#ifdef _WIN32
            WaitForSingleObject(h[k], INFINITE);
            CloseHandle(h[k]);
#else
            pthread_join(h[k], NULL);
#endif
        }
        double dt = seychas() - t0;
        for (int k = 0; k < t; k++) kontrol += ch[k].s;
        if (p > 0) skorosti[p - 1] = bajtov / dt / 1e9;
    }
    double min = skorosti[0], max = skorosti[0];
    for (int p = 0; p < prohodov; p++) {
        if (skorosti[p] < min) min = skorosti[p];
        if (skorosti[p] > max) max = skorosti[p];
    }
    qsort(skorosti, prohodov, sizeof(double), sravnit);
    double med = prohodov % 2 ? skorosti[prohodov / 2] : (skorosti[prohodov / 2 - 1] + skorosti[prohodov / 2]) / 2;
    printf("проходов: %d, ГБ/с: наименьшая %.1f, медиана %.1f, наибольшая %.1f\n", prohodov, min, med, max);
    printf("скорость чтения: %.1f ГБ/с\n", med);
    printf("предел для %g ГБ весов: %.1f слов/с\n", gb_vesov, med / gb_vesov);
    printf("контроль: %llu\n", (unsigned long long)(kontrol & 0xff));
    return 0;
}
