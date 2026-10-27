/* ==== System library of the Dan language: dates and time, the program's
   arguments and environment, running other programs, regular expressions. ==== */

/* ---- dates ---- */

static dan_date dan_today(void) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    dan_date r = { dan_days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) };
    return r;
}

static dr_t dan_parse_date_res(dan_str s, int line) {
    int64_t y, m, d;
    if (!dan_date_scan(s, &y, &m, &d) || !dan_date_valid(y, m, d)) {
        dan_buf b = { NULL, 0, 0 };
        dan_buf_add(&b, "\"");
        dan_buf_add(&b, s);
        dan_buf_add(&b, DAN_TXT_NOT_DATE);
        return dr_t_err(dan_buf_str(&b), line);
    }
    dan_date r = { dan_days_from_civil(y, m, d) };
    return dr_t_some(r);
}

/* срок, исчисляемый месяцами, истекает в соответствующее число последнего
   месяца, а если такого числа нет - в последний день месяца */
static dan_date dan_add_months(dan_date v, int64_t n, int line) {
    int64_t y, m, d;
    dan_civil_from_days(v.d, &y, &m, &d);
    int64_t total;
    if (__builtin_add_overflow(y * 12 + (m - 1), n, &total)) dan_error(DAN_MSG_DATE_RANGE, line);
    int64_t ny = total >= 0 ? total / 12 : -((-total + 11) / 12), nm = total - ny * 12 + 1;
    if (ny < 1 || ny > 9999) dan_error(DAN_MSG_DATE_RANGE, line);
    int64_t last = dan_days_in_month(ny, nm, line);
    dan_date r = { dan_days_from_civil(ny, nm, d < last ? d : last) };
    return r;
}
static dan_date dan_add_years(dan_date v, int64_t n, int line) {
    int64_t m;
    if (__builtin_mul_overflow(n, 12, &m)) dan_error(DAN_MSG_DATE_RANGE, line);
    return dan_add_months(v, m, line);
}

static bool dan_weekend(dan_date v) { return dan_date_weekday(v) >= 6; }
/* нерабочие праздничные дни по статье 112 Трудового кодекса РФ (без переносов) */
static bool dan_holiday(dan_date v) {
    int64_t y, m, d;
    dan_civil_from_days(v.d, &y, &m, &d);
    switch (m) {
    case 1: return d <= 8;
    case 2: return d == 23;
    case 3: return d == 8;
    case 5: return d == 1 || d == 9;
    case 6: return d == 12;
    case 11: return d == 4;
    default: return false;
    }
}
static bool dan_workday(dan_date v) { return !dan_weekend(v) && !dan_holiday(v); }
static dan_date dan_add_workdays(dan_date v, int64_t n, int line) {
    int64_t step = n >= 0 ? 1 : -1, left = n >= 0 ? n : -n;
    while (left > 0) {
        v = dan_date_add(v, step, line);
        if (dan_workday(v)) left--;
    }
    return v;
}
static int64_t dan_month_days(int64_t y, int64_t m, int line) { return dan_days_in_month(y, m, line); }

/* ---- time ---- */

static dan_str dan_now_str(void) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[40];
    snprintf(b, sizeof b, "%02d.%02d.%04d %02d:%02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return dan_strdup(b);
}

/* seconds since 01.01.1970 with fractions: for measuring how long something takes */
static double dan_timestamp(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)(v - 116444736000000000ULL) / 1e7;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void dan_sleep(double seconds, int line) {
    if (seconds < 0 || seconds != seconds) dan_error(DAN_MSG_BAD_PAUSE, line);
    fflush(stdout);
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#endif
}

/* ---- the program's arguments, environment, other programs ---- */

static dl_Ls *dan_program_args(void) {
    dl_Ls *r = dl_Ls_from(0, NULL);
#ifdef _WIN32
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; w && i < n; i++) dl_Ls_push(r, dan_narrow(w[i]));
    if (w) LocalFree(w);
#else
    for (int i = 1; i < dan_argc; i++) dl_Ls_push(r, dan_strdup(dan_argv[i]));
#endif
    return r;
}

static do_s dan_env_var(dan_str name) {
#ifdef _WIN32
    wchar_t *wn = dan_wide(name);
    const wchar_t *v = _wgetenv(wn);
    free(wn);
    if (!v) return do_s_none();
    return do_s_some(dan_narrow(v));
#else
    const char *v = getenv(name);
    if (!v) return do_s_none();
    return do_s_some(dan_strdup(v));
#endif
}

#ifdef _WIN32
/* Starts «cmd.exe /c command» as _wsystem does, but with the standard handles passed
   explicitly: _wsystem passes them in the C library's handle table, and cmd.exe does
   not give them to the program it starts when they are pipes (the output of the Dan
   program is read by another program). flags - creation flags (CREATE_SUSPENDED). */
