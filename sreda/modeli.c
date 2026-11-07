/* ==== Local language models of the Dan language (модуль «модели»). ====

   The models are run by llama.cpp. The library is not built into the
   program: it is loaded when the first model is loaded (dlopen / dlsym, on
   Windows LoadLibraryExW / GetProcAddress), so a program that does not use
   models does not depend on it, and a program that does gets an error value
   with a clear text if the library is missing or of another version.

   The library is looked for in this order (spec.md, «Модели»):
     1. the variable DAN_LLAMA: the full path to the library file or the
        folder with it (when it is set, only this place is tried);
     2. the folder of the program and its subfolder «llama»;
     3. the folder of Dan (the variable DAN_HOME): «llama» in it, then itself;
     4. the folder of Dan's data of the user: %LOCALAPPDATA%\Dan\llama on
        Windows, $XDG_DATA_HOME/dan/llama or ~/.local/share/dan/llama elsewhere;
     5. the system places by the bare name (libllama.so, libllama.dylib, llama.dll).

   The structures passed by value are copied from llama.h of the pinned
   version (DAN_LLAMA_VERSION); when the library is loaded, the defaults it
   fills in are compared with the known ones, so a library of another version
   with another layout is refused instead of being called with wrong fields.

   A model file comes from outside: before llama.cpp sees it, its GGUF
   structure is checked here (header, keys, tensor table, data offsets and
   sizes within the file), so a damaged file gives an error value.

   A program refers to a model by number. Each model has one context; the
   tokens that are in its memory (KV cache) are kept, and a new request that
   begins with the same tokens computes only the rest (prefix cache). */

#if !defined(_WIN32) && !defined(__wasi__)
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define DAN_LLAMA_VERSION "b12190"

typedef int32_t dan_ll_token;

/* llama.h b12190: struct llama_model_params */
struct dan_ll_model_params {
    void *devices;
    const void *tensor_buft_overrides;
    int32_t n_gpu_layers;
    int32_t split_mode;
    int32_t load_mode;
    int32_t lazy_mode;
    int32_t main_gpu;
    const float *tensor_split;
    void *progress_callback;
    void *progress_callback_user_data;
    const void *kv_overrides;
    bool vocab_only, check_tensors, use_extra_bufts, no_host, no_alloc, load_mtp;
};

/* llama.h b12190: struct llama_context_params */
struct dan_ll_context_params {
    uint32_t n_ctx, n_batch, n_ubatch, n_seq_max, n_rs_seq, n_outputs_max, n_outputs_max_per_seq;
    int32_t n_threads, n_threads_batch;
    int32_t ctx_type, rope_scaling_type, pooling_type, attention_type, flash_attn_type;
    float rope_freq_base, rope_freq_scale, yarn_ext_factor, yarn_attn_factor, yarn_beta_fast, yarn_beta_slow;
    uint32_t yarn_orig_ctx;
    float defrag_thold;
    void *cb_eval;
    void *cb_eval_user_data;
    int32_t type_k, type_v;
    size_t moe_cache_size;
    void *abort_callback;
    void *abort_callback_data;
    bool embeddings, offload_kqv, no_perf, op_offload, swa_full, kv_unified;
    void *samplers;
    size_t n_samplers;
    void *ctx_other;
};

struct dan_ll_chain_params { bool no_perf; };
struct dan_ll_chat_message { const char *role; const char *content; };

/* llama.h: struct llama_batch */
struct dan_ll_batch {
    int32_t n_tokens;
    dan_ll_token *token;
    float *embd;
    int32_t *pos;
    int32_t *n_seq_id;
    int32_t **seq_id;
    int8_t *logits;
};

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
/* the sizes in llama.h b12190 on 64-bit systems */
_Static_assert(sizeof(struct dan_ll_model_params) == 80, "llama_model_params");
_Static_assert(sizeof(struct dan_ll_context_params) == 168, "llama_context_params");
_Static_assert(sizeof(struct dan_ll_batch) == 56, "llama_batch");
#endif

static struct {
    int state;                      /* 0 - not tried, 1 - loaded, -1 - error (why) */
    char why[2048];
    void (*backend_init)(void);
    struct dan_ll_model_params (*model_default_params)(void);
    struct dan_ll_context_params (*context_default_params)(void);
    struct dan_ll_chain_params (*chain_default_params)(void);
    void *(*model_load)(const char *, struct dan_ll_model_params);
    void (*model_free)(void *);
    void *(*init_from_model)(void *, struct dan_ll_context_params);
    void (*ctx_free)(void *);
    const void *(*model_vocab)(const void *);
    int32_t (*tokenize)(const void *, const char *, int32_t, dan_ll_token *, int32_t, bool, bool);
    int32_t (*token_to_piece)(const void *, dan_ll_token, char *, int32_t, int32_t, bool);
    int32_t (*decode)(void *, struct dan_ll_batch);
    void *(*get_memory)(const void *);
    bool (*memory_seq_rm)(void *, int32_t, int32_t, int32_t);
    void (*memory_clear)(void *, bool);
    void *(*chain_init)(struct dan_ll_chain_params);
    void (*chain_add)(void *, void *);
    void *(*init_greedy)(void);
    void *(*init_dist)(uint32_t);
    void *(*init_top_k)(int32_t);
    void *(*init_top_p)(float, size_t);
    void *(*init_temp)(float);
    dan_ll_token (*sample)(void *, void *, int32_t);
    void (*sampler_free)(void *);
    bool (*is_eog)(const void *, dan_ll_token);
    int32_t (*chat_apply)(const char *, const struct dan_ll_chat_message *, size_t, bool, char *, int32_t);
    const char *(*chat_template)(const void *, const char *);
    uint32_t (*n_ctx)(const void *);
    uint32_t (*n_batch)(const void *);
    void (*log_set)(void (*)(int, const char *, void *), void *);
    void (*load_all)(void);         /* ggml_backend_load_all: only in builds with backends as separate files */
    /* ggml_backend_load_all_from_path: the variants for the processor (ggml-cpu-*) of the build that Dan
       ships lie next to the library, not next to the program, so they are loaded from its folder */
    void (*load_all_from_path)(const char *);
    void *(*set_abort)(void (*)(const char *));   /* ggml_set_abort_callback */
} dan_ll;

#ifndef DAN_LIB_LOCK
/* a lock for loading system libraries and for their tables; without tasks
   there is only one thread and nothing to lock */
#if defined(_WIN32)
#define DAN_LIB_LOCK(m) static SRWLOCK m = SRWLOCK_INIT
#define DAN_LIB_ACQUIRE(m) AcquireSRWLockExclusive(&m)
#define DAN_LIB_RELEASE(m) ReleaseSRWLockExclusive(&m)
#elif defined(DAN_TASKS)
#define DAN_LIB_LOCK(m) static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER
#define DAN_LIB_ACQUIRE(m) pthread_mutex_lock(&m)
#define DAN_LIB_RELEASE(m) pthread_mutex_unlock(&m)
#else
#define DAN_LIB_LOCK(m) static int m
#define DAN_LIB_ACQUIRE(m) ((void)m)
#define DAN_LIB_RELEASE(m) ((void)m)
#endif
#endif
DAN_LIB_LOCK(dan_ll_lock);
#define DAN_LL_LOCK() DAN_LIB_ACQUIRE(dan_ll_lock)
#define DAN_LL_UNLOCK() DAN_LIB_RELEASE(dan_ll_lock)

