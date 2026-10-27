/* ---- Windows of Linux and macOS: a page in the browser ----

   The program keeps the state of its windows (elements, texts, drawings) and
   serves it over HTTP on the local address 127.0.0.1 with a random key in the
   address, so other programs and sites cannot drive the window. The page,
   opened in the default browser, shows the windows and sends back what the
   person does (presses, typing, clicks, keys). Requests are served while the
   program waits for events (окно_ждать), so as in Windows the program decides
   itself when to react, and its variables are never changed «from outside».

   Without a browser (the variable DAN_GUI_NO_BROWSER is set, for example in
   automatic checks) the window works the same, and the program can be driven
   by «нажать», «ввести», «щёлкнуть» and «клавиша». */

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0          /* macOS: SIGPIPE is ignored instead */
#endif

enum { DAN_W_WINDOW, DAN_W_BUTTON, DAN_W_LABEL, DAN_W_EDIT, DAN_W_MEMO, DAN_W_CHECK, DAN_W_LIST, DAN_W_COMBO,
       DAN_W_CANVAS, DAN_W_PROGRESS, DAN_W_PASSWORD };

static const char *const dan_web_kinds[] = { "окно", "кнопка", "надпись", "поле", "текст", "флажок", "список",
                                             "выпадающий", "холст", "полоса", "пароль" };

typedef struct {
    int kind, window, alive;
    char *text;                 /* text; for lists - items through \n */
    int checked, selected, value, enabled, visible, font, mono;
    int64_t x, y, w, h;
    dan_buf ops;                /* drawing of a canvas in the form of the page (JSON items) */
    int64_t timer_ms;
    double timer_next;
} dan_web_el;

typedef struct { const char *kind; int64_t el; int64_t x, y; char *text; } dan_web_ev;

static dan_web_el *dan_web;
static int dan_web_n, dan_web_cap;
static dan_web_ev *dan_web_q;
static int dan_web_qhead, dan_web_qlen, dan_web_qcap;
static int dan_web_windows_open;
static int64_t dan_web_version = 1;
static int dan_web_listen = -1;
static char dan_web_key[40];
static int dan_web_port;
static double dan_web_seen;         /* when the page last asked for the state (0 - never) */
static struct { int fd; int64_t since; double deadline; } dan_web_poll[32];
static int dan_web_npoll;
/* a message box waiting for the answer of the person */
static struct { int64_t id; char *kind, *title, *text; int answered; char *answer; } dan_web_dialog;
static int64_t dan_web_dialogs;

static double dan_web_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static void dan_web_push(const char *kind, int64_t el, int64_t x, int64_t y, const char *text) {
    if (dan_web_qlen == dan_web_qcap) {
        int cap = dan_web_qcap ? dan_web_qcap * 2 : 64;
        dan_web_ev *q = dan_alloc(sizeof *q * (size_t)cap);
        for (int i = 0; i < dan_web_qlen; i++) q[i] = dan_web_q[(dan_web_qhead + i) % dan_web_qcap];
        free(dan_web_q);
        dan_web_q = q;
        dan_web_qcap = cap;
        dan_web_qhead = 0;
    }
    dan_web_ev *e = &dan_web_q[(dan_web_qhead + dan_web_qlen++) % dan_web_qcap];
    e->kind = kind; e->el = el; e->x = x; e->y = y;
    e->text = NULL;
    if (text) { size_t n = strlen(text); e->text = dan_alloc(n + 1); memcpy(e->text, text, n + 1); }
}

static dan_web_el *dan_web_get(int64_t n, int line) {
    if (n < 1 || n > dan_web_n || !dan_web[n - 1].alive) dan_error(DAN_MSG_GUI_GONE, line);
    return &dan_web[n - 1];
}

static void dan_web_settext(dan_web_el *e, const char *t) {
    free(e->text);
    size_t n = strlen(t);
    e->text = dan_alloc(n + 1);
    memcpy(e->text, t, n + 1);
}

static int64_t dan_web_new(int kind, int window) {
    if (dan_web_n >= 65000) return 0;
    if (dan_web_n == dan_web_cap) {
        dan_web_cap = dan_web_cap ? dan_web_cap * 2 : 32;
        dan_web = dan_realloc(dan_web, sizeof *dan_web * (size_t)dan_web_cap);
    }
    dan_web_el *e = &dan_web[dan_web_n];
    memset(e, 0, sizeof *e);
    e->kind = kind; e->window = window; e->alive = 1;
    e->selected = -1; e->enabled = 1; e->visible = 1; e->font = 15;
    dan_web_settext(e, "");
    return ++dan_web_n;
}

/* ---- the page ---- */

