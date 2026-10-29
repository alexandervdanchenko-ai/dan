/* Сжатие: сжать около 20 МБ текста в формат gzip и распаковать обратно.
   Эталон на C: библиотека zlib (уровень 6, как по умолчанию), её же использует Python. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    size_t ёмкость = 1 << 25, длина = 0;
    char *текст = malloc(ёмкость);
    for (int к = 1; к <= 400000; к++)
        длина += (size_t)sprintf(текст + длина, "Строка номер %d: договор, акт, счёт\n", к);
    z_stream с = {0};
    deflateInit2(&с, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    uLong предел = deflateBound(&с, длина);
    unsigned char *сжатое = malloc(предел);
    с.next_in = (unsigned char *)текст; с.avail_in = (uInt)длина;
    с.next_out = сжатое; с.avail_out = (uInt)предел;
    deflate(&с, Z_FINISH);
    size_t размер = с.total_out;
    deflateEnd(&с);
    z_stream р = {0};
    inflateInit2(&р, 31);
    unsigned char *обратно = malloc(длина + 1);
    р.next_in = сжатое; р.avail_in = (uInt)размер;
    р.next_out = обратно; р.avail_out = (uInt)длина + 1;
    inflate(&р, Z_FINISH);
    size_t получено = р.total_out;
    inflateEnd(&р);
    printf("%s\n%zu\n", получено == длина && !memcmp(обратно, текст, длина) ? "истина" : "ложь", получено);
    return 0;
}
