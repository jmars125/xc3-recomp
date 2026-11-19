/* XC3-Setup.exe: starts python\pythonw.exe -m installer.gui with this exe's folder as the working directory, passing on
   its own arguments unchanged (installer/gui.py main: --dest, --base, --update,
   --keys and --dlc fill in the form, --lang picks the window language).

   No console window. The two message strings come from the catalog (installer/text/<lang>.json, ids launcher.title and
   launcher.python_missing); tools/make_release.py writes them into launcher_text.h before it compiles this file.
   Build: llvm/bin/llvm-windres -I runtime -O coff launcher.rc -o launcher_rc.o, then
   llvm/bin/clang -O2 -mwindows launcher.c launcher_rc.o -o XC3-Setup.exe */
#include <stdlib.h>
#include <windows.h>
#include <wchar.h>

#include "launcher_text.h"

#define LANGID_RUSSIAN 0x19
#define PATH_CHARS (MAX_PATH * 2)

static void fail(void)
{
    int russian = (GetUserDefaultUILanguage() & 0x3ff) == LANGID_RUSSIAN;
    MessageBoxW(NULL, russian ? LAUNCHER_PYTHON_MISSING_RU : LAUNCHER_PYTHON_MISSING_EN,
                russian ? LAUNCHER_TITLE_RU : LAUNCHER_TITLE_EN, MB_OK | MB_ICONERROR);
}

/* The arguments after the program name in the raw command line, quoting untouched: skips argv[0] as
   CommandLineToArgvW reads it (quoted up to the next quote, else up to the first space or tab), then the blanks. */
static const wchar_t *arguments(void)
{
    const wchar_t *c = GetCommandLineW();
    if (*c == L'"') {
        c++;
        while (*c && *c != L'"') c++;
        if (*c) c++;
    } else {
        while (*c && *c != L' ' && *c != L'\t') c++;
    }
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
    wchar_t folder[PATH_CHARS], python[PATH_CHARS], *command;
    const wchar_t *args = arguments();
    size_t cap = PATH_CHARS + 64 + wcslen(args);
    DWORD n = GetModuleFileNameW(NULL, folder, PATH_CHARS);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    (void)instance; (void)previous; (void)command_line; (void)show;
    if (n == 0 || n >= PATH_CHARS) { fail(); return 1; }
    while (n > 0 && folder[n - 1] != L'\\') n--;
    folder[n > 0 ? n - 1 : 0] = 0;
    if (swprintf(python, PATH_CHARS, L"%ls\\python\\pythonw.exe", folder) < 0 ||
        GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES) { fail(); return 1; }
    command = malloc(cap * sizeof *command);
    if (!command || swprintf(command, cap, L"\"%ls\" -m installer.gui %ls", python, args) < 0) { fail(); return 1; }
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, folder, &si, &pi)) { fail(); return 1; }
    free(command);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
