"""Программы для браузера: страница HTML с программой в WebAssembly.

Программа на Дане собирается в модуль WebAssembly с интерфейсом WASI
(тот же модуль выполняют wasmtime, wasmer и другие исполнители). Для браузера
к нему добавляется небольшая страница: модуль встроен в неё (base64), а
функции WASI, которые нужны программе, выполняет сценарий на JavaScript:
вывод - в окно страницы, ввод() - через окно запроса браузера, время,
случайные числа, завершение программы. Файлов и запуска программ в браузере
нет: такие функции возвращают ошибку.

Страница - один файл: её можно открыть двойным щелчком (без сервера),
положить на любой сайт или отправить по почте.
"""

import base64
import html

СЦЕНАРИЙ = r"""
"use strict";
const вывод = document.getElementById("вывод");
const состояние = document.getElementById("состояние");
const декодер = { 1: new TextDecoder("utf-8"), 2: new TextDecoder("utf-8") };
const кодер = new TextEncoder();
let память = null;
let ввод = new Uint8Array(0);
let конецВвода = false;

class Завершение { constructor(код) { this.код = код; } }

function показать(fd, байты) {
  const текст = декодер[fd].decode(байты, { stream: true });
  if (!текст) return;
  const часть = document.createElement("span");
  if (fd === 2) часть.className = "ошибка";
  часть.textContent = текст;
  вывод.appendChild(часть);
}

const ENOSYS = 52, EBADF = 8, ENOENT = 44, ESUCCESS = 0;
function вид() { return new DataView(память.buffer); }

const wasi = {
  args_sizes_get(argc, size) { вид().setUint32(argc, 1, true); вид().setUint32(size, 2, true); return 0; },
  args_get(argv, buf) { вид().setUint32(argv, buf, true); new Uint8Array(память.buffer).set([0x70, 0], buf); return 0; },
  environ_sizes_get(count, size) { вид().setUint32(count, 0, true); вид().setUint32(size, 0, true); return 0; },
  environ_get() { return 0; },
  clock_res_get(id, out) { вид().setBigUint64(out, 1000n, true); return 0; },
  clock_time_get(id, precision, out) {
    const нс = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
    вид().setBigUint64(out, нс, true);
    return 0;
  },
  fd_write(fd, iovs, count, written) {
    if (fd !== 1 && fd !== 2) return EBADF;
    let всего = 0;
    for (let i = 0; i < count; i++) {
      const адрес = вид().getUint32(iovs + i * 8, true), длина = вид().getUint32(iovs + i * 8 + 4, true);
      показать(fd, new Uint8Array(память.buffer, адрес, длина));
      всего += длина;
    }
    вид().setUint32(written, всего, true);
    return 0;
  },
  fd_read(fd, iovs, count, read) {
    if (fd !== 0) return EBADF;
    if (ввод.length === 0 && !конецВвода) {
      const строка = window.prompt("Программа ждёт ввода:", "");
      if (строка === null) конецВвода = true;
      else {
        ввод = кодер.encode(строка + "\n");
        показать(1, ввод);
      }
    }
    let всего = 0;
    for (let i = 0; i < count && ввод.length; i++) {
      const адрес = вид().getUint32(iovs + i * 8, true), длина = вид().getUint32(iovs + i * 8 + 4, true);
      const n = Math.min(длина, ввод.length);
      new Uint8Array(память.buffer, адрес, n).set(ввод.subarray(0, n));
      ввод = ввод.subarray(n);
      всего += n;
    }
    вид().setUint32(read, всего, true);
    return 0;
  },
  fd_fdstat_get(fd, out) {
    if (fd > 2) return EBADF;
    new Uint8Array(память.buffer, out, 24).fill(0);
    вид().setUint8(out, 2, true);            // символьное устройство
    return 0;
  },
  fd_fdstat_set_flags() { return 0; },
  fd_prestat_get() { return EBADF; },        // папок нет
  fd_prestat_dir_name() { return EBADF; },
  fd_close() { return 0; },
  fd_seek() { return 70; },                  // ESPIPE
  path_open() { return ENOENT; },
  random_get(buf, len) { crypto.getRandomValues(new Uint8Array(память.buffer, buf, len)); return 0; },
  sched_yield() { return 0; },
  poll_oneoff(inp, out, n, nevents) { вид().setUint32(nevents, 0, true); return 0; },
  proc_exit(код) { throw new Завершение(код); },
};
const импорт = { wasi_snapshot_preview1: new Proxy(wasi, { get: (ц, имя) => ц[имя] || (() => ENOSYS) }) };

async function запустить() {
  const байты = Uint8Array.from(atob(document.getElementById("программа").textContent.trim()), (с) => с.charCodeAt(0));
  const { instance } = await WebAssembly.instantiate(байты, импорт);
  память = instance.exports.memory;
  let код = 0;
  try {
    instance.exports._start();
  } catch (о) {
    if (о instanceof Завершение) код = о.код;
    else if (о instanceof RangeError) {
      показать(2, кодер.encode("\nОшибка выполнения: слишком глубокая рекурсия (стек браузера исчерпан)\n")); код = 1;
    } else { показать(2, кодер.encode("\nОшибка выполнения: " + о + "\n")); код = 1; }
  }
  показать(1, new Uint8Array(0));
  состояние.textContent = код === 0 ? "Программа завершена" : "Программа завершена с кодом " + код;
}
запустить();
"""

СТРАНИЦА = """<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{заголовок}</title>
<style>
  :root {{ --фон: #fbfaf7; --текст: #1d1d1b; --тихий: #6b6a66; --ошибка: #b3261e; --рамка: #e4e1da; }}
  @media (prefers-color-scheme: dark) {{
    :root {{ --фон: #161614; --текст: #e9e7e2; --тихий: #9b998f; --ошибка: #f2b8b5; --рамка: #2e2d29; }}
  }}
  body {{ margin: 0; background: var(--фон); color: var(--текст); font: 16px/1.5 system-ui, sans-serif; }}
  main {{ max-width: 860px; margin: 0 auto; padding: 24px 16px; }}
  h1 {{ font-size: 20px; font-weight: 600; margin: 0 0 4px; }}
  #состояние {{ color: var(--тихий); font-size: 14px; margin-bottom: 16px; }}
  pre {{ margin: 0; padding: 16px; border: 1px solid var(--рамка); border-radius: 8px; white-space: pre-wrap;
        word-break: break-word; font: 15px/1.45 ui-monospace, "Cascadia Mono", Consolas, monospace; min-height: 4em; }}
  .ошибка {{ color: var(--ошибка); }}
</style>
</head>
<body>
<main>
<h1>{заголовок}</h1>
<div id="состояние">Программа выполняется...</div>
<pre id="вывод"></pre>
</main>
<script type="application/octet-stream" id="программа">{wasm}</script>
<script>{сценарий}</script>
</body>
</html>
"""


def страница(байты_wasm, заголовок):
    """Текст страницы HTML с встроенной программой."""
    return СТРАНИЦА.format(заголовок=html.escape(заголовок), wasm=base64.b64encode(байты_wasm).decode("ascii"),
                           сценарий=СЦЕНАРИЙ)