static const char dan_web_page[] =
"<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><title>\xd0\x94\xd0\xb0\xd0\xbd</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>"
":root{--bg:#e9e7e2;--win:#f6f5f2;--text:#1d1d1b;--line:#b9b6ad;--title:#dcd9d1;--accent:#2f5ea8}"
"@media (prefers-color-scheme:dark){:root{--bg:#1c1c1a;--win:#262624;--text:#e9e7e2;--line:#4a4944;--title:#33322f;--accent:#7aa2e3}}"
"body{margin:0;background:var(--bg);color:var(--text);font:15px system-ui,'Segoe UI',sans-serif;padding:16px;"
"display:flex;flex-wrap:wrap;gap:16px;align-items:flex-start}"
".w{background:var(--win);border:1px solid var(--line);border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,.15);overflow:hidden}"
".t{background:var(--title);padding:6px 10px;display:flex;justify-content:space-between;align-items:center;font-weight:600}"
".t button{border:0;background:none;font-size:18px;cursor:pointer;color:var(--text)}"
".b{position:relative}.b>*{position:absolute;box-sizing:border-box;margin:0;font:inherit}"
".b input,.b textarea,.b select{border:1px solid var(--line);border-radius:4px;background:#fff;color:#111;padding:2px 6px}"
".b button{border:1px solid var(--line);border-radius:4px;background:var(--title);color:var(--text);cursor:pointer}"
".b button:active{filter:brightness(.9)}.b label{display:flex;align-items:center;gap:6px}"
".b canvas{border:1px solid var(--line);background:#fff}.b progress{width:100%}"
".off{color:#777}#gone{display:none;padding:24px;font-size:18px}</style></head><body>"
"<div id=\"gone\">\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb3\xd1\x80\xd0\xb0\xd0\xbc\xd0\xbc\xd0\xb0 \xd0\xb7\xd0\xb0\xd0\xb2\xd0\xb5\xd1\x80\xd1\x88\xd0\xb5\xd0\xbd\xd0\xb0.</div><script>"
"'use strict';const K=new URLSearchParams(location.search).get('k');let V=0;const els={},wins={};const done={};"
"function send(kind,id,x,y,t){fetch('/event?k='+K,{method:'POST',body:[kind,id,x|0,y|0,t==null?'':t].join('\\n')});}"
"const keys={ArrowLeft:'\xd0\x92\xd0\xbb\xd0\xb5\xd0\xb2\xd0\xbe',ArrowRight:'\xd0\x92\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbe',"
"ArrowUp:'\xd0\x92\xd0\xb2\xd0\xb5\xd1\x80\xd1\x85',ArrowDown:'\xd0\x92\xd0\xbd\xd0\xb8\xd0\xb7',Escape:'Esc',Enter:'Enter',"
"Backspace:'Backspace',Tab:'Tab',Delete:'Delete',Home:'Home',End:'End',PageUp:'PageUp',PageDown:'PageDown',Insert:'Insert'};"
"function col(c){return c<0?null:'#'+('00000'+c.toString(16)).slice(-6);}"
"function make(e){let d;switch(e.kind){"
"case 'кнопка':d=document.createElement('button');d.onclick=()=>send('нажатие',e.id);break;"
"case 'надпись':d=document.createElement('div');d.style.whiteSpace='pre-wrap';break;"
"case 'поле':case 'пароль':d=document.createElement('input');d.type=e.kind=='поле'?'text':'password';"
"d.oninput=()=>send('изменение',e.id,0,0,d.value);break;"
"case 'текст':d=document.createElement('textarea');d.oninput=()=>send('изменение',e.id,0,0,d.value);break;"
"case 'флажок':{d=document.createElement('label');const c=document.createElement('input');c.type='checkbox';"
"const s=document.createElement('span');d.append(c,s);c.onchange=()=>send('изменение',e.id,0,0,c.checked?'1':'0');break;}"
"case 'список':case 'выпадающий':d=document.createElement('select');if(e.kind=='список')d.size=2;"
"d.onchange=()=>send('изменение',e.id,0,0,String(d.selectedIndex));break;"
"case 'холст':d=document.createElement('canvas');d.tabIndex=0;"
"d.onmousedown=ev=>{d.focus();const r=d.getBoundingClientRect();send(ev.button==2?'мышь_правая':'мышь',e.id,ev.clientX-r.left,ev.clientY-r.top);};"
"d.oncontextmenu=ev=>ev.preventDefault();d.onkeydown=ev=>{const k=keys[ev.key]||(/^F\\d\\d?$/.test(ev.key)?ev.key:"
"(ev.key.length==1&&!ev.ctrlKey&&!ev.metaKey?ev.key:null));if(k){ev.preventDefault();send('клавиша',e.id,0,0,k);}};break;"
"case 'полоса':d=document.createElement('progress');d.max=100;break;default:d=document.createElement('div');}"
"return d;}"
"function draw(d,e){const r=window.devicePixelRatio||1;d.width=e.w*r;d.height=e.h*r;const g=d.getContext('2d');"
"g.setTransform(r,0,0,r,0,0);g.fillStyle='#fff';g.fillRect(0,0,e.w,e.h);for(const o of e.ops){const[op,a,b,c,dd,ln,fl,wd,sz,t]=o;"
"if(op==0){g.fillStyle=col(fl)||'#fff';g.fillRect(0,0,e.w,e.h);continue;}g.lineWidth=wd||1;"
"if(op==1){if(ln!=null&&ln>=0){g.strokeStyle=col(ln);g.beginPath();g.moveTo(a,b);g.lineTo(c,dd);g.stroke();}}"
"else if(op==2||op==3){g.beginPath();if(op==2)g.rect(a,b,c,dd);else g.ellipse(a+c/2,b+dd/2,Math.abs(c/2),Math.abs(dd/2),0,0,7);"
"if(fl>=0){g.fillStyle=col(fl);g.fill();}if(ln>=0){g.strokeStyle=col(ln);g.stroke();}}"
"else if(op==4){g.fillStyle=col(ln)||'#000';g.font=sz+'px system-ui,sans-serif';g.textBaseline='top';g.fillText(t,a,b);}}}"
"function render(s){if(s.ended){document.getElementById('gone').style.display='block';for(const k in wins)wins[k].remove();return;}"
"const alive={};for(const w of s.windows){alive[w.id]=1;let W=wins[w.id];if(!W){W=document.createElement('div');W.className='w';"
"const t=document.createElement('div');t.className='t';const n=document.createElement('span');const x=document.createElement('button');"
"x.textContent='\xc3\x97';x.title='\xd0\x97\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c';x.onclick=()=>send('закрытие',w.id);t.append(n,x);const b=document.createElement('div');"
"b.className='b';W.append(t,b);document.body.append(W);wins[w.id]=W;}"
"W.firstChild.firstChild.textContent=w.title;document.title=s.windows[0].title;const B=W.lastChild;B.style.width=w.w+'px';B.style.height=w.h+'px';"
"for(const e of w.elements){let d=els[e.id];if(!d){d=make(e);els[e.id]=d;B.append(d);}"
"d.style.left=e.x+'px';d.style.top=e.y+'px';d.style.width=e.w+'px';d.style.height=e.h+'px';d.style.fontSize=e.font+'px';d.style.fontFamily=e.mono?'Consolas,ui-monospace,monospace':'';"
"d.style.display=e.visible?'':'none';const inp=d.tagName=='LABEL'?d.firstChild:d;inp.disabled=!e.enabled;"
"if(e.kind=='кнопка'||e.kind=='надпись')d.textContent=e.text;"
"else if(e.kind=='флажок'){d.lastChild.textContent=e.text;d.firstChild.checked=!!e.checked;}"
"else if(e.kind=='поле'||e.kind=='пароль'||e.kind=='текст'){if(document.activeElement!==d&&d.value!==e.text)d.value=e.text;}"
"else if(e.kind=='список'||e.kind=='выпадающий'){const it=e.text==''?[]:e.text.split('\\n');"
"if(d.dataset.items!==e.text){d.innerHTML='';for(const i of it){const o=document.createElement('option');o.textContent=i;d.append(o);}"
"d.dataset.items=e.text;}d.selectedIndex=e.selected;}"
"else if(e.kind=='полоса')d.value=e.value;else if(e.kind=='холст')draw(d,e);}}"
"for(const k in wins)if(!alive[k]){wins[k].remove();delete wins[k];}"
"if(s.dialog&&!done[s.dialog.id]){done[s.dialog.id]=1;const D=s.dialog;setTimeout(()=>{let a='';"
"if(D.kind=='вопрос')a=confirm(D.title+'\\n\\n'+D.text)?'1':'0';else if(D.kind=='файл'){a=prompt(D.title+'\\n'+D.text,'');a=a==null?'':a;}"
"else{alert(D.title+'\\n\\n'+D.text);a='1';}send('ответ',D.id,0,0,a);},10);}}"
"async function loop(){let bad=0;for(;;){try{const r=await fetch('/state?k='+K+'&v='+V);if(r.status!=200)throw 0;const s=await r.json();"
"bad=0;V=s.v;render(s);if(s.ended)return;}catch(e){if(++bad>=3&&V>0){render({ended:true});return;}"
"await new Promise(r=>setTimeout(r,1000));}}}loop();"
"</script></body></html>";