static BOOL dan_win_spawn(dan_str command, DWORD flags, PROCESS_INFORMATION *pi) {
    wchar_t *wc = dan_wide(command);
    const wchar_t *comspec = _wgetenv(L"ComSpec");
    if (!comspec || !*comspec) comspec = L"cmd.exe";
    size_t n = wcslen(comspec) + wcslen(wc) + 8;
    wchar_t *cl = dan_alloc(sizeof(wchar_t) * n);
    swprintf(cl, n, L"\"%ls\" /c %ls", comspec, wc);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    HANDLE own[3] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    HANDLE dup[3] = { NULL, NULL, NULL };
    for (int k = 0; k < 3; k++) {
        /* the started program gets only handles that can be inherited */
        if (own[k] && own[k] != INVALID_HANDLE_VALUE
                && !DuplicateHandle(GetCurrentProcess(), own[k], GetCurrentProcess(), &dup[k], 0, TRUE, DUPLICATE_SAME_ACCESS))
            dup[k] = NULL;
    }
    si.hStdInput = dup[0] ? dup[0] : own[0];
    si.hStdOutput = dup[1] ? dup[1] : own[1];
    si.hStdError = dup[2] ? dup[2] : own[2];
    BOOL ok = CreateProcessW(NULL, cl, NULL, NULL, TRUE, flags, NULL, NULL, &si, pi);
    for (int k = 0; k < 3; k++) if (dup[k]) CloseHandle(dup[k]);
    free(cl);
    free(wc);
    return ok;
}
#endif

static int64_t dan_run(dan_str command, int line) {
    fflush(stdout);
#if defined(__wasi__)
    (void)command;
    dan_error(DAN_MSG_RUN_FAILED, line);
    return -1;
#elif defined(_WIN32)
    fflush(stderr);
    PROCESS_INFORMATION pi;
    if (!dan_win_spawn(command, 0, &pi)) dan_error(DAN_MSG_RUN_FAILED, line);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int64_t)code;
#else
    int r = system(command);
    if (r == -1) dan_error(DAN_MSG_RUN_FAILED, line);
    if (WIFEXITED(r)) return WEXITSTATUS(r);
    return 128 + (WIFSIGNALED(r) ? WTERMSIG(r) : 0);
#endif
}

#if !defined(_WIN32) && !defined(__wasi__)
#include <signal.h>
#endif

/* The command, but no longer than «seconds»: a command that has not finished is
   stopped together with the programs it started, and the result is -1. */
static int64_t dan_run_limit(dan_str command, double seconds, int line) {
    if (seconds < 0 || seconds != seconds) dan_error(DAN_MSG_BAD_PAUSE, line);
    fflush(stdout);
    fflush(stderr);
#if defined(__wasi__)
    (void)command;
    dan_error(DAN_MSG_RUN_FAILED, line);
    return -1;
#elif defined(_WIN32)
    /* the program and everything it starts are in one job: stopping the job stops them all */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    PROCESS_INFORMATION pi;
    if (!dan_win_spawn(command, CREATE_SUSPENDED, &pi)) {
        if (job) CloseHandle(job);
        dan_error(DAN_MSG_RUN_FAILED, line);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    double ms = seconds * 1000.0;
    DWORD w = WaitForSingleObject(pi.hProcess, ms >= 4294967294.0 ? INFINITE : (DWORD)ms);
    int64_t result;
    if (w == WAIT_TIMEOUT) {
        if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result = -1;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result = (int64_t)code;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return result;
#else
    pid_t pid = fork();
    if (pid < 0) dan_error(DAN_MSG_RUN_FAILED, line);
    if (pid == 0) {
        setpgid(0, 0);                  /* own group: stopping it stops the programs the command started */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    struct timespec start, now, step = { 0, 5000000 };
    clock_gettime(CLOCK_MONOTONIC, &start);
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) dan_error(DAN_MSG_RUN_FAILED, line);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9 > seconds) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        nanosleep(&step, NULL);
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}

static dr_s dan_run_output(dan_str command, int line) {
    fflush(stdout);
#ifdef __wasi__
    /* WebAssembly cannot start other programs */
    (void)command;
    return dr_s_err(dan_strdup(DAN_MSG_RUN_FAILED), line);
#else
#ifdef _WIN32
    wchar_t *wc = dan_wide(command);
    FILE *p = _wpopen(wc, L"rb");
    free(wc);
#else
    FILE *p = popen(command, "r");
#endif
    if (!p) return dr_s_err(dan_strdup(DAN_MSG_RUN_FAILED), line);
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk - 1, p)) > 0) {
        chunk[got] = 0;
        /* line ends of Windows programs become \n */
        char *w = chunk;
        for (char *q = chunk; *q; q++) if (*q != '\r') *w++ = *q;
        *w = 0;
        dan_buf_add(&b, chunk);
    }
#ifdef _WIN32
    int code = _pclose(p);
#else
    int code = pclose(p);
#ifndef __wasi__
    if (code != -1 && WIFEXITED(code)) code = WEXITSTATUS(code);
#endif
#endif
    if (code != 0) {
        dan_buf_free(&b);
        char m[200];
        snprintf(m, sizeof m, DAN_MSG_RUN_CODE, code);
        return dr_s_err(dan_strdup(m), line);
    }
    while (b.len > 0 && b.s[b.len - 1] == '\n') b.s[--b.len] = 0;   /* the final line ends are dropped */
    return dr_s_some(dan_buf_str(&b));
#endif
}

static void dan_exit(int64_t code) {
    fflush(stdout);
    exit((int)code);
}

