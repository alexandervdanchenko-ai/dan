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

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <winhttp.h>
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

#ifndef _WIN32

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