static void dan_json_str(dan_buf *b, const char *s) {
    dan_buf_add(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char t[8];
        if (*p == '"' || *p == '\\') { t[0] = '\\'; t[1] = (char)*p; t[2] = 0; dan_buf_add(b, t); }
        else if (*p < 32) { snprintf(t, sizeof t, "\\u%04x", *p); dan_buf_add(b, t); }
        else { t[0] = (char)*p; t[1] = 0; dan_buf_add(b, t); }
    }
    dan_buf_add(b, "\"");
}

static dan_buf dan_web_state(void) {
    dan_buf b = { NULL, 0, 0 };
    char t[200];
    snprintf(t, sizeof t, "{\"v\":%lld,\"ended\":%s,\"windows\":[", (long long)dan_web_version,
             dan_web_windows_open <= 0 ? "true" : "false");
    dan_buf_add(&b, t);
    int first = 1;
    for (int i = 0; i < dan_web_n; i++) {
        dan_web_el *w = &dan_web[i];
        if (!w->alive || w->kind != DAN_W_WINDOW) continue;
        if (!first) dan_buf_add(&b, ",");
        first = 0;
        snprintf(t, sizeof t, "{\"id\":%d,\"w\":%lld,\"h\":%lld,\"title\":", i + 1, (long long)w->w, (long long)w->h);
        dan_buf_add(&b, t);
        dan_json_str(&b, w->text);
        dan_buf_add(&b, ",\"elements\":[");
        int f2 = 1;
        for (int j = 0; j < dan_web_n; j++) {
            dan_web_el *e = &dan_web[j];
            if (!e->alive || e->window != i + 1) continue;
            if (!f2) dan_buf_add(&b, ",");
            f2 = 0;
            snprintf(t, sizeof t, "{\"id\":%d,\"kind\":", j + 1);
            dan_buf_add(&b, t);
            dan_json_str(&b, dan_web_kinds[e->kind]);
            snprintf(t, sizeof t, ",\"x\":%lld,\"y\":%lld,\"w\":%lld,\"h\":%lld,\"checked\":%d,\"selected\":%d,\"value\":%d,"
                     "\"enabled\":%d,\"visible\":%d,\"font\":%d,\"mono\":%d,\"text\":", (long long)e->x, (long long)e->y,
                     (long long)e->w, (long long)e->h, e->checked, e->selected, e->value, e->enabled, e->visible, e->font,
                     e->mono);
            dan_buf_add(&b, t);
            dan_json_str(&b, e->text);
            dan_buf_add(&b, ",\"ops\":[");
            if (e->ops.s) dan_buf_add(&b, e->ops.s);
            dan_buf_add(&b, "]}");
        }
        dan_buf_add(&b, "]}");
    }
    dan_buf_add(&b, "]");
    if (dan_web_dialog.id && !dan_web_dialog.answered) {
        snprintf(t, sizeof t, ",\"dialog\":{\"id\":%lld,\"kind\":", (long long)dan_web_dialog.id);
        dan_buf_add(&b, t);
        dan_json_str(&b, dan_web_dialog.kind);
        dan_buf_add(&b, ",\"title\":");
        dan_json_str(&b, dan_web_dialog.title);
        dan_buf_add(&b, ",\"text\":");
        dan_json_str(&b, dan_web_dialog.text);
        dan_buf_add(&b, "}");
    }
    dan_buf_add(&b, "}");
    return b;
}

