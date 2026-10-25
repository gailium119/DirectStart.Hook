/*
 * DirectStart - runtime for the standalone Windows 8.1 start-menu d3d11 proxy.
 *
 * Uses:
 *   - libvalinet valinet/pdb/pdb.h   PDB download + symbol lookup
 *   - libvalinet valinet/ini/ini.h   function addresses / configuration
 *   - MinHook                        inline hooks
 *
 * No Windhawk compatibility layer is involved.
 */

#include "DirectStart.h"
#include "KnownSymbols.g.h"

#include <shlobj.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

#include <valinet/pdb/pdb.h>

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#include <valinet/ini/ini.h>
#undef min

#undef ASSERT // libvalinet's pdb.h ASSERT terminates the process

#include "MinHook.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{
    constexpr wchar_t kSettingsKey[] = L"Software\\DirectStart\\StartMenu";
    constexpr wchar_t kLogFileName[] = L"StartMenu.log";
    constexpr DWORD kMaxLogSize = 8 * 1024 * 1024;
    constexpr DWORD64 kAbsoluteAddressThreshold = 0x100000000ULL;

    CRITICAL_SECTION g_logLock;
    INIT_ONCE g_logLockOnce = INIT_ONCE_STATIC_INIT;
    volatile LONG g_logEnabled = 0;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    std::wstring g_logPath;
    std::wstring g_dllTimestamp;

    INIT_ONCE g_settingsOnce = INIT_ONCE_STATIC_INIT;
    BOOL g_startMenuEnabled = TRUE;

    INIT_ONCE g_hookOnce = INIT_ONCE_STATIC_INIT;

    // -----------------------------------------------------------------------
    // Basic helpers
    // -----------------------------------------------------------------------

    std::wstring GetLocalAppDataDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(
                nullptr,
                CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                nullptr,
                SHGFP_TYPE_CURRENT,
                path)) &&
            path[0])
        {
            return path;
        }

        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", path, static_cast<DWORD>(_countof(path)));
        if (length > 0 && length < _countof(path))
        {
            return path;
        }
        return {};
    }

    std::wstring GetModulePathW(HMODULE module)
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                module, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < path.size() - 1)
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    std::wstring BaseName(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    void MakeLower(std::wstring& value)
    {
        for (wchar_t& character : value)
        {
            character = static_cast<wchar_t>(towlower(character));
        }
    }

    std::wstring Trim(const std::wstring& value)
    {
        const size_t begin = value.find_first_not_of(L" \t\r\n");
        if (begin == std::wstring::npos)
        {
            return {};
        }
        const size_t end = value.find_last_not_of(L" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    std::string ToAnsi(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int length = WideCharToMultiByte(
            CP_ACP,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }

        std::string result(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(
            CP_ACP,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr);
        return result;
    }

    std::wstring FromAnsi(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int length = MultiByteToWideChar(
            CP_ACP,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (length <= 0)
        {
            return {};
        }

        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_ACP,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            result.data(),
            length);
        return result;
    }

    bool EnsureDirectory(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 2)
        {
            EnsureDirectory(path.substr(0, slash));
        }

        return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
            GetLastError() == ERROR_ALREADY_EXISTS;
    }

    DWORD GetImageSize(HMODULE module)
    {
        if (!module)
        {
            return 0;
        }

        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        {
            return 0;
        }

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }
        return nt->OptionalHeader.SizeOfImage;
    }

    // -----------------------------------------------------------------------
    // Logging
    // -----------------------------------------------------------------------

    BOOL CALLBACK InitLogLock(PINIT_ONCE, PVOID, PVOID*)
    {
        InitializeCriticalSection(&g_logLock);
        return TRUE;
    }

    bool OpenLogFile()
    {
        if (g_logPath.empty())
        {
            return false;
        }
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            return true;
        }

        g_logFile = CreateFileW(
            g_logPath.c_str(),
            FILE_APPEND_DATA | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return g_logFile != INVALID_HANDLE_VALUE;
    }

    bool RecreateLogFile()
    {
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        if (g_logPath.empty())
        {
            return false;
        }

        // A stale/locked log is the usual write failure. Delete and recreate it
        // once instead of silently dropping the remaining diagnostics.
        DeleteFileW(g_logPath.c_str());
        g_logFile = CreateFileW(
            g_logPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return g_logFile != INVALID_HANDLE_VALUE;
    }

    std::wstring GetTempLogPath()
    {
        wchar_t directory[MAX_PATH] = {};
        const DWORD length =
            GetTempPathW(static_cast<DWORD>(_countof(directory)), directory);
        if (length == 0 || length >= _countof(directory))
        {
            return {};
        }
        return std::wstring(directory) + L"DirectStart-StartMenu.log";
    }

    std::wstring GetDllTimestamp()
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllTimestamp),
                &module))
        {
            return L"unknown";
        }

        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(module, path, _countof(path)))
        {
            return L"unknown";
        }

        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExW(
                path, GetFileExInfoStandard, &attributes))
        {
            return L"unknown";
        }

        FILETIME localTime = {};
        SYSTEMTIME time = {};
        if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localTime) ||
            !FileTimeToSystemTime(&localTime, &time))
        {
            return L"unknown";
        }

        wchar_t text[64] = {};
        swprintf_s(
            text,
            L"%04u-%02u-%02u %02u:%02u:%02u",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond);
        return text;
    }

    void RestartExplorer()
    {
        wchar_t comspec[MAX_PATH] = L"C:\\Windows\\System32\\cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, _countof(comspec));

        wchar_t command[] =
            L"/c taskkill /f /im explorer.exe & "
            L"ping -n 2 127.0.0.1 >nul & start \"\" explorer.exe";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(
                comspec,
                command,
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            Sleep(300);
        }
        ExitProcess(0);
    }

    void ReportLogFailure(DWORD error, const std::wstring& path)
    {
        wchar_t text[1024] = {};
        swprintf_s(
            text,
            L"DirectStart cannot write its log file:\n%s\n\n"
            L"Error: %lu\n\nExplorer will be restarted.",
            path.empty() ? L"<no path>" : path.c_str(),
            error);
        MessageBoxW(
            nullptr,
            text,
            L"DirectStart logging error",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        RestartExplorer();
    }

    // -----------------------------------------------------------------------
    // Registry settings
    // -----------------------------------------------------------------------

    BOOL CALLBACK InitSettings(PINIT_ONCE, PVOID, PVOID*)
    {
        g_startMenuEnabled = DS_GetIntValue(L"enabled", 1) != 0;
        return TRUE;
    }

    bool RegQueryDword(HKEY root, const wchar_t* name, DWORD* value)
    {
        DWORD data = 0;
        DWORD size = sizeof(data);
        const LSTATUS status = RegGetValueW(
            root,
            kSettingsKey,
            name,
            RRF_RT_REG_DWORD,
            nullptr,
            &data,
            &size);
        if (status == ERROR_SUCCESS)
        {
            *value = data;
            return true;
        }
        return false;
    }

    bool RegQueryString(HKEY root, const wchar_t* name, std::wstring& value)
    {
        DWORD size = 0;
        LSTATUS status = RegGetValueW(
            root,
            kSettingsKey,
            name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            nullptr,
            &size);
        if (status != ERROR_SUCCESS || size < sizeof(wchar_t))
        {
            return false;
        }

        std::wstring buffer(size / sizeof(wchar_t), L'\0');
        status = RegGetValueW(
            root,
            kSettingsKey,
            name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            buffer.data(),
            &size);
        if (status != ERROR_SUCCESS)
        {
            return false;
        }

        while (!buffer.empty() && buffer.back() == L'\0')
        {
            buffer.pop_back();
        }
        value.swap(buffer);
        return true;
    }

    // -----------------------------------------------------------------------
    // INI (libvalinet valinet/ini/ini.h)
    // -----------------------------------------------------------------------

    std::wstring GetIniString(
        const wchar_t* section,
        const wchar_t* key)
    {
        wchar_t buffer[512] = {};
        VnGetString(
            section,
            key,
            buffer,
            static_cast<DWORD>(_countof(buffer)),
            static_cast<DWORD>(_countof(buffer) - 1),
            L"",
            DS_GetIniPath());
        return buffer;
    }

    void SetIniString(
        const wchar_t* section,
        const wchar_t* key,
        const std::wstring& value)
    {
        VnWriteString(section, key, value.c_str(), DS_GetIniPath());
    }

    // -----------------------------------------------------------------------
    // Module identity and INI addresses
    // -----------------------------------------------------------------------

    std::wstring GetModuleSection(HMODULE module)
    {
        std::wstring section = BaseName(GetModulePathW(module));
        const size_t dot = section.find_last_of(L'.');
        if (dot != std::wstring::npos)
        {
            section.resize(dot);
        }
        MakeLower(section);
        return section;
    }

    std::wstring GetFileSha256(const std::wstring& modulePath)
    {
        if (modulePath.empty())
        {
            return {};
        }

        HANDLE file = CreateFileW(
            modulePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD resultLength = 0;
        std::vector<BYTE> object;
        std::vector<BYTE> digest;
        std::wstring result;

        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status >= 0)
        {
            status = BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &resultLength,
                0);
        }
        if (status >= 0)
        {
            status = BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashLength),
                sizeof(hashLength),
                &resultLength,
                0);
        }
        if (status >= 0 && objectLength && hashLength)
        {
            object.resize(objectLength);
            digest.resize(hashLength);
            status = BCryptCreateHash(
                algorithm,
                &hash,
                object.data(),
                objectLength,
                nullptr,
                0,
                0);
        }
        if (status >= 0)
        {
            BYTE buffer[64 * 1024];
            DWORD read = 0;
            while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) &&
                   read != 0)
            {
                status = BCryptHashData(hash, buffer, read, 0);
                if (status < 0)
                {
                    break;
                }
            }
        }
        if (status >= 0)
        {
            status = BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
        if (status >= 0)
        {
            static const wchar_t kHex[] = L"0123456789ABCDEF";
            result.resize(digest.size() * 2);
            for (size_t i = 0; i < digest.size(); ++i)
            {
                result[i * 2] = kHex[digest[i] >> 4];
                result[i * 2 + 1] = kHex[digest[i] & 0x0F];
            }
        }

        if (hash)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        CloseHandle(file);
        return result;
    }

    std::wstring GetModuleFingerprint(const std::wstring& modulePath)
    {
        // Cache identity is the module SHA-256. Version/size/time stamps are
        // not stable enough across servicing and VM snapshots.
        return GetFileSha256(modulePath);
    }

    bool ParseAddress(
        const std::wstring& raw,
        DWORD64 moduleBase,
        DWORD imageSize,
        DWORD64* address)
    {
        if (!address || imageSize == 0)
        {
            return false;
        }

        std::wstring value = Trim(raw);
        if (value.empty())
        {
            return false;
        }

        bool absolute = false;
        const wchar_t* cursor = value.c_str();
        if (wcsncmp(cursor, L"va+", 3) == 0)
        {
            absolute = true;
            cursor += 3;
        }
        else if (wcsncmp(cursor, L"module+", 7) == 0)
        {
            cursor += 7;
        }
        else if (wcsncmp(cursor, L"rva+", 4) == 0)
        {
            cursor += 4;
        }

        while (*cursor == L' ' || *cursor == L'\t')
        {
            ++cursor;
        }
        if (cursor[0] == L'0' && (cursor[1] == L'x' || cursor[1] == L'X'))
        {
            cursor += 2;
        }

        wchar_t* end = nullptr;
        const unsigned long long number = wcstoull(cursor, &end, 16);
        if (end == cursor)
        {
            return false;
        }

        if (!absolute && number >= kAbsoluteAddressThreshold)
        {
            absolute = true;
        }

        if (!absolute && number == 0)
        {
            return false; // 0 is not a valid RVA (it points at the PE header)
        }

        const DWORD64 result = absolute
            ? static_cast<DWORD64>(number)
            : moduleBase + static_cast<DWORD64>(number);
        if (result < moduleBase || result >= moduleBase + imageSize)
        {
            return false;
        }

        *address = result;
        return true;
    }

    // Converts a Windhawk-style signature into a public PDB symbol name.
    std::wstring NormalizeSymbolName(const wchar_t* signature)
    {
        if (!signature || !*signature)
        {
            return {};
        }

        std::wstring text(signature);
        const size_t parenthesis = text.find(L'(');
        if (parenthesis != std::wstring::npos)
        {
            text.resize(parenthesis);
        }

        const size_t end = text.find_last_not_of(L" \t\r\n");
        if (end == std::wstring::npos)
        {
            return {};
        }
        text.resize(end + 1);

        const size_t start = text.find_last_of(L" \t");
        std::wstring token = start == std::wstring::npos
            ? text
            : text.substr(start + 1);

        while (!token.empty() &&
            (token.back() == L'*' || token.back() == L'&'))
        {
            token.pop_back();
        }

        if (token == L"__cdecl" || token == L"__stdcall" ||
            token == L"__fastcall" || token == L"__thiscall")
        {
            return {};
        }
        return token;
    }

    // -----------------------------------------------------------------------
    // Symbol resolution helpers
    // -----------------------------------------------------------------------

    struct SymbolRequest
    {
        std::size_t hookIndex;
        std::wstring name;
    };

    // VnGetSymbols requires every requested name to match exactly one symbol.
    // If the whole group fails (one missing/ambiguous name), split it in half
    // and retry. A single missing symbol then costs O(log n) PDB loads instead
    // of one load per symbol.
    void ResolveSymbolGroup(
        const std::string& pdbAnsi,
        DWORD imageSize,
        const std::vector<SymbolRequest>& requests,
        std::vector<DWORD>& rvas,
        std::vector<BOOL>& resolved,
        std::vector<std::wstring>& matchedNames)
    {
        if (requests.empty())
        {
            return;
        }

        std::vector<std::string> storage(requests.size());
        std::vector<char*> names(requests.size(), nullptr);
        std::vector<DWORD> addresses(requests.size(), 0);
        bool convertible = true;

        for (std::size_t index = 0; index < requests.size(); ++index)
        {
            storage[index] = ToAnsi(requests[index].name);
            if (storage[index].empty())
            {
                convertible = false;
                break;
            }
        }

        if (convertible)
        {
            for (std::size_t index = 0; index < requests.size(); ++index)
            {
                names[index] = &storage[index][0];
            }

            if (VnGetSymbols(
                    pdbAnsi.c_str(),
                    addresses.data(),
                    names.data(),
                    static_cast<DWORD>(requests.size())) == 0)
            {
                for (std::size_t index = 0; index < requests.size(); ++index)
                {
                    if (addresses[index] >= imageSize)
                    {
                        continue;
                    }
                    const std::size_t hookIndex = requests[index].hookIndex;
                    rvas[hookIndex] = addresses[index];
                    resolved[hookIndex] = TRUE;
                    matchedNames[hookIndex] = requests[index].name;
                }
                return;
            }
        }

        if (requests.size() == 1)
        {
            return;
        }

        const std::size_t middle = requests.size() / 2;
        ResolveSymbolGroup(
            pdbAnsi,
            imageSize,
            std::vector<SymbolRequest>(
                requests.begin(), requests.begin() + middle),
            rvas,
            resolved,
            matchedNames);
        ResolveSymbolGroup(
            pdbAnsi,
            imageSize,
            std::vector<SymbolRequest>(
                requests.begin() + middle, requests.end()),
            rvas,
            resolved,
            matchedNames);
    }

    // -----------------------------------------------------------------------
    // PDB cache (libvalinet valinet/pdb/pdb.h)
    // -----------------------------------------------------------------------

    bool FileTimeOlderThan(
        const std::wstring& file,
        const std::wstring& reference)
    {
        WIN32_FILE_ATTRIBUTE_DATA fileData = {};
        if (!GetFileAttributesExW(
                file.c_str(), GetFileExInfoStandard, &fileData))
        {
            return true;
        }

        WIN32_FILE_ATTRIBUTE_DATA referenceData = {};
        if (!GetFileAttributesExW(
                reference.c_str(), GetFileExInfoStandard, &referenceData))
        {
            return false;
        }

        return CompareFileTime(
            &fileData.ftLastWriteTime,
            &referenceData.ftLastWriteTime) < 0;
    }

    bool DownloadPdb(
        const std::wstring& modulePath,
        const std::wstring& pdbPath,
        std::wstring& result)
    {
        const std::string moduleAnsi = ToAnsi(modulePath);
        const std::string pdbAnsi = ToAnsi(pdbPath);
        if (moduleAnsi.empty() || pdbAnsi.empty() ||
            pdbAnsi.size() >= MAX_PATH)
        {
            return false;
        }

        char buffer[MAX_PATH] = {};
        strncpy_s(buffer, pdbAnsi.c_str(), _TRUNCATE);

        DS_Log(
            L"Symbols: downloading PDB for %s (target %s)",
            modulePath.c_str(),
            pdbPath.c_str());
        const int rc = VnDownloadSymbols(
            nullptr,
            const_cast<char*>(moduleAnsi.c_str()),
            buffer,
            MAX_PATH);
        if (rc != 0)
        {
            DS_Log(
                L"Symbols: VnDownloadSymbols failed rc=%d for %s",
                rc,
                modulePath.c_str());
            return false;
        }

        result = FromAnsi(buffer);
        if (result.empty())
        {
            result = pdbPath;
        }
        DS_Log(L"Symbols: PDB ready at %s", result.c_str());
        return true;
    }

    bool IsValidPdbFile(const std::wstring& path)
    {
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LARGE_INTEGER size = {};
        const bool sized = GetFileSizeEx(file, &size) != FALSE;
        char header[32] = {};
        DWORD read = 0;
        const bool readOk =
            ReadFile(file, header, sizeof(header), &read, nullptr) != FALSE;
        CloseHandle(file);

        static const char kMagic[] =
            "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS";
        const DWORD magicLength = static_cast<DWORD>(sizeof(kMagic) - 1);
        return sized && size.QuadPart >= 0x1000 && readOk &&
            read >= magicLength &&
            memcmp(header, kMagic, magicLength) == 0;
    }

    std::wstring EnsurePdb(HMODULE module, bool forceDownload = false)
    {
        static std::unordered_map<HMODULE, std::wstring> cache;
        if (!forceDownload)
        {
            const auto cached = cache.find(module);
            if (cached != cache.end())
            {
                return cached->second;
            }
        }

        const std::wstring modulePath = GetModulePathW(module);
        if (modulePath.empty())
        {
            return {};
        }

        std::wstring moduleName = BaseName(modulePath);
        const size_t dot = moduleName.find_last_of(L'.');
        if (dot != std::wstring::npos)
        {
            moduleName.resize(dot);
        }
        MakeLower(moduleName);

        const std::wstring directory = DS_GetSymbolsDir();
        if (directory.empty())
        {
            return {};
        }

        const std::wstring pdbPath =
            directory + L"\\" + moduleName + L".pdb";
        bool exists =
            GetFileAttributesW(pdbPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (exists && !IsValidPdbFile(pdbPath))
        {
            DS_Log(
                L"Symbols: local PDB is invalid/truncated, deleting %s",
                pdbPath.c_str());
            DeleteFileW(pdbPath.c_str());
            exists = false;
        }

        // Local-first: use an existing PDB even if it is older than the module.
        // Only re-download when the caller says reading it failed.
        if (exists && !forceDownload)
        {
            DS_Log(L"Symbols: using local PDB %s", pdbPath.c_str());
            cache[module] = pdbPath;
            return pdbPath;
        }

        std::wstring downloaded;
        if (DownloadPdb(modulePath, pdbPath, downloaded))
        {
            cache[module] = downloaded;
            return downloaded;
        }
        if (!exists)
        {
            return {};
        }

        // A failed refresh may have removed the local file; do not pretend it
        // is still available.
        if (GetFileAttributesW(pdbPath.c_str()) ==
            INVALID_FILE_ATTRIBUTES)
        {
            return {};
        }

        DS_Log(
            L"Symbols: download failed, using local PDB %s",
            pdbPath.c_str());
        cache[module] = pdbPath;
        return pdbPath;
    }

    // -----------------------------------------------------------------------
    // Hooking
    // -----------------------------------------------------------------------

    BOOL CALLBACK InitHooks(PINIT_ONCE, PVOID, PVOID*)
    {
        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            DS_Log(
                L"MH_Initialize failed with status %d",
                static_cast<int>(status));
            return FALSE;
        }
        return TRUE;
    }

    void* ResolveExport(const wchar_t* moduleName, const char* procName)
    {
        HMODULE module = GetModuleHandleW(moduleName);
        if (!module)
        {
            module = LoadLibraryExW(
                moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        return module
            ? reinterpret_cast<void*>(GetProcAddress(module, procName))
            : nullptr;
    }

    void* FindImportedFunctionAddress(HMODULE module, const char* procName)
    {
        if (!module || !procName)
        {
            return nullptr;
        }

        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        {
            return nullptr;
        }

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return nullptr;
        }

        const IMAGE_DATA_DIRECTORY& imports =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!imports.VirtualAddress ||
            imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
        {
            return nullptr;
        }

        auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            base + imports.VirtualAddress);
        for (; descriptor->Name; ++descriptor)
        {
            const DWORD nameTableRva = descriptor->OriginalFirstThunk
                ? descriptor->OriginalFirstThunk
                : descriptor->FirstThunk;
            if (!nameTableRva || !descriptor->FirstThunk)
            {
                continue;
            }

            const auto* nameThunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                base + nameTableRva);
            const auto* addressThunk =
                reinterpret_cast<const IMAGE_THUNK_DATA*>(
                    base + descriptor->FirstThunk);

            for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk)
            {
                if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
                {
                    continue;
                }

                const auto* importName =
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                        base + nameThunk->u1.AddressOfData);
                if (strcmp(
                        reinterpret_cast<const char*>(importName->Name),
                        procName) == 0)
                {
                    return reinterpret_cast<void*>(
                        addressThunk->u1.Function);
                }
            }
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DS_InitLog()
{
    InitOnceExecuteOnce(&g_logLockOnce, InitLogLock, nullptr, nullptr);

    g_dllTimestamp = GetDllTimestamp();

    std::wstring directory = GetLocalAppDataDirectory();
    std::wstring path;
    if (!directory.empty())
    {
        directory += L"\\DirectStart";
        CreateDirectoryW(directory.c_str(), nullptr);
        path = directory + L"\\" + kLogFileName;
    }
    else
    {
        // Never fall back to the explorer.exe directory (C:\Windows).
        path = GetTempLogPath();
    }
    if (path.empty())
    {
        path = GetTempLogPath();
    }
    g_logPath = path;

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!g_logPath.empty() &&
        GetFileAttributesExW(
            g_logPath.c_str(), GetFileExInfoStandard, &attributes) &&
        attributes.nFileSizeHigh == 0 &&
        attributes.nFileSizeLow > kMaxLogSize)
    {
        const std::wstring oldPath = g_logPath + L".old";
        MoveFileExW(
            g_logPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }

    if (!OpenLogFile() && !RecreateLogFile())
    {
        const std::wstring tempPath = GetTempLogPath();
        if (!tempPath.empty() && tempPath != g_logPath)
        {
            g_logPath = tempPath;
            RecreateLogFile();
        }
    }
    LeaveCriticalSection(&g_logLock);
}