#if defined(_WIN32)
#define DAN_LL_NAME "llama.dll"
#define DAN_LL_SEP "\\"
#elif defined(__APPLE__)
#define DAN_LL_NAME "libllama.dylib"
#define DAN_LL_SEP "/"
#else
#define DAN_LL_NAME "libllama.so"
#define DAN_LL_SEP "/"
#endif

/* the first error that llama.cpp wrote to its log since the last reset (it explains a failed load) */
static char dan_ll_last[512];
static int dan_ll_echo;

static void dan_ll_log(int level, const char *text, void *data) {
    (void)data;
    if (dan_ll_echo && text) fputs(text, stderr);
    if (level == 4 /* GGML_LOG_LEVEL_ERROR */ && text && !dan_ll_last[0]) {
        size_t n = strlen(text);
        while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
        if (n >= sizeof dan_ll_last) n = sizeof dan_ll_last - 1;
        if (n > 0) {
            memcpy(dan_ll_last, text, n);
            dan_ll_last[n] = 0;
        }
    }
}

/* A failed check inside llama.cpp (GGML_ASSERT, for example on strange numbers in a
   damaged model file) calls abort() and would stop the whole program. While the
   thread of the program is inside a call to llama.cpp, the abort handler of ggml
   jumps back to it instead: the call gives an error value. The memory that
   llama.cpp held in that call is lost, and a model after such a failure is not
   used any more. A failure in another thread (a thread of ggml) still stops the
   program, with the message. */
#ifndef __wasi__
static _Thread_local jmp_buf *dan_ll_jump;
static _Thread_local char dan_ll_abort_text[512];

static void dan_ll_abort(const char *message) {
    if (dan_ll_jump) {
        snprintf(dan_ll_abort_text, sizeof dan_ll_abort_text, "%s", message ? message : "");
        jmp_buf *j = dan_ll_jump;
        dan_ll_jump = NULL;
        longjmp(*j, 1);
    }
    fprintf(stderr, "llama.cpp остановила программу: %s\n", message ? message : "");
}
#define DAN_LL_GUARD(ok, call) do { \
        jmp_buf dan_ll_jb; \
        if (setjmp(dan_ll_jb) == 0) { dan_ll_jump = &dan_ll_jb; call; dan_ll_jump = NULL; (ok) = 1; } \
        else { (ok) = 0; } \
    } while (0)
#else
static char dan_ll_abort_text[1];
#define DAN_LL_GUARD(ok, call) do { call; (ok) = 1; } while (0)
#endif

static void *dan_ll_sym(void *lib, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)lib, name);
#elif defined(__wasi__)
    (void)lib; (void)name;
    return NULL;
#else
    return dlsym(lib, name);
#endif
}

static int dan_ll_is_dir(const char *p) {
#if defined(_WIN32)
    wchar_t *w = dan_wide(p);
    DWORD a = GetFileAttributesW(w);
    dan_free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int dan_ll_exists(const char *p) {
#if defined(_WIN32)
    wchar_t *w = dan_wide(p);
    DWORD a = GetFileAttributesW(w);
    dan_free(w);
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

/* open a library; «where» - a full path (its folder is searched for the
   libraries it needs) or a bare name (system places) */
static void *dan_ll_open(const char *where, int full) {
#if defined(_WIN32)
    wchar_t *w = dan_wide(where);
    /* a file that is not a library must give an error value, not a message box of the system */
    UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    void *lib = (void *)(full ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : LoadLibraryW(w));
    SetErrorMode(mode);
    dan_free(w);
    return lib;
#elif defined(__wasi__)
    (void)where; (void)full;
    return NULL;
#else
    if (full) {
        /* the parts of ggml that lie next to the library are opened first: then
           they are found even if the library does not say where to look for them */
        static const char *const deps[] = {
#ifdef __APPLE__
            "libggml-base.dylib", "libggml-cpu.dylib", "libggml.dylib"
#else
            "libggml-base.so", "libggml-cpu.so", "libggml.so"
#endif
        };
        const char *slash = strrchr(where, '/');
        if (slash) {
            size_t d = (size_t)(slash - where) + 1;
            for (size_t i = 0; i < sizeof deps / sizeof deps[0]; i++) {
                size_t n = strlen(deps[i]);
                char *p = malloc(d + n + 1);
                if (!p) break;
                memcpy(p, where, d);
                memcpy(p + d, deps[i], n + 1);
                if (dan_ll_exists(p)) dlopen(p, RTLD_NOW | RTLD_GLOBAL);
                free(p);
            }
        }
    }
    return dlopen(where, RTLD_NOW | RTLD_LOCAL);
#endif
}

/* the folder of the running program (without the separator at the end) or "" */
static void dan_ll_program_dir(char *out, size_t cap) {
    out[0] = 0;
#if defined(_WIN32)
    wchar_t w[4096];
    DWORD n = GetModuleFileNameW(NULL, w, 4096);
    if (n == 0 || n >= 4096) return;
    w[n] = 0;
    dan_str s = dan_narrow(w);
    size_t len = strlen(s);
    if (len < cap) memcpy(out, s, len + 1);
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) != 0) { out[0] = 0; return; }
#elif defined(__wasi__)
    (void)cap;
    return;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
#endif
    char *a = strrchr(out, '/');
#if defined(_WIN32)
    char *b = strrchr(out, '\\');
    if (!a || (b && b > a)) a = b;
#endif
    if (a) *a = 0;
    else out[0] = 0;
}

static void dan_ll_getenv(const char *name, char *out, size_t cap) {
    out[0] = 0;
#if defined(_WIN32)
    wchar_t *wn = dan_wide(name);
    const wchar_t *v = _wgetenv(wn);
    dan_free(wn);
    if (!v) return;
    dan_str s = dan_narrow(v);
    size_t n = strlen(s);
    if (n < cap) memcpy(out, s, n + 1);
#else
    const char *v = getenv(name);
    if (v && strlen(v) < cap) strcpy(out, v);
#endif
}

/* «a» + «b» + «c» into out, if it fits */
static int dan_ll_join(char *out, size_t cap, const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > cap) return 0;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc + 1);
    return 1;
}

static void dan_ll_why(const char *a, const char *b, const char *c) {
    dan_ll_join(dan_ll.why, sizeof dan_ll.why, a, b, c);
}

static const char dan_ll_help[] =
    " Модуль «модели» работает с библиотекой llama.cpp версии " DAN_LLAMA_VERSION
    ": её ставит команда «dan модели установить»; свою сборку той же версии можно указать "
    "полным путём к файлу " DAN_LL_NAME " в переменной среды DAN_LLAMA.";

/* the defaults of the pinned version: a library with another layout of the
   structures fills other places */