/* ---- the server ---- */

static void dan_web_send(int fd, const char *status, const char *type, const char *body, size_t n) {
    char head[256];
    int k = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, n);
    size_t off = 0;
    while (off < (size_t)k) { ssize_t w = send(fd, head + off, (size_t)k - off, MSG_NOSIGNAL); if (w <= 0) break; off += (size_t)w; }
    off = 0;
    while (off < n) { ssize_t w = send(fd, body + off, n - off, MSG_NOSIGNAL); if (w <= 0) break; off += (size_t)w; }
    close(fd);
}

static void dan_web_answer_polls(int force) {
    double now = dan_web_now();
    for (int i = 0; i < dan_web_npoll;) {
        if (force || dan_web_version > dan_web_poll[i].since || now >= dan_web_poll[i].deadline) {
            dan_buf st = dan_web_state();
            dan_web_send(dan_web_poll[i].fd, "200 OK", "application/json; charset=utf-8", st.s, st.len);
            dan_buf_free(&st);
            dan_web_poll[i] = dan_web_poll[--dan_web_npoll];
            dan_web_seen = now;
        } else i++;
    }
}

static void dan_web_changed(void) {
    dan_web_version++;
    if (dan_web_npoll) dan_web_answer_polls(0);
}

static const char *dan_web_ev_kind(const char *k) {
    static const char *const known[] = { "нажатие", "изменение", "мышь", "мышь_правая", "клавиша", "закрытие", "ответ" };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) if (!strcmp(known[i], k)) return known[i];
    return NULL;
}

static void dan_web_close_window(int64_t n) {
    for (int i = 0; i < dan_web_n; i++)
        if (dan_web[i].alive && (i + 1 == n || dan_web[i].window == n)) {
            dan_web[i].alive = 0;
            dan_buf_free(&dan_web[i].ops);
            dan_web[i].ops.len = dan_web[i].ops.cap = 0;
        }
    dan_web_windows_open--;
    dan_web_changed();
}

