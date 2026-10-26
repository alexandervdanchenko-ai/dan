/* ==== Database library of the Dan language: SQLite. ====

   The SQLite library of the system is loaded when the first database is
   opened: libsqlite3 on Linux and macOS, winsqlite3.dll (part of Windows 10
   and newer) or sqlite3.dll on Windows. A program that does not use a
   database does not depend on it.

   Values cross the border between Dan and C as byte strings with a type
   letter in front: «n» - NULL, «i» - integer (decimal text), «f» - real
   number (text), «t» - text, «b» - bytes. The module библиотека/sqlite.dan
   turns them into values of the type sqlite.Значение. */

#if !defined(_WIN32) && !defined(__wasi__)
#include <dlfcn.h>
#endif

typedef struct dan_sqlite3 dan_sqlite3;
typedef struct dan_sqlite3_stmt dan_sqlite3_stmt;

static struct {
    int state;
    int (*open_v2)(const char *, dan_sqlite3 **, int, const char *);
    int (*close_v2)(dan_sqlite3 *);
    int (*prepare_v2)(dan_sqlite3 *, const char *, int, dan_sqlite3_stmt **, const char **);
    int (*step)(dan_sqlite3_stmt *);
    int (*finalize)(dan_sqlite3_stmt *);
    int (*bind_null)(dan_sqlite3_stmt *, int);
    int (*bind_int64)(dan_sqlite3_stmt *, int, int64_t);
    int (*bind_double)(dan_sqlite3_stmt *, int, double);
    int (*bind_text)(dan_sqlite3_stmt *, int, const char *, int, void (*)(void *));
    int (*bind_blob)(dan_sqlite3_stmt *, int, const void *, int, void (*)(void *));
    int (*bind_parameter_count)(dan_sqlite3_stmt *);
    int (*column_count)(dan_sqlite3_stmt *);
    const char *(*column_name)(dan_sqlite3_stmt *, int);
    int (*column_type)(dan_sqlite3_stmt *, int);
    int64_t (*column_int64)(dan_sqlite3_stmt *, int);
    double (*column_double)(dan_sqlite3_stmt *, int);
    const unsigned char *(*column_text)(dan_sqlite3_stmt *, int);
    const void *(*column_blob)(dan_sqlite3_stmt *, int);
    int (*column_bytes)(dan_sqlite3_stmt *, int);
    const char *(*errmsg)(dan_sqlite3 *);
    int (*changes)(dan_sqlite3 *);
    int64_t (*last_insert_rowid)(dan_sqlite3 *);
    int (*busy_timeout)(dan_sqlite3 *, int);
} dan_sq;

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
DAN_LIB_LOCK(dan_sq_lock);
#define DAN_SQ_LOCK() DAN_LIB_ACQUIRE(dan_sq_lock)
#define DAN_SQ_UNLOCK() DAN_LIB_RELEASE(dan_sq_lock)

static void *dan_sq_sym(void *lib, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)lib, name);
#elif defined(__wasi__)
    (void)lib; (void)name;
    return NULL;
#else
    return dlsym(lib, name);
#endif
}