static int dan_ll_layout_ok(void) {
    struct dan_ll_model_params m = dan_ll.model_default_params();
    struct dan_ll_context_params c = dan_ll.context_default_params();
    return m.n_gpu_layers == -1 && m.load_mode == -1 && m.lazy_mode == 1 && m.main_gpu == 0
        && m.tensor_split == NULL && m.kv_overrides == NULL && !m.vocab_only && m.use_extra_bufts && !m.load_mtp
        && c.n_ctx == 512 && c.n_batch == 2048 && c.n_ubatch == 512 && c.n_seq_max == 1 && c.n_rs_seq == 0
        && c.n_outputs_max == 0 && c.n_outputs_max_per_seq == 1 && c.n_threads == c.n_threads_batch
        && c.yarn_ext_factor == -1.0f && c.defrag_thold == -1.0f && c.type_k == 1 && c.type_v == 1
        && c.moe_cache_size == 0 && c.abort_callback == NULL && !c.embeddings && c.offload_kqv && c.no_perf
        && c.op_offload && c.swa_full && !c.kv_unified && c.samplers == NULL && c.n_samplers == 0
        && c.ctx_other == NULL;
}

static const char *const dan_ll_names[] = {
    "llama_backend_init", "llama_model_default_params", "llama_context_default_params",
    "llama_sampler_chain_default_params", "llama_model_load_from_file", "llama_model_free",
    "llama_init_from_model", "llama_free", "llama_model_get_vocab", "llama_tokenize", "llama_token_to_piece",
    "llama_decode", "llama_get_memory", "llama_memory_seq_rm", "llama_memory_clear", "llama_sampler_chain_init",
    "llama_sampler_chain_add", "llama_sampler_init_greedy", "llama_sampler_init_dist", "llama_sampler_init_top_k",
    "llama_sampler_init_top_p", "llama_sampler_init_temp", "llama_sampler_sample", "llama_sampler_free",
    "llama_vocab_is_eog", "llama_chat_apply_template", "llama_model_chat_template", "llama_n_ctx", "llama_n_batch",
    "llama_log_set"
};

/* take the functions from an opened library: 1 - all are there, 0 - not (why is written) */
static int dan_ll_take(void *lib, const char *path) {
    void **f = (void **)(void *)&dan_ll.backend_init;
    for (size_t i = 0; i < sizeof dan_ll_names / sizeof dan_ll_names[0]; i++) {
        f[i] = dan_ll_sym(lib, dan_ll_names[i]);
        if (!f[i]) {
            char m[300];
            snprintf(m, sizeof m, "»: нет функции %s (другая версия llama.cpp).", dan_ll_names[i]);
            dan_ll_why("библиотека llama.cpp несовместима с Даном: «", path, m);
            return 0;
        }
    }
    *(void **)(void *)&dan_ll.load_all = dan_ll_sym(lib, "ggml_backend_load_all");
    *(void **)(void *)&dan_ll.load_all_from_path = dan_ll_sym(lib, "ggml_backend_load_all_from_path");
    *(void **)(void *)&dan_ll.set_abort = dan_ll_sym(lib, "ggml_set_abort_callback");
#if defined(_WIN32)
    /* GetProcAddress looks only in the library itself: the functions of ggml are in ggml-base.dll */
    HMODULE base = GetModuleHandleW(L"ggml-base.dll");
    if (!dan_ll.load_all && base) *(void **)(void *)&dan_ll.load_all = (void *)GetProcAddress(base, "ggml_backend_load_all");
    if (!dan_ll.set_abort && base) *(void **)(void *)&dan_ll.set_abort = (void *)GetProcAddress(base, "ggml_set_abort_callback");
    HMODULE all = GetModuleHandleW(L"ggml.dll");
    if (!dan_ll.load_all && all) *(void **)(void *)&dan_ll.load_all = (void *)GetProcAddress(all, "ggml_backend_load_all");
    if (!dan_ll.load_all_from_path && all)
        *(void **)(void *)&dan_ll.load_all_from_path = (void *)GetProcAddress(all, "ggml_backend_load_all_from_path");
#endif
    if (!dan_ll_layout_ok()) {
        dan_ll_why("библиотека llama.cpp несовместима с Даном: «", path,
                   "»: другое устройство структур параметров (другая версия llama.cpp).");
        return 0;
    }
    return 1;
}

static int dan_ll_load(void) {
    DAN_LL_LOCK();
    if (dan_ll.state == 0) {
        static char tried[1200];
        char place[4096], base[4096];
        void *lib = NULL;
        const char *found = NULL;
        tried[0] = 0;
        dan_ll.state = -1;
        char echo[8];
        dan_ll_getenv("DAN_LLAMA_LOG", echo, sizeof echo);
        dan_ll_echo = echo[0] == '1';
        dan_ll_getenv("DAN_LLAMA", base, sizeof base);
        if (base[0]) {
            /* set explicitly: only this place */
            if (dan_ll_is_dir(base)) dan_ll_join(place, sizeof place, base, DAN_LL_SEP, DAN_LL_NAME);
            else dan_ll_join(place, sizeof place, base, "", "");
            if (!dan_ll_exists(place)) {
                dan_ll_why("библиотека llama.cpp не найдена: в переменной среды DAN_LLAMA указан «", place,
                           "», но такого файла нет.");
            } else if (!(lib = dan_ll_open(place, 1))) {
                dan_ll_why("библиотека llama.cpp «", place,
                           "» (переменная DAN_LLAMA) не открывается: это не библиотека для этой системы или рядом "
                           "нет её частей ggml.");
            } else {
                found = place;
            }
        } else {
            char dirs[5][4096];
            int nd = 0;
            dan_ll_program_dir(base, sizeof base);
            if (base[0]) {
                dan_ll_join(dirs[nd++], sizeof dirs[0], base, "", "");
                dan_ll_join(dirs[nd++], sizeof dirs[0], base, DAN_LL_SEP, "llama");
            }
            dan_ll_getenv("DAN_HOME", base, sizeof base);
            if (base[0]) {
                dan_ll_join(dirs[nd++], sizeof dirs[0], base, DAN_LL_SEP, "llama");
                dan_ll_join(dirs[nd++], sizeof dirs[0], base, "", "");
            }
#if defined(_WIN32)
            dan_ll_getenv("LOCALAPPDATA", base, sizeof base);
            if (base[0]) dan_ll_join(dirs[nd++], sizeof dirs[0], base, "\\Dan", "\\llama");
#else
            dan_ll_getenv("XDG_DATA_HOME", base, sizeof base);
            if (base[0]) dan_ll_join(dirs[nd++], sizeof dirs[0], base, "/dan", "/llama");
            else {
                dan_ll_getenv("HOME", base, sizeof base);
                if (base[0]) dan_ll_join(dirs[nd++], sizeof dirs[0], base, "/.local/share/dan", "/llama");
            }
#endif
            for (int i = 0; i < nd && !lib; i++) {
                if (!dirs[i][0] || !dan_ll_join(place, sizeof place, dirs[i], DAN_LL_SEP, DAN_LL_NAME)) continue;
                if (strlen(tried) + strlen(dirs[i]) + 4 < sizeof tried) {
                    if (tried[0]) strcat(tried, ", ");
                    strcat(tried, dirs[i]);
                }
                if (dan_ll_exists(place) && (lib = dan_ll_open(place, 1)) != NULL) found = place;
            }
            if (!lib) {
                static const char *const sys[] = {
#if defined(_WIN32)
                    "llama.dll"
#elif defined(__APPLE__)
                    "libllama.dylib", "/opt/homebrew/lib/libllama.dylib", "/usr/local/lib/libllama.dylib"
#else
                    "libllama.so", "libllama.so.0"
#endif
                };
                for (size_t i = 0; i < sizeof sys / sizeof sys[0] && !lib; i++) {
                    if ((lib = dan_ll_open(sys[i], sys[i][0] == '/')) != NULL) found = sys[i];
                }
            }
            if (!lib) {
                char m[1600];
                snprintf(m, sizeof m, "» не найдена. Искали в папках: %s и в системных папках.",
                         tried[0] ? tried : "-");
                dan_ll_why("библиотека llama.cpp «" DAN_LL_NAME, m, "");
            }
        }
        if (lib && dan_ll_take(lib, found)) {
            dan_ll.log_set(dan_ll_log, NULL);
#ifndef __wasi__
            if (dan_ll.set_abort) dan_ll.set_abort(dan_ll_abort);
#endif
            /* the folder of the found library (a path with a folder; a library found by its bare name
               in the system places loads its parts from where its build put them) */
            char folder[4096];
            folder[0] = 0;
            const char *cut = NULL;
            for (const char *c = found; *c; c++) {
                if (*c == '/' || *c == '\\') cut = c;
            }
            if (cut && (size_t)(cut - found) < sizeof folder) {
                memcpy(folder, found, (size_t)(cut - found));
                folder[cut - found] = 0;
                if (!folder[0]) strcpy(folder, "/");
            }
            if (folder[0] && dan_ll.load_all_from_path) dan_ll.load_all_from_path(folder);
            else if (dan_ll.load_all) dan_ll.load_all();
            dan_ll.backend_init();
            dan_ll.state = 1;
        }
        if (dan_ll.state != 1) {
            size_t n = strlen(dan_ll.why);
            if (n + sizeof dan_ll_help < sizeof dan_ll.why) memcpy(dan_ll.why + n, dan_ll_help, sizeof dan_ll_help);
        }
    }
    int ok = dan_ll.state == 1;
    DAN_LL_UNLOCK();
    return ok;
}

