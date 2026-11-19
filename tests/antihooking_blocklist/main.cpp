#include "../../AntiHooking/dllblocklist.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>

static bool check(bool result, const char* message)
{
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return result;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: antihooking_blocklist <AntiHooking.dll>\n");
        return 2;
    }
    bool ok = true;
    ok &= check(matchesOptionalDllBlocklist(L"C:\\Windows\\NvPresent64.dll",
                                            L" other.dll ; NVPRESENT64.DLL "),
                "basename and case");
    ok &= check(matchesOptionalDllBlocklist(L"C:/Windows/nvspcap64.dll", L";nvspcap64.dll;"),
                "forward slash and empty entries");
    ok &= check(!matchesOptionalDllBlocklist(L"NvPresent64.dll", L"*;NvPresent64"),
                "no wildcard or extension guessing");
    ok &= check(!matchesOptionalDllBlocklist(L"NvPresent64.dll", L"C:\\NvPresent64.dll"),
                "no paths in list");
    ok &= check(!matchesOptionalDllBlocklist(L"NvPresent64.dll", L""), "empty list");
    ok &= check(!matchesOptionalDllBlocklist(nullptr, L"nvspcap64.dll"), "null input");

    if (argc == 2) {
        SetEnvironmentVariableW(L"MOONLIGHT_BLOCK_DLLS", nullptr);
        HMODULE antiHooking = LoadLibraryW(argv[1]);
        ok &= check(antiHooking != nullptr, "load AntiHooking.dll");
        if (antiHooking != nullptr) {
            HMODULE baseline = LoadLibraryW(L"version.dll");
            ok &= check(baseline != nullptr, "default allows LoadLibraryW");
            if (baseline != nullptr)
                FreeLibrary(baseline);
            SetEnvironmentVariableW(L"MOONLIGHT_BLOCK_DLLS", L"version.dll");
            HMODULE blocked = LoadLibraryW(L"version.dll");
            ok &= check(blocked == nullptr && GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY,
                        "configured LoadLibraryW is blocked");
            if (blocked != nullptr)
                FreeLibrary(blocked);
            blocked = LoadLibraryA("version.dll");
            ok &= check(blocked == nullptr && GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY,
                        "configured LoadLibraryA is blocked");
            if (blocked != nullptr)
                FreeLibrary(blocked);
            blocked = LoadLibraryExW(L"version.dll", nullptr, 0);
            ok &= check(blocked == nullptr && GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY,
                        "configured LoadLibraryExW is blocked");
            if (blocked != nullptr)
                FreeLibrary(blocked);
            blocked = LoadLibraryExA("version.dll", nullptr, 0);
            ok &= check(blocked == nullptr && GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY,
                        "configured LoadLibraryExA is blocked");
            if (blocked != nullptr)
                FreeLibrary(blocked);
            SetEnvironmentVariableW(L"MOONLIGHT_BLOCK_DLLS", nullptr);
            blocked = LoadLibraryW(L"NahimicOSD.dll");
            ok &= check(blocked == nullptr && GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY,
                        "built-in blocklist remains active");
            if (blocked != nullptr)
                FreeLibrary(blocked);
            FreeLibrary(antiHooking);
        }
    }
    return ok ? 0 : 1;
}