BOOL DS_SetLogEnabled(BOOL enabled)
{
    return InterlockedExchange(&g_logEnabled, enabled ? 1 : 0) != 0;
}

BOOL DS_IsLogEnabled()
{
    return g_logEnabled != 0;
}

void DS_Log(const wchar_t* format, ...)
{
    if (!g_logEnabled || !format)
    {
        return;
    }

    wchar_t message[4096] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringW(L"[DirectStart] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");

    InitOnceExecuteOnce(&g_logLockOnce, InitLogLock, nullptr, nullptr);
    if (!TryEnterCriticalSection(&g_logLock))
    {
        return; // Never deadlock a crashing thread on its own log entry.
    }

    if (g_dllTimestamp.empty())
    {
        g_dllTimestamp = GetDllTimestamp();
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t prefix[256] = {};
    swprintf_s(
        prefix,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] [d3d11 %s] ",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        GetCurrentThreadId(),
        g_dllTimestamp.c_str());

    std::wstring line;
    line.reserve(wcslen(prefix) + wcslen(message) + 2);
    line += prefix;
    line += message;
    line += L"\r\n";

    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        if (!OpenLogFile())
        {
            RecreateLogFile();
        }
    }

    auto writeLine = [&]() -> bool
    {
        if (g_logFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const DWORD bytes =
            static_cast<DWORD>(line.size() * sizeof(wchar_t));
        DWORD written = 0;
        return WriteFile(g_logFile, line.data(), bytes, &written, nullptr) &&
            written == bytes;
    };

    bool failed = false;
    DWORD error = ERROR_WRITE_FAULT;
    const std::wstring failedPath = g_logPath;

    if (!writeLine())
    {
        error = GetLastError();
        if (!RecreateLogFile() || !writeLine())
        {
            if (error == ERROR_SUCCESS)
            {
                error = GetLastError();
            }
            failed = true;
        }
    }

    LeaveCriticalSection(&g_logLock);

    if (failed)
    {
        ReportLogFailure(error, failedPath);
    }
}

