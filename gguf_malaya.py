"""Крошечная языковая модель в формате GGUF для тестов модуля «модели».

Писатель формата GGUF (версия 3) без сторонних библиотек: модель архитектуры
llama с двумя слоями, словарь SentencePiece из нескольких сотен частей
(служебные части, 256 байтов, русские и латинские буквы и слова), шаблон чата
ChatML и случайные веса с постоянным зерном. Модель ничего не умеет, но
llama.cpp загружает её, разбивает текст на части и порождает текст, как из
настоящей модели; одно зерно - побайтно один и тот же файл.

Для движка «нейро» (библиотека/нейро.dan) модель бывает и других видов: веса в 8 битах
(Q8_0, как в llama.cpp), словарь BPE (как у Qwen2 и Llama 3), архитектура qwen2 со сдвигами
запроса, ключа и значения и общими весами встраивания и выхода.

Запуск: python gguf_malaya.py путь.gguf [зерно]
"""

import random
import struct
import sys

# типы значений GGUF
UINT32, INT32, FLOAT32, BOOL, STRING, ARRAY = 4, 5, 6, 7, 8, 9
ВЫРАВНИВАНИЕ = 32

ШАБЛОН_ЧАТА = ("{% for message in messages %}{{'<|im_start|>' + message['role'] + '\\n' + message['content'] "
               "+ '<|im_end|>' + '\\n'}}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n' }}"
               "{% endif %}")

СЛОВА = ("привет мир как дела язык дан модель ответ вопрос текст слово число строка список работа время "
         "день ночь утро вечер дом город река море солнце луна звезда книга письмо поле лес гора "
         "истина ложь один два три четыре пять да нет хорошо плохо очень можно нужно было будет есть "
         "hello world the a of and to is in it you that this model test").split()


def _строка(с):
    б = с.encode("utf-8")
    return struct.pack("<Q", len(б)) + б


def _значение(тип, з):
    if тип == UINT32:
        return struct.pack("<I", з)
    if тип == INT32:
        return struct.pack("<i", з)
    if тип == FLOAT32:
        return struct.pack("<f", з)
    if тип == BOOL:
        return struct.pack("<B", 1 if з else 0)
    if тип == STRING:
        return _строка(з)
    raise ValueError(тип)


def _пара(ключ, тип, з):
    if тип == ARRAY:
        вид, значения = з
        return (_строка(ключ) + struct.pack("<I", ARRAY) + struct.pack("<IQ", вид, len(значения))
                + b"".join(_значение(вид, х) for х in значения))
    return _строка(ключ) + struct.pack("<I", тип) + _значение(тип, з)


def словарь():
    """Части словаря: (текст, оценка, вид). Виды: 1 - обычная, 2 - неизвестная, 3 - служебная, 6 - байт."""
    части = [("<unk>", 0.0, 2), ("<s>", 0.0, 3), ("</s>", 0.0, 3), ("<|im_start|>", 0.0, 3), ("<|im_end|>", 0.0, 3)]
    части += [("<0x%02X>" % б, 0.0, 6) for б in range(256)]
    обычные = []
    for з in "абвгдеёжзийклмнопрстуфхцчшщъыьэюяabcdefghijklmnopqrstuvwxyz0123456789.,!?-:":
        обычные += [з, "▁" + з]
    обычные.append("▁")
    # SentencePiece соединяет соседние части, только если соединение тоже есть в словаре:
    # поэтому в словаре и все начала слов («▁п», «▁пр», ..., «▁привет»)
    for с in СЛОВА:
        for к in range(1, len(с) + 1):
            обычные += ["▁" + с[:к], с[:к]]
    уже = set()
    for т in обычные:
        if т not in уже:
            уже.add(т)
            # длинные части выгоднее: SentencePiece соединяет по наибольшей оценке
            части.append((т, -10.0 + len(т), 1))
    return части


def знаки_байтов():
    """Знаки, которыми словарь BPE GPT-2 записывает байты (bytes_to_unicode)."""
    печатные = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    знаки = {}
    следующий = 256
    for б in range(256):
        if б in печатные:
            знаки[б] = chr(б)
        else:
            знаки[б] = chr(следующий)
            следующий += 1
    return знаки