/* ---------- checking a GGUF file before llama.cpp reads it ---------- */

/* block size and bytes per block of the tensor types of ggml (b12190); 0 - no such type */
static const uint16_t dan_gguf_blck[44] = {
    1, 1, 32, 32, 0, 0, 32, 32, 32, 32, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 32, 256,
    256, 256, 1, 1, 1, 1, 1, 256, 1, 0, 0, 0, 256, 256, 0, 0, 0, 32, 64, 128, 64, 1 };
static const uint16_t dan_gguf_size[44] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292, 66, 74, 98, 50, 18, 110,
    82, 136, 1, 2, 4, 8, 8, 56, 2, 0, 0, 0, 54, 66, 0, 0, 0, 17, 36, 18, 18, 1 };

typedef struct {
    FILE *f;
    uint64_t pos, size;
    int bad;
} dan_gguf_rd;

static int dan_gguf_read(dan_gguf_rd *r, void *to, uint64_t n) {
    if (r->bad || n > r->size - r->pos) { r->bad = 1; return 0; }
    if (n && fread(to, 1, (size_t)n, r->f) != (size_t)n) { r->bad = 1; return 0; }
    r->pos += n;
    return 1;
}

static int dan_gguf_skip(dan_gguf_rd *r, uint64_t n) {
    if (r->bad || n > r->size - r->pos) { r->bad = 1; return 0; }
#if defined(_WIN32)
    if (_fseeki64(r->f, (long long)n, SEEK_CUR) != 0) { r->bad = 1; return 0; }
#else
    if (fseeko(r->f, (off_t)n, SEEK_CUR) != 0) { r->bad = 1; return 0; }
#endif
    r->pos += n;
    return 1;
}

