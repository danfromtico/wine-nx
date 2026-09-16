/* On-screen keyboard smoke test: needs a person at the Switch, since swkbd is
 * a real interactive applet, not something a script can drive. An Edit
 * control gets focus, which the win32u Switch driver (winnx_drv.c) should
 * open the on-screen keyboard for by itself unless no-swkbd-auto.txt turns
 * that off (dlls/win32u/input.c: maybe_show_software_keyboard); Minus + the
 * right stick click forces it open too, at any point, whatever has focus
 * (dlls/win32u/winnx_drv.c: wine_nx_swkbd_hotkey). What the player types
 * arrives as ordinary WM_CHAR, the same way SendInput's KEYEVENTF_UNICODE
 * delivers it, so the standard EDIT control puts it in its own text with no
 * help from this test. Exit 42 means the edit control holds some text after
 * 30 seconds; 0 means it is still empty. */
#include <windows.h>
#include <winternl.h>

__declspec(dllimport) NTSTATUS NTAPI NtDisplayString( const UNICODE_STRING *str );
__declspec(dllimport) NTSTATUS NTAPI NtTerminateProcess( HANDLE process, NTSTATUS status );

static void report( const WCHAR *message, const WCHAR *detail )
{
    static const WCHAR prefix[] = L"[SWKBD TEST] ";
    WCHAR text[256];
    UNICODE_STRING str;
    unsigned int i, n = 0;

    for (i = 0; prefix[i]; i++) text[n++] = prefix[i];
    for (i = 0; message[i] && n < 120; i++) text[n++] = message[i];
    if (detail)
    {
        text[n++] = L':'; text[n++] = L' ';
        for (i = 0; detail[i] && n < 250; i++) text[n++] = detail[i];
    }
    text[n++] = L'\n';
    text[n] = 0;
    str.Buffer = text;
    str.Length = n * sizeof(WCHAR);
    str.MaximumLength = (n + 1) * sizeof(WCHAR);
    NtDisplayString( &str );
}

void __stdcall start(void)
{
    WNDCLASSW cls = {0};
    HWND main_wnd, edit;
    WCHAR text[256];
    MSG msg;
    DWORD deadline;
    int len;

    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW( NULL );
    cls.lpszClassName = L"WineNxSwkbdTest";
    RegisterClassW( &cls );

    main_wnd = CreateWindowW( cls.lpszClassName, L"swkbd test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, NULL, NULL, cls.hInstance, NULL );
    edit = CreateWindowW( L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
                          10, 10, 360, 24, main_wnd, NULL, cls.hInstance, NULL );
    ShowWindow( main_wnd, SW_SHOW );

    report( L"created, focusing the edit control", NULL );
    SetFocus( edit );  /* should open the keyboard by itself (no-swkbd-auto.txt) */
    report( L"waiting up to 30s for typed text (or Minus + right stick click)", NULL );

    deadline = GetTickCount() + 30000;
    while (GetTickCount() < deadline)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        len = GetWindowTextW( edit, text, sizeof(text) / sizeof(text[0]) );
        if (len > 0) break;
        Sleep( 100 );
    }

    len = GetWindowTextW( edit, text, sizeof(text) / sizeof(text[0]) );
    report( len > 0 ? L"PASS, edit control holds" : L"FAIL, edit control is still empty", len > 0 ? text : NULL );
    NtTerminateProcess( GetCurrentProcess(), len > 0 ? 42 : 0 );
}