def словарь_bpe():
    """Словарь BPE: служебные части, 256 байтов и слияния для слов СЛОВА (с пробелом впереди и без).
    Результат: (части [(текст, вид)], слияния ["левая правая"])."""
    знаки = знаки_байтов()
    части = [(знаки[б], 1) for б in range(256)]
    уже = {т for т, _ in части}
    слияния = []
    for с in СЛОВА + [" " + с for с in СЛОВА] + ["123", " 2026", "...", "!!", " \n"]:
        с = с.replace("\\n", "\n")
        байты_ = [знаки[б] for б in с.encode("utf-8")]
        текущее = байты_[0]
        for следующее in байты_[1:]:
            новое = текущее + следующее
            if новое not in уже:
                уже.add(новое)
                части.append((новое, 1))
                слияния.append(текущее + " " + следующее)
            текущее = новое
    части += [("<|endoftext|>", 3), ("<|im_start|>", 3), ("<|im_end|>", 3)]
    return части, слияния


def квантовать_q8(числа):
    """Q8_0 как quantize_row_q8_0_ref в ggml: блоки по 32 числа, масштаб в половинной точности."""
    итог = bytearray()
    for н in range(0, len(числа), 32):
        блок = числа[н:н + 32]
        наиб = max(abs(х) for х in блок)
        д = наиб / 127.0
        д = struct.unpack("<f", struct.pack("<f", д))[0]
        обр = 1.0 / д if д else 0.0
        итог += struct.pack("<e", д)
        for х in блок:
            q = struct.unpack("<f", struct.pack("<f", х * обр))[0]
            q = int(q + 0.5) if q >= 0 else -int(-q + 0.5)       # roundf: половина - от нуля
            итог += struct.pack("<b", max(-127, min(127, q)))
    return bytes(итог)


