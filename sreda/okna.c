/* ==== Window library of the Dan language: graphical interface. ====

   Windows: the Win32 interface (user32, gdi32, comdlg32), which is part of
   every Windows. A program with windows is built as a Windows application
   (-mwindows), so no console window appears behind it.

   The program itself waits for events: окно_ждать returns the next event
   (a button pressed, a text changed, a mouse click on a canvas, the window
   closed) as a list of strings [kind, element, x, y, text]. So the state of
   the program lives in its ordinary variables, and no function is called
   «from outside» at an unexpected moment.

   Coordinates and sizes are given in points of a screen with 96 dots per
   inch and are scaled to the real screen, so a window looks the same on
   ordinary and high-resolution screens. Windows are created and served by
   the main program (not by tasks). */

#ifdef _WIN32

#include <commdlg.h>

enum { DAN_W_WINDOW, DAN_W_BUTTON, DAN_W_LABEL, DAN_W_EDIT, DAN_W_MEMO, DAN_W_CHECK, DAN_W_LIST, DAN_W_COMBO,
       DAN_W_CANVAS, DAN_W_PROGRESS, DAN_W_PASSWORD };

typedef struct { int op, a, b, c, d, width; COLORREF line, fill; int size; wchar_t *text; } dan_draw_op;

typedef struct {
    int kind, window, alive;
    HWND h;
    HFONT font;
    dan_draw_op *ops;
    int nops, capops;
} dan_gui_el;

typedef struct { const char *kind; int64_t el; int x, y; char *text; } dan_gui_ev;

static dan_gui_el *dan_gui;          /* element number n is dan_gui[n - 1] */
static int dan_gui_n, dan_gui_cap;
static dan_gui_ev *dan_gui_q;        /* the queue of events */
static int dan_gui_qhead, dan_gui_qlen, dan_gui_qcap;
static int dan_gui_quiet;            /* the program itself changes a text: no event */
static int dan_gui_windows_open;
static double dan_gui_scale = 1.0;
static int dan_gui_ready;

static int dan_px(int64_t v) { return (int)(v * dan_gui_scale + (v >= 0 ? 0.5 : -0.5)); }
static int dan_pt(int v) { return (int)(v / dan_gui_scale + (v >= 0 ? 0.5 : -0.5)); }

static void dan_gui_push(const char *kind, int64_t el, int x, int y, const char *text) {
    if (dan_gui_qlen == dan_gui_qcap) {
        int cap = dan_gui_qcap ? dan_gui_qcap * 2 : 64;
        dan_gui_ev *q = dan_alloc(sizeof *q * (size_t)cap);
        for (int i = 0; i < dan_gui_qlen; i++) q[i] = dan_gui_q[(dan_gui_qhead + i) % dan_gui_qcap];
        free(dan_gui_q);
        dan_gui_q = q;
        dan_gui_qcap = cap;
        dan_gui_qhead = 0;
    }
    dan_gui_ev *e = &dan_gui_q[(dan_gui_qhead + dan_gui_qlen++) % dan_gui_qcap];
    e->kind = kind;
    e->el = el;
    e->x = x;
    e->y = y;
    e->text = NULL;
    if (text) {
        size_t n = strlen(text);
        e->text = dan_alloc(n + 1);
        memcpy(e->text, text, n + 1);
    }
}

static int64_t dan_gui_find(HWND h) {
    for (int i = 0; i < dan_gui_n; i++) if (dan_gui[i].alive && dan_gui[i].h == h) return i + 1;
    return 0;
}

static dan_gui_el *dan_gui_get(int64_t n, int line) {
    if (n < 1 || n > dan_gui_n || !dan_gui[n - 1].alive) dan_error(DAN_MSG_GUI_GONE, line);
    return &dan_gui[n - 1];
}

