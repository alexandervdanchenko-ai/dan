/* ==== Network library of the Dan language: HTTP and HTTPS requests. ====

   Linux and macOS use the libcurl library of the system, Windows uses
   WinHTTP, which is part of every Windows. Both are loaded when the first
   request is made (dlopen / LoadLibrary), so a program that does not use
   the network does not depend on them, and a program that does starts
   even where the library is missing: the request then returns an error
   with an explanation. Proxy settings of the system are respected.

   The result of сетевой_запрос is a list of two byte strings: the first
   holds the status code and the response headers as text («200», then
   lines «name: value» with lower-case names), the second - the body. */

#if defined(_WIN32)
#include <winhttp.h>
#elif !defined(__wasi__)
#include <dlfcn.h>
#endif

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

typedef struct { uint8_t *data; size_t len, cap; } dan_netbuf;

static void dan_netbuf_add(dan_netbuf *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 256;
        b->data = dan_realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

static dan_bytes dan_netbuf_bytes(dan_netbuf *b) {
    dan_bytes_s *r = dan_bytes_alloc((int64_t)b->len);
    if (b->len) memcpy(r->data, b->data, b->len);
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    return r;
}

/* «Content-Type: text/html\r\n» -> «content-type: text/html\n» */
static void dan_net_header_line(dan_netbuf *h, const char *line, size_t n) {
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
    const char *colon = memchr(line, ':', n);
    if (!colon || colon == line) return;
    for (const char *p = line; p < colon; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        dan_netbuf_add(h, &c, 1);
    }
    dan_netbuf_add(h, ": ", 2);
    const char *v = colon + 1;
    while (v < line + n && (*v == ' ' || *v == '\t')) v++;
    dan_netbuf_add(h, v, (size_t)(line + n - v));
    dan_netbuf_add(h, "\n", 1);
}

static dr_Ly dan_net_fail(dan_str url, const char *why, int line) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, url);
    dan_buf_add(&b, ": ");
    dan_buf_add(&b, why);
    return dr_Ly_err(dan_buf_str(&b), line);
}

static dr_Ly dan_net_result(int64_t status, dan_netbuf *headers, dan_netbuf *body) {
    char code[32];
    snprintf(code, sizeof code, "%lld\n", (long long)status);
    dan_netbuf all = { NULL, 0, 0 };
    dan_netbuf_add(&all, code, strlen(code));
    if (headers->len) dan_netbuf_add(&all, headers->data, headers->len);
    free(headers->data);
    dl_Ly *r = dl_Ly_from(0, NULL);
    dl_Ly_push(r, dan_netbuf_bytes(&all));
    dl_Ly_push(r, dan_netbuf_bytes(body));
    return dr_Ly_some(r);
}

/* only http:// and https://: other protocols of the libraries (file://, ftp://)
   would let an address from outside read local files */
static int dan_net_scheme_ok(dan_str url) {
    const char *p = url;
    const char *h = "http";
    for (int i = 0; i < 4; i++, p++) if ((*p | 0x20) != h[i]) return 0;
    if ((*p | 0x20) == 's') p++;
    return strncmp(p, "://", 3) == 0;
}

static int dan_net_has_body(dan_str method, dan_bytes body) {
    return body->len > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0;
}

#if defined(__wasi__)

/* WebAssembly: the program has no network of its own */
static dr_Ly dan_http(dan_str method, dan_str url, dl_Ls *headers, dan_bytes body, int64_t seconds, int line) {
    (void)method; (void)headers; (void)body; (void)seconds;
    return dan_net_fail(url, DAN_TXT_NET_WASM, line);
}

#elif !defined(_WIN32)

/* ---- libcurl ---- */

#define DAN_CURLOPT_WRITEDATA 10001
#define DAN_CURLOPT_URL 10002
#define DAN_CURLOPT_ERRORBUFFER 10010
#define DAN_CURLOPT_POSTFIELDS 10015
#define DAN_CURLOPT_USERAGENT 10018
#define DAN_CURLOPT_HTTPHEADER 10023
#define DAN_CURLOPT_HEADERDATA 10029
#define DAN_CURLOPT_CUSTOMREQUEST 10036
#define DAN_CURLOPT_ACCEPT_ENCODING 10102
#define DAN_CURLOPT_WRITEFUNCTION 20011
#define DAN_CURLOPT_HEADERFUNCTION 20079
#define DAN_CURLOPT_POSTFIELDSIZE_LARGE 30120
#define DAN_CURLOPT_TIMEOUT 13
#define DAN_CURLOPT_NOBODY 44
#define DAN_CURLOPT_FOLLOWLOCATION 52
#define DAN_CURLOPT_MAXREDIRS 68
#define DAN_CURLOPT_CONNECTTIMEOUT 78
#define DAN_CURLOPT_PROTOCOLS 181
#define DAN_CURLOPT_REDIR_PROTOCOLS 182
#define DAN_CURLOPT_NOSIGNAL 99
#define DAN_CURLINFO_RESPONSE_CODE 0x200002
#define DAN_CURL_GLOBAL_DEFAULT 3