int DS_GetIntValue(const wchar_t* name, int defaultValue)
{
    if (!name)
    {
        return defaultValue;
    }

    DWORD value = 0;
    if (RegQueryDword(HKEY_CURRENT_USER, name, &value) ||
        RegQueryDword(HKEY_LOCAL_MACHINE, name, &value))
    {
        return static_cast<int>(value);
    }
    return defaultValue;
}

int DS_GetIntSetting(const wchar_t* name)
{
    return DS_GetIntValue(name, 0);
}

PCWSTR DS_GetStringSetting(const wchar_t* name)
{
    std::wstring value;
    if (name &&
        !RegQueryString(HKEY_CURRENT_USER, name, value) &&
        !RegQueryString(HKEY_LOCAL_MACHINE, name, value))
    {
        value.clear();
    }

    const size_t characters = value.size() + 1;
    wchar_t* result = static_cast<wchar_t*>(
        malloc(characters * sizeof(wchar_t)));
    if (!result)
    {
        return nullptr;
    }

    if (!value.empty())
    {
        memcpy(result, value.c_str(), (characters - 1) * sizeof(wchar_t));
    }
    result[characters - 1] = L'\0';
    return result;
}

void DS_FreeStringSetting(PCWSTR value)
{
    free(const_cast<wchar_t*>(value));
}