/* an event from the page: kind, element, x, y, text through \n */
static void dan_web_event(char *body) {
    char *f[5] = { body, "", "0", "0", "" };
    for (int i = 1; i < 5; i++) {
        char *nl = strchr(f[i - 1], '\n');
        if (!nl) break;
        *nl = 0;
        f[i] = nl + 1;
    }
    const char *kind = dan_web_ev_kind(f[0]);
    if (!kind) return;
    int64_t id = strtoll(f[1], NULL, 10);
    if (!strcmp(kind, "ответ")) {
        if (id == dan_web_dialog.id && !dan_web_dialog.answered) {
            dan_web_dialog.answered = 1;
            size_t n = strlen(f[4]);
            dan_web_dialog.answer = dan_alloc(n + 1);
            memcpy(dan_web_dialog.answer, f[4], n + 1);
            dan_web_changed();
        }
        return;
    }
    if (id < 1 || id > dan_web_n || !dan_web[id - 1].alive) return;
    dan_web_el *e = &dan_web[id - 1];
    if (!strcmp(kind, "закрытие")) {
        if (e->kind != DAN_W_WINDOW) return;
        dan_web_push("закрытие", id, 0, 0, NULL);
        dan_web_close_window(id);
        return;
    }
    if (!e->enabled) return;
    if (!strcmp(kind, "изменение")) {
        if (e->kind == DAN_W_EDIT || e->kind == DAN_W_PASSWORD || e->kind == DAN_W_MEMO) dan_web_settext(e, f[4]);
        else if (e->kind == DAN_W_CHECK) e->checked = !strcmp(f[4], "1");
        else if (e->kind == DAN_W_LIST || e->kind == DAN_W_COMBO) e->selected = (int)strtol(f[4], NULL, 10);
        else return;
    } else if (!strcmp(kind, "нажатие") && e->kind != DAN_W_BUTTON) return;
    dan_web_push(kind, id, strtoll(f[2], NULL, 10), strtoll(f[3], NULL, 10), !strcmp(kind, "клавиша") ? f[4] : NULL);
}

static void dan_web_request(int fd) {
    /* the request is read with a time limit: a silent connection does not stop the program */
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char *buf = dan_alloc(65536 + 1);
    size_t n = 0, head = 0, need = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, 65536 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        if (!head) {
            char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                head = (size_t)(e - buf) + 4;
                char *cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                need = cl && cl < e ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
                if (need > 65536 - head) need = 65536 - head;
            }
        }
        if (head && n >= head + need) break;
        if (n >= 65536) break;
    }
    buf[n] = 0;
    char method[8] = "", path[2048] = "";
    if (!head || sscanf(buf, "%7s %2047s", method, path) != 2) { close(fd); free(buf); return; }
    /* the key: k=... in the address */
    char *k = strstr(path, "k=");
    int ok = k && !strncmp(k + 2, dan_web_key, strlen(dan_web_key)) && !isalnum((unsigned char)k[2 + strlen(dan_web_key)]);
    if (!ok) {
        const char *m = "403";
        dan_web_send(fd, "403 Forbidden", "text/plain", m, strlen(m));
        free(buf);
        return;
    }
    dan_web_seen = dan_web_now();
    if (!strcmp(method, "GET") && (!strncmp(path, "/?", 2) || !strncmp(path, "/index", 6))) {
        dan_web_send(fd, "200 OK", "text/html; charset=utf-8", dan_web_page, sizeof dan_web_page - 1);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/state", 6)) {
        char *v = strstr(path, "&v=");
        int64_t since = v ? strtoll(v + 3, NULL, 10) : 0;
        if (dan_web_version > since || dan_web_npoll == (int)(sizeof dan_web_poll / sizeof dan_web_poll[0])) {
            dan_buf st = dan_web_state();
            dan_web_send(fd, "200 OK", "application/json; charset=utf-8", st.s, st.len);
            dan_buf_free(&st);
        } else {
            dan_web_poll[dan_web_npoll].fd = fd;
            dan_web_poll[dan_web_npoll].since = since;
            dan_web_poll[dan_web_npoll].deadline = dan_web_now() + 15;
            dan_web_npoll++;
        }
    } else if (!strcmp(method, "POST") && !strncmp(path, "/event", 6)) {
        dan_web_event(buf + head);
        dan_web_send(fd, "200 OK", "text/plain", "ok", 2);
    } else {
        dan_web_send(fd, "404 Not Found", "text/plain", "404", 3);
    }
    free(buf);
}

/* serves the page for at most «seconds» (0 - only what is ready) */
static void dan_web_serve(double seconds) {
    if (dan_web_listen < 0) return;
    dan_web_answer_polls(0);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(dan_web_listen, &rd);
    struct timeval tv = { (time_t)seconds, (suseconds_t)((seconds - (double)(time_t)seconds) * 1e6) };
    if (select(dan_web_listen + 1, &rd, NULL, NULL, &tv) > 0 && FD_ISSET(dan_web_listen, &rd)) {
        int fd = accept(dan_web_listen, NULL, NULL);
        if (fd >= 0) dan_web_request(fd);
    }
    dan_web_answer_polls(0);
    /* the page was closed: it has not asked for the state for a long time */
    if (dan_web_seen > 0 && dan_web_npoll == 0 && dan_web_now() - dan_web_seen > 40) {
        for (int i = 0; i < dan_web_n; i++)
            if (dan_web[i].alive && dan_web[i].kind == DAN_W_WINDOW) {
                dan_web_push("закрытие", i + 1, 0, 0, NULL);
                dan_web_close_window(i + 1);
            }
        dan_web_seen = 0;
    }
}