static void dan_eprint(dan_str s) {
    DAN_OUT_LOCK();
    fflush(stdout);
    fputs(s, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    DAN_OUT_UNLOCK();
}

/* ---- regular expressions (шаблоны) ----
   The pattern is compiled into a program for a Pike VM: all variants are
   followed simultaneously, so matching takes time proportional to the
   length of the text times the size of the pattern and never «explodes»
   on unlucky patterns (as backtracking engines do). Characters are Unicode
   code points of UTF-8 text; \w and (?i) know Latin and Cyrillic letters. */

enum { RX_CHAR, RX_ANY, RX_CLASS, RX_SPLIT, RX_JMP, RX_SAVE, RX_MATCH, RX_BOL, RX_EOL, RX_WORDB, RX_NWORDB };
typedef struct { int op, x, y; uint32_t c; } dan_rx_inst;
typedef struct { uint32_t lo, hi; } dan_rx_range;
typedef struct { dan_rx_range *r; int n, cap, neg; } dan_rx_class;
#define DAN_RX_MAXCAP 20                  /* groups, including the whole match */
typedef struct {
    dan_rx_inst *code; int n, cap;
    dan_rx_class *cls; int ncls, capcls;
    int ngroups, icase;
} dan_rx;

/* AST of the pattern */
enum { RXN_CHAR, RXN_ANY, RXN_CLASS, RXN_CAT, RXN_ALT, RXN_REP, RXN_GROUP, RXN_BOL, RXN_EOL, RXN_WORDB, RXN_NWORDB, RXN_EMPTY };
typedef struct dan_rxn { int kind, min, max, greedy, idx; uint32_t c; struct dan_rxn *a, *b; } dan_rxn;
typedef struct { const char *p, *start; dan_rx *rx; int line; dan_str pattern; dan_rxn **all; int nall, capall; } dan_rxp;

static void dan_rx_fail(dan_rxp *P, const char *why) {
    char m[600];
    snprintf(m, sizeof m, DAN_MSG_RX_BAD, P->pattern, why);
    dan_error(m, P->line);
}
static dan_rxn *dan_rxn_new(dan_rxp *P, int kind) {
    dan_rxn *n = dan_alloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = kind;
    if (P->nall == P->capall) { P->capall = P->capall ? P->capall * 2 : 32; P->all = dan_realloc(P->all, sizeof *P->all * (size_t)P->capall); }
    P->all[P->nall++] = n;
    return n;
}
static uint32_t dan_utf8_decode(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = p[0];
    int k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (k == 2 && p[1]) c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    else if (k == 3 && p[1] && p[2]) c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else if (k == 4 && p[1] && p[2] && p[3]) c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    else k = 1;
    *pp += k;
    return c;
}
static uint32_t dan_rx_fold(uint32_t c) {           /* lower case for Latin and Cyrillic */
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c == 0x401) return 0x451;
    return c;
}
static int dan_rx_word(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451 || (c >= 0xC0 && c <= 0x24F);
}
static int dan_rx_space(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0xA0; }

static void dan_rx_add_range(dan_rx_class *k, uint32_t lo, uint32_t hi) {
    if (k->n == k->cap) { k->cap = k->cap ? k->cap * 2 : 8; k->r = dan_realloc(k->r, sizeof *k->r * (size_t)k->cap); }
    k->r[k->n].lo = lo; k->r[k->n].hi = hi; k->n++;
}
/* \d \w \s and their opposites, as ranges */
static void dan_rx_add_named(dan_rx_class *k, char e) {
    if (e == 'd') { dan_rx_add_range(k, '0', '9'); return; }
    if (e == 's') { dan_rx_add_range(k, '\t', '\r'); dan_rx_add_range(k, ' ', ' '); dan_rx_add_range(k, 0xA0, 0xA0); return; }
    /* w */
    dan_rx_add_range(k, '0', '9'); dan_rx_add_range(k, 'A', 'Z'); dan_rx_add_range(k, '_', '_');
    dan_rx_add_range(k, 'a', 'z'); dan_rx_add_range(k, 0xC0, 0x24F); dan_rx_add_range(k, 0x401, 0x401);
    dan_rx_add_range(k, 0x410, 0x44F); dan_rx_add_range(k, 0x451, 0x451);
}
static int dan_rx_new_class(dan_rx *rx) {
    if (rx->ncls == rx->capcls) { rx->capcls = rx->capcls ? rx->capcls * 2 : 8; rx->cls = dan_realloc(rx->cls, sizeof *rx->cls * (size_t)rx->capcls); }
    memset(&rx->cls[rx->ncls], 0, sizeof rx->cls[0]);
    return rx->ncls++;
}
static uint32_t dan_rx_escape_char(dan_rxp *P, char e) {
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    default: break;
    }
    if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '0' && e <= '9')) {
        char why[80];
        snprintf(why, sizeof why, DAN_TXT_RX_ESCAPE, e);
        dan_rx_fail(P, why);
    }
    return (uint32_t)(unsigned char)e;
}

static dan_rxn *dan_rx_alt(dan_rxp *P, int depth);

