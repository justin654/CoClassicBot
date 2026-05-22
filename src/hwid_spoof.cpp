// winsock2.h must precede any header that pulls windows.h (which includes the
// legacy winsock.h and conflicts with winsock2). Keep these at the very top.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   // prevent windows.h from pulling in legacy winsock.h
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "hwid_spoof.h"
#include "log.h"
#include <detours.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <random>
#include <string>

namespace HwidSpoof {

// =====================================================================
// State
// =====================================================================
static std::mutex   g_mtx;
static Identity     g_id;
static std::atomic<bool> g_ready{false};
static std::wstring g_storePath;

static Identity Snapshot()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_id;
}

// =====================================================================
// Real function pointers (trampolines)
// =====================================================================
using GetVolumeInformationW_t = BOOL (WINAPI*)(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD);
using GetVolumeInformationA_t = BOOL (WINAPI*)(LPCSTR,  LPSTR,  DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR,  DWORD);
using GetComputerNameW_t      = BOOL (WINAPI*)(LPWSTR, LPDWORD);
using GetComputerNameA_t      = BOOL (WINAPI*)(LPSTR,  LPDWORD);
using GetComputerNameExW_t    = BOOL (WINAPI*)(COMPUTER_NAME_FORMAT, LPWSTR, LPDWORD);
using GetComputerNameExA_t    = BOOL (WINAPI*)(COMPUTER_NAME_FORMAT, LPSTR,  LPDWORD);
using GetUserNameW_t          = BOOL (WINAPI*)(LPWSTR, LPDWORD);
using GetUserNameA_t          = BOOL (WINAPI*)(LPSTR,  LPDWORD);
using GetAdaptersInfo_t       = ULONG (WINAPI*)(PIP_ADAPTER_INFO, PULONG);
using GetAdaptersAddresses_t  = ULONG (WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
using RegQueryValueExW_t      = LSTATUS (APIENTRY*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQueryValueExA_t      = LSTATUS (APIENTRY*)(HKEY, LPCSTR,  LPDWORD, LPDWORD, LPBYTE, LPDWORD);

static GetVolumeInformationW_t Real_GetVolumeInformationW = nullptr;
static GetVolumeInformationA_t Real_GetVolumeInformationA = nullptr;
static GetComputerNameW_t      Real_GetComputerNameW      = nullptr;
static GetComputerNameA_t      Real_GetComputerNameA      = nullptr;
static GetComputerNameExW_t    Real_GetComputerNameExW    = nullptr;
static GetComputerNameExA_t    Real_GetComputerNameExA    = nullptr;
static GetUserNameW_t          Real_GetUserNameW          = nullptr;
static GetUserNameA_t          Real_GetUserNameA          = nullptr;
static GetAdaptersInfo_t       Real_GetAdaptersInfo       = nullptr;
static GetAdaptersAddresses_t  Real_GetAdaptersAddresses  = nullptr;
static RegQueryValueExW_t      Real_RegQueryValueExW      = nullptr;
static RegQueryValueExA_t      Real_RegQueryValueExA      = nullptr;

// =====================================================================
// Hooks — Volume serial
// =====================================================================
static BOOL WINAPI Hk_GetVolumeInformationW(
    LPCWSTR root, LPWSTR volName, DWORD volNameSize,
    LPDWORD volSerial, LPDWORD maxCompLen, LPDWORD fsFlags,
    LPWSTR fsName, DWORD fsNameSize)
{
    BOOL ok = Real_GetVolumeInformationW(root, volName, volNameSize,
                                         volSerial, maxCompLen, fsFlags,
                                         fsName, fsNameSize);
    if (ok && volSerial) {
        const Identity id = Snapshot();
        *volSerial = id.volumeSerial;
    }
    return ok;
}

static BOOL WINAPI Hk_GetVolumeInformationA(
    LPCSTR root, LPSTR volName, DWORD volNameSize,
    LPDWORD volSerial, LPDWORD maxCompLen, LPDWORD fsFlags,
    LPSTR fsName, DWORD fsNameSize)
{
    BOOL ok = Real_GetVolumeInformationA(root, volName, volNameSize,
                                         volSerial, maxCompLen, fsFlags,
                                         fsName, fsNameSize);
    if (ok && volSerial) {
        const Identity id = Snapshot();
        *volSerial = id.volumeSerial;
    }
    return ok;
}

// =====================================================================
// Hooks — Computer name (GetComputerName returns length WITHOUT null on success,
//                       required size INCLUDING null on overflow)
// =====================================================================
static BOOL CopyComputerNameW(const wchar_t* name, LPWSTR buf, LPDWORD pSize)
{
    if (!pSize) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const size_t need = wcslen(name);
    if (!buf || *pSize <= need) {
        *pSize = static_cast<DWORD>(need + 1);
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    wcscpy_s(buf, *pSize, name);
    *pSize = static_cast<DWORD>(need);
    return TRUE;
}

static BOOL CopyComputerNameA(const wchar_t* nameW, LPSTR buf, LPDWORD pSize)
{
    if (!pSize) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char ansi[64];
    const int n = WideCharToMultiByte(CP_ACP, 0, nameW, -1, ansi, sizeof(ansi), nullptr, nullptr);
    if (n <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const size_t need = static_cast<size_t>(n) - 1;  // excludes null
    if (!buf || *pSize <= need) {
        *pSize = static_cast<DWORD>(need + 1);
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    strcpy_s(buf, *pSize, ansi);
    *pSize = static_cast<DWORD>(need);
    return TRUE;
}

static BOOL WINAPI Hk_GetComputerNameW(LPWSTR buf, LPDWORD pSize)
{
    const Identity id = Snapshot();
    return CopyComputerNameW(id.computerName, buf, pSize);
}

static BOOL WINAPI Hk_GetComputerNameA(LPSTR buf, LPDWORD pSize)
{
    const Identity id = Snapshot();
    return CopyComputerNameA(id.computerName, buf, pSize);
}

static BOOL WINAPI Hk_GetComputerNameExW(COMPUTER_NAME_FORMAT fmt, LPWSTR buf, LPDWORD pSize)
{
    switch (fmt) {
    case ComputerNameNetBIOS:
    case ComputerNameDnsHostname:
    case ComputerNameDnsFullyQualified:
    case ComputerNamePhysicalNetBIOS:
    case ComputerNamePhysicalDnsHostname:
    case ComputerNamePhysicalDnsFullyQualified: {
        const Identity id = Snapshot();
        return CopyComputerNameW(id.computerName, buf, pSize);
    }
    default:
        // Domain queries delegate to the OS — we are not spoofing a domain.
        return Real_GetComputerNameExW ? Real_GetComputerNameExW(fmt, buf, pSize) : FALSE;
    }
}

static BOOL WINAPI Hk_GetComputerNameExA(COMPUTER_NAME_FORMAT fmt, LPSTR buf, LPDWORD pSize)
{
    switch (fmt) {
    case ComputerNameNetBIOS:
    case ComputerNameDnsHostname:
    case ComputerNameDnsFullyQualified:
    case ComputerNamePhysicalNetBIOS:
    case ComputerNamePhysicalDnsHostname:
    case ComputerNamePhysicalDnsFullyQualified: {
        const Identity id = Snapshot();
        return CopyComputerNameA(id.computerName, buf, pSize);
    }
    default:
        return Real_GetComputerNameExA ? Real_GetComputerNameExA(fmt, buf, pSize) : FALSE;
    }
}

// =====================================================================
// Hooks — User name (returns size INCLUDING null on both success and overflow)
// =====================================================================
static BOOL WINAPI Hk_GetUserNameW(LPWSTR buf, LPDWORD pSize)
{
    if (!pSize) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const Identity id = Snapshot();
    wchar_t wname[64] = {};
    const int n = MultiByteToWideChar(CP_ACP, 0, id.userName, -1, wname, 64);
    if (n <= 0) return Real_GetUserNameW ? Real_GetUserNameW(buf, pSize) : FALSE;
    const DWORD need = static_cast<DWORD>(n);  // includes null
    if (!buf || *pSize < need) {
        *pSize = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    wcscpy_s(buf, *pSize, wname);
    *pSize = need;
    return TRUE;
}

static BOOL WINAPI Hk_GetUserNameA(LPSTR buf, LPDWORD pSize)
{
    if (!pSize) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const Identity id = Snapshot();
    const DWORD need = static_cast<DWORD>(strlen(id.userName) + 1);
    if (!buf || *pSize < need) {
        *pSize = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    strcpy_s(buf, *pSize, id.userName);
    *pSize = need;
    return TRUE;
}

// =====================================================================
// Hooks — Network adapters (rewrite MAC in returned linked lists)
// =====================================================================
static ULONG WINAPI Hk_GetAdaptersInfo(PIP_ADAPTER_INFO info, PULONG bufLen)
{
    const ULONG rc = Real_GetAdaptersInfo(info, bufLen);
    if (rc == NO_ERROR && info) {
        const Identity id = Snapshot();
        for (PIP_ADAPTER_INFO p = info; p; p = p->Next) {
            std::memcpy(p->Address, id.macAddress, 6);
            p->AddressLength = 6;
        }
    }
    return rc;
}

static ULONG WINAPI Hk_GetAdaptersAddresses(
    ULONG family, ULONG flags, PVOID reserved,
    PIP_ADAPTER_ADDRESSES addrs, PULONG sizePtr)
{
    const ULONG rc = Real_GetAdaptersAddresses(family, flags, reserved, addrs, sizePtr);
    if (rc == NO_ERROR && addrs) {
        const Identity id = Snapshot();
        for (PIP_ADAPTER_ADDRESSES p = addrs; p; p = p->Next) {
            if (p->PhysicalAddressLength > 0) {
                std::memcpy(p->PhysicalAddress, id.macAddress, 6);
                p->PhysicalAddressLength = 6;
            }
        }
    }
    return rc;
}

// =====================================================================
// Hooks — Registry (MachineGuid + VM identifier masking)
// =====================================================================
namespace {

bool IEqualsW(LPCWSTR a, LPCWSTR b) { return a && _wcsicmp(a, b) == 0; }
bool IEqualsA(LPCSTR  a, LPCSTR  b) { return a && _stricmp(a, b) == 0; }

bool LooksLikeVmStringW(const wchar_t* data, size_t cch)
{
    if (!data || cch == 0) return false;
    std::wstring s(data, cch);
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    static const wchar_t* kNeedles[] = {
        L"VBOX", L"VIRTUAL", L"VMWARE", L"QEMU",
        L"INNOTEK", L"XEN", L"BOCHS", L"PARALLELS", L"HYPER-V",
    };
    for (auto* needle : kNeedles) {
        if (s.find(needle) != std::wstring::npos)
            return true;
    }
    return false;
}

bool LooksLikeVmStringA(const char* data, size_t cch)
{
    if (!data || cch == 0) return false;
    std::string s(data, cch);
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    static const char* kNeedles[] = {
        "VBOX", "VIRTUAL", "VMWARE", "QEMU",
        "INNOTEK", "XEN", "BOCHS", "PARALLELS", "HYPER-V",
    };
    for (auto* needle : kNeedles) {
        if (s.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Generic, plausible replacement for masked BIOS / vendor / product strings.
constexpr const wchar_t* kGenericVendorW   = L"Dell Inc.";
constexpr const wchar_t* kGenericProductW  = L"OptiPlex 7090";
constexpr const wchar_t* kGenericBiosVerW  = L"2.18.0";
constexpr const char*    kGenericVendorA   = "Dell Inc.";
constexpr const char*    kGenericProductA  = "OptiPlex 7090";
constexpr const char*    kGenericBiosVerA  = "2.18.0";

const wchar_t* PickReplacementW(LPCWSTR valueName)
{
    if (IEqualsW(valueName, L"SystemBiosVersion") ||
        IEqualsW(valueName, L"VideoBiosVersion")  ||
        IEqualsW(valueName, L"BIOSVersion"))
        return kGenericBiosVerW;
    if (IEqualsW(valueName, L"SystemProductName") ||
        IEqualsW(valueName, L"BaseBoardProduct"))
        return kGenericProductW;
    return kGenericVendorW;
}

const char* PickReplacementA(LPCSTR valueName)
{
    if (IEqualsA(valueName, "SystemBiosVersion") ||
        IEqualsA(valueName, "VideoBiosVersion")  ||
        IEqualsA(valueName, "BIOSVersion"))
        return kGenericBiosVerA;
    if (IEqualsA(valueName, "SystemProductName") ||
        IEqualsA(valueName, "BaseBoardProduct"))
        return kGenericProductA;
    return kGenericVendorA;
}

static const wchar_t* kVmTargetsW[] = {
    L"SystemBiosVersion", L"VideoBiosVersion", L"BIOSVersion",
    L"SystemManufacturer", L"SystemProductName",
    L"BIOSVendor", L"BaseBoardManufacturer", L"BaseBoardProduct",
    L"Identifier",
};
static const char* kVmTargetsA[] = {
    "SystemBiosVersion", "VideoBiosVersion", "BIOSVersion",
    "SystemManufacturer", "SystemProductName",
    "BIOSVendor", "BaseBoardManufacturer", "BaseBoardProduct",
    "Identifier",
};

}  // namespace

static LSTATUS APIENTRY Hk_RegQueryValueExW(
    HKEY hKey, LPCWSTR valueName, LPDWORD reserved,
    LPDWORD type, LPBYTE data, LPDWORD dataSize)
{
    const LSTATUS rc = Real_RegQueryValueExW(hKey, valueName, reserved, type, data, dataSize);
    if (!valueName) return rc;
    if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) return rc;

    // MachineGuid → always serve our spoofed GUID
    if (IEqualsW(valueName, L"MachineGuid")) {
        const Identity id = Snapshot();
        const DWORD need = static_cast<DWORD>((wcslen(id.machineGuid) + 1) * sizeof(wchar_t));
        if (type) *type = REG_SZ;
        if (!data || !dataSize || *dataSize < need) {
            if (dataSize) *dataSize = need;
            return data ? ERROR_MORE_DATA : ERROR_SUCCESS;
        }
        std::memcpy(data, id.machineGuid, need);
        *dataSize = need;
        return ERROR_SUCCESS;
    }

    // Mask VM-identifying registry strings (only when they actually look like one)
    if (rc != ERROR_SUCCESS || !data || !dataSize || *dataSize == 0)
        return rc;
    const DWORD t = type ? *type : REG_SZ;
    if (t != REG_SZ && t != REG_EXPAND_SZ && t != REG_MULTI_SZ)
        return rc;

    for (auto* target : kVmTargetsW) {
        if (!IEqualsW(valueName, target))
            continue;
        const size_t cch = *dataSize / sizeof(wchar_t);
        if (!LooksLikeVmStringW(reinterpret_cast<const wchar_t*>(data), cch))
            return rc;
        const wchar_t* repl = PickReplacementW(valueName);
        const DWORD replBytes = static_cast<DWORD>((wcslen(repl) + 1) * sizeof(wchar_t));
        if (*dataSize < replBytes) return rc;
        std::memcpy(data, repl, replBytes);
        *dataSize = replBytes;
        if (type) *type = REG_SZ;
        return ERROR_SUCCESS;
    }
    return rc;
}

static LSTATUS APIENTRY Hk_RegQueryValueExA(
    HKEY hKey, LPCSTR valueName, LPDWORD reserved,
    LPDWORD type, LPBYTE data, LPDWORD dataSize)
{
    const LSTATUS rc = Real_RegQueryValueExA(hKey, valueName, reserved, type, data, dataSize);
    if (!valueName) return rc;
    if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) return rc;

    if (IEqualsA(valueName, "MachineGuid")) {
        const Identity id = Snapshot();
        char ansi[64];
        const int n = WideCharToMultiByte(CP_ACP, 0, id.machineGuid, -1, ansi, sizeof(ansi), nullptr, nullptr);
        if (n <= 0) return rc;
        const DWORD need = static_cast<DWORD>(n);  // includes null
        if (type) *type = REG_SZ;
        if (!data || !dataSize || *dataSize < need) {
            if (dataSize) *dataSize = need;
            return data ? ERROR_MORE_DATA : ERROR_SUCCESS;
        }
        std::memcpy(data, ansi, need);
        *dataSize = need;
        return ERROR_SUCCESS;
    }

    if (rc != ERROR_SUCCESS || !data || !dataSize || *dataSize == 0)
        return rc;
    const DWORD t = type ? *type : REG_SZ;
    if (t != REG_SZ && t != REG_EXPAND_SZ && t != REG_MULTI_SZ)
        return rc;

    for (auto* target : kVmTargetsA) {
        if (!IEqualsA(valueName, target))
            continue;
        if (!LooksLikeVmStringA(reinterpret_cast<const char*>(data), *dataSize))
            return rc;
        const char* repl = PickReplacementA(valueName);
        const DWORD replBytes = static_cast<DWORD>(strlen(repl) + 1);
        if (*dataSize < replBytes) return rc;
        std::memcpy(data, repl, replBytes);
        *dataSize = replBytes;
        if (type) *type = REG_SZ;
        return ERROR_SUCCESS;
    }
    return rc;
}

// =====================================================================
// Random identity generation
// =====================================================================
static std::mt19937& Rng()
{
    static std::mt19937 rng(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        GetCurrentProcessId() ^ (GetCurrentThreadId() << 16)));
    return rng;
}

static void RandomGuidStringV4(wchar_t out[40])
{
    auto& rng = Rng();
    std::uniform_int_distribution<uint32_t> d32;
    const uint32_t a = d32(rng);
    const uint16_t b = static_cast<uint16_t>(d32(rng));
    uint16_t c = static_cast<uint16_t>(d32(rng));
    c = (c & 0x0FFF) | 0x4000;                  // version 4
    uint16_t d = static_cast<uint16_t>(d32(rng));
    d = (d & 0x3FFF) | 0x8000;                  // variant 1 (RFC 4122)
    uint64_t e = (static_cast<uint64_t>(d32(rng)) << 32) | d32(rng);
    e &= 0x0000FFFFFFFFFFFFULL;
    swprintf(out, 40, L"%08x-%04x-%04x-%04x-%012llx",
             a, b, c, d, static_cast<unsigned long long>(e));
}

Identity GenerateRandomIdentity()
{
    Identity id{};
    auto& rng = Rng();
    std::uniform_int_distribution<uint32_t> d32;
    std::uniform_int_distribution<int>      dbyte(0, 255);

    id.volumeSerial = d32(rng) | 0x00000001u;  // never zero

    // Realistic OUI prefixes (Dell / Intel / ASUS / HP — all common consumer NICs)
    static const uint8_t kOui[][3] = {
        {0xD8, 0x9E, 0xF3}, {0xF4, 0x8E, 0x38}, {0x00, 0x21, 0x9B},   // Dell
        {0x00, 0x1E, 0xC9}, {0xB8, 0xCA, 0x3A},                       // Dell
        {0x18, 0x03, 0x73}, {0x3C, 0x97, 0x0E},                       // Intel
        {0x2C, 0xFD, 0xA1}, {0x60, 0x45, 0xCB},                       // ASUS
        {0x70, 0x5A, 0x0F}, {0xA0, 0xB3, 0xCC},                       // HP
    };
    std::uniform_int_distribution<size_t> dprefix(0, (sizeof(kOui) / 3) - 1);
    const size_t pi = dprefix(rng);
    id.macAddress[0] = kOui[pi][0];
    id.macAddress[1] = kOui[pi][1];
    id.macAddress[2] = kOui[pi][2];
    id.macAddress[3] = static_cast<uint8_t>(dbyte(rng));
    id.macAddress[4] = static_cast<uint8_t>(dbyte(rng));
    id.macAddress[5] = static_cast<uint8_t>(dbyte(rng));

    RandomGuidStringV4(id.machineGuid);

    // Hostname: DESKTOP-XXXXXXX  (Windows installer's default pattern)
    static const wchar_t kAlnum[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<size_t> dch(0, wcslen(kAlnum) - 1);
    wchar_t suffix[8] = {};
    for (int i = 0; i < 7; ++i) suffix[i] = kAlnum[dch(rng)];
    swprintf(id.computerName, 64, L"DESKTOP-%s", suffix);

    // Generic user name pool
    static const char* kUsers[] = {
        "User", "Admin", "Owner", "Player", "Justin", "Mike", "Alex", "Sam", "Chris", "Pat",
    };
    std::uniform_int_distribution<size_t> dn(0, (sizeof(kUsers) / sizeof(kUsers[0])) - 1);
    strcpy_s(id.userName, kUsers[dn(rng)]);

    return id;
}

// =====================================================================
// Persistence
// =====================================================================
static std::wstring DefaultStorePath(HMODULE selfModule)
{
    wchar_t path[MAX_PATH] = L"";
    if (selfModule && GetModuleFileNameW(selfModule, path, MAX_PATH)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) slash[1] = 0;
    }
    std::wstring p = path;
    p += L"hwid.txt";
    return p;
}

static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool LoadFromDisk()
{
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        path = g_storePath;
    }
    if (path.empty()) return false;

    std::ifstream in(path);
    if (!in) return false;

    Identity id = Snapshot();   // start from current — only override what is in file
    bool any = false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));

        if (key == "volumeSerial") {
            id.volumeSerial = static_cast<uint32_t>(std::stoul(val, nullptr, 16));
            any = true;
        } else if (key == "mac") {
            unsigned m[6]{};
            if (sscanf_s(val.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int i = 0; i < 6; ++i) id.macAddress[i] = static_cast<uint8_t>(m[i]);
                any = true;
            }
        } else if (key == "machineGuid") {
            const std::wstring w = FromUtf8(val);
            wcsncpy_s(id.machineGuid, w.c_str(), _TRUNCATE);
            any = true;
        } else if (key == "computerName") {
            const std::wstring w = FromUtf8(val);
            wcsncpy_s(id.computerName, w.c_str(), _TRUNCATE);
            any = true;
        } else if (key == "userName") {
            strncpy_s(id.userName, val.c_str(), _TRUNCATE);
            any = true;
        }
    }

    if (any) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_id = id;
    }
    return any;
}

bool SaveToDisk()
{
    Identity id;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        id = g_id;
        path = g_storePath;
    }
    if (path.empty()) return false;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    char macStr[32];
    sprintf_s(macStr, "%02x:%02x:%02x:%02x:%02x:%02x",
              id.macAddress[0], id.macAddress[1], id.macAddress[2],
              id.macAddress[3], id.macAddress[4], id.macAddress[5]);

    out << "# Spoofed hardware identity — edit by hand or regenerate via overlay UI.\n";
    out << "volumeSerial=" << std::hex << id.volumeSerial << "\n" << std::dec;
    out << "mac=" << macStr << "\n";
    out << "machineGuid=" << ToUtf8(id.machineGuid) << "\n";
    out << "computerName=" << ToUtf8(id.computerName) << "\n";
    out << "userName=" << id.userName << "\n";
    return static_cast<bool>(out);
}

void SetIdentity(const Identity& id, bool persist)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_id = id;
    }
    if (persist) SaveToDisk();
}