static void dan_web_open_browser(void) {
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/?k=%s", dan_web_port, dan_web_key);
    fflush(stdout);
    fprintf(stderr, DAN_TXT_GUI_BROWSER, url);
    fflush(stderr);
    if (getenv("DAN_GUI_NO_BROWSER")) return;
    /* two forks: the program waits only for the first, which ends at once, so no
       finished process remains and the handling of other child processes is not changed */
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
#ifdef __APPLE__
            execlp("open", "open", url, (char *)NULL);
#else
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            execlp("sensible-browser", "sensible-browser", url, (char *)NULL);
#endif
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

static int dan_web_start(void) {
    if (dan_web_listen >= 0) return 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) { close(fd); return 0; }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    dan_web_port = ntohs(a.sin_port);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* the key: 128 random bits */
    unsigned char r[16];
    int ok = 0;
    FILE *u = fopen("/dev/urandom", "rb");
    if (u) { ok = fread(r, 1, 16, u) == 16; fclose(u); }
    if (!ok) for (int i = 0; i < 16; i++) r[i] = (unsigned char)(rand() ^ (int)(dan_web_now() * 1000) >> i);
    for (int i = 0; i < 16; i++) snprintf(dan_web_key + 2 * i, 3, "%02x", r[i]);
    dan_web_listen = fd;
    signal(SIGPIPE, SIG_IGN);
    dan_web_open_browser();
    return 1;
}

/* ---- the functions of the window library ---- */

static dr_i dan_window_create(dan_str title, int64_t width, int64_t height, int line) {
    if (width < 50 || height < 30 || width > 20000 || height > 20000) dan_error(DAN_MSG_GUI_SIZE, line);
    if (!dan_web_start()) return dr_i_err(dan_strdup(DAN_TXT_GUI_START), line);
    int64_t n = dan_web_new(DAN_W_WINDOW, 0);
    if (!n) dan_error(DAN_MSG_GUI_MANY, line);
    dan_web_el *w = &dan_web[n - 1];
    dan_web_settext(w, title);
    w->w = width; w->h = height;
    dan_web_windows_open++;
    dan_web_changed();
    return dr_i_some(n);
}

static int64_t dan_window_add(int64_t window, dan_str kind, dan_str text, int64_t x, int64_t y, int64_t w, int64_t hgt, int line) {
    dan_web_el *win = dan_web_get(window, line);
    if (win->kind != DAN_W_WINDOW) dan_error(DAN_MSG_GUI_NOT_WINDOW, line);
    int k = 1;
    while (k < (int)(sizeof dan_web_kinds / sizeof dan_web_kinds[0]) && strcmp(dan_web_kinds[k], kind) != 0) k++;
    if (k == (int)(sizeof dan_web_kinds / sizeof dan_web_kinds[0])) dan_error(DAN_MSG_GUI_KIND, line);
    int64_t n = dan_web_new(k, (int)window);
    if (!n) dan_error(DAN_MSG_GUI_MANY, line);
    dan_web_el *e = &dan_web[n - 1];
    if (k != DAN_W_LIST && k != DAN_W_COMBO) dan_web_settext(e, text);
    e->x = x; e->y = y; e->w = w; e->h = hgt;
    dan_web_changed();
    return n;
}