static dan_rxn *dan_rx_class_node(dan_rxp *P) {      /* after «[» */
    dan_rxn *n = dan_rxn_new(P, RXN_CLASS);
    n->idx = dan_rx_new_class(P->rx);
    dan_rx_class *k = &P->rx->cls[n->idx];
    if (*P->p == '^') { k->neg = 1; P->p++; }
    int first = 1;
    while (*P->p && (*P->p != ']' || first)) {
        first = 0;
        uint32_t lo;
        if (*P->p == '\\') {
            char e = P->p[1];
            if (!e) dan_rx_fail(P, DAN_TXT_RX_END);
            P->p += 2;
            if (e == 'd' || e == 'w' || e == 's') { k = &P->rx->cls[n->idx]; dan_rx_add_named(k, e); continue; }
            lo = dan_rx_escape_char(P, e);
        } else {
            lo = dan_utf8_decode(&P->p);
        }
        uint32_t hi = lo;
        if (*P->p == '-' && P->p[1] && P->p[1] != ']') {
            P->p++;
            if (*P->p == '\\') { char e = P->p[1]; if (!e) dan_rx_fail(P, DAN_TXT_RX_END); P->p += 2; hi = dan_rx_escape_char(P, e); }
            else hi = dan_utf8_decode(&P->p);
            if (hi < lo) dan_rx_fail(P, DAN_TXT_RX_RANGE);
        }
        k = &P->rx->cls[n->idx];
        dan_rx_add_range(k, lo, hi);
    }
    if (*P->p != ']') dan_rx_fail(P, DAN_TXT_RX_CLASS);
    P->p++;
    return n;
}

static int dan_rx_number(dan_rxp *P, int *out) {
    int v = 0, any = 0;
    while (*P->p >= '0' && *P->p <= '9') { v = v * 10 + (*P->p++ - '0'); any = 1; if (v > 1000) dan_rx_fail(P, DAN_TXT_RX_COUNT); }
    *out = v;
    return any;
}

static dan_rxn *dan_rx_atom(dan_rxp *P, int depth) {
    char c = *P->p;
    if (c == '(') {
        P->p++;
        int capture = 1;
        if (P->p[0] == '?' && P->p[1] == ':') { capture = 0; P->p += 2; }
        else if (P->p[0] == '?') dan_rx_fail(P, DAN_TXT_RX_LOOK);
        int idx = 0;
        if (capture) {
            if (P->rx->ngroups >= DAN_RX_MAXCAP) dan_rx_fail(P, DAN_TXT_RX_GROUPS);
            idx = P->rx->ngroups++;
        }
        dan_rxn *inner = dan_rx_alt(P, depth + 1);
        if (*P->p != ')') dan_rx_fail(P, DAN_TXT_RX_PAREN);
        P->p++;
        if (!capture) return inner;
        dan_rxn *g = dan_rxn_new(P, RXN_GROUP);
        g->a = inner; g->idx = idx;
        return g;
    }
    if (c == '[') { P->p++; return dan_rx_class_node(P); }
    if (c == '.') { P->p++; return dan_rxn_new(P, RXN_ANY); }
    if (c == '^') { P->p++; return dan_rxn_new(P, RXN_BOL); }
    if (c == '$') { P->p++; return dan_rxn_new(P, RXN_EOL); }
    if (c == '\\') {
        char e = P->p[1];
        if (!e) dan_rx_fail(P, DAN_TXT_RX_END);
        P->p += 2;
        if (e == 'b') return dan_rxn_new(P, RXN_WORDB);
        if (e == 'B') return dan_rxn_new(P, RXN_NWORDB);
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            dan_rxn *n = dan_rxn_new(P, RXN_CLASS);
            n->idx = dan_rx_new_class(P->rx);
            dan_rx_class *k = &P->rx->cls[n->idx];
            dan_rx_add_named(k, (char)(e | 0x20));
            k->neg = e < 'a';
            return n;
        }
        dan_rxn *n = dan_rxn_new(P, RXN_CHAR);
        n->c = dan_rx_escape_char(P, e);
        return n;
    }
    if (c == '*' || c == '+' || c == '?' || c == '{') dan_rx_fail(P, DAN_TXT_RX_NOTHING);
    if (c == ')') dan_rx_fail(P, DAN_TXT_RX_PAREN2);
    dan_rxn *n = dan_rxn_new(P, RXN_CHAR);
    n->c = dan_utf8_decode(&P->p);
    return n;
}

static dan_rxn *dan_rx_repeat(dan_rxp *P, int depth) {
    dan_rxn *a = dan_rx_atom(P, depth);
    for (;;) {
        int min, max;
        char c = *P->p;
        if (c == '*') { min = 0; max = -1; P->p++; }
        else if (c == '+') { min = 1; max = -1; P->p++; }
        else if (c == '?') { min = 0; max = 1; P->p++; }
        else if (c == '{' && P->p[1] >= '0' && P->p[1] <= '9') {
            P->p++;
            dan_rx_number(P, &min);
            max = min;
            if (*P->p == ',') { P->p++; if (!dan_rx_number(P, &max)) max = -1; }
            if (*P->p != '}') dan_rx_fail(P, DAN_TXT_RX_BRACE);
            P->p++;
            if (max >= 0 && max < min) dan_rx_fail(P, DAN_TXT_RX_COUNT);
        } else {
            return a;
        }
        if (a->kind == RXN_BOL || a->kind == RXN_EOL || a->kind == RXN_WORDB || a->kind == RXN_NWORDB) dan_rx_fail(P, DAN_TXT_RX_NOTHING);
        dan_rxn *r = dan_rxn_new(P, RXN_REP);
        r->a = a; r->min = min; r->max = max; r->greedy = 1;
        if (*P->p == '?') { r->greedy = 0; P->p++; }
        a = r;
    }
}

