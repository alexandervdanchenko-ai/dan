# Среда выполнения на C

Код C, который компилятор вставляет в программы на Дане (подробнее - `AGENTS.md`, раздел «Как устроен компилятор»). Код среды написан для Дана и распространяется по лицензии проекта (`LICENSE` в корне), кроме частей, перечисленных ниже.

## Код, основанный на чужих программах

Проверка 07.11.2026: код сжатия и распаковки в `dannye.c` сравнивался с исходниками zlib (с contrib/puff), miniz, xz-embedded, liblzma (XZ Utils) и LZMA SDK. Строк, совпадающих дословно, нет; совпадения по строению перечислены здесь.

- **Распаковка DEFLATE** (`dannye.c`, раздел «inflate»: `dan_inf_bits`, `dan_huff_build`, медленная часть `dan_huff_decode`, `dan_inflate_into`) - изменённая версия puff.c Марка Адлера (zlib, contrib/puff, версия 2.3): те же функции, имена переменных и порядок проверок. Лицензия puff (лицензия zlib) разрешает изменять и распространять код при условии, что изменённая версия помечена как изменённая и уведомление сохранено. Уведомление стоит в комментарии перед разделом «inflate» и повторено здесь без изменений:

```
Copyright (C) 2002-2013 Mark Adler, all rights reserved
version 2.3, 21 Jan 2013

This software is provided 'as-is', without any express or implied
warranty.  In no event will the author be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Mark Adler    madler@alumni.caltech.edu
```

- **Распаковка LZMA** (`dan_lzma_decode_chunk` и функции `dan_rcd_*`) следует эталонному декодеру LzmaSpec.cpp из LZMA SDK Игоря Павлова; **сдвиг диапазонного кодера** при сжатии (`dan_rce_shift`) - тот же, что в LZMA SDK и liblzma. LZMA SDK - общественное достояние, liblzma - 0BSD (раньше общественное достояние): условий для копирования нет, источник указан в комментарии.
- **Poly1305** (`dan_poly1305_*`) - по poly1305-donna-32.h Эндрю Муна (общественное достояние или MIT, на выбор): источник указан в комментарии.

Остальное (сжатие DEFLATE с цепочками хешей и выбором вида блока, ограничение длины кодов Хаффмана, таблица быстрых кодов при распаковке, сжатие LZMA с ценами и оптимальным разбором, поиск совпадений, контейнеры gzip, zip и xz) написано для Дана; общее с другими программами - только то, что задают сами форматы: таблицы длин и расстояний и порядок кодов длин RFC 1951, канонические коды Хаффмана (RFC 1951, раздел 3.2.2), состояния, вероятности и устройство диапазонного кодера LZMA, заголовки xz.