static uint64_t dan_gguf_u64(dan_gguf_rd *r) {
    uint8_t b[8] = { 0 };
    dan_gguf_read(r, b, 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | b[i];
    return v;
}

static uint32_t dan_gguf_u32(dan_gguf_rd *r) {
    uint8_t b[4] = { 0 };
    dan_gguf_read(r, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* bytes of one value of a simple GGUF type; 0 - a string or an array, -1 - no such type */
static int dan_gguf_width(uint32_t t) {
    static const int w[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };
    return t < 13 ? w[t] : -1;
}

/* NULL - the file is a correct GGUF file, otherwise the reason */
static const char *dan_gguf_check(const char *path, uint64_t *file_size) {
#if defined(_WIN32)
    wchar_t *w = dan_wide(path);
    FILE *f = _wfopen(w, L"rb");
    dan_free(w);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) return dan_ll_exists(path) ? "файл не открывается для чтения" : "файл не найден";
    dan_gguf_rd r = { f, 0, 0, 0 };
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) == 0) { long long s = _ftelli64(f); r.size = s > 0 ? (uint64_t)s : 0; }
    _fseeki64(f, 0, SEEK_SET);
#else
    if (fseeko(f, 0, SEEK_END) == 0) { off_t s = ftello(f); r.size = s > 0 ? (uint64_t)s : 0; }
    fseeko(f, 0, SEEK_SET);
#endif
    *file_size = r.size;
    const char *why = NULL;
    char magic[4];
    if (r.size == 0) why = "файл пустой";
    else if (!dan_gguf_read(&r, magic, 4) || memcmp(magic, "GGUF", 4) != 0) why = "это не файл GGUF (нет подписи «GGUF» в начале)";
    uint32_t version = why ? 0 : dan_gguf_u32(&r);
    if (!why && (version < 2 || version > 3)) why = "версия формата GGUF не 2 и не 3";
    uint64_t n_tensors = why ? 0 : dan_gguf_u64(&r);
    uint64_t n_kv = why ? 0 : dan_gguf_u64(&r);
    if (!why && r.bad) why = "файл обрывается в заголовке";
    /* a key takes at least 12 bytes, a tensor description at least 28 */
    if (!why && (n_kv > r.size / 12 || n_tensors > r.size / 28)) why = "в заголовке неверное число ключей или тензоров";
    uint64_t align = 32;
    for (uint64_t i = 0; !why && i < n_kv; i++) {
        uint64_t klen = dan_gguf_u64(&r);
        char key[64];
        if (r.bad || klen == 0 || klen > 65535) { why = "неверная длина имени ключа"; break; }
        int is_align = 0;
        if (klen == 17) {
            dan_gguf_read(&r, key, klen);
            is_align = memcmp(key, "general.alignment", 17) == 0;
        } else {
            dan_gguf_skip(&r, klen);
        }
        uint32_t t = dan_gguf_u32(&r);
        if (r.bad) { why = "файл обрывается в таблице ключей"; break; }
        int wd = dan_gguf_width(t);
        if (wd < 0) { why = "неизвестный тип значения ключа"; break; }
        if (is_align) {
            if (t != 4) { why = "ключ general.alignment не типа uint32"; break; }
            align = dan_gguf_u32(&r);
            if (align == 0 || (align & (align - 1)) != 0 || align > 65536) { why = "неверное выравнивание данных"; break; }
            continue;
        }
        if (t == 8) {
            uint64_t sl = dan_gguf_u64(&r);
            if (!dan_gguf_skip(&r, sl)) { why = "строковое значение ключа выходит за конец файла"; break; }
        } else if (t == 9) {
            uint32_t et = dan_gguf_u32(&r);
            uint64_t n = dan_gguf_u64(&r);
            int ew = dan_gguf_width(et);
            if (r.bad || ew < 0 || et == 9) { why = "неверный тип элементов массива в ключе"; break; }
            if (et == 8) {
                if (n > (r.size - r.pos) / 8) { why = "массив строк выходит за конец файла"; break; }
                for (uint64_t k = 0; k < n && !r.bad; k++) dan_gguf_skip(&r, dan_gguf_u64(&r));
                if (r.bad) { why = "массив строк выходит за конец файла"; break; }
            } else if (n > (r.size - r.pos) / (uint64_t)ew || !dan_gguf_skip(&r, n * (uint64_t)ew)) {
                why = "массив выходит за конец файла";
                break;
            }
        } else if (!dan_gguf_skip(&r, (uint64_t)wd)) {
            why = "файл обрывается в таблице ключей";
            break;
        }
    }
    /* the tensors: offset and size within the data part of the file */
    uint64_t *ends = NULL;
    if (!why && n_tensors) {
        ends = malloc(sizeof *ends * (size_t)n_tensors * 2);
        if (!ends) why = "слишком много тензоров";
    }
    for (uint64_t i = 0; !why && i < n_tensors; i++) {
        uint64_t nl = dan_gguf_u64(&r);
        if (r.bad || nl == 0 || nl >= 64 || !dan_gguf_skip(&r, nl)) { why = "неверное имя тензора"; break; }
        uint32_t nd = dan_gguf_u32(&r);
        if (r.bad || nd == 0 || nd > 4) { why = "неверное число измерений тензора"; break; }
        uint64_t ne = 1, d0 = 0;
        for (uint32_t k = 0; k < nd; k++) {
            uint64_t d = dan_gguf_u64(&r);
            if (k == 0) d0 = d;
            if (d > (uint64_t)INT64_MAX || (d && ne > (uint64_t)INT64_MAX / d)) { why = "размер тензора слишком велик"; break; }
            ne *= d;
        }
        if (why) break;
        uint32_t t = dan_gguf_u32(&r);
        uint64_t off = dan_gguf_u64(&r);
        if (r.bad) { why = "файл обрывается в таблице тензоров"; break; }
        if (t >= 44 || dan_gguf_blck[t] == 0) { why = "неизвестный тип тензора"; break; }
        if (d0 % dan_gguf_blck[t] != 0) { why = "размер тензора не делится на размер блока его типа"; break; }
        uint64_t bytes = ne / dan_gguf_blck[t];
        if (bytes > (uint64_t)INT64_MAX / dan_gguf_size[t]) { why = "размер тензора слишком велик"; break; }
        bytes *= dan_gguf_size[t];
        if (off % align != 0) { why = "смещение тензора не выровнено"; break; }
        if (off > (uint64_t)INT64_MAX - bytes) { why = "смещение тензора слишком велико"; break; }
        ends[i] = off + bytes;
    }
    if (!why) {
        uint64_t data = r.pos + (align - r.pos % align) % align;
        if (data > r.size) why = "нет части с данными тензоров";
        for (uint64_t i = 0; !why && i < n_tensors; i++) {
            if (ends[i] > r.size - data) why = "данные тензора выходят за конец файла (файл обрезан?)";
        }
    }
    free(ends);
    fclose(f);
    return why;
}

/* ---------- models ---------- */

typedef struct {
    void *model, *ctx;
    const void *vocab;
    dan_ll_token *cache;            /* the tokens in the memory of the context */
    int32_t n_cache, cap_cache;
    uint32_t n_ctx, n_batch;
    int busy;
    int broken;                     /* llama.cpp failed inside a call: the model is not used any more */
    /* generation */
    void *smpl;
    int active;
    int64_t left;                   /* tokens still allowed; < 0 - up to the end of the context */
    char *pending;                  /* text not given to the program yet */
    size_t n_pending, cap_pending;
    char **stops;
    int32_t n_stops;
} dan_ll_m;

static dan_ll_m **dan_ll_models;
static int64_t dan_ll_count;

static dr_i dan_ll_fail_i(const char *a, const char *b, const char *c, int line) {
    dan_buf m = { NULL, 0, 0 };
    dan_buf_add(&m, a);
    dan_buf_add(&m, b);
    dan_buf_add(&m, c);
    return dr_i_err(dan_buf_str(&m), line);
}

static dr_s dan_ll_fail_s(const char *a, const char *b, int line) {
    dan_buf m = { NULL, 0, 0 };
    dan_buf_add(&m, a);
    dan_buf_add(&m, b);
    return dr_s_err(dan_buf_str(&m), line);
}

static int dan_ll_threads(int64_t asked) {
    if (asked >= 1) return asked > 512 ? 512 : (int)asked;
    char v[32];
    dan_ll_getenv("DAN_POTOKI", v, sizeof v);
    long t = v[0] ? strtol(v, NULL, 10) : 0;
    if (t < 1) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        t = (long)si.dwNumberOfProcessors;
#elif defined(__wasi__)
        t = 1;
#else
        t = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    }
    if (t < 1) t = 1;
    return t > 512 ? 512 : (int)t;
}

