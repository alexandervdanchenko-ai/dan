/* Сверка движка «нейро» с llama.cpp b12190: токены, логиты первого шага и жадный ответ.

   Библиотека llama.cpp открывается во время работы (dlopen), заголовки llama.h не нужны:
   структуры параметров переписаны из llama.h b12190, как в sreda/modeli.c.

       cc -std=c11 -O2 сверка_llama.c -o сверка_llama -ldl
       ./сверка_llama папка_или_libllama модель.gguf запрос.txt N [потоков] [кэш]

   Запрос читается из файла (UTF-8, как есть). Печатает строки:
       токены: 1 2 3                 - разбиение запроса (служебные части разбираются, BOS по модели)
       логиты: x0 x1 ...             - логиты после всего запроса (по одному токену за шаг)
       ответ: 5 6 7                  - N токенов жадной выборки (наибольший логит, при равенстве - меньший номер)
       текст: ...                    - тот же ответ текстом (llama_token_to_piece, без служебных частей)
       скорость: ...                 - токенов в секунду порождения (без обработки запроса)
   Кэш ключей и значений по умолчанию f32 и без flash attention (ближе к движку на Дане); «f16» -
   как по умолчанию в llama.cpp. Запрос подаётся по одному токену, как в движке. */

#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef int32_t tok;

struct model_params {
    void *devices;
    const void *tensor_buft_overrides;
    int32_t n_gpu_layers, split_mode, load_mode, lazy_mode, main_gpu;
    const float *tensor_split;
    void *progress_callback, *progress_callback_user_data;
    const void *kv_overrides;
    bool vocab_only, check_tensors, use_extra_bufts, no_host, no_alloc, load_mtp;
};

struct context_params {
    uint32_t n_ctx, n_batch, n_ubatch, n_seq_max, n_rs_seq, n_outputs_max, n_outputs_max_per_seq;
    int32_t n_threads, n_threads_batch;
    int32_t ctx_type, rope_scaling_type, pooling_type, attention_type, flash_attn_type;
    float rope_freq_base, rope_freq_scale, yarn_ext_factor, yarn_attn_factor, yarn_beta_fast, yarn_beta_slow;
    uint32_t yarn_orig_ctx;
    float defrag_thold;
    void *cb_eval, *cb_eval_user_data;
    int32_t type_k, type_v;
    size_t moe_cache_size;
    void *abort_callback, *abort_callback_data;
    bool embeddings, offload_kqv, no_perf, op_offload, swa_full, kv_unified;
    void *samplers;
    size_t n_samplers;
    void *ctx_other;
};

struct batch {
    int32_t n_tokens;
    tok *token;
    float *embd;
    int32_t *pos, *n_seq_id, **seq_id;
    int8_t *logits;
};

_Static_assert(sizeof(struct model_params) == 80, "llama_model_params");
_Static_assert(sizeof(struct context_params) == 168, "llama_context_params");
_Static_assert(sizeof(struct batch) == 56, "llama_batch");

static void *lib;
static void *need(const char *name) {
    void *f = dlsym(lib, name);
    if (!f) {
        fprintf(stderr, "в библиотеке нет функции %s\n", name);
        exit(2);
    }
    return f;
}