bool DS_IsEnabled()
{
    InitOnceExecuteOnce(&g_settingsOnce, InitSettings, nullptr, nullptr);
    return g_startMenuEnabled != FALSE;
}

PCWSTR DS_GetIniPath()
{
    static const std::wstring path = []() -> std::wstring {
        PCWSTR configured = DS_GetStringSetting(L"ini");
        std::wstring result;
        if (configured && *configured)
        {
            result = configured;
        }
        else
        {
            const std::wstring directory = GetLocalAppDataDirectory();
            if (!directory.empty())
            {
                result = directory + L"\\DirectStart\\DirectStart.ini";
            }
        }
        DS_FreeStringSetting(configured);

        if (!result.empty())
        {
            const size_t slash = result.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                EnsureDirectory(result.substr(0, slash));
            }
        }
        return result;
    }();
    return path.c_str();
}

PCWSTR DS_GetSymbolsDir()
{
    static const std::wstring path = []() -> std::wstring {
        PCWSTR configured = DS_GetStringSetting(L"symbols");
        std::wstring result;
        if (configured && *configured)
        {
            result = configured;
        }
        else
        {
            const std::wstring directory = GetLocalAppDataDirectory();
            if (!directory.empty())
            {
                result = directory + L"\\DirectStart\\symbols";
            }
        }
        DS_FreeStringSetting(configured);

        if (!result.empty())
        {
            EnsureDirectory(result);
        }
        return result;
    }();
    return path.c_str();
}

