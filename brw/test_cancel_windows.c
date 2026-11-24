#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

/* Launch in a separate console process group so the test can send Ctrl+Break
   without interrupting PowerShell or another test. */
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3)
        return 90;
    wchar_t command[4096];
    if (wcslen(argv[1]) + 40 >= sizeof command / sizeof *command)
        return 91;
    swprintf(command, sizeof command / sizeof *command,
             L"\"%ls\" install cancelled --json", argv[1]);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!GetConsoleCP() && !AllocConsole())
        return 90;
    PROCESS_INFORMATION child = {0};
    if (!CreateProcessW(argv[1], command, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &startup, &child))
        return 92;
    CloseHandle(child.hThread);
    DWORD result = 93;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (WaitForSingleObject(child.hProcess, 20) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        if (GetFileAttributesW(argv[2]) != INVALID_FILE_ATTRIBUTES) {
            if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child.dwProcessId) &&
                WaitForSingleObject(child.hProcess, 12000) == WAIT_OBJECT_0)
                GetExitCodeProcess(child.hProcess, &result);
            break;
        }
    }
    if (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(child.hProcess, result);
        WaitForSingleObject(child.hProcess, 3000);
    }
    CloseHandle(child.hProcess);
    return (int)result;
}