Identity GetIdentity()
{
    return Snapshot();
}

// =====================================================================
// Hook install / uninstall
// =====================================================================
template <typename T>
static T Resolve(HMODULE mod, const char* name)
{
    if (!mod) return nullptr;
    return reinterpret_cast<T>(GetProcAddress(mod, name));
}

bool Init(HMODULE selfModule)
{
    if (g_ready.load()) return true;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_storePath = DefaultStorePath(selfModule);
        g_id = GenerateRandomIdentity();
    }
    // Override with persisted values if a hwid.txt exists; otherwise create one.
    if (!LoadFromDisk())
        SaveToDisk();

    HMODULE k32   = GetModuleHandleA("kernel32.dll");
    HMODULE adv   = LoadLibraryA("advapi32.dll");
    HMODULE iphlp = LoadLibraryA("iphlpapi.dll");

    Real_GetVolumeInformationW = Resolve<GetVolumeInformationW_t>(k32, "GetVolumeInformationW");
    Real_GetVolumeInformationA = Resolve<GetVolumeInformationA_t>(k32, "GetVolumeInformationA");
    Real_GetComputerNameW      = Resolve<GetComputerNameW_t>     (k32, "GetComputerNameW");
    Real_GetComputerNameA      = Resolve<GetComputerNameA_t>     (k32, "GetComputerNameA");
    Real_GetComputerNameExW    = Resolve<GetComputerNameExW_t>   (k32, "GetComputerNameExW");
    Real_GetComputerNameExA    = Resolve<GetComputerNameExA_t>   (k32, "GetComputerNameExA");
    Real_GetUserNameW          = Resolve<GetUserNameW_t>         (adv, "GetUserNameW");
    Real_GetUserNameA          = Resolve<GetUserNameA_t>         (adv, "GetUserNameA");
    Real_GetAdaptersInfo       = Resolve<GetAdaptersInfo_t>      (iphlp, "GetAdaptersInfo");
    Real_GetAdaptersAddresses  = Resolve<GetAdaptersAddresses_t> (iphlp, "GetAdaptersAddresses");
    Real_RegQueryValueExW      = Resolve<RegQueryValueExW_t>     (adv, "RegQueryValueExW");
    Real_RegQueryValueExA      = Resolve<RegQueryValueExA_t>     (adv, "RegQueryValueExA");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