BOOL DS_InitHookEngine()
{
    return InitOnceExecuteOnce(&g_hookOnce, InitHooks, nullptr, nullptr);
}

BOOL DS_HookFunction(void* target, void* hook, void** original)
{
    if (original)
    {
        *original = nullptr;
    }
    if (!target || !hook)
    {
        return FALSE;
    }
    if (!DS_InitHookEngine())
    {
        return FALSE;
    }

    void* trampoline = nullptr;
    MH_STATUS status = MH_CreateHook(target, hook, &trampoline);
    if (status != MH_OK)
    {
        DS_Log(
            L"MH_CreateHook target=%p failed with status %d",
            target,
            static_cast<int>(status));
        return FALSE;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        DS_Log(
            L"MH_EnableHook target=%p failed with status %d",
            target,
            static_cast<int>(status));
        MH_RemoveHook(target);
        return FALSE;
    }

    if (original)
    {
        *original = trampoline;
    }
    return TRUE;
}

void* DS_GetLoadStringWTarget()
{
    // Explorer imports LoadStringW from api-ms-win-core-libraryloader-l1-2-0,
    // which resolves to kernelbase!LoadStringW. user32!LoadStringW is a
    // different function and must not be preferred here.
    void* address = ResolveExport(
        L"api-ms-win-core-libraryloader-l1-2-0.dll", "LoadStringW");
    if (!address)
    {
        address = ResolveExport(L"kernelbase.dll", "LoadStringW");
    }
    if (!address)
    {
        address = ResolveExport(L"kernel32.dll", "LoadStringW");
    }
    if (!address)
    {
        address = FindImportedFunctionAddress(
            GetModuleHandleW(nullptr), "LoadStringW");
    }
    return address;
}

    const DS_KNOWN_MODULE* FindKnownModule(
        const std::wstring& modulePath,
        const std::wstring& sha256)
    {
        const std::wstring base = BaseName(modulePath);
        for (const DS_KNOWN_MODULE& known : kKnownModules)
        {
            if (_wcsicmp(base.c_str(), known.name) == 0 &&
                _wcsicmp(sha256.c_str(), known.sha256) == 0)
            {
                return &known;
            }
        }
        return nullptr;
    }

    bool FindKnownRva(
        const DS_KNOWN_MODULE* known,
        const std::wstring& name,
        DWORD* rva)
    {
        if (!known || !rva)
        {
            return false;
        }
        for (std::size_t i = 0; i < known->count; ++i)
        {
            if (_wcsicmp(name.c_str(), known->symbols[i].name) == 0)
            {
                *rva = known->symbols[i].rva;
                return true;
            }
        }
        return false;
    }

