#pragma once

#include <windows.h>
#include <cstdint>

// =====================================================================
// HwidSpoof — intercepts the Windows APIs the game (and most anti-cheats)
// use to collect a machine fingerprint for the login integrity check.
//
// Hooks are installed on system DLLs (kernel32, advapi32, iphlpapi) — never
// on game code — so the server-side VM/integrity scan does not trigger.
//
// Spoofed surfaces:
//   • Disk volume serial   (GetVolumeInformationW/A)
//   • Adapter MAC address  (GetAdaptersInfo, GetAdaptersAddresses)
//   • Computer name        (GetComputerNameW/A, GetComputerNameExW/A)
//   • User name            (GetUserNameW/A)
//   • MachineGuid          (RegQueryValueExW/A on HKLM\Software\...\Cryptography)
//   • Common VM identifiers in HARDWARE\DESCRIPTION\System\BIOS values
//
// The identity is randomized at first launch and persisted to <dll dir>\hwid.txt
// so subsequent launches present the same machine to the server.
// =====================================================================

namespace HwidSpoof {

struct Identity
{
    uint32_t volumeSerial      = 0;
    uint8_t  macAddress[6]     = {};
    wchar_t  machineGuid[64]   = L"";   // e.g. L"3a8f5c12-..." (no braces)
    wchar_t  computerName[64]  = L"";   // e.g. L"DESKTOP-AB12CDE"
    char     userName[64]      = "";    // e.g. "User"
};

// Install all API hooks. `selfModule` is the bot DLL's HMODULE — used to
// locate the persistent identity file next to the DLL. Safe to call once.
// Returns true if all hooks committed successfully.
bool Init(HMODULE selfModule);

// Remove all hooks. Safe to call multiple times.
void Shutdown();

// Replace the active spoofed identity. The change takes effect immediately
// for every subsequent hooked call. When `persist` is true, the identity is
// written to the hwid.txt store so it survives a restart.
void SetIdentity(const Identity& id, bool persist = true);

// Snapshot the currently active identity.
Identity GetIdentity();

// Build a fresh randomized identity that resembles a typical home PC.
Identity GenerateRandomIdentity();

// Load identity from <dll dir>\hwid.txt. Returns false if the file does not
// exist or could not be parsed. Init() calls this automatically.
bool LoadFromDisk();

// Write the current identity to <dll dir>\hwid.txt. Returns false on I/O error.
bool SaveToDisk();

}  // namespace HwidSpoof