static dr_i dan_llm_load(dan_str path, int64_t n_ctx, int64_t threads, bool mmap, int64_t limit_mb, int line) {
    if (n_ctx < 0 || n_ctx > 1048576) return dr_i_err(dan_strdup("размер контекста должен быть от 0 (как в модели) до 1048576 токенов"), line);
    if (limit_mb < 0) return dr_i_err(dan_strdup("предел памяти не может быть отрицательным"), line);
    uint64_t size = 0;
    const char *bad = dan_gguf_check(path, &size);
    if (bad) {
        dan_buf m = { NULL, 0, 0 };
        dan_buf_add(&m, "файл модели «");
        dan_buf_add(&m, path);
        dan_buf_add(&m, "» не подходит: ");
        dan_buf_add(&m, bad);
        return dr_i_err(dan_buf_str(&m), line);
    }
    if (!dan_ll_load()) return dr_i_err(dan_strdup(dan_ll.why), line);
    if (limit_mb > 0 && size / 1048576 >= (uint64_t)limit_mb) {
        char m[300];
        snprintf(m, sizeof m, "» занимает %llu МБ, а предел памяти - %lld МБ",
                 (unsigned long long)((size + 1048575) / 1048576), (long long)limit_mb);
        return dan_ll_fail_i("модель «", path, m, line);
    }
    struct dan_ll_model_params mp = dan_ll.model_default_params();
    mp.load_mode = mmap ? 1 : 0;         /* LLAMA_LOAD_MODE_MMAP or LLAMA_LOAD_MODE_NONE */
    mp.n_gpu_layers = 0;                 /* the processor: the same results on every computer */
    dan_ll_last[0] = 0;
    void *model = NULL;
    int ok = 0;
    DAN_LL_GUARD(ok, model = dan_ll.model_load(path, mp));
    if (!ok) {
        dan_buf b = { NULL, 0, 0 };
        dan_buf_add(&b, "llama.cpp не смогла загрузить модель «");
        dan_buf_add(&b, path);
        dan_buf_add(&b, "»: файл повреждён или его значения не подходят этой версии llama.cpp (llama.cpp сообщила: ");
        dan_buf_add(&b, dan_ll_abort_text);
        dan_buf_add(&b, ")");
        return dr_i_err(dan_buf_str(&b), line);
    }
    if (!model) {
        dan_buf b = { NULL, 0, 0 };
        dan_buf_add(&b, "llama.cpp не смогла загрузить модель «");
        dan_buf_add(&b, path);
        dan_buf_add(&b, "»");
        if (dan_ll_last[0]) {
            dan_buf_add(&b, " (");
            dan_buf_add(&b, dan_ll_last);
            dan_buf_add(&b, ")");
        }
        dan_buf_add(&b, ": это не языковая модель, её устройство не знакомо этой версии llama.cpp или файл повреждён; "
                        "подробности - с переменной среды DAN_LLAMA_LOG=1");
        return dr_i_err(dan_buf_str(&b), line);
    }
    struct dan_ll_context_params cp = dan_ll.context_default_params();
    cp.n_ctx = (uint32_t)n_ctx;
    cp.n_threads = cp.n_threads_batch = dan_ll_threads(threads);
    cp.no_perf = true;
    void *ctx = NULL;
    DAN_LL_GUARD(ok, ctx = dan_ll.init_from_model(model, cp));
    if (!ok) return dan_ll_fail_i("llama.cpp не смогла создать контекст для модели: ", dan_ll_abort_text, "", line);
    if (!ctx) {
        dan_ll.model_free(model);
        return dan_ll_fail_i("llama.cpp не смогла создать контекст для модели «", path,
                             "» (не хватает памяти? уменьшите размер контекста)", line);
    }
    dan_ll_m *m = calloc(1, sizeof *m);
    if (!m) dan_error(DAN_MSG_NO_MEMORY, line);
    m->model = model;
    m->ctx = ctx;
    m->vocab = dan_ll.model_vocab(model);
    m->n_ctx = dan_ll.n_ctx(ctx);
    m->n_batch = dan_ll.n_batch(ctx);
    if (m->n_batch == 0) m->n_batch = 512;
    DAN_LL_LOCK();
    dan_ll_m **t = realloc(dan_ll_models, sizeof *t * (size_t)(dan_ll_count + 1));
    if (!t) { DAN_LL_UNLOCK(); dan_error(DAN_MSG_NO_MEMORY, line); }
    dan_ll_models = t;
    dan_ll_models[dan_ll_count++] = m;
    int64_t n = dan_ll_count;
    DAN_LL_UNLOCK();
    return dr_i_some(n);
}

/* take a model for one call; NULL - there is no such model or it is busy (why) */
static dan_ll_m *dan_ll_take_model(int64_t n, const char **why) {
    DAN_LL_LOCK();
    dan_ll_m *m = n >= 1 && n <= dan_ll_count ? dan_ll_models[n - 1] : NULL;
    if (!m) *why = "модель уже освобождена или не загружалась";
    else if (m->broken) { *why = "после сбоя внутри llama.cpp модель больше не используется: загрузите её заново"; m = NULL; }
    else if (m->busy) { *why = "модель занята: с одной моделью одновременно работает одна задача"; m = NULL; }
    else m->busy = 1;
    DAN_LL_UNLOCK();
    return m;
}

static void dan_ll_give_model(dan_ll_m *m) {
    DAN_LL_LOCK();
    m->busy = 0;
    DAN_LL_UNLOCK();
}

static void dan_ll_stop_generation(dan_ll_m *m) {
    if (m->smpl) dan_ll.sampler_free(m->smpl);
    m->smpl = NULL;
    m->active = 0;
    m->n_pending = 0;
    for (int32_t i = 0; i < m->n_stops; i++) free(m->stops[i]);
    free(m->stops);
    m->stops = NULL;
    m->n_stops = 0;
}

static void dan_llm_free(int64_t n, int line) {
    (void)line;
    DAN_LL_LOCK();
    dan_ll_m *m = n >= 1 && n <= dan_ll_count ? dan_ll_models[n - 1] : NULL;
    if (m && m->busy) m = NULL;          /* a model in use by another task is freed when the program ends */
    if (m) dan_ll_models[n - 1] = NULL;
    DAN_LL_UNLOCK();
    if (!m) return;
    if (!m->broken) {                    /* after a failure inside llama.cpp its objects are left as they are */
        dan_ll_stop_generation(m);
        dan_ll.ctx_free(m->ctx);
        dan_ll.model_free(m->model);
    }
    free(m->cache);
    free(m->pending);
    free(m);
}

/* tokens of a text into *out (malloc); the number or < 0 */
static int32_t dan_ll_tokens(dan_ll_m *m, dan_str text, bool add_special, dan_ll_token **out) {
    size_t len = strlen(text);
    if (len > INT32_MAX - 16) return -1;
    int32_t cap = (int32_t)len + 16;
    dan_ll_token *t = malloc(sizeof *t * (size_t)cap);
    if (!t) return -1;
    int32_t n = dan_ll.tokenize(m->vocab, text, (int32_t)len, t, cap, add_special, true);
    if (n < 0 && n != INT32_MIN) {
        cap = -n;
        dan_ll_token *t2 = realloc(t, sizeof *t * (size_t)cap);
        if (!t2) { free(t); return -1; }
        t = t2;
        n = dan_ll.tokenize(m->vocab, text, (int32_t)len, t, cap, add_special, true);
    }
    if (n < 0) { free(t); return -1; }
    *out = t;
    return n;
}

static int dan_ll_decode(dan_ll_m *m, dan_ll_token *t, int32_t n) {
    for (int32_t i = 0; i < n; ) {
        int32_t k = n - i;
        if ((uint32_t)k > m->n_batch) k = (int32_t)m->n_batch;
        struct dan_ll_batch b = { k, t + i, NULL, NULL, NULL, NULL, NULL };
        int32_t rc = 0;
        int ok = 0;
        DAN_LL_GUARD(ok, rc = dan_ll.decode(m->ctx, b));
        if (!ok) {
            m->broken = 1;
            return INT32_MIN;
        }
        if (rc != 0) return rc;
        i += k;
    }
    return 0;
}