static void dan_window_set(int64_t el, dan_str what, dan_str value, int line) {
    dan_web_el *e = dan_web_get(el, line);
    if (!strcmp(what, "текст")) {
        dan_web_settext(e, value);
        if (e->kind == DAN_W_LIST || e->kind == DAN_W_COMBO) e->selected = -1;
    } else if (!strcmp(what, "отмечен")) e->checked = !strcmp(value, "1");
    else if (!strcmp(what, "выбран")) e->selected = (int)strtol(value, NULL, 10);
    else if (!strcmp(what, "значение")) e->value = (int)strtol(value, NULL, 10);
    else if (!strcmp(what, "доступен")) e->enabled = !strcmp(value, "1");
    else if (!strcmp(what, "виден")) e->visible = !strcmp(value, "1");
    else if (!strcmp(what, "шрифт")) { e->font = (int)strtol(value, NULL, 10); e->mono = 0; }
    else if (!strcmp(what, "шрифт_кода")) { e->font = (int)strtol(value, NULL, 10); e->mono = 1; }
    else if (!strcmp(what, "таймер")) {
        e->timer_ms = strtoll(value, NULL, 10);
        e->timer_next = dan_web_now() + e->timer_ms / 1000.0;
        return;
    } else if (!strcmp(what, "место")) {
        long v[4] = { 0, 0, 0, 0 };
        const char *p = value;
        for (int i = 0; i < 4; i++) v[i] = strtol(p, (char **)&p, 10);
        if (e->kind == DAN_W_WINDOW) { e->w = v[2]; e->h = v[3]; }
        else { e->x = v[0]; e->y = v[1]; e->w = v[2]; e->h = v[3]; }
    } else if (!strcmp(what, "нажать")) {
        if (!e->enabled) return;
        if (e->kind == DAN_W_BUTTON) dan_web_push("нажатие", el, 0, 0, NULL);
        else if (e->kind == DAN_W_CHECK) { e->checked = !e->checked; dan_web_push("изменение", el, 0, 0, NULL); }
    } else if (!strcmp(what, "ввести")) {
        dan_web_settext(e, value);
        dan_web_push("изменение", el, 0, 0, NULL);
    } else if (!strcmp(what, "щёлкнуть")) {
        char *p;
        long x = strtol(value, &p, 10), y = strtol(p, NULL, 10);
        dan_web_push("мышь", el, x, y, NULL);
        return;
    } else if (!strcmp(what, "клавиша")) {
        dan_web_push("клавиша", el, 0, 0, value);
        return;
    } else if (!strcmp(what, "закрыть")) {
        if (e->kind == DAN_W_WINDOW) dan_web_close_window(el);
        return;
    } else if (!strcmp(what, "фокус")) {
        return;
    } else dan_error(DAN_MSG_GUI_PROPERTY, line);
    dan_web_changed();
}

static dan_str dan_window_get(int64_t el, dan_str what, int line) {
    dan_web_el *e = dan_web_get(el, line);
    /* what the person did in the page reaches the program while it waits; here the page gets a moment too */
    dan_web_serve(0);
    if (!strcmp(what, "текст")) {
        if (e->kind == DAN_W_LIST || e->kind == DAN_W_COMBO) {
            if (e->selected < 0) return dan_empty_s.s;
            const char *p = e->text;
            for (int i = 0; i < e->selected && p; i++) { p = strchr(p, '\n'); if (p) p++; }
            if (!p) return dan_empty_s.s;
            const char *q = strchr(p, '\n');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            char *s = dan_str_new(n);
            memcpy(s, p, n);
            s[n] = 0;
            return s;
        }
        return dan_strdup(e->text);
    }
    if (!strcmp(what, "отмечен")) return e->checked ? dan_true_s.s : dan_false_s.s;
    if (!strcmp(what, "выбран")) { char b[32]; snprintf(b, sizeof b, "%d", e->selected); return dan_strdup(b); }
    if (!strcmp(what, "размер")) { char b[64]; snprintf(b, sizeof b, "%lld %lld", (long long)e->w, (long long)e->h); return dan_strdup(b); }
    dan_error(DAN_MSG_GUI_PROPERTY, line);
    return dan_empty_s.s;
}

static void dan_window_draw(int64_t el, dan_str what, dl_Li *v, dan_str text, int line) {
    dan_web_el *e = dan_web_get(el, line);
    if (e->kind != DAN_W_CANVAS) dan_error(DAN_MSG_GUI_NOT_CANVAS, line);
    static const char *const names[] = { "очистить", "линия", "прямоугольник", "эллипс", "текст" };
    int op = 0;
    while (op < 5 && strcmp(names[op], what) != 0) op++;
    if (op == 5) dan_error(DAN_MSG_GUI_DRAW, line);
    int64_t a[8] = { 0, 0, 0, 0, 0, -1, 1, 15 };
    for (int64_t i = 0; i < v->len && i < 8; i++) a[i] = v->items[i];
    if (op == 0) { dan_buf_free(&e->ops); e->ops.len = e->ops.cap = 0; }
    char t[256];
    if (op == 0) snprintf(t, sizeof t, "[0,0,0,0,0,-1,%lld,1,15,\"\"]", (long long)a[0]);
    else if (op == 4)
        snprintf(t, sizeof t, "[4,%lld,%lld,0,0,%lld,-1,1,%lld,", (long long)a[0], (long long)a[1], (long long)a[2],
                 (long long)(a[3] > 0 ? a[3] : 15));
    else snprintf(t, sizeof t, "[%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,15,\"\"]", op, (long long)a[0], (long long)a[1],
                  (long long)a[2], (long long)a[3], (long long)a[4], (long long)a[5], (long long)a[6]);
    if (e->ops.len) dan_buf_add(&e->ops, ",");
    else dan_buf_add(&e->ops, "");
    dan_buf_add(&e->ops, t);
    if (op == 4) { dan_json_str(&e->ops, text); dan_buf_add(&e->ops, "]"); }
    dan_web_changed();
}

