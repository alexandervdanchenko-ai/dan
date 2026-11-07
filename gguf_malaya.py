"""Крошечная языковая модель в формате GGUF для тестов модуля «модели».

Писатель формата GGUF (версия 3) без сторонних библиотек: модель архитектуры
llama с двумя слоями, словарь SentencePiece из нескольких сотен частей
(служебные части, 256 байтов, русские и латинские буквы и слова), шаблон чата
ChatML и случайные веса с постоянным зерном. Модель ничего не умеет, но
llama.cpp загружает её, разбивает текст на части и порождает текст, как из
настоящей модели; одно зерно - побайтно один и тот же файл.

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


def создать(путь, зерно=20, слоёв=2, ширина=64, голов=4, голов_kv=2, ширина_ffn=128, контекст=512,
           архитектура="llama"):
    сл = словарь()
    генератор = random.Random(зерно)
    тензоры = []

    def тензор(имя, размеры, разброс):
        число = 1
        for р in размеры:
            число *= р
        данные = struct.pack("<%df" % число, *(генератор.gauss(0.0, разброс) for _ in range(число)))
        тензоры.append((имя, размеры, данные))

    def единицы(имя, n):
        тензоры.append((имя, [n], struct.pack("<%df" % n, *([1.0] * n))))

    тензор("token_embd.weight", [ширина, len(сл)], 1.0)
    for с in range(слоёв):
        п = "blk.%d." % с
        единицы(п + "attn_norm.weight", ширина)
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
    выход = [генератор.gauss(0.0, 0.02 if н // ширина < служебных else 1.0) for н in range(ширина * len(сл))]
    тензоры.append(("output.weight", [ширина, len(сл)], struct.pack("<%df" % len(выход), *выход)))

    пары = [
        ("general.architecture", STRING, архитектура),
        ("general.name", STRING, "malaya-dan"),
        ("general.alignment", UINT32, ВЫРАВНИВАНИЕ),
        ("llama.context_length", UINT32, контекст),
        ("llama.embedding_length", UINT32, ширина),
        ("llama.block_count", UINT32, слоёв),
        ("llama.feed_forward_length", UINT32, ширина_ffn),
        ("llama.attention.head_count", UINT32, голов),
        ("llama.attention.head_count_kv", UINT32, голов_kv),
        ("llama.rope.dimension_count", UINT32, ширина // голов),
        ("llama.attention.layer_norm_rms_epsilon", FLOAT32, 1e-5),
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
    данные = b""
    for имя, размеры, д in тензоры:
        шапка += _строка(имя) + struct.pack("<I", len(размеры)) + b"".join(struct.pack("<Q", р) for р in размеры)
        шапка += struct.pack("<IQ", 0, смещение)          # тип 0 - F32
        данные += д
        добавка = (-len(данные)) % ВЫРАВНИВАНИЕ
        данные += b"\0" * добавка
        смещение = len(данные)
    шапка += b"\0" * ((-len(шапка)) % ВЫРАВНИВАНИЕ)
    with open(путь, "wb") as ф:
        ф.write(шапка + данные)
    return len(шапка)


if __name__ == "__main__":
    print(создать(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20))