#define HWID_ATTACH(real, hook) \
    if (real) DetourAttach(&reinterpret_cast<PVOID&>(real), reinterpret_cast<PVOID>(hook))

    HWID_ATTACH(Real_GetVolumeInformationW, Hk_GetVolumeInformationW);
    HWID_ATTACH(Real_GetVolumeInformationA, Hk_GetVolumeInformationA);
    HWID_ATTACH(Real_GetComputerNameW,      Hk_GetComputerNameW);
    HWID_ATTACH(Real_GetComputerNameA,      Hk_GetComputerNameA);
    HWID_ATTACH(Real_GetComputerNameExW,    Hk_GetComputerNameExW);
    HWID_ATTACH(Real_GetComputerNameExA,    Hk_GetComputerNameExA);
    HWID_ATTACH(Real_GetUserNameW,          Hk_GetUserNameW);
    HWID_ATTACH(Real_GetUserNameA,          Hk_GetUserNameA);
    HWID_ATTACH(Real_GetAdaptersInfo,       Hk_GetAdaptersInfo);
    HWID_ATTACH(Real_GetAdaptersAddresses,  Hk_GetAdaptersAddresses);
    HWID_ATTACH(Real_RegQueryValueExW,      Hk_RegQueryValueExW);
    HWID_ATTACH(Real_RegQueryValueExA,      Hk_RegQueryValueExA);