def создать(путь, зерно=20, слоёв=2, ширина=64, голов=4, голов_kv=2, ширина_ffn=128, контекст=512,
           архитектура="llama", f16=False, numpy_=None, q8=False, токенизатор="llama", предразбиение="qwen2",
           сдвиги=False, общий_выход=False):
    """f16 - матрицы весов в половинной точности (как у настоящих моделей), numpy_ - модуль numpy для
    быстрого создания большой модели для замеров (веса тогда другие, но тоже с постоянным зерном);
    q8 - матрицы весов в формате Q8_0; токенизатор «gpt2» - словарь BPE с правилом предразбиения;
    сдвиги - тензоры attn_q.bias, attn_k.bias, attn_v.bias (как у Qwen2); общий_выход - без
    output.weight (выход - через веса встраивания)."""
    слияния = []
    if токенизатор == "gpt2":
        части_bpe, слияния = словарь_bpe()
        сл = [(т, 0.0, вид) for т, вид in части_bpe]
    else:
        сл = словарь()
    генератор = random.Random(зерно)
    тензоры = []
    быстрый = numpy_.random.default_rng(зерно) if numpy_ else None

    def тензор(имя, размеры, разброс, вид=None):
        число = 1
        for р in размеры:
            число *= р
        половинная = f16 and len(размеры) == 2
        if быстрый is not None:
            данные = (быстрый.standard_normal(число) * разброс).astype("<f2" if половинная else "<f4").tobytes()
        else:
            данные = struct.pack("<%d%s" % (число, "e" if половинная else "f"),
                                 *(генератор.gauss(0.0, разброс) for _ in range(число)))
        if q8 and len(размеры) == 2 and вид is None:
            числа = struct.unpack("<%df" % число, данные)
            тензоры.append((имя, размеры, квантовать_q8(числа), 8))
            return
        тензоры.append((имя, размеры, данные, 1 if половинная else 0))

    def единицы(имя, n):
        тензоры.append((имя, [n], struct.pack("<%df" % n, *([1.0] * n)), 0))

    тензор("token_embd.weight", [ширина, len(сл)], 1.0)
    for с in range(слоёв):
        п = "blk.%d." % с
        единицы(п + "attn_norm.weight", ширина)
        if сдвиги:
            тензор(п + "attn_q.bias", [ширина], 0.3)
            тензор(п + "attn_k.bias", [ширина // голов * голов_kv], 0.3)
            тензор(п + "attn_v.bias", [ширина // голов * голов_kv], 0.3)
        тензор(п + "attn_q.weight", [ширина, ширина], 0.3)
        тензор(п + "attn_k.weight", [ширина, ширина // голов * голов_kv], 0.3)
        тензор(п + "attn_v.weight", [ширина, ширина // голов * голов_kv], 0.3)
        тензор(п + "attn_output.weight", [ширина, ширина], 0.3)
        единицы(п + "ffn_norm.weight", ширина)
        тензор(п + "ffn_gate.weight", [ширина, ширина_ffn], 0.3)
        тензор(п + "ffn_up.weight", [ширина, ширина_ffn], 0.3)
        тензор(п + "ffn_down.weight", [ширина_ffn, ширина], 0.3)
    единицы("output_norm.weight", ширина)
    # служебные части и байты почти не выбираются: ответ состоит из букв и слов
    служебных = 5 + 256
    if токенизатор == "gpt2":
        служебных = 0
    if not общий_выход:
        выход = [генератор.gauss(0.0, 0.02 if н // ширина < служебных else 1.0) for н in range(ширина * len(сл))]
        тензоры.append(("output.weight", [ширина, len(сл)], struct.pack("<%df" % len(выход), *выход), 0))

    а = "llama" if архитектура not in ("qwen2",) else архитектура
    пары = [
        ("general.architecture", STRING, архитектура),
        ("general.name", STRING, "malaya-dan"),
        ("general.alignment", UINT32, ВЫРАВНИВАНИЕ),
        (а + ".context_length", UINT32, контекст),
        (а + ".embedding_length", UINT32, ширина),
        (а + ".block_count", UINT32, слоёв),
        (а + ".feed_forward_length", UINT32, ширина_ffn),
        (а + ".attention.head_count", UINT32, голов),
        (а + ".attention.head_count_kv", UINT32, голов_kv),
        (а + ".rope.dimension_count", UINT32, ширина // голов),
        (а + ".attention.layer_norm_rms_epsilon", FLOAT32, 1e-5),
    ]
    if токенизатор == "gpt2":
        пары += [
            ("tokenizer.ggml.model", STRING, "gpt2"),
            ("tokenizer.ggml.pre", STRING, предразбиение),
            ("tokenizer.ggml.tokens", ARRAY, (STRING, [ч[0] for ч in сл])),
            ("tokenizer.ggml.token_type", ARRAY, (INT32, [ч[2] for ч in сл])),
            ("tokenizer.ggml.merges", ARRAY, (STRING, слияния)),
            ("tokenizer.ggml.bos_token_id", UINT32, len(сл) - 3),
            ("tokenizer.ggml.eos_token_id", UINT32, len(сл) - 3),
            ("tokenizer.chat_template", STRING, ШАБЛОН_ЧАТА),
        ]
    else:
        пары += [
            ("tokenizer.ggml.model", STRING, "llama"),
            ("tokenizer.ggml.tokens", ARRAY, (STRING, [ч[0] for ч in сл])),
            ("tokenizer.ggml.scores", ARRAY, (FLOAT32, [ч[1] for ч in сл])),
            ("tokenizer.ggml.token_type", ARRAY, (INT32, [ч[2] for ч in сл])),
            ("tokenizer.ggml.unknown_token_id", UINT32, 0),
            ("tokenizer.ggml.bos_token_id", UINT32, 1),
            ("tokenizer.ggml.eos_token_id", UINT32, 2),
            ("tokenizer.ggml.add_bos_token", BOOL, True),
            ("tokenizer.ggml.add_eos_token", BOOL, False),
            ("tokenizer.chat_template", STRING, ШАБЛОН_ЧАТА),
        ]
    шапка = b"GGUF" + struct.pack("<IQQ", 3, len(тензоры), len(пары))
    шапка += b"".join(_пара(к, т, з) for к, т, з in пары)
    смещение = 0
    данные = []
    for имя, размеры, д, тип in тензоры:
        шапка += _строка(имя) + struct.pack("<I", len(размеры)) + b"".join(struct.pack("<Q", р) for р in размеры)
        шапка += struct.pack("<IQ", тип, смещение)        # тип 0 - F32, 1 - F16, 8 - Q8_0
        данные.append(д)
        добавка = (-len(д)) % ВЫРАВНИВАНИЕ
        данные.append(b"\0" * добавка)
        смещение += len(д) + добавка
    шапка += b"\0" * ((-len(шапка)) % ВЫРАВНИВАНИЕ)
    with open(путь, "wb") as ф:
        ф.write(шапка)
        for д in данные:
            ф.write(д)
    return len(шапка)


if __name__ == "__main__":
    print(создать(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20))
