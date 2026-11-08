/* Обёртка «zig cc» для поставки Дана в Windows: zig-cc.exe лежит в папке zig рядом с zig.exe.

   Компилятор на Дане вызывает компилятор C одной программой (переменная DAN_COMPILER), а zig
   собирает код C командой из двух частей: «zig cc». Обёртка запускает zig.exe из своей папки
   с первым аргументом «cc» и всеми своими аргументами, ждёт его и возвращает его код завершения.
   Параметр -mwindows (программа с окнами без окна консоли) zig не знает: он заменяется тем же
   самым по частям, как в dan.py.

   Собирается сценарием поставка/собрать.py: zig cc -O2 zig_cc.c -o zig-cc.exe -lshell32 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    wchar_t *p;
    size_t n, cap;
} Text;

static void add(Text *t, const wchar_t *s, size_t n) {
    if (t->n + n + 1 > t->cap) {
        size_t cap = (t->cap ? t->cap * 2 : 1024);
        while (cap < t->n + n + 1) cap *= 2;
        wchar_t *p = (wchar_t *)realloc(t->p, cap * sizeof(wchar_t));
        if (!p) ExitProcess(3);
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n * sizeof(wchar_t));
    t->n += n;
    t->p[t->n] = 0;
}

static void add_s(Text *t, const wchar_t *s) { add(t, s, wcslen(s)); }

/* Аргумент в записи, которую CommandLineToArgvW и библиотека C разбирают обратно в тот же текст. */
static void add_arg(Text *t, const wchar_t *a) {
    add_s(t, L" ");
    if (*a && !wcspbrk(a, L" \t\n\v\"")) {
        add_s(t, a);
        return;
    }
    add_s(t, L"\"");
    for (const wchar_t *c = a;; c++) {
        size_t slashes = 0;
        while (*c == L'\\') { slashes++; c++; }
        if (*c == 0) {
            for (size_t i = 0; i < slashes * 2; i++) add_s(t, L"\\");
            break;
        }
        if (*c == L'"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) add_s(t, L"\\");
        } else {
            for (size_t i = 0; i < slashes; i++) add_s(t, L"\\");
        }
        add(t, c, 1);
    }
    add_s(t, L"\"");
}

int main(void) {
    wchar_t self[32768];
    DWORD len = GetModuleFileNameW(NULL, self, 32768);
    if (len == 0 || len >= 32768) return 3;
    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return 3;
    slash[1] = 0;
    Text zig = {0};
    add_s(&zig, self);
    add_s(&zig, L"zig.exe");

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 3;
    Text cmd = {0};
    add_s(&cmd, L"\"");
    add_s(&cmd, zig.p);
    add_s(&cmd, L"\" cc");
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-mwindows") == 0) {
            add_s(&cmd, L" -Wl,--subsystem,windows -luser32 -lgdi32 -lcomdlg32");
        } else {
            add_arg(&cmd, argv[i]);
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(zig.p, cmd.p, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        static const char msg[] = "zig-cc: не удалось запустить zig.exe рядом с обёрткой\n";
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)(sizeof msg - 1), &written, NULL);
        return 3;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