static int64_t dan_gui_new(int kind, int window, HWND h) {
    if (dan_gui_n == dan_gui_cap) {
        dan_gui_cap = dan_gui_cap ? dan_gui_cap * 2 : 32;
        dan_gui = dan_realloc(dan_gui, sizeof *dan_gui * (size_t)dan_gui_cap);
    }
    dan_gui_el *e = &dan_gui[dan_gui_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->window = window;
    e->h = h;
    e->alive = 1;
    return dan_gui_n;
}

static void dan_gui_free_ops(dan_gui_el *e) {
    for (int i = 0; i < e->nops; i++) free(e->ops[i].text);
    e->nops = 0;
}

static const char *dan_gui_key_name(WPARAM vk) {
    switch (vk) {
    case VK_LEFT: return DAN_TXT_KEY_LEFT;
    case VK_RIGHT: return DAN_TXT_KEY_RIGHT;
    case VK_UP: return DAN_TXT_KEY_UP;
    case VK_DOWN: return DAN_TXT_KEY_DOWN;
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_INSERT: return "Insert";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        static const char *const f[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        return f[vk - VK_F1];
    }
    return NULL;
}

static LRESULT CALLBACK dan_canvas_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right > 0 ? r.right : 1, r.bottom > 0 ? r.bottom : 1);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &r, white);
        DeleteObject(white);
        SetBkMode(mem, TRANSPARENT);
        int64_t n = dan_gui_find(h);
        dan_gui_el *e = n ? &dan_gui[n - 1] : NULL;
        for (int i = 0; e && i < e->nops; i++) {
            dan_draw_op *o = &e->ops[i];
            HPEN pen = o->line == (COLORREF)-1 ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, o->width > 0 ? o->width : 1, o->line);
            HBRUSH brush = o->fill == (COLORREF)-1 ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(o->fill);
            HGDIOBJ op = SelectObject(mem, pen), ob = SelectObject(mem, brush);
            switch (o->op) {
            case 0: {                                  /* clear */
                HBRUSH b = CreateSolidBrush(o->fill);
                FillRect(mem, &r, b);
                DeleteObject(b);
                break;
            }
            case 1: MoveToEx(mem, o->a, o->b, NULL); LineTo(mem, o->c, o->d); break;
            case 2: Rectangle(mem, o->a, o->b, o->a + o->c, o->b + o->d); break;
            case 3: Ellipse(mem, o->a, o->b, o->a + o->c, o->b + o->d); break;
            case 4: {
                HFONT f = CreateFontW(-o->size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HGDIOBJ of = SelectObject(mem, f);
                SetTextColor(mem, o->line == (COLORREF)-1 ? 0 : o->line);
                TextOutW(mem, o->a, o->b, o->text, (int)wcslen(o->text));
                SelectObject(mem, of);
                DeleteObject(f);
                break;
            }
            default: break;
            }
            SelectObject(mem, op);
            SelectObject(mem, ob);
            if (o->line != (COLORREF)-1) DeleteObject(pen);
            if (o->fill != (COLORREF)-1) DeleteObject(brush);
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        SetFocus(h);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        dan_gui_push(m == WM_LBUTTONDOWN ? "мышь" : "мышь_правая", dan_gui_find(h), dan_pt(x), dan_pt(y), NULL);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        const char *name = dan_gui_key_name(w);
        if (name) dan_gui_push("клавиша", dan_gui_find(h), 0, 0, name);
        return 0;
    }
    case WM_CHAR: {
        const char *special = w == 13 ? "Enter" : w == 27 ? "Esc" : w == 8 ? "Backspace" : w == 9 ? "Tab" : NULL;
        if (special) { dan_gui_push("клавиша", dan_gui_find(h), 0, 0, special); return 0; }
        if (w < 32) return 0;
        wchar_t s[2] = { (wchar_t)w, 0 };
        dan_gui_push("клавиша", dan_gui_find(h), 0, 0, dan_narrow(s));
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK dan_window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int64_t n = (int64_t)LOWORD(w);
        int code = HIWORD(w);
        if (n < 1 || n > dan_gui_n || !dan_gui[n - 1].alive || dan_gui_quiet) break;
        int kind = dan_gui[n - 1].kind;
        if (kind == DAN_W_BUTTON && code == BN_CLICKED) dan_gui_push("нажатие", n, 0, 0, NULL);
        else if (kind == DAN_W_CHECK && code == BN_CLICKED) dan_gui_push("изменение", n, 0, 0, NULL);
        else if ((kind == DAN_W_EDIT || kind == DAN_W_MEMO || kind == DAN_W_PASSWORD) && code == EN_CHANGE)
            dan_gui_push("изменение", n, 0, 0, NULL);
        else if (kind == DAN_W_LIST && code == LBN_SELCHANGE) dan_gui_push("изменение", n, 0, 0, NULL);
        else if (kind == DAN_W_COMBO && code == CBN_SELCHANGE) dan_gui_push("изменение", n, 0, 0, NULL);
        return 0;
    }
    case WM_SIZE: {
        int64_t n = dan_gui_find(h);
        if (n) dan_gui_push("размер", n, dan_pt(LOWORD(l)), dan_pt(HIWORD(l)), NULL);
        return 0;
    }
    case WM_TIMER: {
        int64_t n = dan_gui_find(h);
        if (n) dan_gui_push("таймер", n, 0, 0, NULL);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* labels and check boxes on the background of the window */
        HDC dc = (HDC)w;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_CLOSE: {
        int64_t n = dan_gui_find(h);
        if (n) dan_gui_push("закрытие", n, 0, 0, NULL);
        DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY: {
        int64_t n = dan_gui_find(h);
        if (n) {
            int window = (int)n;
            for (int i = 0; i < dan_gui_n; i++)
                if (dan_gui[i].alive && (i + 1 == window || dan_gui[i].window == window)) {
                    dan_gui[i].alive = 0;
                    dan_gui_free_ops(&dan_gui[i]);
                    if (dan_gui[i].font) DeleteObject(dan_gui[i].font);
                }
            dan_gui_windows_open--;
        }
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

static int dan_gui_start(void) {
    if (dan_gui_ready) return 1;
    HMODULE user = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *aware)(void) = user ? (BOOL (WINAPI *)(void))(void *)GetProcAddress(user, "SetProcessDPIAware") : NULL;
    if (aware) aware();
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    dan_gui_scale = dpi > 0 ? dpi / 96.0 : 1.0;
    /* progress bars need the common controls */
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    if (cc) {
        typedef struct { DWORD size, icc; } dan_icc;
        BOOL (WINAPI *init)(const dan_icc *) = (BOOL (WINAPI *)(const dan_icc *))(void *)GetProcAddress(cc, "InitCommonControlsEx");
        dan_icc icc = { sizeof icc, 0x20 /* ICC_PROGRESS_CLASS */ };
        if (init) init(&icc);
    }
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = dan_window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DanWindow";
    if (!RegisterClassExW(&wc)) return 0;
    wc.lpfnWndProc = dan_canvas_proc;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DanCanvas";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&wc)) return 0;
    dan_gui_ready = 1;
    return 1;
}

static HFONT dan_gui_font(int size) {
    return CreateFontW(-dan_px(size), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static dr_i dan_window_create(dan_str title, int64_t width, int64_t height, int line) {
    if (width < 50 || height < 30 || width > 20000 || height > 20000) dan_error(DAN_MSG_GUI_SIZE, line);
    if (!dan_gui_start()) return dr_i_err(dan_strdup(DAN_TXT_GUI_START), line);
    RECT r = { 0, 0, dan_px(width), dan_px(height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t *t = dan_wide(title);
    HWND h = CreateWindowExW(0, L"DanWindow", t, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
    free(t);
    if (!h) return dr_i_err(dan_strdup(DAN_TXT_GUI_START), line);
    int64_t n = dan_gui_new(DAN_W_WINDOW, 0, h);
    dan_gui[n - 1].font = dan_gui_font(15);
    dan_gui_windows_open++;
    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);
    return dr_i_some(n);
}

static int64_t dan_window_add(int64_t window, dan_str kind, dan_str text, int64_t x, int64_t y, int64_t w, int64_t hgt, int line) {
    dan_gui_el *win = dan_gui_get(window, line);
    if (win->kind != DAN_W_WINDOW) dan_error(DAN_MSG_GUI_NOT_WINDOW, line);
    static const struct { const char *name; int kind; const wchar_t *cls; DWORD style; DWORD ex; } kinds[] = {
        { "кнопка", DAN_W_BUTTON, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 0 },
        { "надпись", DAN_W_LABEL, L"STATIC", SS_LEFT | SS_NOPREFIX, 0 },
        { "поле", DAN_W_EDIT, L"EDIT", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0 },
        { "пароль", DAN_W_PASSWORD, L"EDIT", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | WS_TABSTOP, 0 },
        { "текст", DAN_W_MEMO, L"EDIT", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 0 },
        { "флажок", DAN_W_CHECK, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP, 0 },
        { "список", DAN_W_LIST, L"LISTBOX", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 0 },
        { "выпадающий", DAN_W_COMBO, L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0 },
        { "холст", DAN_W_CANVAS, L"DanCanvas", WS_TABSTOP, 0 },
        { "полоса", DAN_W_PROGRESS, L"msctls_progress32", 0, 0 },
    };
    size_t k = 0;
    while (k < sizeof kinds / sizeof kinds[0] && strcmp(kinds[k].name, kind) != 0) k++;
    if (k == sizeof kinds / sizeof kinds[0]) dan_error(DAN_MSG_GUI_KIND, line);
    if (dan_gui_n >= 65000) dan_error(DAN_MSG_GUI_MANY, line);
    int64_t n = dan_gui_n + 1;
    int real_h = kinds[k].kind == DAN_W_COMBO ? dan_px(hgt) + dan_px(200) : dan_px(hgt);   /* room for the drop-down part */
    wchar_t *t = dan_wide(kinds[k].kind == DAN_W_LIST || kinds[k].kind == DAN_W_COMBO ? "" : text);
    dan_gui_quiet++;
    HWND h = CreateWindowExW(kinds[k].ex, kinds[k].cls, t, WS_CHILD | WS_VISIBLE | kinds[k].style, dan_px(x), dan_px(y),
                             dan_px(w), real_h, win->h, (HMENU)(INT_PTR)n, GetModuleHandleW(NULL), NULL);
    dan_gui_quiet--;
    free(t);
    if (!h) dan_error(DAN_MSG_GUI_CREATE, line);
    dan_gui_new(kinds[k].kind, (int)window, h);
    SendMessageW(h, WM_SETFONT, (WPARAM)win->font, TRUE);
    if (kinds[k].kind == DAN_W_PROGRESS) SendMessageW(h, 0x0400 + 6 /* PBM_SETRANGE32 */, 0, 100);
    return n;
}

static void dan_gui_set_items(dan_gui_el *e, dan_str items) {
    int list = e->kind == DAN_W_LIST;
    dan_gui_quiet++;
    SendMessageW(e->h, list ? LB_RESETCONTENT : CB_RESETCONTENT, 0, 0);
    const char *p = items;
    while (*p) {
        const char *q = strchr(p, '\n');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        char *one = dan_alloc(n + 1);
        memcpy(one, p, n);
        one[n] = 0;
        wchar_t *w = dan_wide(one);
        SendMessageW(e->h, list ? LB_ADDSTRING : CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
        free(one);
        p += n;
        if (*p == '\n') p++;
    }
    dan_gui_quiet--;
}

static void dan_window_set(int64_t el, dan_str what, dan_str value, int line) {
    dan_gui_el *e = dan_gui_get(el, line);
    if (strcmp(what, "текст") == 0) {
        if (e->kind == DAN_W_LIST || e->kind == DAN_W_COMBO) { dan_gui_set_items(e, value); return; }
        /* line ends of the program (\n) become the line ends of Windows (\r\n) */
        dan_buf b = { NULL, 0, 0 };
        dan_buf_add(&b, "");
        for (const char *p = value; *p; p++) {
            char one[2] = { *p, 0 };
            if (*p == '\n' && e->kind == DAN_W_MEMO) dan_buf_add(&b, "\r");
            dan_buf_add(&b, one);
        }
        wchar_t *w = dan_wide(b.s);
        dan_buf_free(&b);
        dan_gui_quiet++;
        SetWindowTextW(e->h, w);
        dan_gui_quiet--;
        free(w);
    } else if (strcmp(what, "отмечен") == 0) {
        dan_gui_quiet++;
        SendMessageW(e->h, BM_SETCHECK, strcmp(value, "1") == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        dan_gui_quiet--;
    } else if (strcmp(what, "выбран") == 0) {
        dan_gui_quiet++;
        SendMessageW(e->h, e->kind == DAN_W_LIST ? LB_SETCURSEL : CB_SETCURSEL, (WPARAM)strtol(value, NULL, 10), 0);
        dan_gui_quiet--;
    } else if (strcmp(what, "значение") == 0) {
        SendMessageW(e->h, 0x0400 + 2 /* PBM_SETPOS */, (WPARAM)strtol(value, NULL, 10), 0);
    } else if (strcmp(what, "доступен") == 0) {
        EnableWindow(e->h, strcmp(value, "1") == 0);
    } else if (strcmp(what, "виден") == 0) {
        ShowWindow(e->h, strcmp(value, "1") == 0 ? SW_SHOW : SW_HIDE);
    } else if (strcmp(what, "шрифт") == 0) {
        HFONT f = dan_gui_font((int)strtol(value, NULL, 10));
        SendMessageW(e->h, WM_SETFONT, (WPARAM)f, TRUE);
        if (e->font && e->kind != DAN_W_WINDOW) DeleteObject(e->font);
        e->font = f;
    } else if (strcmp(what, "таймер") == 0) {
        long ms = strtol(value, NULL, 10);
        if (ms > 0) SetTimer(e->h, 1, (UINT)ms, NULL); else KillTimer(e->h, 1);
    } else if (strcmp(what, "место") == 0) {
        long v[4] = { 0, 0, 0, 0 };
        const char *p = value;
        for (int i = 0; i < 4; i++) { v[i] = strtol(p, (char **)&p, 10); }
        if (e->kind == DAN_W_WINDOW) {
            RECT r = { 0, 0, dan_px(v[2]), dan_px(v[3]) };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(e->h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        } else {
            MoveWindow(e->h, dan_px(v[0]), dan_px(v[1]), dan_px(v[2]), dan_px(v[3]), TRUE);
        }
    } else if (strcmp(what, "нажать") == 0) {
        /* for automatic checks of a program: as if the person pressed the button */
        if (e->kind == DAN_W_BUTTON || e->kind == DAN_W_CHECK) SendMessageW(e->h, BM_CLICK, 0, 0);
    } else if (strcmp(what, "ввести") == 0) {
        /* as if the person typed the text into the field: the event Изменение follows */
        wchar_t *w = dan_wide(value);
        SetWindowTextW(e->h, w);
        free(w);
    } else if (strcmp(what, "щёлкнуть") == 0) {
        long x = 0, y = 0;
        char *p;
        x = strtol(value, &p, 10);
        y = strtol(p, NULL, 10);
        PostMessageW(e->h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(dan_px(x), dan_px(y)));
    } else if (strcmp(what, "клавиша") == 0) {
        wchar_t *w = dan_wide(value);
        if (w[0] && !w[1]) PostMessageW(e->h, WM_CHAR, w[0], 0);
        else if (strcmp(value, "Enter") == 0) PostMessageW(e->h, WM_CHAR, 13, 0);
        else if (strcmp(value, "Esc") == 0) PostMessageW(e->h, WM_CHAR, 27, 0);
        else if (strcmp(value, DAN_TXT_KEY_LEFT) == 0) PostMessageW(e->h, WM_KEYDOWN, VK_LEFT, 0);
        else if (strcmp(value, DAN_TXT_KEY_RIGHT) == 0) PostMessageW(e->h, WM_KEYDOWN, VK_RIGHT, 0);
        else if (strcmp(value, DAN_TXT_KEY_UP) == 0) PostMessageW(e->h, WM_KEYDOWN, VK_UP, 0);
        else if (strcmp(value, DAN_TXT_KEY_DOWN) == 0) PostMessageW(e->h, WM_KEYDOWN, VK_DOWN, 0);
        free(w);
    } else if (strcmp(what, "закрыть") == 0) {
        if (e->kind == DAN_W_WINDOW) DestroyWindow(e->h);
    } else if (strcmp(what, "фокус") == 0) {
        SetFocus(e->h);
    } else {
        dan_error(DAN_MSG_GUI_PROPERTY, line);
    }
}

static dan_str dan_window_get(int64_t el, dan_str what, int line) {
    dan_gui_el *e = dan_gui_get(el, line);
    if (strcmp(what, "текст") == 0) {
        if (e->kind == DAN_W_LIST || e->kind == DAN_W_COMBO) {
            LRESULT i = SendMessageW(e->h, e->kind == DAN_W_LIST ? LB_GETCURSEL : CB_GETCURSEL, 0, 0);
            if (i < 0) return dan_empty_s.s;
            LRESULT n = SendMessageW(e->h, e->kind == DAN_W_LIST ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, (WPARAM)i, 0);
            wchar_t *w = dan_alloc(sizeof(wchar_t) * (size_t)(n + 1));
            SendMessageW(e->h, e->kind == DAN_W_LIST ? LB_GETTEXT : CB_GETLBTEXT, (WPARAM)i, (LPARAM)w);
            dan_str s = dan_narrow(w);
            free(w);
            return s;
        }
        int n = GetWindowTextLengthW(e->h);
        wchar_t *w = dan_alloc(sizeof(wchar_t) * (size_t)(n + 1));
        GetWindowTextW(e->h, w, n + 1);
        wchar_t *o = w;
        for (wchar_t *p = w; *p; p++) if (*p != L'\r') *o++ = *p;    /* \r\n -> \n */
        *o = 0;
        dan_str s = dan_narrow(w);
        free(w);
        return s;
    }
    if (strcmp(what, "отмечен") == 0) return SendMessageW(e->h, BM_GETCHECK, 0, 0) == BST_CHECKED ? dan_true_s.s : dan_false_s.s;
    if (strcmp(what, "выбран") == 0) {
        char b[32];
        snprintf(b, sizeof b, "%lld", (long long)SendMessageW(e->h, e->kind == DAN_W_LIST ? LB_GETCURSEL : CB_GETCURSEL, 0, 0));
        return dan_strdup(b);
    }
    if (strcmp(what, "размер") == 0) {
        RECT r;
        GetClientRect(e->h, &r);
        char b[64];
        snprintf(b, sizeof b, "%d %d", dan_pt(r.right), dan_pt(r.bottom));
        return dan_strdup(b);
    }
    dan_error(DAN_MSG_GUI_PROPERTY, line);
    return dan_empty_s.s;
}

/* drawing on a canvas: «очистить», «линия», «прямоугольник», «эллипс», «текст» */
static void dan_window_draw(int64_t el, dan_str what, dl_Li *v, dan_str text, int line) {
    dan_gui_el *e = dan_gui_get(el, line);
    if (e->kind != DAN_W_CANVAS) dan_error(DAN_MSG_GUI_NOT_CANVAS, line);
    static const char *const names[] = { "очистить", "линия", "прямоугольник", "эллипс", "текст" };
    int op = 0;
    while (op < 5 && strcmp(names[op], what) != 0) op++;
    if (op == 5) dan_error(DAN_MSG_GUI_DRAW, line);
    int64_t a[8] = { 0, 0, 0, 0, 0, -1, 1, 15 };
    for (int64_t i = 0; i < v->len && i < 8; i++) a[i] = v->items[i];
    if (op == 0) dan_gui_free_ops(e);
    if (e->nops == e->capops) {
        e->capops = e->capops ? e->capops * 2 : 64;
        e->ops = dan_realloc(e->ops, sizeof *e->ops * (size_t)e->capops);
    }
    dan_draw_op *o = &e->ops[e->nops++];
    memset(o, 0, sizeof *o);
    o->op = op;
    /* colours are 0xRRGGBB, -1 - none; Windows stores them as 0xBBGGRR */
#define DAN_RGB(c) ((c) < 0 ? (COLORREF)-1 : RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255))
    if (op == 0) { o->fill = DAN_RGB(a[0]); }
    else {
        o->a = dan_px(a[0]); o->b = dan_px(a[1]); o->c = dan_px(a[2]); o->d = dan_px(a[3]);
        o->line = DAN_RGB(a[4]); o->fill = DAN_RGB(a[5]); o->width = dan_px(a[6]); o->size = dan_px(a[7]);
        if (op == 1) { o->c = dan_px(a[2]); o->d = dan_px(a[3]); }
        if (op == 4) { o->line = DAN_RGB(a[2]); o->size = dan_px(a[3] > 0 ? a[3] : 15); o->text = dan_wide(text); }
    }
#undef DAN_RGB
    InvalidateRect(e->h, NULL, FALSE);
}

static dl_Ls *dan_gui_event_list(dan_gui_ev *e) {
    dl_Ls *r = dl_Ls_from(0, NULL);
    char num[32];
    dl_Ls_push(r, dan_strdup(e->kind));
    snprintf(num, sizeof num, "%lld", (long long)e->el); dl_Ls_push(r, dan_strdup(num));
    snprintf(num, sizeof num, "%d", e->x); dl_Ls_push(r, dan_strdup(num));
    snprintf(num, sizeof num, "%d", e->y); dl_Ls_push(r, dan_strdup(num));
    dl_Ls_push(r, dan_strdup(e->text ? e->text : ""));
    free(e->text);
    return r;
}

/* the next event; [«нет»] when no window is open any more, [«ожидание»] when
   «ms» milliseconds passed without events (ms < 0 - wait as long as needed) */
static dl_Ls *dan_window_wait(int64_t ms) {
    DWORD start = GetTickCount();
    for (;;) {
        if (dan_gui_qlen > 0) {
            dan_gui_ev e = dan_gui_q[dan_gui_qhead];
            dan_gui_qhead = (dan_gui_qhead + 1) % dan_gui_qcap;
            dan_gui_qlen--;
            return dan_gui_event_list(&e);
        }
        if (dan_gui_windows_open <= 0) {
            dan_gui_ev e = { "нет", 0, 0, 0, NULL };
            return dan_gui_event_list(&e);
        }
        MSG msg;
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            HWND top = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : NULL;
            if (top && IsDialogMessageW(top, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        DWORD wait = INFINITE;
        if (ms >= 0) {
            DWORD passed = GetTickCount() - start;
            if (passed >= (DWORD)ms) {
                dan_gui_ev e = { "ожидание", 0, 0, 0, NULL };
                return dan_gui_event_list(&e);
            }
            wait = (DWORD)ms - passed;
        }
        fflush(stdout);
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
    }
}

static int64_t dan_window_message(dan_str title, dan_str text, dan_str kind) {
    wchar_t *t = dan_wide(title), *x = dan_wide(text);
    UINT flags = strcmp(kind, "вопрос") == 0 ? MB_YESNO | MB_ICONQUESTION
               : strcmp(kind, "ошибка") == 0 ? MB_OK | MB_ICONERROR
               : strcmp(kind, "предупреждение") == 0 ? MB_OK | MB_ICONWARNING : MB_OK | MB_ICONINFORMATION;
    HWND owner = GetActiveWindow();
    int r = MessageBoxW(owner, x, t, flags);
    free(t);
    free(x);
    return r == IDYES || r == IDOK;
}

/* «Текст|*.txt|Все файлы|*.*» -> the double-zero list of Windows */
static do_s dan_window_file(dan_str kind, dan_str title, dan_str filter) {
    wchar_t *t = dan_wide(title), *f = dan_wide(*filter ? filter : DAN_TXT_GUI_ALL_FILES);
    size_t n = wcslen(f);
    wchar_t *ff = dan_alloc(sizeof(wchar_t) * (n + 2));
    for (size_t i = 0; i <= n; i++) ff[i] = f[i] == L'|' ? 0 : f[i];
    ff[n + 1] = 0;
    wchar_t file[4096] = L"";
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = GetActiveWindow();
    o.lpstrFilter = ff;
    o.lpstrFile = file;
    o.nMaxFile = 4096;
    o.lpstrTitle = t;
    int save = strcmp(kind, "сохранить") == 0;
    o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
    free(t);
    free(f);
    free(ff);
    if (!ok) return do_s_none();
    return do_s_some(dan_narrow(file));
}

#elif defined(__wasi__)  /* WebAssembly: windows are not available */


static dr_i dan_window_create(dan_str title, int64_t width, int64_t height, int line) {
    (void)title; (void)width; (void)height;
    return dr_i_err(dan_strdup(DAN_TXT_GUI_ONLY_WINDOWS), line);
}
static int64_t dan_window_add(int64_t window, dan_str kind, dan_str text, int64_t x, int64_t y, int64_t w, int64_t h, int line) {
    (void)window; (void)kind; (void)text; (void)x; (void)y; (void)w; (void)h;
    dan_error(DAN_MSG_GUI_GONE, line);
    return 0;
}
static void dan_window_set(int64_t el, dan_str what, dan_str value, int line) { (void)el; (void)what; (void)value; dan_error(DAN_MSG_GUI_GONE, line); }
static dan_str dan_window_get(int64_t el, dan_str what, int line) { (void)el; (void)what; dan_error(DAN_MSG_GUI_GONE, line); return dan_empty_s.s; }
static void dan_window_draw(int64_t el, dan_str what, dl_Li *v, dan_str text, int line) {
    (void)el; (void)what; (void)v; (void)text; dan_error(DAN_MSG_GUI_GONE, line);
}
static dl_Ls *dan_window_wait(int64_t ms) {
    (void)ms;
    dl_Ls *r = dl_Ls_from(0, NULL);
    dl_Ls_push(r, dan_strdup("нет"));
    for (int i = 0; i < 4; i++) dl_Ls_push(r, dan_strdup(i < 3 ? "0" : ""));
    return r;
}
/* without windows messages and questions go to the console */
static int64_t dan_window_message(dan_str title, dan_str text, dan_str kind) {
    fflush(stdout);
    fprintf(stderr, "%s: %s\n", title, text);
    if (strcmp(kind, "вопрос") != 0) return 1;
    fprintf(stderr, "%s", DAN_TXT_GUI_YES_NO);
    char b[64];
    if (!fgets(b, sizeof b, stdin)) return 0;
    const unsigned char *u = (const unsigned char *)b;
    return u[0] == 'y' || u[0] == 'Y' || (u[0] == 0xD0 && (u[1] == 0xB4 || u[1] == 0x94));   /* «д» or «Д» */
}
static do_s dan_window_file(dan_str kind, dan_str title, dan_str filter) { (void)kind; (void)title; (void)filter; return do_s_none(); }

#else  /* Linux, macOS: the window is a page in the browser */

#include "okna_veb.c"

#endif