typedef struct dan_curl_slist dan_curl_slist;
static struct {
    int state;                 /* 0 - not loaded, 1 - loaded, -1 - not available */
    void *(*easy_init)(void);
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void (*easy_cleanup)(void *);
    dan_curl_slist *(*slist_append)(dan_curl_slist *, const char *);
    void (*slist_free_all)(dan_curl_slist *);
    int (*global_init)(long);
} dan_curl;
DAN_LIB_LOCK(dan_curl_lock);

static int dan_curl_load(void) {
    DAN_LIB_ACQUIRE(dan_curl_lock);
    if (dan_curl.state == 0) {
        static const char *const names[] = {
#ifdef __APPLE__
            "libcurl.4.dylib", "libcurl.dylib",
#endif
            "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "libcurl-nss.so.4" };
        void *lib = NULL;
        for (size_t i = 0; !lib && i < sizeof names / sizeof names[0]; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        dan_curl.state = -1;
        if (lib) {
            *(void **)&dan_curl.easy_init = dlsym(lib, "curl_easy_init");
            *(void **)&dan_curl.easy_setopt = dlsym(lib, "curl_easy_setopt");
            *(void **)&dan_curl.easy_perform = dlsym(lib, "curl_easy_perform");
            *(void **)&dan_curl.easy_getinfo = dlsym(lib, "curl_easy_getinfo");
            *(void **)&dan_curl.easy_cleanup = dlsym(lib, "curl_easy_cleanup");
            *(void **)&dan_curl.slist_append = dlsym(lib, "curl_slist_append");
            *(void **)&dan_curl.slist_free_all = dlsym(lib, "curl_slist_free_all");
            *(void **)&dan_curl.global_init = dlsym(lib, "curl_global_init");
            if (dan_curl.easy_init && dan_curl.easy_setopt && dan_curl.easy_perform && dan_curl.easy_getinfo
                && dan_curl.easy_cleanup && dan_curl.slist_append && dan_curl.slist_free_all && dan_curl.global_init
                && dan_curl.global_init(DAN_CURL_GLOBAL_DEFAULT) == 0)
                dan_curl.state = 1;
        }
    }
    int ok = dan_curl.state == 1;
    DAN_LIB_RELEASE(dan_curl_lock);
    return ok;
}

static size_t dan_curl_body(char *p, size_t size, size_t n, void *data) {
    dan_netbuf_add((dan_netbuf *)data, p, size * n);
    return size * n;
}

static size_t dan_curl_header(char *p, size_t size, size_t n, void *data) {
    dan_netbuf *h = data;
    size_t len = size * n;
    if (len >= 5 && memcmp(p, "HTTP/", 5) == 0) h->len = 0;   /* a new response after a redirect */
    else dan_net_header_line(h, p, len);
    return len;
}

static const char *dan_curl_why(int code) {
    switch (code) {
    case 1: return DAN_TXT_NET_PROTOCOL;
    case 3: return DAN_TXT_NET_URL;
    case 5: case 6: return DAN_TXT_NET_HOST;
    case 7: return DAN_TXT_NET_CONNECT;
    case 28: return DAN_TXT_NET_TIMEOUT;
    case 35: case 51: case 53: case 54: case 58: case 59: case 60: case 77: case 80: case 83: case 90: case 91:
        return DAN_TXT_NET_TLS;
    case 47: return DAN_TXT_NET_REDIRECTS;
    case 52: return DAN_TXT_NET_EMPTY;
    case 55: case 56: return DAN_TXT_NET_BROKEN;
    case 27: return DAN_TXT_NET_MEMORY;
    default: return NULL;
    }
}

static dr_Ly dan_http(dan_str method, dan_str url, dl_Ls *headers, dan_bytes body, int64_t seconds, int line) {
    if (!dan_net_scheme_ok(url)) return dan_net_fail(url, DAN_TXT_NET_PROTOCOL, line);
    if (!dan_curl_load()) return dan_net_fail(url, DAN_TXT_NET_NOCURL, line);
    void *c = dan_curl.easy_init();
    if (!c) return dan_net_fail(url, DAN_TXT_NET_MEMORY, line);
    dan_netbuf h = { NULL, 0, 0 }, b = { NULL, 0, 0 };
    dan_curl_slist *list = NULL;
    for (int64_t i = 0; i < headers->len; i++) list = dan_curl.slist_append(list, headers->items[i]);
    char err[256] = "";
    dan_curl.easy_setopt(c, DAN_CURLOPT_URL, url);
    dan_curl.easy_setopt(c, DAN_CURLOPT_ERRORBUFFER, err);
    dan_curl.easy_setopt(c, DAN_CURLOPT_NOSIGNAL, 1L);
    dan_curl.easy_setopt(c, DAN_CURLOPT_FOLLOWLOCATION, 1L);
    dan_curl.easy_setopt(c, DAN_CURLOPT_PROTOCOLS, 3L);          /* CURLPROTO_HTTP | CURLPROTO_HTTPS */
    dan_curl.easy_setopt(c, DAN_CURLOPT_REDIR_PROTOCOLS, 3L);
    dan_curl.easy_setopt(c, DAN_CURLOPT_MAXREDIRS, 20L);
    dan_curl.easy_setopt(c, DAN_CURLOPT_CONNECTTIMEOUT, 30L);
    if (seconds > 0) dan_curl.easy_setopt(c, DAN_CURLOPT_TIMEOUT, (long)seconds);
    dan_curl.easy_setopt(c, DAN_CURLOPT_USERAGENT, "Dan/0.19");
    dan_curl.easy_setopt(c, DAN_CURLOPT_ACCEPT_ENCODING, "");
    dan_curl.easy_setopt(c, DAN_CURLOPT_WRITEFUNCTION, dan_curl_body);
    dan_curl.easy_setopt(c, DAN_CURLOPT_WRITEDATA, (void *)&b);
    dan_curl.easy_setopt(c, DAN_CURLOPT_HEADERFUNCTION, dan_curl_header);
    dan_curl.easy_setopt(c, DAN_CURLOPT_HEADERDATA, (void *)&h);
    if (list) dan_curl.easy_setopt(c, DAN_CURLOPT_HTTPHEADER, list);
    if (strcmp(method, "HEAD") == 0) dan_curl.easy_setopt(c, DAN_CURLOPT_NOBODY, 1L);
    else if (strcmp(method, "GET") != 0) dan_curl.easy_setopt(c, DAN_CURLOPT_CUSTOMREQUEST, method);
    if (dan_net_has_body(method, body)) {
        dan_curl.easy_setopt(c, DAN_CURLOPT_POSTFIELDSIZE_LARGE, (int64_t)body->len);
        dan_curl.easy_setopt(c, DAN_CURLOPT_POSTFIELDS, (const char *)body->data);
        if (strcmp(method, "POST") == 0) dan_curl.easy_setopt(c, DAN_CURLOPT_CUSTOMREQUEST, (char *)NULL);
    }
    int code = dan_curl.easy_perform(c);
    long status = 0;
    if (code == 0) dan_curl.easy_getinfo(c, DAN_CURLINFO_RESPONSE_CODE, &status);
    dan_curl.easy_cleanup(c);
    if (list) dan_curl.slist_free_all(list);
    if (code != 0) {
        free(h.data);
        free(b.data);
        const char *why = dan_curl_why(code);
        char m[400];
        if (why) snprintf(m, sizeof m, "%s", why);
        else snprintf(m, sizeof m, DAN_TXT_NET_OTHER, code, err);
        return dan_net_fail(url, m, line);
    }
    return dan_net_result(status, &h, &b);
}

#else

/* ---- WinHTTP ---- */

static struct {
    int state;
    __typeof__(WinHttpOpen) *Open;
    __typeof__(WinHttpCrackUrl) *CrackUrl;
    __typeof__(WinHttpConnect) *Connect;
    __typeof__(WinHttpOpenRequest) *OpenRequest;
    __typeof__(WinHttpSetTimeouts) *SetTimeouts;
    __typeof__(WinHttpAddRequestHeaders) *AddRequestHeaders;
    __typeof__(WinHttpSendRequest) *SendRequest;
    __typeof__(WinHttpReceiveResponse) *ReceiveResponse;
    __typeof__(WinHttpQueryHeaders) *QueryHeaders;
    __typeof__(WinHttpQueryDataAvailable) *QueryDataAvailable;
    __typeof__(WinHttpReadData) *ReadData;
    __typeof__(WinHttpCloseHandle) *CloseHandle;
    __typeof__(WinHttpSetOption) *SetOption;
} dan_winhttp;
DAN_LIB_LOCK(dan_winhttp_lock);

static int dan_winhttp_load(void) {
    DAN_LIB_ACQUIRE(dan_winhttp_lock);
    if (dan_winhttp.state == 0) {
        HMODULE lib = LoadLibraryW(L"winhttp.dll");
        dan_winhttp.state = -1;
        if (lib) {
#define DAN_WH(name) *(FARPROC *)&dan_winhttp.name = GetProcAddress(lib, "WinHttp" #name)
            DAN_WH(Open); DAN_WH(CrackUrl); DAN_WH(Connect); DAN_WH(OpenRequest); DAN_WH(SetTimeouts);
            DAN_WH(AddRequestHeaders); DAN_WH(SendRequest); DAN_WH(ReceiveResponse); DAN_WH(QueryHeaders);
            DAN_WH(QueryDataAvailable); DAN_WH(ReadData); DAN_WH(CloseHandle); DAN_WH(SetOption);
#undef DAN_WH
            if (dan_winhttp.Open && dan_winhttp.CrackUrl && dan_winhttp.Connect && dan_winhttp.OpenRequest
                && dan_winhttp.SetTimeouts && dan_winhttp.AddRequestHeaders && dan_winhttp.SendRequest
                && dan_winhttp.ReceiveResponse && dan_winhttp.QueryHeaders && dan_winhttp.QueryDataAvailable
                && dan_winhttp.ReadData && dan_winhttp.CloseHandle && dan_winhttp.SetOption)
                dan_winhttp.state = 1;
        }
    }
    int ok = dan_winhttp.state == 1;
    DAN_LIB_RELEASE(dan_winhttp_lock);
    return ok;
}

static const char *dan_winhttp_why(DWORD e) {
    switch (e) {
    case 12006: return DAN_TXT_NET_PROTOCOL;          /* ERROR_WINHTTP_UNRECOGNIZED_SCHEME */
    case 12005: return DAN_TXT_NET_URL;               /* ERROR_WINHTTP_INVALID_URL */
    case 12007: return DAN_TXT_NET_HOST;              /* ERROR_WINHTTP_NAME_NOT_RESOLVED */
    case 12029: return DAN_TXT_NET_CONNECT;           /* ERROR_WINHTTP_CANNOT_CONNECT */
    case 12002: return DAN_TXT_NET_TIMEOUT;           /* ERROR_WINHTTP_TIMEOUT */
    case 12175: case 12045: case 12169: case 12037: case 12038: case 12057:
        return DAN_TXT_NET_TLS;                       /* certificate and secure channel errors */
    case 12156: return DAN_TXT_NET_REDIRECTS;         /* ERROR_WINHTTP_REDIRECT_FAILED */
    case 12152: return DAN_TXT_NET_EMPTY;             /* ERROR_WINHTTP_INVALID_SERVER_RESPONSE */
    case 12030: return DAN_TXT_NET_BROKEN;            /* ERROR_WINHTTP_CONNECTION_ERROR */
    default: return NULL;
    }
}

static dr_Ly dan_winhttp_error(dan_str url, HINTERNET s, HINTERNET c, HINTERNET r, int line) {
    DWORD e = GetLastError();
    if (r) dan_winhttp.CloseHandle(r);
    if (c) dan_winhttp.CloseHandle(c);
    if (s) dan_winhttp.CloseHandle(s);
    const char *why = dan_winhttp_why(e);
    char m[200];
    if (why) snprintf(m, sizeof m, "%s", why);
    else snprintf(m, sizeof m, DAN_TXT_NET_OTHER_WIN, (unsigned long)e);
    return dan_net_fail(url, m, line);
}

static dr_Ly dan_http(dan_str method, dan_str url, dl_Ls *headers, dan_bytes body, int64_t seconds, int line) {
    if (!dan_net_scheme_ok(url)) return dan_net_fail(url, DAN_TXT_NET_PROTOCOL, line);
    if (!dan_winhttp_load()) return dan_net_fail(url, DAN_TXT_NET_NOWINHTTP, line);
    wchar_t *wurl = dan_wide(url), *wmethod = dan_wide(method);
    URL_COMPONENTS u;
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    static DAN_TLS wchar_t host[512], path[16384], extra[8192];
    u.lpszHostName = host; u.dwHostNameLength = 512;
    u.lpszUrlPath = path; u.dwUrlPathLength = 8192;
    u.lpszExtraInfo = extra; u.dwExtraInfoLength = 8192;
    if (!dan_winhttp.CrackUrl(wurl, 0, 0, &u)) { free(wurl); free(wmethod); return dan_winhttp_error(url, NULL, NULL, NULL, line); }
    wcscat(path, extra);     /* the path and the query «?а=1»; each part is shorter than 8192 */
    HINTERNET s = dan_winhttp.Open(L"Dan/0.19", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, NULL, NULL, 0);
    if (!s) s = dan_winhttp.Open(L"Dan/0.19", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) { free(wurl); free(wmethod); return dan_winhttp_error(url, NULL, NULL, NULL, line); }
    int ms = seconds > 0 && seconds < 2000000 ? (int)seconds * 1000 : 0;
    dan_winhttp.SetTimeouts(s, 0, 30000, ms ? ms : 30000, ms ? ms : 0);
    HINTERNET c = dan_winhttp.Connect(s, host, u.nPort, 0);
    if (!c) { free(wurl); free(wmethod); return dan_winhttp_error(url, s, NULL, NULL, line); }
    HINTERNET r = dan_winhttp.OpenRequest(c, wmethod, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    free(wurl);
    free(wmethod);
    if (!r) return dan_winhttp_error(url, s, c, NULL, line);
    dan_winhttp.SetTimeouts(r, 0, 30000, ms ? ms : 30000, ms ? ms : 0);
    DWORD decompress = 3;   /* WINHTTP_DECOMPRESSION_FLAG_ALL (Windows 8.1 and newer) */
    dan_winhttp.SetOption(r, 118 /* WINHTTP_OPTION_DECOMPRESSION */, &decompress, sizeof decompress);
    for (int64_t i = 0; i < headers->len; i++) {
        wchar_t *wh = dan_wide(headers->items[i]);
        dan_winhttp.AddRequestHeaders(r, wh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        free(wh);
    }
    DWORD n = dan_net_has_body(method, body) ? (DWORD)body->len : 0;
    if (!dan_winhttp.SendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, n ? (LPVOID)body->data : WINHTTP_NO_REQUEST_DATA, n, n, 0)
        || !dan_winhttp.ReceiveResponse(r, NULL))
        return dan_winhttp_error(url, s, c, r, line);
    DWORD status = 0, size = sizeof status;
    dan_winhttp.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX);
    dan_netbuf h = { NULL, 0, 0 }, b = { NULL, 0, 0 };
    size = 0;
    dan_winhttp.QueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    if (size) {
        wchar_t *raw = dan_alloc(size + sizeof(wchar_t));
        if (dan_winhttp.QueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw, &size, WINHTTP_NO_HEADER_INDEX)) {
            raw[size / sizeof(wchar_t)] = 0;
            dan_str text = dan_narrow(raw);
            const char *p = strchr(text, '\n');          /* the first line is the status line */
            while (p && *++p) {
                const char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                dan_net_header_line(&h, p, len);
                p = e;
            }
        }
        free(raw);
    }
    for (;;) {
        DWORD avail = 0;
        if (!dan_winhttp.QueryDataAvailable(r, &avail)) { free(h.data); free(b.data); return dan_winhttp_error(url, s, c, r, line); }
        if (avail == 0) break;
        uint8_t *chunk = dan_alloc(avail);
        DWORD got = 0;
        if (!dan_winhttp.ReadData(r, chunk, avail, &got)) { free(chunk); free(h.data); free(b.data); return dan_winhttp_error(url, s, c, r, line); }
        dan_netbuf_add(&b, chunk, got);
        free(chunk);
        if (got == 0) break;
    }
    dan_winhttp.CloseHandle(r);
    dan_winhttp.CloseHandle(c);
    dan_winhttp.CloseHandle(s);
    return dan_net_result(status, &h, &b);
}

#endif

/* ==== Electronic mail: SMTP and IMAP ====
   почтовый_запрос(адрес, пользователь, пароль, отправитель, получатели, данные,
   команда, starttls, секунд): with recipients - sends the message (SMTP),
   otherwise - performs an IMAP request (the command or, without it, reads what
   the address names: «imaps://сервер/INBOX;UID=5») and returns the answer.
   Linux and macOS use libcurl, Windows - curl.exe, which is part of Windows 10
   and newer. */

static dr_y dan_mail_fail(const char *why, int line) { return dr_y_err(dan_strdup(why), line); }

static int dan_mail_scheme(dan_str url) {
    if (!strncmp(url, "smtps://", 8)) return 1;
    if (!strncmp(url, "smtp://", 7)) return 2;
    if (!strncmp(url, "imaps://", 8)) return 3;
    if (!strncmp(url, "imap://", 7)) return 4;
    return 0;
}

static const char *dan_mail_why(int code) {
    switch (code) {
    case 6: return DAN_TXT_MAIL_HOST;
    case 7: return DAN_TXT_MAIL_CONNECT;
    case 28: return DAN_TXT_MAIL_TIMEOUT;
    case 67: return DAN_TXT_MAIL_LOGIN;
    case 64: return DAN_TXT_MAIL_NOTLS;
    case 55: case 56: return DAN_TXT_MAIL_BROKEN;
    case 78: return DAN_TXT_MAIL_NOTFOUND;
    case 35: case 51: case 53: case 54: case 58: case 59: case 60: case 77: case 80: case 83: case 90: case 91:
        return DAN_TXT_NET_TLS;
    default: return NULL;
    }
}

#if defined(__wasi__)

static dr_y dan_mail(dan_str url, dan_str user, dan_str password, dan_str from, dl_Ls *rcpt, dan_bytes data,
                     dan_str command, bool starttls, int64_t seconds, int line) {
    (void)url; (void)user; (void)password; (void)from; (void)rcpt; (void)data; (void)command; (void)starttls; (void)seconds;
    return dan_mail_fail(DAN_TXT_MAIL_WASM, line);
}

#elif !defined(_WIN32)

#define DAN_CURLOPT_READDATA 10009
#define DAN_CURLOPT_READFUNCTION 20012
#define DAN_CURLOPT_UPLOAD 46
#define DAN_CURLOPT_INFILESIZE_LARGE 30115
#define DAN_CURLOPT_USE_SSL 119
#define DAN_CURLOPT_USERNAME 10173
#define DAN_CURLOPT_PASSWORD 10174
#define DAN_CURLOPT_MAIL_FROM 10186
#define DAN_CURLOPT_MAIL_RCPT 10187

typedef struct { const uint8_t *d; size_t n, pos; } dan_mail_src;

static size_t dan_mail_read(char *p, size_t size, size_t n, void *data) {
    dan_mail_src *s = data;
    size_t k = size * n;
    if (k > s->n - s->pos) k = s->n - s->pos;
    memcpy(p, s->d + s->pos, k);
    s->pos += k;
    return k;
}

static dr_y dan_mail(dan_str url, dan_str user, dan_str password, dan_str from, dl_Ls *rcpt, dan_bytes data,
                     dan_str command, bool starttls, int64_t seconds, int line) {
    int scheme = dan_mail_scheme(url);
    if (!scheme) return dan_mail_fail(DAN_TXT_MAIL_SCHEME, line);
    if (!dan_curl_load()) return dan_mail_fail(DAN_TXT_NET_NOCURL, line);
    void *c = dan_curl.easy_init();
    if (!c) return dan_mail_fail(DAN_TXT_NET_MEMORY, line);
    dan_netbuf b = { NULL, 0, 0 };
    dan_curl_slist *list = NULL;
    char err[256] = "";
    dan_mail_src src = { data->data, (size_t)data->len, 0 };
    long protocols = scheme == 1 ? 1L << 17 : scheme == 2 ? 1L << 16 : scheme == 3 ? 1L << 13 : 1L << 12;
    dan_curl.easy_setopt(c, DAN_CURLOPT_URL, url);
    dan_curl.easy_setopt(c, DAN_CURLOPT_ERRORBUFFER, err);
    dan_curl.easy_setopt(c, DAN_CURLOPT_NOSIGNAL, 1L);
    dan_curl.easy_setopt(c, DAN_CURLOPT_PROTOCOLS, protocols);
    dan_curl.easy_setopt(c, DAN_CURLOPT_CONNECTTIMEOUT, 30L);
    if (seconds > 0) dan_curl.easy_setopt(c, DAN_CURLOPT_TIMEOUT, (long)seconds);
    if (user[0]) dan_curl.easy_setopt(c, DAN_CURLOPT_USERNAME, user);
    if (password[0]) dan_curl.easy_setopt(c, DAN_CURLOPT_PASSWORD, password);
    if (starttls) dan_curl.easy_setopt(c, DAN_CURLOPT_USE_SSL, 3L);         /* CURLUSESSL_ALL */
    dan_curl.easy_setopt(c, DAN_CURLOPT_WRITEFUNCTION, dan_curl_body);
    dan_curl.easy_setopt(c, DAN_CURLOPT_WRITEDATA, (void *)&b);
    if (rcpt->len > 0) {
        for (int64_t i = 0; i < rcpt->len; i++) list = dan_curl.slist_append(list, rcpt->items[i]);
        dan_curl.easy_setopt(c, DAN_CURLOPT_MAIL_FROM, from);
        dan_curl.easy_setopt(c, DAN_CURLOPT_MAIL_RCPT, list);
        dan_curl.easy_setopt(c, DAN_CURLOPT_UPLOAD, 1L);
        dan_curl.easy_setopt(c, DAN_CURLOPT_READFUNCTION, dan_mail_read);
        dan_curl.easy_setopt(c, DAN_CURLOPT_READDATA, (void *)&src);
        dan_curl.easy_setopt(c, DAN_CURLOPT_INFILESIZE_LARGE, (int64_t)data->len);
    } else if (command[0]) dan_curl.easy_setopt(c, DAN_CURLOPT_CUSTOMREQUEST, command);
    int code = dan_curl.easy_perform(c);
    dan_curl.easy_cleanup(c);
    if (list) dan_curl.slist_free_all(list);
    if (code != 0) {
        free(b.data);
        /* libcurl reports a missing IMAP folder as a refused login */
        const char *why = (code == 67 && strstr(err, "Select")) ? DAN_TXT_MAIL_NOTFOUND : dan_mail_why(code);
        char m[400];
        if (why) snprintf(m, sizeof m, "%s", why);
        else snprintf(m, sizeof m, DAN_TXT_NET_OTHER, code, err);
        return dan_mail_fail(m, line);
    }
    return dr_y_some(dan_netbuf_bytes(&b));
}

#else

/* Windows: curl.exe; the password is passed in a settings file that only the
   current user can read, not on the command line */
static void dan_mail_cfg(dan_netbuf *cfg, const char *key, const char *value) {
    dan_netbuf_add(cfg, key, strlen(key));
    dan_netbuf_add(cfg, " = \"", 4);
    for (const char *p = value; *p; p++) {
        if (*p == '"' || *p == '\\') dan_netbuf_add(cfg, "\\", 1);
        if (*p == '\r' || *p == '\n') continue;
        dan_netbuf_add(cfg, p, 1);
    }
    dan_netbuf_add(cfg, "\"\n", 2);
}

static wchar_t *dan_mail_temp(const wchar_t *dir, const wchar_t *kind, unsigned n) {
    wchar_t *p = dan_alloc(sizeof(wchar_t) * (wcslen(dir) + 64));
    swprintf(p, wcslen(dir) + 64, L"%lsdan-mail-%lu-%u-%ls", dir, (unsigned long)GetCurrentProcessId(), n, kind);
    return p;
}

static int dan_mail_write(const wchar_t *path, const void *d, size_t n) {
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n;
}

static dr_y dan_mail(dan_str url, dan_str user, dan_str password, dan_str from, dl_Ls *rcpt, dan_bytes data,
                     dan_str command, bool starttls, int64_t seconds, int line) {
    if (!dan_mail_scheme(url)) return dan_mail_fail(DAN_TXT_MAIL_SCHEME, line);
    wchar_t sys[MAX_PATH], tmp[MAX_PATH], exe[MAX_PATH + 16];
    if (!GetSystemDirectoryW(sys, MAX_PATH) || !GetTempPathW(MAX_PATH, tmp)) return dan_mail_fail(DAN_TXT_MAIL_NOCURLEXE, line);
    swprintf(exe, MAX_PATH + 16, L"%ls\\curl.exe", sys);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return dan_mail_fail(DAN_TXT_MAIL_NOCURLEXE, line);
    static volatile LONG counter;
    unsigned n = (unsigned)InterlockedIncrement(&counter);
    wchar_t *wcfg = dan_mail_temp(tmp, L"cfg", n), *wdata = dan_mail_temp(tmp, L"data", n),
            *wout = dan_mail_temp(tmp, L"out", n), *werr = dan_mail_temp(tmp, L"err", n);
    dan_str cdata = dan_narrow(wdata), cout = dan_narrow(wout), cerr = dan_narrow(werr);
    dan_netbuf cfg = { NULL, 0, 0 };
    dan_mail_cfg(&cfg, "url", url);
    if (user[0]) {
        dan_str up = dan_concat(dan_concat(user, ":"), password);
        dan_mail_cfg(&cfg, "user", up);
    }
    if (starttls) dan_netbuf_add(&cfg, "ssl-reqd\n", 9);
    dan_netbuf_add(&cfg, "silent\nshow-error\n", 18);
    char t[64];
    snprintf(t, sizeof t, "%lld", (long long)(seconds > 0 ? seconds : 300));
    dan_mail_cfg(&cfg, "max-time", t);
    dan_mail_cfg(&cfg, "connect-timeout", "30");
    dan_mail_cfg(&cfg, "output", cout);
    dan_mail_cfg(&cfg, "stderr", cerr);
    int ok = 1;
    if (rcpt->len > 0) {
        dan_mail_cfg(&cfg, "mail-from", from);
        for (int64_t i = 0; i < rcpt->len; i++) dan_mail_cfg(&cfg, "mail-rcpt", rcpt->items[i]);
        dan_mail_cfg(&cfg, "upload-file", cdata);
        ok = dan_mail_write(wdata, data->data, (size_t)data->len);
    } else if (command[0]) dan_mail_cfg(&cfg, "request", command);
    ok = ok && dan_mail_write(wcfg, cfg.data, cfg.len);
    free(cfg.data);
    dr_y result;
    if (!ok) result = dan_mail_fail(DAN_TXT_MAIL_TEMP, line);
    else {
        size_t cl = wcslen(exe) + wcslen(wcfg) + 16;
        wchar_t *cmd = dan_alloc(sizeof(wchar_t) * cl);
        swprintf(cmd, cl, L"\"%ls\" -K \"%ls\"", exe, wcfg);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        DWORD code = 1;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (code == 0) {
                FILE *f = _wfopen(wout, L"rb");
                dan_netbuf b = { NULL, 0, 0 };
                if (f) {
                    char chunk[8192];
                    size_t got;
                    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) dan_netbuf_add(&b, chunk, got);
                    fclose(f);
                }
                result = dr_y_some(dan_netbuf_bytes(&b));
            } else {
                FILE *f = _wfopen(werr, L"rb");
                char e[400] = "";
                if (f) { size_t got = fread(e, 1, sizeof e - 1, f); e[got] = 0; fclose(f); }
                for (char *q = e; *q; q++) if (*q == '\r' || *q == '\n') *q = ' ';
                const char *why = (code == 67 && strstr(e, "Select")) ? DAN_TXT_MAIL_NOTFOUND : dan_mail_why((int)code);
                char m[600] = "";
                if (why) snprintf(m, sizeof m, "%s", why);
                else snprintf(m, sizeof m, DAN_TXT_NET_OTHER, (int)code, e);
                result = dan_mail_fail(m, line);
            }
        } else result = dan_mail_fail(DAN_TXT_MAIL_NOCURLEXE, line);
        free(cmd);
    }
    DeleteFileW(wcfg); DeleteFileW(wdata); DeleteFileW(wout); DeleteFileW(werr);
    free(wcfg); free(wdata); free(wout); free(werr);
    return result;
}

#endif

/* Date and time for the «Date:» header of a message: «Mon, 26 Oct 2026 10:30:00 +0300» */
static dan_str dan_mail_date(void) {
    time_t now = time(NULL);
    struct tm lt, gt;
#ifdef _WIN32
    localtime_s(&lt, &now);
    gmtime_s(&gt, &now);
#else
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
#endif
    /* the offset from UTC in minutes */
    long off = (long)((lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min));
    int dd = lt.tm_yday - gt.tm_yday;
    if (dd > 1) dd = -1;
    if (dd < -1) dd = 1;
    off += dd * 24 * 60;
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char t[64];
    snprintf(t, sizeof t, "%s, %d %s %d %02d:%02d:%02d %c%02ld%02ld", wd[lt.tm_wday], lt.tm_mday, mo[lt.tm_mon],
             lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec, off < 0 ? '-' : '+', labs(off) / 60, labs(off) % 60);
    return dan_strdup(t);
}