static int dan_sq_load(void) {
    DAN_SQ_LOCK();
    if (dan_sq.state == 0) {
        void *lib = NULL;
#if defined(_WIN32)
        lib = (void *)LoadLibraryW(L"winsqlite3.dll");
        if (!lib) lib = (void *)LoadLibraryW(L"sqlite3.dll");
#elif defined(__wasi__)
        lib = NULL;                       /* WebAssembly cannot load libraries */
#else
        static const char *const names[] = {
#ifdef __APPLE__
            "/usr/lib/libsqlite3.dylib", "libsqlite3.dylib",
#endif
            "libsqlite3.so.0", "libsqlite3.so" };
        for (size_t i = 0; !lib && i < sizeof names / sizeof names[0]; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
#endif
        dan_sq.state = -1;
        if (lib) {
            void **f = (void **)&dan_sq.open_v2;
            static const char *const fn[] = {
                "sqlite3_open_v2", "sqlite3_close_v2", "sqlite3_prepare_v2", "sqlite3_step", "sqlite3_finalize",
                "sqlite3_bind_null", "sqlite3_bind_int64", "sqlite3_bind_double", "sqlite3_bind_text",
                "sqlite3_bind_blob", "sqlite3_bind_parameter_count", "sqlite3_column_count", "sqlite3_column_name",
                "sqlite3_column_type", "sqlite3_column_int64", "sqlite3_column_double", "sqlite3_column_text",
                "sqlite3_column_blob", "sqlite3_column_bytes", "sqlite3_errmsg", "sqlite3_changes",
                "sqlite3_last_insert_rowid", "sqlite3_busy_timeout" };
            int all = 1;
            for (size_t i = 0; i < sizeof fn / sizeof fn[0]; i++) {
                f[i] = dan_sq_sym(lib, fn[i]);
                if (!f[i]) all = 0;
            }
            if (all) dan_sq.state = 1;
        }
    }
    int ok = dan_sq.state == 1;
    DAN_SQ_UNLOCK();
    return ok;
}

/* open databases: a Dan program refers to them by number */
static dan_sqlite3 **dan_sq_dbs;
static int64_t dan_sq_count;

static dan_sqlite3 *dan_sq_get(int64_t n, int line) {
    DAN_SQ_LOCK();
    dan_sqlite3 *db = n >= 1 && n <= dan_sq_count ? dan_sq_dbs[n - 1] : NULL;
    DAN_SQ_UNLOCK();
    if (!db) dan_error(DAN_MSG_DB_CLOSED, line);
    return db;
}

static dan_str dan_sq_message(const char *what, const char *detail) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, what);
    if (detail && *detail) {
        dan_buf_add(&b, " (");
        dan_buf_add(&b, detail);
        dan_buf_add(&b, ")");
    }
    return dan_buf_str(&b);
}

static const char *dan_sq_why(int code) {
    switch (code & 0xFF) {
    case 5: case 6: return DAN_TXT_DB_BUSY;          /* SQLITE_BUSY, SQLITE_LOCKED */
    case 8: return DAN_TXT_DB_READONLY;              /* SQLITE_READONLY */
    case 10: return DAN_TXT_DB_IO;                   /* SQLITE_IOERR */
    case 11: case 26: return DAN_TXT_DB_CORRUPT;     /* SQLITE_CORRUPT, SQLITE_NOTADB */
    case 13: return DAN_TXT_DB_FULL;                 /* SQLITE_FULL */
    case 14: return DAN_TXT_DB_CANTOPEN;             /* SQLITE_CANTOPEN */
    case 19: return DAN_TXT_DB_CONSTRAINT;           /* SQLITE_CONSTRAINT */
    case 20: return DAN_TXT_DB_MISMATCH;             /* SQLITE_MISMATCH */
    case 25: return DAN_TXT_DB_RANGE;                /* SQLITE_RANGE */
    default: return DAN_TXT_DB_ERROR;
    }
}

static dr_i dan_db_open(dan_str path, int line) {
    if (!dan_sq_load()) return dr_i_err(dan_strdup(DAN_TXT_DB_NOLIB), line);
    dan_sqlite3 *db = NULL;
    int rc = dan_sq.open_v2(path, &db, 2 | 4 /* SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE */, NULL);
    if (rc != 0) {
        dan_str m = dan_sq_message(dan_sq_why(rc), db ? dan_sq.errmsg(db) : NULL);
        if (db) dan_sq.close_v2(db);
        dan_buf b = { NULL, 0, 0 };
        dan_buf_add(&b, path);
        dan_buf_add(&b, ": ");
        dan_buf_add(&b, m);
        return dr_i_err(dan_buf_str(&b), line);
    }
    dan_sq.busy_timeout(db, 5000);
    DAN_SQ_LOCK();
    dan_sq_dbs = dan_realloc(dan_sq_dbs, sizeof *dan_sq_dbs * (size_t)(dan_sq_count + 1));
    dan_sq_dbs[dan_sq_count++] = db;
    int64_t n = dan_sq_count;
    DAN_SQ_UNLOCK();
    return dr_i_some(n);
}

static void dan_db_close(int64_t n, int line) {
    dan_sqlite3 *db = dan_sq_get(n, line);
    DAN_SQ_LOCK();
    dan_sq_dbs[n - 1] = NULL;
    DAN_SQ_UNLOCK();
    dan_sq.close_v2(db);
}

static dan_bytes dan_sq_value(char tag, const void *p, size_t n) {
    dan_bytes_s *b = dan_bytes_alloc((int64_t)n + 1);
    b->data[0] = (uint8_t)tag;
    if (n) memcpy(b->data + 1, p, n);
    return b;
}

