// CrashHandler.cpp — see CrashHandler.h. Windows-only implementation using
// MiniDumpWriteDump (dbghelp) + a textual crash line. Artifacts land in
// <AppData>/agent-monitor/crash/.

#include "CrashHandler.h"

#ifdef Q_OS_WIN

#include <QStandardPaths>

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

namespace {

QString crashDir()
{
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/crash");
}

QString g_lastCrashFile;

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info)
{
    const DWORD pid = GetCurrentProcessId();
    wchar_t dirBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, dirBuf, MAX_PATH);
    wchar_t* slash = wcsrchr(dirBuf, L'\\');
    if (slash)
        *slash = 0;
    wcscat_s(dirBuf, L"\\crash");
    CreateDirectoryW(dirBuf, nullptr);

    wchar_t base[64];
    swprintf_s(base, L"\\agent-monitor-crash-%u", pid);

    wchar_t dmpPath[MAX_PATH], logPath[MAX_PATH];
    wcscpy_s(dmpPath, dirBuf); wcscat_s(dmpPath, base); wcscat_s(dmpPath, L".dmp");
    wcscpy_s(logPath, dirBuf); wcscat_s(logPath, base); wcscat_s(logPath, L".log");

    typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE,
                                               MINIDUMP_TYPE,
                                               PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION,
                                               PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp) {
        auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        if (writeDump) {
            HANDLE file = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                writeDump(GetCurrentProcess(), pid, file,
                          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory
                                        | MiniDumpScanMemory),
                          &mei, nullptr, nullptr);
                CloseHandle(file);
            }
        }
    }

    HANDLE log = CreateFileW(logPath, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE && info && info->ExceptionRecord) {
        char line[256];
        const int n = snprintf(line, sizeof(line),
                               "code=0x%08lX addr=%p thread=%lu\n",
                               (unsigned long)info->ExceptionRecord->ExceptionCode,
                               info->ExceptionRecord->ExceptionAddress,
                               (unsigned long)GetCurrentThreadId());
        DWORD written = 0;
        WriteFile(log, line, DWORD(n), &written, nullptr);
        CloseHandle(log);
        g_lastCrashFile = QString::fromWCharArray(logPath);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

namespace CrashHandler {

bool install()
{
    SetUnhandledExceptionFilter(unhandledFilter);
    return true;
}

QString lastCrashFile()
{
    return g_lastCrashFile;
}

} // namespace CrashHandler

#else // !Q_OS_WIN

namespace CrashHandler {
bool install() { return false; }
QString lastCrashFile() { return QString(); }
} // namespace CrashHandler

#endif