static int dan_ll_cache_add(dan_ll_m *m, const dan_ll_token *t, int32_t n) {
    if (m->n_cache + n > m->cap_cache) {
        int32_t cap = (m->n_cache + n) * 2 + 64;
        dan_ll_token *c = realloc(m->cache, sizeof *c * (size_t)cap);
        if (!c) return 0;
        m->cache = c;
        m->cap_cache = cap;
    }
    memcpy(m->cache + m->n_cache, t, sizeof *t * (size_t)n);
    m->n_cache += n;
    return 1;
}

/* start a generation; the result - how many tokens of the request were computed (not taken from the cache) */
static dr_i dan_llm_start(int64_t n, dan_str prompt, double temp, int64_t top_k, double top_p, int64_t seed,
                          int64_t max_tokens, dl_Ls *stops, int line) {
    const char *why = NULL;
    dan_ll_m *m = dan_ll_take_model(n, &why);
    if (!m) return dr_i_err(dan_strdup(why), line);
    dan_ll_stop_generation(m);
    if (!(temp >= 0.0 && temp <= 100.0) || !(top_p > 0.0 && top_p <= 1.0) || top_k < 0) {
        dan_ll_give_model(m);
        return dr_i_err(dan_strdup("неверные параметры выборки: температура от 0 до 100, верх_п больше 0 и не больше 1, "
                                   "верх_к не меньше 0"), line);
    }
    dan_ll_token *t = NULL;
    int32_t nt = dan_ll_tokens(m, prompt, true, &t);
    if (nt <= 0) {
        free(t);
        dan_ll_give_model(m);
        return dr_i_err(dan_strdup(nt == 0 ? "пустой запрос к модели" : "не удалось разбить запрос на токены"), line);
    }
    if ((uint32_t)nt >= m->n_ctx) {
        free(t);
        dan_ll_give_model(m);
        char msg[300];
        snprintf(msg, sizeof msg, "запрос длиннее контекста модели: %d токенов при контексте %u; увеличьте размер "
                 "контекста (Настройки.контекст)", nt, m->n_ctx);
        return dr_i_err(dan_strdup(msg), line);
    }
    /* the common beginning with the tokens in memory is not computed again; the
       last token of the request is always computed: its result gives the next token */
    int32_t same = 0;
    while (same < nt && same < m->n_cache && m->cache[same] == t[same]) same++;
    if (same == nt) same--;
    void *mem = dan_ll.get_memory(m->ctx);
    if (!dan_ll.memory_seq_rm(mem, 0, same, -1)) {
        dan_ll.memory_clear(mem, true);
        same = 0;
    }
    m->n_cache = same;
    int rc = dan_ll_decode(m, t + same, nt - same);
    if (rc == INT32_MIN) {
        free(t);
        dan_ll_give_model(m);
        return dan_ll_fail_i("сбой внутри llama.cpp: ", dan_ll_abort_text, "", line);
    }
    if (rc != 0 || !dan_ll_cache_add(m, t + same, nt - same)) {
        free(t);
        dan_ll.memory_clear(mem, true);
        m->n_cache = 0;
        dan_ll_give_model(m);
        char msg[200];
        snprintf(msg, sizeof msg, "llama.cpp не смогла обработать запрос (код %d)", rc);
        return dr_i_err(dan_strdup(msg), line);
    }
    free(t);
    struct dan_ll_chain_params sp = dan_ll.chain_default_params();
    sp.no_perf = true;
    m->smpl = dan_ll.chain_init(sp);
    if (temp <= 0.0) {
        dan_ll.chain_add(m->smpl, dan_ll.init_greedy());
    } else {
        if (top_k > 0) dan_ll.chain_add(m->smpl, dan_ll.init_top_k(top_k > INT32_MAX ? INT32_MAX : (int32_t)top_k));
        if (top_p < 1.0) dan_ll.chain_add(m->smpl, dan_ll.init_top_p((float)top_p, 1));
        dan_ll.chain_add(m->smpl, dan_ll.init_temp((float)temp));
        dan_ll.chain_add(m->smpl, dan_ll.init_dist(seed < 0 ? 0xFFFFFFFFu : (uint32_t)(seed & 0x7FFFFFFF)));
    }
    m->left = max_tokens > 0 ? max_tokens : -1;
    m->active = 1;
    m->n_pending = 0;
    if (stops->len > 0) {
        m->stops = calloc((size_t)stops->len, sizeof *m->stops);
        if (!m->stops) dan_error(DAN_MSG_NO_MEMORY, line);
        for (int64_t i = 0; i < stops->len; i++) {
            if (!stops->items[i][0]) continue;
            size_t k = strlen(stops->items[i]);
            char *s = malloc(k + 1);
            if (!s) dan_error(DAN_MSG_NO_MEMORY, line);
            memcpy(s, stops->items[i], k + 1);
            m->stops[m->n_stops++] = s;
        }
    }
    dan_ll_give_model(m);
    return dr_i_some(nt - same);
}

/* the length of the longest correct UTF-8 beginning of p[0..n) that does not cut a character */
static size_t dan_ll_utf8_whole(const char *p, size_t n) {
    size_t i = n, back = 0;
    while (i > 0 && back < 4 && ((unsigned char)p[i - 1] & 0xC0) == 0x80) { i--; back++; }
    if (i == 0) return n;
    unsigned char c = (unsigned char)p[i - 1];
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (c >= 0xC0 && back + 1 < need) return i - 1;    /* the last character is not complete yet */
    return n;
}

/* p[0..n) as a string of correct UTF-8: wrong bytes become U+FFFD, zero bytes are dropped */
static dan_str dan_ll_text(const char *p, size_t n) {
    char *r = dan_str_new(n * 3);
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)p[i];
        size_t k = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        int ok = k > 0 && i + k <= n;
        for (size_t j = 1; ok && j < k; j++) ok = ((unsigned char)p[i + j] & 0xC0) == 0x80;
        if (ok && k == 3) {
            unsigned char d = (unsigned char)p[i + 1];
            ok = !(c == 0xE0 && d < 0xA0) && !(c == 0xED && d >= 0xA0);
        }
        if (ok && k == 4) {
            unsigned char d = (unsigned char)p[i + 1];
            ok = !(c == 0xF0 && d < 0x90) && !(c == 0xF4 && d >= 0x90);
        }
        if (!ok) {
            memcpy(r + o, "\xEF\xBF\xBD", 3);
            o += 3;
            i++;
            continue;
        }
        if (c != 0) {
            memcpy(r + o, p + i, k);
            o += k;
        }
        i += k;
    }
    r[o] = 0;
    return r;
}

static void dan_ll_pending_add(dan_ll_m *m, const char *p, size_t n, int line) {
    if (m->n_pending + n + 1 > m->cap_pending) {
        size_t cap = (m->n_pending + n + 1) * 2 + 64;
        char *q = realloc(m->pending, cap);
        if (!q) dan_error(DAN_MSG_NO_MEMORY, line);
        m->pending = q;
        m->cap_pending = cap;
    }
    memcpy(m->pending + m->n_pending, p, n);
    m->n_pending += n;
}