static dan_rxn *dan_rx_cat(dan_rxp *P, int depth) {
    dan_rxn *left = NULL;
    while (*P->p && *P->p != '|' && *P->p != ')') {
        dan_rxn *r = dan_rx_repeat(P, depth);
        if (!left) left = r;
        else { dan_rxn *c = dan_rxn_new(P, RXN_CAT); c->a = left; c->b = r; left = c; }
    }
    return left ? left : dan_rxn_new(P, RXN_EMPTY);
}

static dan_rxn *dan_rx_alt(dan_rxp *P, int depth) {
    if (depth > 200) dan_rx_fail(P, DAN_TXT_RX_DEEP);
    dan_rxn *left = dan_rx_cat(P, depth);
    while (*P->p == '|') {
        P->p++;
        dan_rxn *right = dan_rx_cat(P, depth);
        dan_rxn *a = dan_rxn_new(P, RXN_ALT);
        a->a = left; a->b = right;
        left = a;
    }
    return left;
}

static int dan_rx_emit(dan_rxp *P, int op, int x, int y, uint32_t c) {
    dan_rx *rx = P->rx;
    if (rx->n >= 200000) dan_rx_fail(P, DAN_TXT_RX_BIG);
    if (rx->n == rx->cap) { rx->cap = rx->cap ? rx->cap * 2 : 64; rx->code = dan_realloc(rx->code, sizeof *rx->code * (size_t)rx->cap); }
    rx->code[rx->n].op = op; rx->code[rx->n].x = x; rx->code[rx->n].y = y; rx->code[rx->n].c = c;
    return rx->n++;
}

static void dan_rx_gen(dan_rxp *P, dan_rxn *n) {
    dan_rx *rx = P->rx;
    switch (n->kind) {
    case RXN_EMPTY: break;
    case RXN_CHAR:
        dan_rx_emit(P, RX_CHAR, 0, rx->icase, rx->icase ? dan_rx_fold(n->c) : n->c);
        break;
    case RXN_ANY: dan_rx_emit(P, RX_ANY, 0, 0, 0); break;
    case RXN_CLASS: dan_rx_emit(P, RX_CLASS, n->idx, 0, 0); break;
    case RXN_BOL: dan_rx_emit(P, RX_BOL, 0, 0, 0); break;
    case RXN_EOL: dan_rx_emit(P, RX_EOL, 0, 0, 0); break;
    case RXN_WORDB: dan_rx_emit(P, RX_WORDB, 0, 0, 0); break;
    case RXN_NWORDB: dan_rx_emit(P, RX_NWORDB, 0, 0, 0); break;
    case RXN_CAT: dan_rx_gen(P, n->a); dan_rx_gen(P, n->b); break;
    case RXN_GROUP:
        dan_rx_emit(P, RX_SAVE, 2 * n->idx, 0, 0);
        dan_rx_gen(P, n->a);
        dan_rx_emit(P, RX_SAVE, 2 * n->idx + 1, 0, 0);
        break;
    case RXN_ALT: {
        int s = dan_rx_emit(P, RX_SPLIT, 0, 0, 0);
        rx->code[s].x = rx->n;
        dan_rx_gen(P, n->a);
        int j = dan_rx_emit(P, RX_JMP, 0, 0, 0);
        rx->code[s].y = rx->n;
        dan_rx_gen(P, n->b);
        rx->code[j].x = rx->n;
        break;
    }
    case RXN_REP: {
        for (int i = 0; i < n->min; i++) dan_rx_gen(P, n->a);
        if (n->max < 0) {
            int s = dan_rx_emit(P, RX_SPLIT, 0, 0, 0);
            int body = rx->n;
            dan_rx_gen(P, n->a);
            int j = dan_rx_emit(P, RX_JMP, s, 1, 0);
            if (n->greedy) { rx->code[s].x = body; rx->code[s].y = rx->n; }
            else { rx->code[s].x = rx->n; rx->code[s].y = body; }
            rx->code[j].c = (uint32_t)rx->n;      /* where the loop ends */
        } else {
            int *ends = dan_alloc(sizeof(int) * (size_t)(n->max - n->min + 1));
            int k = 0;
            for (int i = n->min; i < n->max; i++) {
                int s = dan_rx_emit(P, RX_SPLIT, 0, 0, 0);
                ends[k++] = s;
                int body = rx->n;
                dan_rx_gen(P, n->a);
                if (n->greedy) rx->code[s].x = body; else rx->code[s].y = body;
            }
            for (int i = 0; i < k; i++) { if (n->greedy) rx->code[ends[i]].y = rx->n; else rx->code[ends[i]].x = rx->n; }
            free(ends);
        }
        break;
    }
    }
}

static void dan_rx_free(dan_rx *rx) {
    for (int i = 0; i < rx->ncls; i++) free(rx->cls[i].r);
    free(rx->cls);
    free(rx->code);
    free(rx);
}

/* compiled patterns are kept: a pattern in a loop is compiled once */
#define DAN_RX_CACHE 16
static DAN_TLS struct { char *pattern; dan_rx *rx; } dan_rx_cache[DAN_RX_CACHE];
static DAN_TLS int dan_rx_next;