static void quiet(int level, const char *text, void *data) {
    (void)level; (void)text; (void)data;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "запуск: %s папка_или_libllama модель.gguf запрос.txt N [потоков] [кэш]\n", argv[0]);
        return 2;
    }
    char path[4096];
    struct stat st;
    if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) snprintf(path, sizeof path, "%s/libllama.so", argv[1]);
    else snprintf(path, sizeof path, "%s", argv[1]);
    lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "не открывается %s: %s\n", path, dlerror());
        return 2;
    }
    int threads = argc > 5 ? atoi(argv[5]) : 1;
    int f16 = argc > 6 && strcmp(argv[6], "f16") == 0;
    int n_gen = atoi(argv[4]);

    FILE *f = fopen(argv[3], "rb");
    if (!f) { fprintf(stderr, "нет файла запроса\n"); return 2; }
    char *text = malloc(1 << 20);
    size_t len = fread(text, 1, (1 << 20) - 1, f);
    text[len] = 0;
    fclose(f);

    void (*log_set)(void (*)(int, const char *, void *), void *) = need("llama_log_set");
    if (!getenv("DAN_LLAMA_LOG")) log_set(quiet, NULL);
    ((void (*)(void))need("llama_backend_init"))();
    struct model_params mp = ((struct model_params (*)(void))need("llama_model_default_params"))();
    mp.n_gpu_layers = 0;
    void *model = ((void *(*)(const char *, struct model_params))need("llama_model_load_from_file"))(argv[2], mp);
    if (!model) { fprintf(stderr, "модель не загрузилась\n"); return 3; }
    struct context_params cp = ((struct context_params (*)(void))need("llama_context_default_params"))();
    cp.n_ctx = 2048;
    cp.n_threads = cp.n_threads_batch = threads;
    cp.no_perf = true;
    if (!f16) {
        cp.type_k = cp.type_v = 0;        /* GGML_TYPE_F32 */
        cp.flash_attn_type = 0;           /* LLAMA_FLASH_ATTN_TYPE_DISABLED */
    }
    void *ctx = ((void *(*)(void *, struct context_params))need("llama_init_from_model"))(model, cp);
    if (!ctx) { fprintf(stderr, "контекст не создан\n"); return 3; }
    const void *vocab = ((const void *(*)(const void *))need("llama_model_get_vocab"))(model);
    int32_t n_vocab = ((int32_t (*)(const void *))need("llama_vocab_n_tokens"))(vocab);
    int32_t (*tokenize)(const void *, const char *, int32_t, tok *, int32_t, bool, bool) = need("llama_tokenize");
    int32_t (*decode)(void *, struct batch) = need("llama_decode");
    float *(*logits_ith)(void *, int32_t) = need("llama_get_logits_ith");
    int32_t (*to_piece)(const void *, tok, char *, int32_t, int32_t, bool) = need("llama_token_to_piece");
    bool (*is_eog)(const void *, tok) = need("llama_vocab_is_eog");

    int32_t cap = (int32_t)len + 16;
    tok *t = malloc(sizeof *t * (size_t)(cap + n_gen + 1));
    int32_t n = tokenize(vocab, text, (int32_t)len, t, cap, true, true);
    if (n < 0) { fprintf(stderr, "запрос не разбился на токены\n"); return 3; }
    printf("токены:");
    for (int32_t i = 0; i < n; i++) printf(" %d", t[i]);
    printf("\n");
    if (n == 0) return 0;
    for (int32_t i = 0; i < n; i++) {
        struct batch b = { 1, t + i, NULL, NULL, NULL, NULL, NULL };
        if (decode(ctx, b) != 0) { fprintf(stderr, "llama_decode не выполнилась\n"); return 3; }
    }
    float *lg = logits_ith(ctx, -1);
    printf("логиты:");
    for (int32_t i = 0; i < n_vocab; i++) printf(" %.9g", lg[i]);
    printf("\n");
    printf("ответ:");
    char *out = malloc(1 << 20);
    size_t n_out = 0;
    double start = now();
    int made = 0;
    for (int k = 0; k < n_gen; k++) {
        tok best = 0;
        for (int32_t i = 1; i < n_vocab; i++)
            if (lg[i] > lg[best]) best = i;
        printf(" %d", best);
        made++;
        if (is_eog(vocab, best)) break;
        char piece[256];
        int32_t m = to_piece(vocab, best, piece, sizeof piece, 0, false);
        if (m > 0 && n_out + (size_t)m < (1 << 20)) { memcpy(out + n_out, piece, (size_t)m); n_out += (size_t)m; }
        if (k + 1 == n_gen) break;
        struct batch b = { 1, &best, NULL, NULL, NULL, NULL, NULL };
        if (decode(ctx, b) != 0) { fprintf(stderr, "llama_decode не выполнилась\n"); return 3; }
        lg = logits_ith(ctx, -1);
    }
    double sec = now() - start;
    printf("\n");
    out[n_out] = 0;
    for (size_t i = 0; i < n_out; i++) if (out[i] == '\n') out[i] = ' ';
    printf("текст: %s\n", out);
    printf("скорость: %.3f\n", made > 1 ? (made - 1) / sec : 0.0);
    return 0;
}
