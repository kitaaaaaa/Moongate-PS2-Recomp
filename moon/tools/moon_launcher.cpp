// Moongate.exe (built by setup.sh): starts moon\bin\ps2EntryRunner.exe on the extracted disc (moon\iso_jp) with
// the exact GS renderer (PS2X_GS_BACKEND, kept when already set in the environment). Log: moon\logs\run_play.log.
#include <windows.h>

#include <string>

static void setDefault(const wchar_t *name, const std::wstring &value)
{
    if (GetEnvironmentVariableW(name, nullptr, 0) == 0)
        SetEnvironmentVariableW(name, value.c_str());
}

static int fail(const std::wstring &text)
{
    MessageBoxW(nullptr, text.c_str(), L"Moongate", MB_ICONERROR);
    return 1;
}

int wmain()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring top(self);
    top.resize(top.find_last_of(L'\\'));
    const std::wstring moon = top + L"\\moon";
    const std::wstring bin = moon + L"\\bin";
    const std::wstring elf = moon + L"\\iso_jp\\SLPM_627.67";
    if (GetFileAttributesW(elf.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"Missing " + elf + L"\nRun setup.bat first.");

    setDefault(L"PS2X_GS_BACKEND", L"exact");

    CreateDirectoryW((moon + L"\\logs").c_str(), nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((moon + L"\\logs\\run_play.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    const std::wstring runner = bin + L"\\ps2EntryRunner.exe";
    std::wstring cmd = L"\"" + runner + L"\" \"" + elf + L"\"";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(runner.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, bin.c_str(), &si, &pi))
        return fail(L"Could not start " + runner);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(log);
    return 0;
}
