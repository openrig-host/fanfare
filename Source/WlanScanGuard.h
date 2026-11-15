#pragma once

#include "Logger.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wlanapi.h>
#include <vector>

namespace Fanfare {

/**
 * Suppresses Windows WLAN AutoConfig periodic background BSS scans while
 * Fanfare is running so Wi-Fi stays connected for the Tablet Stage HUD
 * without 60s/120s NDIS/DPC latency spikes.
 *
 * Dynamically loads wlanapi.dll (no link dependency) and restores the
 * previous setting + closes the client handle cleanly on shutdown.
 */
class WlanScanGuard {
public:
  WlanScanGuard() = default;
  ~WlanScanGuard() { restore(); }

  WlanScanGuard(const WlanScanGuard&) = delete;
  WlanScanGuard& operator=(const WlanScanGuard&) = delete;

  void suppressBackgroundScans() {
    if (active)
      return;

    wlanModule = ::LoadLibraryA("wlanapi.dll");
    if (wlanModule == nullptr) {
      LOG_INFO("WlanScanGuard: wlanapi.dll not present (no Wi-Fi service).");
      return;
    }

    pfnOpen = reinterpret_cast<decltype(pfnOpen)>(
        ::GetProcAddress(wlanModule, "WlanOpenHandle"));
    pfnClose = reinterpret_cast<decltype(pfnClose)>(
        ::GetProcAddress(wlanModule, "WlanCloseHandle"));
    pfnEnum = reinterpret_cast<decltype(pfnEnum)>(
        ::GetProcAddress(wlanModule, "WlanEnumInterfaces"));
    pfnQuery = reinterpret_cast<decltype(pfnQuery)>(
        ::GetProcAddress(wlanModule, "WlanQueryInterface"));
    pfnSet = reinterpret_cast<decltype(pfnSet)>(
        ::GetProcAddress(wlanModule, "WlanSetInterface"));
    pfnFree = reinterpret_cast<decltype(pfnFree)>(
        ::GetProcAddress(wlanModule, "WlanFreeMemory"));

    if (!pfnOpen || !pfnClose || !pfnEnum || !pfnQuery || !pfnSet || !pfnFree) {
      LOG_WARN("WlanScanGuard: failed to resolve wlanapi.dll entry points.");
      cleanupModule();
      return;
    }

    DWORD negotiated = 0;
    DWORD err = pfnOpen(2, nullptr, &negotiated, &wlanHandle);
    if (err != ERROR_SUCCESS || wlanHandle == nullptr) {
      LOG_INFO("WlanScanGuard: WlanOpenHandle returned " + juce::String((int)err) +
               " (WLAN AutoConfig inactive or unavailable).");
      cleanupModule();
      return;
    }

    PWLAN_INTERFACE_INFO_LIST ifList = nullptr;
    err = pfnEnum(wlanHandle, nullptr, &ifList);
    if (err != ERROR_SUCCESS || ifList == nullptr) {
      LOG_INFO("WlanScanGuard: WlanEnumInterfaces returned " + juce::String((int)err));
      restore();
      return;
    }

    for (DWORD i = 0; i < ifList->dwNumberOfItems; ++i) {
      const WLAN_INTERFACE_INFO& info = ifList->InterfaceInfo[i];
      DWORD dataSize = 0;
      PVOID dataPtr = nullptr;
      WLAN_OPCODE_VALUE_TYPE valueType = wlan_opcode_value_type_invalid;

      DWORD qErr = pfnQuery(wlanHandle, &info.InterfaceGuid,
                            wlan_intf_opcode_background_scan_enabled,
                            nullptr, &dataSize, &dataPtr, &valueType);
      bool wasEnabled = true;
      if (qErr == ERROR_SUCCESS && dataPtr != nullptr && dataSize >= sizeof(BOOL)) {
        wasEnabled = (*reinterpret_cast<const BOOL*>(dataPtr) != FALSE);
        pfnFree(dataPtr);
      } else if (dataPtr != nullptr) {
        pfnFree(dataPtr);
      }

      const juce::String ifaceName(info.strInterfaceDescription);
      if (!wasEnabled) {
        LOG_INFO("WlanScanGuard: background Wi-Fi scanning already disabled on '" +
                 ifaceName + "'.");
        continue;
      }

      BOOL enableFlag = FALSE;
      DWORD sErr = pfnSet(wlanHandle, &info.InterfaceGuid,
                          wlan_intf_opcode_background_scan_enabled,
                          sizeof(BOOL), &enableFlag, nullptr);
      if (sErr == ERROR_SUCCESS) {
        suppressedInterfaces.push_back({info.InterfaceGuid, ifaceName});
        LOG_INFO("WlanScanGuard: disabled periodic Wi-Fi background scanning on '" +
                 ifaceName + "' (Tablet HUD stays connected).");
      } else {
        LOG_WARN("WlanScanGuard: WlanSetInterface(background_scan=FALSE) on '" +
                 ifaceName + "' failed with Win32 error " + juce::String((int)sErr));
      }
    }

    pfnFree(ifList);

    if (suppressedInterfaces.empty()) {
      // No handle needs to stay open if nothing was changed
      pfnClose(wlanHandle, nullptr);
      wlanHandle = nullptr;
      cleanupModule();
      return;
    }

    active = true;
  }

  void restore() {
    if (wlanHandle != nullptr && pfnSet != nullptr) {
      for (const auto& item : suppressedInterfaces) {
        BOOL enableFlag = TRUE;
        DWORD sErr = pfnSet(wlanHandle, &item.guid,
                            wlan_intf_opcode_background_scan_enabled,
                            sizeof(BOOL), &enableFlag, nullptr);
        if (sErr == ERROR_SUCCESS) {
          LOG_INFO("WlanScanGuard: restored Wi-Fi background scanning on '" +
                   item.name + "'.");
        }
      }
    }
    suppressedInterfaces.clear();
    active = false;

    if (wlanHandle != nullptr && pfnClose != nullptr) {
      pfnClose(wlanHandle, nullptr);
      wlanHandle = nullptr;
    }
    cleanupModule();
  }

private:
  struct SuppressedInterface {
    GUID guid{};
    juce::String name;
  };

  void cleanupModule() {
    pfnOpen = nullptr;
    pfnClose = nullptr;
    pfnEnum = nullptr;
    pfnQuery = nullptr;
    pfnSet = nullptr;
    pfnFree = nullptr;
    if (wlanModule != nullptr) {
      ::FreeLibrary(wlanModule);
      wlanModule = nullptr;
    }
  }

  HMODULE wlanModule = nullptr;
  HANDLE wlanHandle = nullptr;
  bool active = false;
  std::vector<SuppressedInterface> suppressedInterfaces;

  DWORD(WINAPI* pfnOpen)(DWORD, PVOID, PDWORD, PHANDLE) = nullptr;
  DWORD(WINAPI* pfnClose)(HANDLE, PVOID) = nullptr;
  DWORD(WINAPI* pfnEnum)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*) = nullptr;
  DWORD(WINAPI* pfnQuery)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE) = nullptr;
  DWORD(WINAPI* pfnSet)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, PVOID, PVOID) = nullptr;
  VOID(WINAPI* pfnFree)(PVOID) = nullptr;
};

} // namespace Fanfare
#endif