BOOL DS_HookSymbols(
    HMODULE module,
    const DS_SYMBOL_HOOK* hooks,
    std::size_t count)
{
    if (!module || !hooks || count == 0)
    {
        return FALSE;
    }

    const DWORD64 moduleBase = reinterpret_cast<DWORD64>(module);
    const DWORD imageSize = GetImageSize(module);
    if (imageSize == 0)
    {
        DS_Log(L"Symbols: invalid image for module %p", module);
        return FALSE;
    }

    const std::wstring modulePath = GetModulePathW(module);
    const std::wstring section = GetModuleSection(module);
    const std::wstring fingerprint = GetModuleFingerprint(modulePath);

    const DS_KNOWN_MODULE* known =
        FindKnownModule(modulePath, fingerprint);
    if (known)
    {
        DS_Log(
            L"%s: embedded table matched %s SHA256=%s version=%s",
            section.c_str(),
            known->name,
            fingerprint.c_str(),
            known->version);
    }
    else
    {
        DS_Log(
            L"%s: no embedded table for SHA256=%s; falling back to INI/PDB",
            section.c_str(),
            fingerprint.c_str());
    }

    std::vector<DWORD> rvas(count, 0);
    std::vector<BOOL> resolved(count, FALSE);
    std::vector<BOOL> absent(count, FALSE);
    std::vector<std::wstring> matchedNames(count);

    struct PendingSymbol
    {
        std::size_t hookIndex;
        std::vector<std::wstring> names;
    };
    std::vector<PendingSymbol> pending;

    for (std::size_t index = 0; index < count; ++index)
    {
        PendingSymbol item;
        item.hookIndex = index;

        for (std::size_t candidate = 0;
             candidate < _countof(hooks[index].symbols) &&
                 hooks[index].symbols[candidate];
             ++candidate)
        {
            const std::wstring name =
                NormalizeSymbolName(hooks[index].symbols[candidate]);
            if (name.empty() ||
                std::find(item.names.begin(), item.names.end(), name) !=
                    item.names.end())
            {
                continue;
            }
            item.names.push_back(name);

            DWORD knownRva = 0;
            if (known && FindKnownRva(known, name, &knownRva))
            {
                rvas[index] = knownRva;
                resolved[index] = TRUE;
                matchedNames[index] = name;
                DS_Log(
                    L"%s: %s from embedded -> module+0x%lX",
                    section.c_str(),
                    name.c_str(),
                    static_cast<unsigned long>(knownRva));
                break;
            }
            if (known)
            {
                // Exact SHA-256 match: do not trust an INI entry that might
                // belong to another build. Missing optional symbols go to PDB.
                continue;
            }

            DWORD64 address = 0;
            const std::wstring iniValue =
                Trim(GetIniString(section.c_str(), name.c_str()));
            if (iniValue == L"0x0" || iniValue == L"0X0" ||
                iniValue == L"0")
            {
                absent[index] = TRUE;
                resolved[index] = TRUE;
                matchedNames[index] = name;
                DS_Log(
                    L"%s: %s marked absent in ini",
                    section.c_str(),
                    name.c_str());
                break;
            }
            if (ParseAddress(
                    iniValue,
                    moduleBase,
                    imageSize,
                    &address))
            {
                rvas[index] = static_cast<DWORD>(address - moduleBase);
                resolved[index] = TRUE;
                matchedNames[index] = name;
                DS_Log(
                    L"%s: %s from ini -> %p",
                    section.c_str(),
                    name.c_str(),
                    reinterpret_cast<void*>(address));
                break;
            }
        }

        if (!resolved[index] && !item.names.empty())
        {
            pending.push_back(std::move(item));
        }
    }

    if (!pending.empty())
    {
        const std::wstring pdbPath = EnsurePdb(module);
        const std::string pdbAnsi = ToAnsi(pdbPath);
        if (!pdbPath.empty() && !pdbAnsi.empty())
        {
            std::vector<SymbolRequest> requests;
            requests.reserve(pending.size());
            for (const PendingSymbol& item : pending)
            {
                if (!item.names.empty())
                {
                    requests.push_back(
                        SymbolRequest{item.hookIndex, item.names.front()});
                }
            }

            ResolveSymbolGroup(
                pdbAnsi,
                imageSize,
                requests,
                rvas,
                resolved,
                matchedNames);

            auto countResolved = [&]() -> std::size_t
            {
                std::size_t count = 0;
                for (const PendingSymbol& item : pending)
                {
                    if (resolved[item.hookIndex])
                    {
                        ++count;
                    }
                }
                return count;
            };

            // Do not re-download just because this build's PDB lacks some
            // symbols: a missing symbol means the DLL build does not export or
            // contain that function, not that the PDB is unreadable.
            // Re-download only happens in EnsurePdb when the local file is
            // missing or fails the MSF validity check.

            // A few entries carry an alternative (renamed) symbol name.
            for (const PendingSymbol& item : pending)
            {
                if (resolved[item.hookIndex])
                {
                    continue;
                }

                for (std::size_t nameIndex = 1;
                     nameIndex < item.names.size();
                     ++nameIndex)
                {
                    ResolveSymbolGroup(
                        pdbAnsi,
                        imageSize,
                        std::vector<SymbolRequest>{
                            SymbolRequest{
                                item.hookIndex,
                                item.names[nameIndex]}},
                        rvas,
                        resolved,
                        matchedNames);
                    if (resolved[item.hookIndex])
                    {
                        break;
                    }
                }
            }

            // The PDB is usable, so any still-unresolved entries are genuinely
            // absent from this DLL build. Cache 0x0 so the next start skips
            // PDB lookup for them instead of downloading again.
            for (PendingSymbol& item : pending)
            {
                if (resolved[item.hookIndex])
                {
                    continue;
                }

                absent[item.hookIndex] = TRUE;
                resolved[item.hookIndex] = TRUE;
                matchedNames[item.hookIndex] =
                    item.names.empty() ? std::wstring() : item.names.front();
                for (const std::wstring& name : item.names)
                {
                    SetIniString(section.c_str(), name.c_str(), L"0x0");
                }
                DS_Log(
                    L"%s: %s absent from PDB; cached as 0x0",
                    section.c_str(),
                    item.names.empty() ? L"<empty>" : item.names.front().c_str());
            }

            const std::size_t resolvedCount = countResolved();
            DS_Log(
                L"%s: resolved %llu of %llu pending symbols from %s",
                section.c_str(),
                static_cast<unsigned long long>(resolvedCount),
                static_cast<unsigned long long>(pending.size()),
                pdbPath.c_str());

            // Cache the freshly resolved addresses for the next start.
            for (const PendingSymbol& item : pending)
            {
                if (!resolved[item.hookIndex])
                {
                    continue;
                }

                const std::wstring& name = matchedNames[item.hookIndex];
                if (!name.empty())
                {
                    wchar_t value[32] = {};
                    swprintf_s(
                        value,
                        L"0x%lX",
                        static_cast<unsigned long>(
                            rvas[item.hookIndex]));
                    SetIniString(section.c_str(), name.c_str(), value);
                }
            }
        }
        else
        {
            DS_Log(
                L"%s: no PDB available; unresolved symbols need ini addresses",
                section.c_str());
        }
    }

    // No [meta] fingerprint cache: known SHA-256 modules are resolved from
    // KnownSymbols.g.h; unknown modules use INI/PDB without a build cache.

    BOOL allRequired = TRUE;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!resolved[index])
        {
            DS_Log(
                L"%s: NOT FOUND %s%s",
                section.c_str(),
                hooks[index].symbols[0] ? hooks[index].symbols[0] : L"<empty>",
                hooks[index].optional ? L" [optional]" : L"");
            if (!hooks[index].optional)
            {
                allRequired = FALSE;
            }
            continue;
        }

        if (absent[index])
        {
            DS_Log(
                L"%s: %s absent; skipped",
                section.c_str(),
                hooks[index].symbols[0] ? hooks[index].symbols[0] : L"<empty>");
            continue;
        }

        void* target = reinterpret_cast<BYTE*>(module) + rvas[index];
        void** slot = reinterpret_cast<void**>(hooks[index].pFunction);

        if (hooks[index].hookFunction && !slot)
        {
            DS_Log(
                L"%s: hook %s has no original-pointer storage",
                section.c_str(),
                matchedNames[index].c_str());
            if (!hooks[index].optional)
            {
                allRequired = FALSE;
            }
            continue;
        }

        if (hooks[index].hookFunction)
        {
            if (*slot)
            {
                DS_Log(
                    L"%s: already hooked %s at %p",
                    section.c_str(),
                    matchedNames[index].c_str(),
                    target);
                continue;
            }

            if (!DS_HookFunction(
                    target, hooks[index].hookFunction, slot))
            {
                DS_Log(
                    L"%s: hook FAILED %s at %p",
                    section.c_str(),
                    matchedNames[index].c_str(),
                    target);
                if (!hooks[index].optional)
                {
                    allRequired = FALSE;
                }
                continue;
            }
        }
        else if (slot)
        {
            if (!*slot)
            {
                *slot = target;
            }
        }

        DS_Log(
            L"%s: %s -> %p (module+0x%lX)%s",
            section.c_str(),
            matchedNames[index].c_str(),
            target,
            static_cast<unsigned long>(rvas[index]),
            hooks[index].optional ? L"" : L" [required]");
    }

    return allRequired;
}