/* give the first k bytes of the pending text */
static dan_str dan_ll_give(dan_ll_m *m, size_t k) {
    dan_str s = dan_ll_text(m->pending, k);
    memmove(m->pending, m->pending + k, m->n_pending - k);
    m->n_pending -= k;
    return s;
}

/* the next part of the answer; "" - the answer is over */
static dr_s dan_llm_next(int64_t n, int line) {
    const char *why = NULL;
    dan_ll_m *m = dan_ll_take_model(n, &why);
    if (!m) return dr_s_err(dan_strdup(why), line);
    char piece[256];
    for (;;) {
        if (!m->active) {
            /* the end: what is left is given once */
            dan_str s = dan_ll_give(m, m->n_pending);
            dan_ll_stop_generation(m);
            dan_ll_give_model(m);
            return dr_s_some(s);
        }
        if (m->left == 0) { m->active = 0; continue; }
        dan_ll_token tok = 0;
        int ok = 0;
        DAN_LL_GUARD(ok, tok = dan_ll.sample(m->smpl, m->ctx, -1));
        if (!ok) {
            m->broken = 1;
            dan_ll_give_model(m);
            return dan_ll_fail_s("сбой внутри llama.cpp: ", dan_ll_abort_text, line);
        }
        if (dan_ll.is_eog(m->vocab, tok)) { m->active = 0; continue; }
        int32_t k = dan_ll.token_to_piece(m->vocab, tok, piece, (int32_t)sizeof piece, 0, false);
        if (k < 0) k = 0;                /* a part longer than 256 bytes is not in real vocabularies */
        dan_ll_pending_add(m, piece, (size_t)k, line);
        if (m->left > 0) m->left--;
        /* a stop string: the text before it is the end of the answer */
        size_t first = m->n_pending;
        for (int32_t i = 0; i < m->n_stops; i++) {
            size_t sl = strlen(m->stops[i]);
            for (size_t p = 0; p + sl <= m->n_pending && p < first; p++) {
                if (memcmp(m->pending + p, m->stops[i], sl) == 0) { first = p; break; }
            }
        }
        if (first < m->n_pending) {
            m->n_pending = first;
            m->active = 0;
            continue;
        }
        if (m->left != 0) {
            if (m->n_cache >= (int32_t)m->n_ctx) {
                dan_ll_stop_generation(m);
                dan_ll_give_model(m);
                return dr_s_err(dan_strdup("контекст модели заполнен: ответ не поместился; увеличьте размер контекста "
                                           "(Настройки.контекст) или ограничьте ответ (Выборка.максимум_токенов)"), line);
            }
            int rc = dan_ll_decode(m, &tok, 1);
            if (rc == INT32_MIN) {
                dan_ll_give_model(m);
                return dan_ll_fail_s("сбой внутри llama.cpp: ", dan_ll_abort_text, line);
            }
            if (rc != 0 || !dan_ll_cache_add(m, &tok, 1)) {
                dan_ll_stop_generation(m);
                dan_ll_give_model(m);
                char msg[200];
                snprintf(msg, sizeof msg, "llama.cpp не смогла продолжить ответ (код %d)", rc);
                return dr_s_err(dan_strdup(msg), line);
            }
        }
        /* the text that cannot be the beginning of a stop string and does not cut a character */
        size_t safe = m->n_pending;
        for (int32_t i = 0; i < m->n_stops; i++) {
            size_t sl = strlen(m->stops[i]);
            for (size_t keep = sl - 1; keep > 0; keep--) {
                if (keep <= m->n_pending && memcmp(m->pending + m->n_pending - keep, m->stops[i], keep) == 0) {
                    if (m->n_pending - keep < safe) safe = m->n_pending - keep;
                    break;
                }
            }
        }
        safe = dan_ll_utf8_whole(m->pending, safe);
        if (safe > 0) {
            dan_str s = dan_ll_give(m, safe);
            dan_ll_give_model(m);
            return dr_s_some(s);
        }
    }
}

/* a chat by the template of the model: roles and texts in turn */
static dr_s dan_llm_chat(int64_t n, dl_Ls *parts, bool add_assistant, int line) {
    if (parts->len % 2 != 0) return dr_s_err(dan_strdup("сообщения чата: роли и тексты должны идти парами"), line);
    const char *why = NULL;
    dan_ll_m *m = dan_ll_take_model(n, &why);
    if (!m) return dr_s_err(dan_strdup(why), line);
    size_t nmsg = (size_t)(parts->len / 2), total = 64;
    struct dan_ll_chat_message *msg = calloc(nmsg ? nmsg : 1, sizeof *msg);
    if (!msg) dan_error(DAN_MSG_NO_MEMORY, line);
    for (size_t i = 0; i < nmsg; i++) {
        msg[i].role = parts->items[2 * i];
        msg[i].content = parts->items[2 * i + 1];
        total += strlen(msg[i].role) + strlen(msg[i].content) + 64;
    }
    const char *tmpl = dan_ll.chat_template(m->model, NULL);
    if (total > INT32_MAX / 2) { free(msg); dan_ll_give_model(m); return dr_s_err(dan_strdup("сообщения чата слишком длинные"), line); }
    int32_t cap = (int32_t)total * 2;
    char *buf = malloc((size_t)cap);
    if (!buf) dan_error(DAN_MSG_NO_MEMORY, line);
    int32_t r = dan_ll.chat_apply(tmpl, msg, nmsg, add_assistant, buf, cap);
    if (r > cap) {
        cap = r;
        char *b2 = realloc(buf, (size_t)cap);
        if (!b2) dan_error(DAN_MSG_NO_MEMORY, line);
        buf = b2;
        r = dan_ll.chat_apply(tmpl, msg, nmsg, add_assistant, buf, cap);
    }
    free(msg);
    dan_ll_give_model(m);
    if (r < 0 || r > cap) {
        free(buf);
        return dan_ll_fail_s("шаблон чата модели не поддерживается llama.cpp (llama_chat_apply_template знает только "
                             "распространённые шаблоны); соберите запрос сами и вызовите Модель.ответ. Шаблон модели: ",
                             tmpl ? tmpl : "нет", line);
    }
    dan_str s = dan_ll_text(buf, (size_t)r);
    free(buf);
    return dr_s_some(s);
}

static dr_i dan_llm_count(int64_t n, dan_str text, int line) {
    const char *why = NULL;
    dan_ll_m *m = dan_ll_take_model(n, &why);
    if (!m) return dr_i_err(dan_strdup(why), line);
    dan_ll_token *t = NULL;
    int32_t k = dan_ll_tokens(m, text, false, &t);
    free(t);
    dan_ll_give_model(m);
    if (k < 0) return dr_i_err(dan_strdup("не удалось разбить текст на токены"), line);
    return dr_i_some(k);
}

/* the text without a line end, at once (the parts of an answer as they come) */
static void dan_llm_print(dan_str s) {
    DAN_OUT_LOCK();
    fputs(s, stdout);
    fflush(stdout);
    DAN_OUT_UNLOCK();
}