static dan_rx *dan_rx_compile(dan_str pattern, int line) {
    for (int i = 0; i < DAN_RX_CACHE; i++)
        if (dan_rx_cache[i].pattern && strcmp(dan_rx_cache[i].pattern, pattern) == 0) return dan_rx_cache[i].rx;
    dan_rx *rx = dan_alloc(sizeof *rx);
    memset(rx, 0, sizeof *rx);
    dan_rxp P = { pattern, pattern, rx, line, pattern, NULL, 0, 0 };
    if (strncmp(P.p, "(?i)", 4) == 0) { rx->icase = 1; P.p += 4; }
    rx->ngroups = 1;
    dan_rxn *tree = dan_rx_alt(&P, 0);
    if (*P.p == ')') dan_rx_fail(&P, DAN_TXT_RX_PAREN2);
    dan_rx_emit(&P, RX_SAVE, 0, 0, 0);
    dan_rx_gen(&P, tree);
    dan_rx_emit(&P, RX_SAVE, 1, 0, 0);
    dan_rx_emit(&P, RX_MATCH, 0, 0, 0);
    for (int i = 0; i < P.nall; i++) free(P.all[i]);
    free(P.all);
    if (rx->icase)
        for (int i = 0; i < rx->ncls; i++) {        /* classes also match the other case */
            dan_rx_class *k = &rx->cls[i];
            int n = k->n;
            for (int j = 0; j < n; j++) {
                uint32_t lo = k->r[j].lo, hi = k->r[j].hi;
                if (lo <= 'z' && hi >= 'a') dan_rx_add_range(k, (lo > 'a' ? lo : 'a') - 32, (hi < 'z' ? hi : 'z') - 32);
                if (lo <= 'Z' && hi >= 'A') dan_rx_add_range(k, (lo > 'A' ? lo : 'A') + 32, (hi < 'Z' ? hi : 'Z') + 32);
                if (lo <= 0x44F && hi >= 0x430) dan_rx_add_range(k, (lo > 0x430 ? lo : 0x430) - 0x20, (hi < 0x44F ? hi : 0x44F) - 0x20);
                if (lo <= 0x42F && hi >= 0x410) dan_rx_add_range(k, (lo > 0x410 ? lo : 0x410) + 0x20, (hi < 0x42F ? hi : 0x42F) + 0x20);
                if (lo <= 0x451 && hi >= 0x451) dan_rx_add_range(k, 0x401, 0x401);
                if (lo <= 0x401 && hi >= 0x401) dan_rx_add_range(k, 0x451, 0x451);
            }
        }
    int slot = dan_rx_next;
    dan_rx_next = (dan_rx_next + 1) % DAN_RX_CACHE;
    if (dan_rx_cache[slot].pattern) { free(dan_rx_cache[slot].pattern); dan_rx_free(dan_rx_cache[slot].rx); }
    size_t len = strlen(pattern);
    dan_rx_cache[slot].pattern = dan_alloc(len + 1);
    memcpy(dan_rx_cache[slot].pattern, pattern, len + 1);
    dan_rx_cache[slot].rx = rx;
    return rx;
}

static int dan_rx_class_has(const dan_rx_class *k, uint32_t c) {
    int in = 0;
    for (int i = 0; i < k->n && !in; i++) in = c >= k->r[i].lo && c <= k->r[i].hi;
    return in != k->neg;
}

typedef struct { int pc; int caps[2 * DAN_RX_MAXCAP]; } dan_rx_thread;
typedef struct { dan_rx_thread *t; int n; } dan_rx_list;

/* adds a thread and follows the steps that consume no character */
static void dan_rx_add(const dan_rx *rx, dan_rx_list *l, int *mark, int gen, dan_rx_thread *stack,
                       int pc0, const int *caps0, const char *text, const char *at) {
    int sp = 0;
    stack[sp].pc = pc0;
    memcpy(stack[sp].caps, caps0, sizeof stack[sp].caps);
    sp++;
    while (sp > 0) {
        dan_rx_thread th = stack[--sp];
        int pc = th.pc;
        const dan_rx_inst *in = &rx->code[pc];
        /* Steps with one continuation (jumps, saving positions, checks) are
           followed every time: a loop that ends after an empty iteration goes
           through them once more, and the jump at its end decides where to go.
           Only branchings and character steps are visited once per position,
           so the work stays proportional to the size of the pattern. */
        if (in->op == RX_SPLIT || in->op == RX_CHAR || in->op == RX_ANY || in->op == RX_CLASS || in->op == RX_MATCH) {
            if (mark[pc] == gen) continue;
            mark[pc] = gen;
        }
        switch (in->op) {
        case RX_JMP:
            /* an iteration of a loop that consumed nothing ends the loop (as in Python) */
            stack[sp].pc = in->y && mark[in->x] == gen ? (int)in->c : in->x;
            memcpy(stack[sp].caps, th.caps, sizeof th.caps); sp++;
            break;
        case RX_SPLIT:   /* x is preferred: pushed last, taken first */
            stack[sp].pc = in->y; memcpy(stack[sp].caps, th.caps, sizeof th.caps); sp++;
            stack[sp].pc = in->x; memcpy(stack[sp].caps, th.caps, sizeof th.caps); sp++;
            break;
        case RX_SAVE:
            th.caps[in->x] = (int)(at - text);
            th.pc = pc + 1;
            stack[sp++] = th;
            break;
        case RX_BOL:
            if (at == text || at[-1] == '\n') { th.pc = pc + 1; stack[sp++] = th; }
            break;
        case RX_EOL:
            if (!*at || *at == '\n') { th.pc = pc + 1; stack[sp++] = th; }
            break;
        case RX_WORDB: case RX_NWORDB: {
            const char *q = at;
            int before = 0, after = 0;
            if (at > text) {
                const char *b = at - 1;
                while (b > text && ((unsigned char)*b & 0xC0) == 0x80) b--;
                before = dan_rx_word(dan_utf8_decode(&b));
            }
            if (*q) after = dan_rx_word(dan_utf8_decode(&q));
            if ((before != after) == (in->op == RX_WORDB)) { th.pc = pc + 1; stack[sp++] = th; }
            break;
        }
        default:
            l->t[l->n++] = th;
            break;
        }
    }
}