#undef HWID_ATTACH

    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        spdlog::error("[hwid] DetourTransactionCommit failed: {}", err);
        return false;
    }

    g_ready.store(true);

    const Identity snap = Snapshot();
    spdlog::info("[hwid] Installed. host={} mac={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} vol=0x{:08x} guid={}",
                 ToUtf8(snap.computerName),
                 snap.macAddress[0], snap.macAddress[1], snap.macAddress[2],
                 snap.macAddress[3], snap.macAddress[4], snap.macAddress[5],
                 snap.volumeSerial,
                 ToUtf8(snap.machineGuid));
    return true;
}

void Shutdown()
{
    if (!g_ready.load()) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

#define HWID_DETACH(real, hook) \
    if (real) DetourDetach(&reinterpret_cast<PVOID&>(real), reinterpret_cast<PVOID>(hook))

    HWID_DETACH(Real_GetVolumeInformationW, Hk_GetVolumeInformationW);
    HWID_DETACH(Real_GetVolumeInformationA, Hk_GetVolumeInformationA);
    HWID_DETACH(Real_GetComputerNameW,      Hk_GetComputerNameW);
    HWID_DETACH(Real_GetComputerNameA,      Hk_GetComputerNameA);
    HWID_DETACH(Real_GetComputerNameExW,    Hk_GetComputerNameExW);
    HWID_DETACH(Real_GetComputerNameExA,    Hk_GetComputerNameExA);
    HWID_DETACH(Real_GetUserNameW,          Hk_GetUserNameW);
    HWID_DETACH(Real_GetUserNameA,          Hk_GetUserNameA);
    HWID_DETACH(Real_GetAdaptersInfo,       Hk_GetAdaptersInfo);
    HWID_DETACH(Real_GetAdaptersAddresses,  Hk_GetAdaptersAddresses);
    HWID_DETACH(Real_RegQueryValueExW,      Hk_RegQueryValueExW);
    HWID_DETACH(Real_RegQueryValueExA,      Hk_RegQueryValueExA);

#undef HWID_DETACH

    DetourTransactionCommit();
    g_ready.store(false);
    spdlog::info("[hwid] Hooks removed");
}

}  // namespace HwidSpoof