static dl_Ls *dan_web_event_list(dan_web_ev *e) {
    dl_Ls *r = dl_Ls_from(0, NULL);
    char num[32];
    dl_Ls_push(r, dan_strdup(e->kind));
    snprintf(num, sizeof num, "%lld", (long long)e->el); dl_Ls_push(r, dan_strdup(num));
    snprintf(num, sizeof num, "%lld", (long long)e->x); dl_Ls_push(r, dan_strdup(num));
    snprintf(num, sizeof num, "%lld", (long long)e->y); dl_Ls_push(r, dan_strdup(num));
    dl_Ls_push(r, dan_strdup(e->text ? e->text : ""));
    free(e->text);
    return r;
}

static dl_Ls *dan_window_wait(int64_t ms) {
    double start = dan_web_now();
    for (;;) {
        if (dan_web_qlen > 0) {
            dan_web_ev e = dan_web_q[dan_web_qhead];
            dan_web_qhead = (dan_web_qhead + 1) % dan_web_qcap;
            dan_web_qlen--;
            return dan_web_event_list(&e);
        }
        if (dan_web_windows_open <= 0) {
            /* the page learns that the program has ended: it is given up to two seconds to ask */
            if (dan_web_listen >= 0 && dan_web_seen > 0) {
                double limit = dan_web_now() + 2;
                while (dan_web_npoll == 0 && dan_web_now() < limit) dan_web_serve(0.1);
                dan_web_seen = 0;
            }
            dan_web_answer_polls(1);
            dan_web_ev e = { "нет", 0, 0, 0, NULL };
            return dan_web_event_list(&e);
        }
        double now = dan_web_now(), until = ms >= 0 ? start + ms / 1000.0 : now + 3600;
        for (int i = 0; i < dan_web_n; i++) {
            dan_web_el *w = &dan_web[i];
            if (!w->alive || w->kind != DAN_W_WINDOW || w->timer_ms <= 0) continue;
            if (now >= w->timer_next) {
                dan_web_push("таймер", i + 1, 0, 0, NULL);
                w->timer_next = now + w->timer_ms / 1000.0;
            }
            if (w->timer_next < until) until = w->timer_next;
        }
        if (dan_web_qlen > 0) continue;
        if (ms >= 0 && now >= start + ms / 1000.0) {
            dan_web_ev e = { "ожидание", 0, 0, 0, NULL };
            return dan_web_event_list(&e);
        }
        double wait = until - now;
        if (wait > 1) wait = 1;
        if (wait < 0) wait = 0;
        fflush(stdout);
        if (dan_web_listen >= 0) dan_web_serve(wait);
        else {
            struct timeval tv = { (time_t)wait, (suseconds_t)((wait - (double)(time_t)wait) * 1e6) };
            select(0, NULL, NULL, NULL, &tv);
        }
    }
}

/* a message or a question: in the page, if it is open; otherwise in the console */
static dan_str dan_web_ask(const char *kind, dan_str title, dan_str text) {
    dan_web_dialog.id = ++dan_web_dialogs;
    dan_web_dialog.kind = (char *)kind;
    dan_web_dialog.title = (char *)title;
    dan_web_dialog.text = (char *)text;
    dan_web_dialog.answered = 0;
    dan_web_dialog.answer = NULL;
    dan_web_changed();
    while (!dan_web_dialog.answered && dan_web_windows_open > 0 && dan_web_seen > 0) dan_web_serve(1);
    char *a = dan_web_dialog.answer;
    dan_web_dialog.id = 0;
    dan_web_changed();
    dan_str r = dan_strdup(a ? a : "");
    free(a);
    return r;
}

static int64_t dan_window_message(dan_str title, dan_str text, dan_str kind) {
    if (dan_web_listen >= 0 && dan_web_seen > 0 && dan_web_windows_open > 0)
        return strcmp(dan_web_ask(strcmp(kind, "вопрос") == 0 ? "вопрос" : "сообщение", title, text), "1") == 0;
    fflush(stdout);
    fprintf(stderr, "%s: %s\n", title, text);
    if (strcmp(kind, "вопрос") != 0) return 1;
    fprintf(stderr, "%s", DAN_TXT_GUI_YES_NO);
    char b[64];
    if (!fgets(b, sizeof b, stdin)) return 0;
    const unsigned char *u = (const unsigned char *)b;
    return u[0] == 'y' || u[0] == 'Y' || (u[0] == 0xD0 && (u[1] == 0xB4 || u[1] == 0x94));   /* «д» or «Д» */
}

/* choosing a file: the page cannot show the files of the computer, so the path is typed */
static do_s dan_window_file(dan_str kind, dan_str title, dan_str filter) {
    (void)filter;
    if (!(dan_web_listen >= 0 && dan_web_seen > 0 && dan_web_windows_open > 0)) return do_s_none();
    dan_str p = dan_web_ask("файл", title, strcmp(kind, "сохранить") == 0 ? DAN_TXT_GUI_PATH_SAVE : DAN_TXT_GUI_PATH_OPEN);
    if (!p[0]) return do_s_none();
    return do_s_some(p);
}