/* the first (leftmost, then by the pattern's preferences) match from position «from» */
/* noempty >= 0: an empty match at that position is not accepted (after an
   empty match the next search starts at the same place, as in Python) */
static int dan_rx_exec(const dan_rx *rx, const char *text, size_t from, int anchored, int full, int noempty, int *caps) {
    int n = rx->n;
    dan_rx_thread *a = dan_alloc(sizeof *a * (size_t)n), *b = dan_alloc(sizeof *b * (size_t)n);
    dan_rx_thread *stack = dan_alloc(sizeof *stack * (size_t)(n + 2));
    int *mark = dan_alloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) mark[i] = -1;
    int gen = 0, found = 0;
    dan_rx_list cl = { a, 0 }, nl = { b, 0 };
    int empty[2 * DAN_RX_MAXCAP];
    for (int i = 0; i < 2 * DAN_RX_MAXCAP; i++) empty[i] = -1;
    const char *at = text + from;
    dan_rx_add(rx, &cl, mark, gen, stack, 0, empty, text, at);
    for (;;) {
        const char *next = at;
        uint32_t c = 0;
        if (*at) c = dan_utf8_decode(&next);
        gen++;
        nl.n = 0;
        for (int i = 0; i < cl.n; i++) {
            dan_rx_thread *th = &cl.t[i];
            const dan_rx_inst *in = &rx->code[th->pc];
            int ok = 0;
            switch (in->op) {
            case RX_MATCH:
                if (full && *at) break;
                if (noempty >= 0 && th->caps[0] == noempty && at - text == noempty) break;
                found = 1;
                memcpy(caps, th->caps, sizeof th->caps);
                i = cl.n;            /* lower-priority threads are dropped */
                continue;
            case RX_CHAR: ok = *at && (in->y ? dan_rx_fold(c) == in->c : c == in->c); break;
            case RX_ANY: ok = *at && c != '\n'; break;
            case RX_CLASS: ok = *at && dan_rx_class_has(&rx->cls[in->x], c); break;
            default: break;
            }
            if (ok) dan_rx_add(rx, &nl, mark, gen, stack, th->pc + 1, th->caps, text, next);
        }
        if (!*at) break;
        at = next;
        if (!found && !anchored) dan_rx_add(rx, &nl, mark, gen, stack, 0, empty, text, at);
        dan_rx_list t = cl; cl = nl; nl = t;
        if (cl.n == 0 && (found || anchored)) break;
    }
    free(a); free(b); free(stack); free(mark);
    return found;
}

static bool dan_rx_full(dan_str text, dan_str pattern, int line) {
    int caps[2 * DAN_RX_MAXCAP];
    return dan_rx_exec(dan_rx_compile(pattern, line), text, 0, 1, 1, -1, caps);
}

static do_s dan_rx_first(dan_str text, dan_str pattern, int line) {
    int caps[2 * DAN_RX_MAXCAP];
    if (!dan_rx_exec(dan_rx_compile(pattern, line), text, 0, 0, 0, -1, caps)) return do_s_none();
    return do_s_some(dan_str_sub(text + caps[0], text + caps[1]));
}

/* the next match for найти_все, заменить_шаблон and разбить_по_шаблону */
static int dan_rx_next_match(const dan_rx *rx, dan_str text, size_t *from, int *noempty, int *caps) {
    if (*from > strlen(text)) return 0;
    if (!dan_rx_exec(rx, text, *from, 0, 0, *noempty, caps)) return 0;
    *from = (size_t)caps[1];
    *noempty = caps[0] == caps[1] ? caps[1] : -1;
    return 1;
}

static dl_Ls *dan_rx_all(dan_str text, dan_str pattern, int line) {
    dan_rx *rx = dan_rx_compile(pattern, line);
    dl_Ls *r = dl_Ls_from(0, NULL);
    int caps[2 * DAN_RX_MAXCAP], noempty = -1;
    size_t from = 0;
    while (dan_rx_next_match(rx, text, &from, &noempty, caps))
        dl_Ls_push(r, dan_str_sub(text + caps[0], text + caps[1]));
    return r;
}

