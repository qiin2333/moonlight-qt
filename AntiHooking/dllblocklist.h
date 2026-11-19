#pragma once

#include <cwchar>

// A semicolon-separated list of exact DLL basenames. Never accept wildcards
// or paths from the list: an opt-in entry must identify one specific module.
inline bool matchesOptionalDllBlocklist(const wchar_t* path, const wchar_t* list)
{
    if (path == nullptr || list == nullptr) {
        return false;
    }

    const wchar_t* basename = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            basename = p + 1;
        }
    }
    const size_t nameLength = wcslen(basename);
    if (nameLength < 5 || _wcsicmp(basename + nameLength - 4, L".dll") != 0) {
        return false;
    }

    while (*list != L'\0') {
        const wchar_t* end = wcschr(list, L';');
        if (end == nullptr) {
            end = list + wcslen(list);
        }
        while (list < end && (*list == L' ' || *list == L'\t')) {
            ++list;
        }
        const wchar_t* trimmedEnd = end;
        while (trimmedEnd > list && (trimmedEnd[-1] == L' ' || trimmedEnd[-1] == L'\t')) {
            --trimmedEnd;
        }
        if (static_cast<size_t>(trimmedEnd - list) == nameLength &&
            _wcsnicmp(list, basename, nameLength) == 0) {
            return true;
        }
        list = *end == L';' ? end + 1 : end;
    }
    return false;
}