static dr_Ly dan_db_exec(int64_t n, dan_str sql, dl_Ly *params, int line) {
    dan_sqlite3 *db = dan_sq_get(n, line);
    dl_Ly *out = dl_Ly_from(0, NULL);
    dl_Ly *cols = dl_Ly_from(0, NULL), *rows = dl_Ly_from(0, NULL);
    int64_t ncols = 0;
    const char *tail = sql;
    int first = 1;
    while (tail && *tail) {
        dan_sqlite3_stmt *st = NULL;
        const char *next = NULL;
        int rc = dan_sq.prepare_v2(db, tail, -1, &st, &next);
        if (rc != 0) return dr_Ly_err(dan_sq_message(dan_sq_why(rc), dan_sq.errmsg(db)), line);
        tail = next;
        if (!st) continue;                         /* only spaces or a comment */
        int want = dan_sq.bind_parameter_count(st);
        if (first) {
            if (want != params->len) {
                dan_sq.finalize(st);
                char m[200];
                snprintf(m, sizeof m, DAN_TXT_DB_PARAMS, want, (long long)params->len);
                return dr_Ly_err(dan_strdup(m), line);
            }
            for (int i = 0; i < want; i++) {
                dan_bytes v = params->items[i];
                const char *d = (const char *)v->data + 1;
                int len = (int)(v->len - 1);
                switch (v->len ? v->data[0] : 'n') {
                case 'i': dan_sq.bind_int64(st, i + 1, (int64_t)strtoll(d, NULL, 10)); break;
                case 'f': dan_sq.bind_double(st, i + 1, strtod(d, NULL)); break;
                case 't': dan_sq.bind_text(st, i + 1, d, len, (void (*)(void *))(intptr_t)-1); break;
                case 'b': dan_sq.bind_blob(st, i + 1, d, len, (void (*)(void *))(intptr_t)-1); break;
                default: dan_sq.bind_null(st, i + 1); break;
                }
            }
        } else if (want > 0) {
            dan_sq.finalize(st);
            return dr_Ly_err(dan_strdup(DAN_TXT_DB_PARAMS_LATER), line);
        }
        first = 0;
        int c = dan_sq.column_count(st);
        if (c > 0) {                               /* the rows of the last query that returns rows */
            ncols = c;
            cols = dl_Ly_from(0, NULL);           /* the earlier lists are temporary values and are freed later */
            rows = dl_Ly_from(0, NULL);
            for (int i = 0; i < c; i++) {
                const char *name = dan_sq.column_name(st, i);
                dl_Ly_push(cols, dan_sq_value('t', name, strlen(name)));
            }
        }
        for (;;) {
            rc = dan_sq.step(st);
            if (rc == 101) break;                  /* SQLITE_DONE */
            if (rc != 100) {                       /* not SQLITE_ROW */
                dan_str m = dan_sq_message(dan_sq_why(rc), dan_sq.errmsg(db));
                dan_sq.finalize(st);
                return dr_Ly_err(m, line);
            }
            for (int i = 0; i < c; i++) {
                char num[40];
                switch (dan_sq.column_type(st, i)) {
                case 1:
                    snprintf(num, sizeof num, "%lld", (long long)dan_sq.column_int64(st, i));
                    dl_Ly_push(rows, dan_sq_value('i', num, strlen(num)));
                    break;
                case 2:
                    snprintf(num, sizeof num, "%.17g", dan_sq.column_double(st, i));
                    dl_Ly_push(rows, dan_sq_value('f', num, strlen(num)));
                    break;
                case 3: {
                    const unsigned char *t = dan_sq.column_text(st, i);
                    dl_Ly_push(rows, dan_sq_value('t', t, (size_t)dan_sq.column_bytes(st, i)));
                    break;
                }
                case 4: {
                    const void *p = dan_sq.column_blob(st, i);
                    dl_Ly_push(rows, dan_sq_value('b', p, (size_t)dan_sq.column_bytes(st, i)));
                    break;
                }
                default:
                    dl_Ly_push(rows, dan_sq_value('n', NULL, 0));
                }
            }
        }
        dan_sq.finalize(st);
    }
    char head[80];
    snprintf(head, sizeof head, "%lld %d %lld", (long long)ncols, dan_sq.changes(db), (long long)dan_sq.last_insert_rowid(db));
    dl_Ly_push(out, dan_sq_value('t', head, strlen(head)));
    for (int64_t i = 0; i < cols->len; i++) dl_Ly_push(out, cols->items[i]);
    for (int64_t i = 0; i < rows->len; i++) dl_Ly_push(out, rows->items[i]);
    return dr_Ly_some(out);
}