static dl_Ls *dan_rx_groups(dan_str text, dan_str pattern, int line) {
    dan_rx *rx = dan_rx_compile(pattern, line);
    dl_Ls *r = dl_Ls_from(0, NULL);
    int caps[2 * DAN_RX_MAXCAP];
    if (!dan_rx_exec(rx, text, 0, 0, 0, -1, caps)) return r;
    for (int g = 0; g < rx->ngroups; g++) {
        if (caps[2 * g] < 0 || caps[2 * g + 1] < 0) dl_Ls_push(r, dan_empty_s.s);
        else dl_Ls_push(r, dan_str_sub(text + caps[2 * g], text + caps[2 * g + 1]));
    }
    return r;
}

static void dan_buf_addn(dan_buf *b, const char *s, size_t n) {
    char chunk[256];
    while (n > 0) {
        size_t k = n < sizeof chunk - 1 ? n : sizeof chunk - 1;
        memcpy(chunk, s, k);
        chunk[k] = 0;
        dan_buf_add(b, chunk);
        s += k;
        n -= k;
    }
}

/* replacement: $0 - the whole match, $1..$9 - groups, $$ - the sign $ itself */
static dan_str dan_rx_replace(dan_str text, dan_str pattern, dan_str with, int line) {
    dan_rx *rx = dan_rx_compile(pattern, line);
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "");
    int caps[2 * DAN_RX_MAXCAP], noempty = -1;
    size_t from = 0, copied = 0;
    while (dan_rx_next_match(rx, text, &from, &noempty, caps)) {
        dan_buf_addn(&b, text + copied, (size_t)caps[0] - copied);
        for (const char *w = with; *w; w++) {
            if (*w == '$' && w[1] >= '0' && w[1] <= '9') {
                int g = w[1] - '0';
                if (g < rx->ngroups && caps[2 * g] >= 0)
                    dan_buf_addn(&b, text + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]));
                w++;
            } else if (*w == '$' && w[1] == '$') {
                dan_buf_add(&b, "$");
                w++;
            } else {
                const char *q = w;
                while (q[1] && q[1] != '$') q++;
                dan_buf_addn(&b, w, (size_t)(q - w + 1));
                w = q;
            }
        }
        copied = (size_t)caps[1];
    }
    dan_buf_add(&b, text + copied);
    return dan_buf_str(&b);
}

static dl_Ls *dan_rx_split(dan_str text, dan_str pattern, int line) {
    dan_rx *rx = dan_rx_compile(pattern, line);
    dl_Ls *r = dl_Ls_from(0, NULL);
    int caps[2 * DAN_RX_MAXCAP], noempty = -1;
    size_t from = 0, piece = 0;
    while (dan_rx_next_match(rx, text, &from, &noempty, caps)) {
        dl_Ls_push(r, dan_str_sub(text + piece, text + caps[0]));
        piece = (size_t)caps[1];
    }
    dl_Ls_push(r, dan_str_sub(text + piece, text + strlen(text)));
    return r;
}

/* ---- characters and their codes (Unicode code points) ---- */

static dan_str dan_char_from_code(int64_t c, int line) {
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        char m[160];
        snprintf(m, sizeof m, DAN_MSG_BAD_CODE, (long long)c);
        dan_error(m, line);
    }
    char b[5] = { 0 };
    if (c < 0x80) b[0] = (char)c;
    else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); }
    else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); }
    if (c == 0) dan_error(DAN_MSG_ZERO_CODE, line);
    return dan_strdup(b);
}

static int64_t dan_char_code(dan_str s, int line) {
    if (!*s) dan_error(DAN_MSG_EMPTY_CHAR, line);
    const char *p = s;
    return dan_utf8_decode(&p);
}


/* ==== утверждения ==== */

static void dan_assert(bool ok, int line) { if (!ok) dan_error(DAN_MSG_ASSERT, line); }
static void dan_assert_msg(bool ok, dan_str why, int line) { if (!ok) dan_error(why, line); }

static void dan_assert_fail(const char *got, const char *want, int line) {
    size_t n = strlen(got) + strlen(want) + 64;
    char *m = malloc(n);
    if (!m) dan_error(DAN_MSG_ASSERT, line);
    snprintf(m, n, DAN_TXT_ASSERT_EQ, want, got);
    dan_error(m, line);           /* the text is copied by the error: the block is left to the system */
}
static void dan_assert_eq_int(int64_t got, int64_t want, int line) {
    if (got != want) dan_assert_fail(dan_int_to_str(got), dan_int_to_str(want), line);
}
static void dan_assert_eq_float(double got, double want, int line) {
    if (!(got == want || (got != got && want != want)))
        dan_assert_fail(dan_float_to_str(got), dan_float_to_str(want), line);
}
static void dan_assert_eq_str(dan_str got, dan_str want, int line) {
    if (strcmp(got, want) != 0) {
        size_t a = strlen(got) + 3, b = strlen(want) + 3;
        char *g = malloc(a + 4), *w = malloc(b + 4);
        if (!g || !w) dan_error(DAN_MSG_ASSERT, line);
        snprintf(g, a + 4, "\"%s\"", got);
        snprintf(w, b + 4, "\"%s\"", want);
        dan_assert_fail(g, w, line);
    }
}
static void dan_assert_eq_bool(bool got, bool want, int line) {
    if (got != want) dan_assert_fail(got ? DAN_TRUE : DAN_FALSE, want ? DAN_TRUE : DAN_FALSE, line);
}
