// ==WindhawkMod==
// @id              modern-device-manager
// @name            Modern Device Manager
// @description     Replaces the Device Manager snap-in with a dark, three-pane Windows 11 window
// @version         1.4.0
// @author          emirerkul991-1yssssss
// @github          https://github.com/emirerkul991-1yssssss
// @license         MIT
// @include         mmc.exe
// @compilerOptions -ldwmapi -lgdiplus -lsetupapi -lcfgmgr32 -lole32 -lshell32 -lshlwapi -luser32 -lgdi32 -luuid
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Modern Device Manager

Replaces `devmgmt.msc`. The mod loads into `mmc.exe`, checks the command line,
and shows its own window instead of letting the console load. Any other snap-in
runs untouched. `WH_DEVMGMT_CLASSIC=1` gets the original console back for one
launch without disabling the mod.

## Layout

Three panes, following the shape of Microsoft's own "Devices" concept:

1. **This PC** (left) — machine name, processor, graphics, memory, and the
   system drive's capacity bar, then the window's actions: refresh, add legacy
   hardware, open Settings, help.
2. **Device type** (middle) — every device class present on the machine, with
   its real class icon and device count. Click a class to expand it and see its
   devices; a device with a problem carries a warning marker. The search box
   filters across class and device names at once.
3. **Details** (right) — the selected device's name, status, manufacturer,
   class, location and device instance ID, with a Properties button. Empty until
   something is selected, like the concept.

## Actions — Windows does the work

Properties opens the **real** device property sheet
(`devmgr.dll!DeviceProperties_RunDLLW`), which is where driver updates, enable
and disable, and uninstall live. This mod deliberately does not reimplement any
of that: a device manager that edits driver state itself is a device manager
that can leave a machine unbootable.

Refresh re-enumerates.

## Notes

* Devices are grouped by setup class, the same grouping the console's default
  "Devices by type" view uses.
* Class icons come from `SetupDiLoadClassIcon` and are drawn with `DrawIconEx`,
  which composites alpha correctly - unlike the shell-image path, which returns
  straight ARGB and needs premultiplying by hand.
* Hidden (non-present) devices are off by default, matching the console.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- wallpaperTint: true
  $name: Tint the window with the wallpaper
  $description: Mixes the desktop wallpaper's dominant colour into the background, the way Mica does.
- showHiddenDevices: false
  $name: Show hidden devices
  $description: Includes devices that are not currently present, like the console's "Show hidden devices".
- expandFirstClass: false
  $name: Expand the first device class on open
- verboseLog: false
  $name: Log enumeration details
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>

#include <algorithm>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------

struct Settings {
    bool wallpaperTint = true;
    bool showHiddenDevices = false;
    bool expandFirstClass = false;
    bool verboseLog = false;
};

Settings g_settings;

// Set from the window's DPI before enumeration, so class icons are requested at
// the size they will actually be drawn at rather than scaled afterwards.
int g_classIconPixels = 32;

// -----------------------------------------------------------------------------
// Look
// -----------------------------------------------------------------------------

namespace ui {

constexpr COLORREF kWindow = RGB(26, 26, 28);
constexpr COLORREF kCard = RGB(39, 39, 42);
constexpr COLORREF kCardBorder = RGB(55, 55, 59);
constexpr COLORREF kRowHover = RGB(52, 52, 56);
constexpr COLORREF kTextPrimary = RGB(255, 255, 255);
constexpr COLORREF kTextSecondary = RGB(158, 158, 165);
constexpr COLORREF kTextTertiary = RGB(120, 120, 126);
constexpr COLORREF kTextDisabled = RGB(108, 108, 114);
constexpr COLORREF kBarTrack = RGB(58, 58, 62);
constexpr COLORREF kDivider = RGB(52, 52, 57);
constexpr COLORREF kWarning = RGB(226, 179, 91);
constexpr COLORREF kCloseHover = RGB(196, 43, 28);
constexpr COLORREF kButtonHover = RGB(62, 62, 68);

constexpr int kWindowWidth = 1180;
constexpr int kWindowHeight = 760;
constexpr int kTitleHeight = 44;
constexpr int kCaptionButtonWidth = 46;
constexpr int kPadding = 18;
constexpr int kCardRadius = 8;
constexpr int kSidebarWidth = 268;
constexpr int kListWidth = 320;
constexpr int kRowHeight = 34;
// Class rows are taller so their icons can be drawn at 32px. Windows ships
// device class icons at 16/32/48/256; asking for anything else means the shell
// reduces the 32px art and details fall off - the same trap the drive icons hit.
constexpr int kClassRowHeight = 42;
constexpr int kClassIconSize = 32;
constexpr int kPcIconSize = 48;
constexpr int kSearchHeight = 34;
constexpr int kScrollBarWidth = 4;

COLORREF AccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        int r = (color >> 16) & 0xFF;
        int g = (color >> 8) & 0xFF;
        int b = color & 0xFF;
        if (r + g + b > 120) {
            return RGB(r, g, b);
        }
    }
    return RGB(76, 164, 224);
}

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void FillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void FillPlain(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

COLORREF MixColors(COLORREF base, COLORREF tint, int tintPercent) {
    auto mix = [&](int b, int t) {
        return (b * (100 - tintPercent) + t * tintPercent) / 100;
    };
    return RGB(mix(GetRValue(base), GetRValue(tint)),
               mix(GetGValue(base), GetGValue(tint)),
               mix(GetBValue(base), GetBValue(tint)));
}

// DWM will not draw Mica on a window like this - three test programs confirmed
// it - so the window paints a wallpaper-derived tint instead.
COLORREF WallpaperTinted(COLORREF base, int percent) {
    WCHAR path[MAX_PATH] = L"";
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) ||
        !path[0]) {
        return base;
    }

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        return base;
    }

    COLORREF result = base;
    {
        Gdiplus::Bitmap wallpaper(path);
        if (wallpaper.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Bitmap average(1, 1, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&average);
            graphics.SetInterpolationMode(
                Gdiplus::InterpolationModeHighQualityBilinear);
            if (graphics.DrawImage(&wallpaper, 0, 0, 1, 1) == Gdiplus::Ok) {
                Gdiplus::Color color;
                if (average.GetPixel(0, 0, &color) == Gdiplus::Ok) {
                    result = MixColors(
                        base, RGB(color.GetR(), color.GetG(), color.GetB()),
                        percent);
                }
            }
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return result;
}

// Decimal units, matching how Settings reports capacity.
std::wstring FormatSize(ULONGLONG bytes) {
    constexpr PCWSTR kUnits[] = {L"bytes", L"kB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit + 1 < static_cast<int>(ARRAYSIZE(kUnits))) {
        value /= 1000.0;
        unit++;
    }
    WCHAR text[64];
    _snwprintf(text, ARRAYSIZE(text) - 1, unit == 0 ? L"%.0f %s" : L"%.0f %s",
               value, kUnits[unit]);
    text[ARRAYSIZE(text) - 1] = L'\0';
    return text;
}

}  // namespace ui

// -----------------------------------------------------------------------------
// Module and fonts
// -----------------------------------------------------------------------------

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
    return reinterpret_cast<HINSTANCE>(module);
}

// The face name alone is not enough: without an explicit charset and TrueType
// precision the mapper is free to substitute something Arial-like.
HFONT MakeFont(UINT dpi, int points, int weight) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points, static_cast<int>(dpi), 72);
    font.lfWeight = weight;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfOutPrecision = OUT_TT_PRECIS;
    font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    HFONT created = CreateFontIndirectW(&font);
    if (created) {
        return created;
    }
    wcscpy_s(font.lfFaceName, L"Segoe UI Variable Text");
    return CreateFontIndirectW(&font);
}

// -----------------------------------------------------------------------------
// Device model
// -----------------------------------------------------------------------------

struct DeviceInfo {
    std::wstring name;
    std::wstring manufacturer;
    std::wstring instanceId;
    std::wstring location;
    std::wstring service;
    ULONG problem = 0;
    bool hasProblem = false;
    bool disabled = false;
    bool present = true;
    // Kept so the device's own icon can be loaded later without re-enumerating.
    SP_DEVINFO_DATA devInfo{};
    HICON icon = nullptr;
};

struct DeviceClassInfo {
    GUID guid{};
    std::wstring name;
    HICON icon = nullptr;
    std::vector<DeviceInfo> devices;
    bool expanded = false;
    // Device icons cost a shell call each, so a class only pays for them once
    // it is actually expanded.
    bool iconsLoaded = false;
};

struct SystemSummary {
    std::wstring computerName;
    std::wstring processor;
    std::wstring graphics;
    std::wstring memory;
    std::wstring driveLabel;
    ULONGLONG driveTotal = 0;
    ULONGLONG driveFree = 0;
};

// Present in setupapi.dll since Vista, but mingw's setupapi.h does not declare
// it, so it is resolved at runtime rather than linked.
using SetupDiLoadDeviceIconFn = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, UINT,
                                              UINT, DWORD, HICON*);

SetupDiLoadDeviceIconFn LoadDeviceIconProc() {
    static SetupDiLoadDeviceIconFn proc =
        reinterpret_cast<SetupDiLoadDeviceIconFn>(GetProcAddress(
            GetModuleHandleW(L"setupapi.dll"), "SetupDiLoadDeviceIcon"));
    return proc;
}

std::wstring DeviceProperty(HDEVINFO set, SP_DEVINFO_DATA* data, DWORD prop) {
    DWORD type = 0;
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type, nullptr, 0, &size);
    if (size == 0) {
        return L"";
    }
    // Zero-initialised and oversized by a WCHAR, so a property stored without a
    // terminator cannot run off the end.
    std::vector<BYTE> buffer(size + sizeof(WCHAR), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type,
                                           buffer.data(), size, nullptr)) {
        return L"";
    }
    return std::wstring(reinterpret_cast<PCWSTR>(buffer.data()));
}

std::wstring ClassDescription(const GUID& guid) {
    WCHAR description[256] = L"";
    DWORD required = 0;
    if (SetupDiGetClassDescriptionW(&guid, description, ARRAYSIZE(description),
                                    &required)) {
        return description;
    }
    return L"Other devices";
}

std::wstring DeviceInstanceId(HDEVINFO set, SP_DEVINFO_DATA* data) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(set, data, nullptr, 0, &required);
    if (required == 0) {
        return L"";
    }
    std::vector<WCHAR> buffer(required + 1, 0);
    if (!SetupDiGetDeviceInstanceIdW(set, data, buffer.data(), required,
                                     nullptr)) {
        return L"";
    }
    return std::wstring(buffer.data());
}

void ReleaseClassIcons(std::vector<DeviceClassInfo>& classes) {
    for (auto& item : classes) {
        if (item.icon) {
            DestroyIcon(item.icon);
            item.icon = nullptr;
        }
        for (auto& device : item.devices) {
            if (device.icon) {
                DestroyIcon(device.icon);
                device.icon = nullptr;
            }
        }
        item.iconsLoaded = false;
    }
}

// Loads the icons for one class's devices, once. Falls back to the class icon so
// a row is never left blank.
void EnsureDeviceIcons(HDEVINFO set, DeviceClassInfo& item, int pixels) {
    if (item.iconsLoaded || set == INVALID_HANDLE_VALUE) {
        return;
    }
    item.iconsLoaded = true;

    auto loadDeviceIcon = LoadDeviceIconProc();
    for (auto& device : item.devices) {
        if (device.icon) {
            continue;
        }
        if (loadDeviceIcon &&
            loadDeviceIcon(set, &device.devInfo, static_cast<UINT>(pixels),
                           static_cast<UINT>(pixels), 0, &device.icon)) {
            continue;
        }
        device.icon = nullptr;
    }
}

// The device info set is kept open for the window's lifetime rather than
// destroyed here, so device icons can be loaded on demand later.
std::vector<DeviceClassInfo> EnumerateDevices(HDEVINFO* keepSet) {
    std::vector<DeviceClassInfo> classes;
    *keepSet = INVALID_HANDLE_VALUE;

    DWORD flags = DIGCF_ALLCLASSES;
    if (!g_settings.showHiddenDevices) {
        flags |= DIGCF_PRESENT;
    }

    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, flags);
    if (set == INVALID_HANDLE_VALUE) {
        Wh_Log(L"SetupDiGetClassDevs failed: %u", GetLastError());
        return classes;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &data); index++) {
        DeviceInfo device;
        device.name = DeviceProperty(set, &data, SPDRP_FRIENDLYNAME);
        if (device.name.empty()) {
            device.name = DeviceProperty(set, &data, SPDRP_DEVICEDESC);
        }
        if (device.name.empty()) {
            continue;  // nothing to show a user
        }
        device.manufacturer = DeviceProperty(set, &data, SPDRP_MFG);
        device.location = DeviceProperty(set, &data, SPDRP_LOCATION_INFORMATION);
        device.service = DeviceProperty(set, &data, SPDRP_SERVICE);
        device.instanceId = DeviceInstanceId(set, &data);
        device.devInfo = data;

        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) ==
            CR_SUCCESS) {
            device.hasProblem = (status & DN_HAS_PROBLEM) != 0;
            device.problem = problem;
            device.disabled =
                device.hasProblem && problem == CM_PROB_DISABLED;
        } else {
            device.present = false;
        }

        // Group by setup class, which is the console's "Devices by type".
        DeviceClassInfo* bucket = nullptr;
        for (auto& item : classes) {
            if (IsEqualGUID(item.guid, data.ClassGuid)) {
                bucket = &item;
                break;
            }
        }
        if (!bucket) {
            DeviceClassInfo created;
            created.guid = data.ClassGuid;
            created.name = ClassDescription(data.ClassGuid);
            // SetupDiLoadDeviceIcon takes a size; SetupDiLoadClassIcon always
            // hands back 32x32, which then has to be scaled. Asking for the
            // drawn size directly is what keeps these crisp. The class's first
            // device stands in for the class, and the API already falls back to
            // the class icon when a device has none of its own.
            auto loadDeviceIcon = LoadDeviceIconProc();
            if (!loadDeviceIcon ||
                !loadDeviceIcon(set, &data,
                                static_cast<UINT>(g_classIconPixels),
                                static_cast<UINT>(g_classIconPixels), 0,
                                &created.icon)) {
                int iconIndex = 0;
                if (!SetupDiLoadClassIcon(&created.guid, &created.icon,
                                          &iconIndex)) {
                    created.icon = nullptr;
                }
            }
            classes.push_back(std::move(created));
            bucket = &classes.back();
        }
        bucket->devices.push_back(std::move(device));
    }
    *keepSet = set;

    for (auto& item : classes) {
        std::sort(item.devices.begin(), item.devices.end(),
                  [](const DeviceInfo& a, const DeviceInfo& b) {
                      return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });
    }
    std::sort(classes.begin(), classes.end(),
              [](const DeviceClassInfo& a, const DeviceClassInfo& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    if (g_settings.verboseLog) {
        Wh_Log(L"%d device classes", static_cast<int>(classes.size()));
        for (const auto& item : classes) {
            Wh_Log(L"  %s: %d", item.name.c_str(),
                   static_cast<int>(item.devices.size()));
        }
    }
    return classes;
}

std::wstring RegistryString(HKEY root, PCWSTR subKey, PCWSTR value) {
    WCHAR buffer[512] = L"";
    DWORD size = sizeof(buffer) - sizeof(WCHAR);
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buffer;
}

SystemSummary ReadSystemSummary(const std::vector<DeviceClassInfo>& classes) {
    SystemSummary summary;

    WCHAR name[256] = L"";
    DWORD nameSize = ARRAYSIZE(name);
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &nameSize)) {
        summary.computerName = name;
    }

    summary.processor = RegistryString(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        // Physical memory is reported slightly under the installed amount, so
        // round to the nearest sensible figure rather than showing 15.9 GB.
        ULONGLONG gb = (memory.ullTotalPhys + (1ull << 29)) >> 30;
        WCHAR text[64];
        _snwprintf(text, ARRAYSIZE(text) - 1, L"%llu GB RAM", gb);
        text[ARRAYSIZE(text) - 1] = L'\0';
        summary.memory = text;
    }

    // Graphics comes from the display adapters already enumerated - no second
    // enumeration, and it stays consistent with what the middle pane lists.
    for (const auto& item : classes) {
        if (IsEqualGUID(item.guid, GUID_DEVCLASS_DISPLAY)) {
            for (const auto& device : item.devices) {
                if (!summary.graphics.empty()) {
                    summary.graphics += L"\n";
                }
                summary.graphics += device.name;
            }
            break;
        }
    }

    WCHAR windows[MAX_PATH] = L"";
    if (GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) && windows[0]) {
        WCHAR root[8] = {windows[0], L':', L'\\', 0};
        WCHAR label[MAX_PATH] = L"";
        if (GetVolumeInformationW(root, label, ARRAYSIZE(label), nullptr,
                                  nullptr, nullptr, nullptr, 0) &&
            label[0]) {
            summary.driveLabel = label;
        } else {
            summary.driveLabel = L"Windows";
        }
        summary.driveLabel += L" (";
        summary.driveLabel += windows[0];
        summary.driveLabel += L":)";

        ULARGE_INTEGER free{};
        ULARGE_INTEGER total{};
        if (GetDiskFreeSpaceExW(root, nullptr, &total, &free)) {
            summary.driveTotal = total.QuadPart;
            summary.driveFree = free.QuadPart;
        }
    }
    return summary;
}

// -----------------------------------------------------------------------------
// Actions - Windows does the work, not this mod
// -----------------------------------------------------------------------------

// The real device property sheet, which is where driver updates, enable and
// disable, and uninstall live. Deliberately not reimplemented.
void ShowDeviceProperties(HWND owner, const std::wstring& instanceId) {
    if (instanceId.empty()) {
        return;
    }
    HMODULE devmgr = LoadLibraryW(L"devmgr.dll");
    if (!devmgr) {
        Wh_Log(L"could not load devmgr.dll: %u", GetLastError());
        return;
    }
    using DevicePropertiesFn = void(WINAPI*)(HWND, HINSTANCE, LPCWSTR, int);
    auto entry = reinterpret_cast<DevicePropertiesFn>(
        GetProcAddress(devmgr, "DeviceProperties_RunDLLW"));
    if (entry) {
        std::wstring arguments = L"/DeviceID ";
        arguments += instanceId;
        entry(owner, nullptr, arguments.c_str(), 0);
    }
    FreeLibrary(devmgr);
}

// The shell's own desktop-PC icon, extracted at an exact size. Going through
// SHGSI_ICONLOCATION and SHDefExtractIconW rather than SHGSI_ICON means the size
// is ours to choose instead of whatever SM_CXICON happens to be.
HICON LoadPcIcon(int pixels) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DESKTOPPC, SHGSI_ICONLOCATION,
                                     &info))) {
        HICON large = nullptr;
        if (SUCCEEDED(SHDefExtractIconW(info.szPath, info.iIcon, 0, &large,
                                        nullptr,
                                        static_cast<UINT>(pixels))) &&
            large) {
            return large;
        }
    }

    // Fall back to the stock icon at whatever size the shell gives.
    SHSTOCKICONINFO stock{};
    stock.cbSize = sizeof(stock);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DESKTOPPC,
                                     SHGSI_ICON | SHGSI_LARGEICON, &stock))) {
        return stock.hIcon;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// Driver information and driver actions
// -----------------------------------------------------------------------------

struct DriverInfo {
    std::wstring provider;
    std::wstring version;
    std::wstring date;
    std::wstring description;
    std::wstring infName;
};

// The driver's registry key, reached through SPDRP_DRIVER, which holds
// "{class-guid}\\0000". Reading it is far cheaper than building a driver info
// list, and these value names have been stable for two decades.
DriverInfo ReadDriverInfo(HDEVINFO set, SP_DEVINFO_DATA* data) {
    DriverInfo info;
    std::wstring driverKey = DeviceProperty(set, data, SPDRP_DRIVER);
    if (driverKey.empty()) {
        return info;
    }
    std::wstring path =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + driverKey;

    info.provider = RegistryString(HKEY_LOCAL_MACHINE, path.c_str(),
                                   L"ProviderName");
    info.version =
        RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"DriverVersion");
    info.date = RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"DriverDate");
    info.description =
        RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"DriverDesc");
    info.infName =
        RegistryString(HKEY_LOCAL_MACHINE, path.c_str(), L"InfPath");
    return info;
}

// newdev.dll's driver entry points. DiUninstallDevice and DiRollbackDriver are
// documented; DiShowUpdateDevice is not, but it is what Device Manager itself
// uses for "Update driver", and its presence was confirmed by dumping the
// module's exports rather than assumed.
using DiShowUpdateDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA,
                                           DWORD, PBOOL);
using DiRollbackDriverFn = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, HWND, DWORD,
                                         PBOOL);
using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA,
                                          DWORD, PBOOL);

HMODULE NewDevModule() {
    static HMODULE module = LoadLibraryW(L"newdev.dll");
    return module;
}

template <typename Fn>
Fn NewDevProc(const char* name) {
    HMODULE module = NewDevModule();
    if (!module) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

// Driver work needs admin. Reporting that up front beats a wizard that opens
// and then fails.
bool RunningElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, size, &size) &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool SetDeviceEnabled(HDEVINFO set, SP_DEVINFO_DATA* data, bool enable) {
    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(set, data, &params.ClassInstallHeader,
                                       sizeof(params))) {
        return false;
    }
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, data) != FALSE;
}

void LaunchDetached(PCWSTR file, PCWSTR arguments) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpFile = file;
    info.lpParameters = arguments;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

// -----------------------------------------------------------------------------
// Properties window
// -----------------------------------------------------------------------------

namespace props {

constexpr PCWSTR kClassName = L"WindhawkModernDeviceProperties";

enum PropAction {
    kActionNone = 0,
    kActionUpdate,
    kActionRollback,
    kActionToggle,
    kActionUninstall,
    kActionAdvanced,
    kActionClose,
};

struct PropButton {
    RECT rect{};
    int action = kActionNone;
    std::wstring label;
    bool enabled = true;
    bool primary = false;
};

struct PropState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontStrong = nullptr;

    COLORREF cardColor = ui::kCard;
    COLORREF accentColor = ui::AccentColor();

    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA devInfo{};

    std::wstring name;
    std::wstring className;
    std::wstring manufacturer;
    std::wstring location;
    std::wstring instanceId;
    bool hasProblem = false;
    bool disabled = false;
    ULONG problem = 0;

    DriverInfo driver;
    HICON icon = nullptr;

    std::wstring message;
    bool messageIsError = false;
    bool changed = false;  // the caller re-enumerates when this comes back true

    std::vector<PropButton> buttons;
    int hot = -1;
};

PropState* g_props = nullptr;
bool g_classRegistered = false;

void Label(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
           RECT rect, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SelectObject(dc, old);
}

void Surface(HDC dc, const RECT& rect, int radius, COLORREF fill,
             COLORREF border) {
    ui::FillRoundRect(dc, rect, radius, border);
    RECT inner = rect;
    InflateRect(&inner, -1, -1);
    ui::FillRoundRect(dc, inner, radius, fill);
}

int AddPropButton(PropState* state, RECT rect, int action, PCWSTR label,
                  bool enabled, bool primary) {
    PropButton button;
    button.rect = rect;
    button.action = action;
    button.label = label;
    button.enabled = enabled;
    button.primary = primary;
    state->buttons.push_back(std::move(button));
    return static_cast<int>(state->buttons.size()) - 1;
}

void PaintButton(PropState* state, HDC dc, const PropButton& button, bool hot) {
    COLORREF fill = button.primary
                        ? state->accentColor
                        : ui::MixColors(state->cardColor, RGB(255, 255, 255), 9);
    if (!button.enabled) {
        fill = ui::MixColors(state->cardColor, RGB(255, 255, 255), 4);
    } else if (hot) {
        fill = ui::MixColors(fill, RGB(255, 255, 255), 12);
    }
    ui::FillRoundRect(dc, button.rect, ui::Scale(5, state->dpi), fill);
    Label(dc, state->fontStrong,
          button.enabled ? ui::kTextPrimary : ui::kTextDisabled, button.label,
          button.rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

void Paint(PropState* state, HDC dc, const RECT& client) {
    state->buttons.clear();
    ui::FillPlain(dc, client, ui::MixColors(state->cardColor, RGB(0, 0, 0), 30));

    int padding = ui::Scale(24, state->dpi);
    int titleHeight = ui::Scale(ui::kTitleHeight, state->dpi);
    int left = padding;
    int right = client.right - padding;

    // Title bar
    RECT caption{padding, 0, client.right, titleHeight};
    Label(dc, state->fontBody, ui::kTextPrimary, L"Properties", caption,
          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    int closeWidth = ui::Scale(ui::kCaptionButtonWidth, state->dpi);
    RECT closeRect{client.right - closeWidth, 0, client.right, titleHeight};
    int closeIndex =
        AddPropButton(state, closeRect, kActionClose, L"", true, false);
    (void)closeIndex;

    int y = titleHeight + ui::Scale(6, state->dpi);

    // Header: icon and name
    int iconSize = ui::Scale(48, state->dpi);
    if (state->icon) {
        DrawIconEx(dc, left, y, state->icon, iconSize, iconSize, 0, nullptr,
                   DI_NORMAL);
    }
    RECT nameRect{left + iconSize + ui::Scale(16, state->dpi), y, right,
                  y + iconSize};
    Label(dc, state->fontTitle, ui::kTextPrimary, state->name, nameRect,
          DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    y += iconSize + ui::Scale(14, state->dpi);

    // Status
    std::wstring status = L"This device is working properly.";
    COLORREF statusColor = ui::kTextSecondary;
    if (state->disabled) {
        status = L"This device is disabled.";
        statusColor = ui::kWarning;
    } else if (state->hasProblem) {
        WCHAR text[128];
        _snwprintf(text, ARRAYSIZE(text) - 1,
                   L"This device has a problem (code %u).",
                   static_cast<unsigned>(state->problem));
        text[ARRAYSIZE(text) - 1] = L'\0';
        status = text;
        statusColor = ui::kWarning;
    }
    RECT statusRect{left, y, right, y + ui::Scale(22, state->dpi)};
    Label(dc, state->fontSmall, statusColor, status, statusRect,
          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    y = statusRect.bottom + ui::Scale(14, state->dpi);

    auto section = [&](PCWSTR heading) {
        RECT rect{left, y, right, y + ui::Scale(22, state->dpi)};
        Label(dc, state->fontStrong, ui::kTextPrimary, heading, rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        y = rect.bottom + ui::Scale(4, state->dpi);
        RECT rule{left, y, right, y + ui::Scale(1, state->dpi)};
        ui::FillPlain(dc, rule, ui::kDivider);
        y = rule.bottom + ui::Scale(8, state->dpi);
    };

    auto field = [&](PCWSTR label, const std::wstring& value) {
        if (value.empty()) {
            return;
        }
        RECT labelRect{left, y, left + ui::Scale(150, state->dpi),
                       y + ui::Scale(20, state->dpi)};
        Label(dc, state->fontSmall, ui::kTextTertiary, label, labelRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        RECT valueRect{labelRect.right, y, right, labelRect.bottom};
        Label(dc, state->fontSmall, ui::kTextPrimary, value, valueRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
        y = labelRect.bottom + ui::Scale(5, state->dpi);
    };

    section(L"General");
    field(L"Device type", state->className);
    field(L"Manufacturer", state->manufacturer);
    field(L"Location", state->location);
    field(L"Device instance", state->instanceId);

    y += ui::Scale(10, state->dpi);
    section(L"Driver");
    field(L"Provider", state->driver.provider);
    field(L"Version", state->driver.version);
    field(L"Date", state->driver.date);
    field(L"INF name", state->driver.infName);

    // Result of the last action, if any.
    if (!state->message.empty()) {
        RECT messageRect{left, client.bottom - ui::Scale(128, state->dpi), right,
                         client.bottom - ui::Scale(104, state->dpi)};
        Label(dc, state->fontSmall,
              state->messageIsError ? ui::kWarning : ui::kTextSecondary,
              state->message, messageRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    }

    // Driver actions
    bool elevated = RunningElevated();
    int buttonHeight = ui::Scale(32, state->dpi);
    int gap = ui::Scale(8, state->dpi);
    int rowTop = client.bottom - ui::Scale(94, state->dpi);
    int available = right - left - gap * 3;
    int buttonWidth = available / 4;

    struct ActionSpec {
        int action;
        PCWSTR label;
        bool enabled;
        bool primary;
    };
    const ActionSpec actions[] = {
        {kActionUpdate, L"Update driver", elevated, true},
        {kActionRollback, L"Roll back", elevated, false},
        {kActionToggle, state->disabled ? L"Enable" : L"Disable", elevated,
         false},
        {kActionUninstall, L"Uninstall", elevated, false},
    };
    int x = left;
    for (const ActionSpec& spec : actions) {
        RECT rect{x, rowTop, x + buttonWidth, rowTop + buttonHeight};
        AddPropButton(state, rect, spec.action, spec.label, spec.enabled,
                      spec.primary);
        x += buttonWidth + gap;
    }

    if (!elevated) {
        RECT hint{left, rowTop + buttonHeight + ui::Scale(6, state->dpi), right,
                  rowTop + buttonHeight + ui::Scale(26, state->dpi)};
        Label(dc, state->fontSmall, ui::kTextTertiary,
              L"Driver actions need Device Manager to be running as "
              L"administrator.",
              hint, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
    }

    // Secondary row
    int secondTop = client.bottom - ui::Scale(48, state->dpi);
    int wideWidth = ui::Scale(150, state->dpi);
    RECT advanced{left, secondTop, left + wideWidth, secondTop + buttonHeight};
    AddPropButton(state, advanced, kActionAdvanced, L"Windows properties", true,
                  false);
    RECT close{right - ui::Scale(100, state->dpi), secondTop, right,
               secondTop + buttonHeight};
    AddPropButton(state, close, kActionClose, L"Close", true, false);

    for (size_t i = 0; i < state->buttons.size(); i++) {
        const PropButton& button = state->buttons[i];
        if (button.label.empty()) {
            // The caption close button: an X rather than a filled button.
            bool hot = state->hot == static_cast<int>(i);
            if (hot) {
                ui::FillPlain(dc, button.rect, ui::kCloseHover);
            }
            int cx = (button.rect.left + button.rect.right) / 2;
            int cy = (button.rect.top + button.rect.bottom) / 2;
            int size = ui::Scale(10, state->dpi);
            HPEN pen = CreatePen(PS_SOLID, ui::Scale(1, state->dpi),
                                 ui::kTextPrimary);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, cx - size / 2, cy - size / 2, nullptr);
            LineTo(dc, cx + size / 2 + 1, cy + size / 2 + 1);
            MoveToEx(dc, cx + size / 2, cy - size / 2, nullptr);
            LineTo(dc, cx - size / 2 - 1, cy + size / 2 + 1);
            SelectObject(dc, old);
            DeleteObject(pen);
            continue;
        }
        PaintButton(state, dc, button, state->hot == static_cast<int>(i));
    }
}

void ReloadStatus(PropState* state) {
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, state->devInfo.DevInst, 0) ==
        CR_SUCCESS) {
        state->hasProblem = (status & DN_HAS_PROBLEM) != 0;
        state->problem = problem;
        state->disabled = state->hasProblem && problem == CM_PROB_DISABLED;
    }
    state->driver = ReadDriverInfo(state->set, &state->devInfo);
}

void Invoke(PropState* state, const PropButton& button) {
    if (!button.enabled) {
        return;
    }
    BOOL reboot = FALSE;
    state->message.clear();
    state->messageIsError = false;

    switch (button.action) {
        case kActionClose:
            DestroyWindow(state->hwnd);
            return;

        case kActionAdvanced:
            ShowDeviceProperties(state->hwnd, state->instanceId);
            ReloadStatus(state);
            break;

        case kActionUpdate: {
            auto show = NewDevProc<DiShowUpdateDeviceFn>("DiShowUpdateDevice");
            if (!show) {
                // Falling back rather than failing: the real sheet's Driver tab
                // has the same wizard behind it.
                state->message = L"Opening Windows properties instead.";
                ShowDeviceProperties(state->hwnd, state->instanceId);
                break;
            }
            if (show(state->hwnd, state->set, &state->devInfo, 0, &reboot)) {
                state->message = reboot ? L"Driver updated. A restart is needed."
                                        : L"Driver update finished.";
                state->changed = true;
            } else {
                DWORD error = GetLastError();
                if (error == ERROR_CANCELLED) {
                    state->message = L"Driver update cancelled.";
                } else {
                    WCHAR text[128];
                    _snwprintf(text, ARRAYSIZE(text) - 1,
                               L"Driver update failed (error %u).",
                               static_cast<unsigned>(error));
                    text[ARRAYSIZE(text) - 1] = L'\0';
                    state->message = text;
                    state->messageIsError = true;
                }
            }
            break;
        }

        case kActionRollback: {
            auto rollback = NewDevProc<DiRollbackDriverFn>("DiRollbackDriver");
            if (!rollback) {
                state->message = L"Roll back is not available on this system.";
                state->messageIsError = true;
                break;
            }
            if (rollback(state->set, &state->devInfo, state->hwnd, 0, &reboot)) {
                state->message = reboot
                                     ? L"Driver rolled back. A restart is needed."
                                     : L"Driver rolled back.";
                state->changed = true;
            } else {
                DWORD error = GetLastError();
                state->message = error == ERROR_CANCELLED
                                     ? L"Roll back cancelled."
                                     : L"No previous driver to roll back to.";
                state->messageIsError = error != ERROR_CANCELLED;
            }
            break;
        }

        case kActionToggle: {
            bool enable = state->disabled;
            if (SetDeviceEnabled(state->set, &state->devInfo, enable)) {
                state->message =
                    enable ? L"Device enabled." : L"Device disabled.";
                state->changed = true;
            } else {
                WCHAR text[128];
                _snwprintf(text, ARRAYSIZE(text) - 1,
                           L"Could not %s the device (error %u).",
                           enable ? L"enable" : L"disable",
                           static_cast<unsigned>(GetLastError()));
                text[ARRAYSIZE(text) - 1] = L'\0';
                state->message = text;
                state->messageIsError = true;
            }
            break;
        }

        case kActionUninstall: {
            std::wstring prompt =
                L"Uninstall " + state->name +
                L"?\n\nThe device will be removed until the next hardware "
                L"scan or restart.";
            if (MessageBoxW(state->hwnd, prompt.c_str(), L"Uninstall device",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) !=
                IDYES) {
                break;
            }
            auto uninstall =
                NewDevProc<DiUninstallDeviceFn>("DiUninstallDevice");
            if (!uninstall) {
                state->message = L"Uninstall is not available on this system.";
                state->messageIsError = true;
                break;
            }
            if (uninstall(state->hwnd, state->set, &state->devInfo, 0,
                          &reboot)) {
                state->changed = true;
                DestroyWindow(state->hwnd);
                return;
            }
            WCHAR text[128];
            _snwprintf(text, ARRAYSIZE(text) - 1,
                       L"Uninstall failed (error %u).",
                       static_cast<unsigned>(GetLastError()));
            text[ARRAYSIZE(text) - 1] = L'\0';
            state->message = text;
            state->messageIsError = true;
            break;
        }

        default:
            break;
    }

    ReloadStatus(state);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

LRESULT CALLBACK PropProc(HWND hwnd, UINT message, WPARAM wParam,
                          LPARAM lParam) {
    PropState* state = g_props;
    if (!state) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) {
                return 0;
            }
            break;

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            RECT client;
            GetClientRect(hwnd, &client);
            int titleHeight = ui::Scale(ui::kTitleHeight, state->dpi);
            int closeWidth = ui::Scale(ui::kCaptionButtonWidth, state->dpi);
            if (point.y < titleHeight && point.x < client.right - closeWidth) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap =
                CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
            Paint(state, memory, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0,
                   SRCCOPY);
            SelectObject(memory, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hot = -1;
            for (size_t i = 0; i < state->buttons.size(); i++) {
                if (PtInRect(&state->buttons[i].rect, point)) {
                    hot = static_cast<int>(i);
                    break;
                }
            }
            if (hot != state->hot) {
                state->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            state->hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // Copied because Invoke repaints, which rebuilds the vector.
            for (size_t i = 0; i < state->buttons.size(); i++) {
                if (PtInRect(&state->buttons[i].rect, point)) {
                    PropButton button = state->buttons[i];
                    Invoke(state, button);
                    return 0;
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // Not PostQuitMessage: this is a child window, and quitting here
            // would take the whole process down with it.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void UnregisterWindowClassIfNeeded() {
    if (g_classRegistered) {
        UnregisterClassW(kClassName, ModuleInstance());
        g_classRegistered = false;
    }
}

bool EnsureClass() {
    if (g_classRegistered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PropProc;
    wc.hInstance = ModuleInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc)) {
        g_classRegistered = true;
        return true;
    }
    if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        UnregisterClassW(kClassName, ModuleInstance());
        if (RegisterClassExW(&wc)) {
            g_classRegistered = true;
            return true;
        }
    }
    return false;
}

// Returns true when something changed and the caller should re-enumerate.
bool Show(HWND parent, HDEVINFO set, const SP_DEVINFO_DATA& devInfo,
          const DeviceInfo& device, const std::wstring& className, HICON icon,
          COLORREF cardColor, COLORREF accentColor) {
    if (!EnsureClass()) {
        return false;
    }

    PropState state;
    state.set = set;
    state.devInfo = devInfo;
    state.name = device.name;
    state.className = className;
    state.manufacturer = device.manufacturer;
    state.location = device.location;
    state.instanceId = device.instanceId;
    state.hasProblem = device.hasProblem;
    state.disabled = device.disabled;
    state.problem = device.problem;
    state.icon = icon;
    state.cardColor = cardColor;
    state.accentColor = accentColor;
    g_props = &state;

    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"Properties",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        100, 100, parent, nullptr, ModuleInstance(), nullptr);
    if (!state.hwnd) {
        g_props = nullptr;
        return false;
    }

    state.dpi = GetDpiForWindow(state.hwnd);
    state.fontTitle = MakeFont(state.dpi, 14, FW_SEMIBOLD);
    state.fontBody = MakeFont(state.dpi, 10, FW_NORMAL);
    state.fontSmall = MakeFont(state.dpi, 9, FW_NORMAL);
    state.fontStrong = MakeFont(state.dpi, 9, FW_SEMIBOLD);
    state.driver = ReadDriverInfo(set, &state.devInfo);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(state.hwnd, 20, &dark, sizeof(dark));
    DWORD corner = 2;
    DwmSetWindowAttribute(state.hwnd, 33, &corner, sizeof(corner));

    int width = ui::Scale(620, state.dpi);
    int height = ui::Scale(560, state.dpi);
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    SetWindowPos(
        state.hwnd, nullptr,
        parentRect.left + ((parentRect.right - parentRect.left) - width) / 2,
        parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2,
        width, height, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);

    EnableWindow(parent, FALSE);
    ShowWindow(state.hwnd, SW_SHOW);
    SetForegroundWindow(state.hwnd);

    MSG msg;
    while (IsWindow(state.hwnd)) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.fontTitle) DeleteObject(state.fontTitle);
    if (state.fontBody) DeleteObject(state.fontBody);
    if (state.fontSmall) DeleteObject(state.fontSmall);
    if (state.fontStrong) DeleteObject(state.fontStrong);

    bool changed = state.changed;
    g_props = nullptr;
    return changed;
}

}  // namespace props

// -----------------------------------------------------------------------------
// Window
// -----------------------------------------------------------------------------

namespace devui {

constexpr PCWSTR kClassName = L"WindhawkModernDeviceManager";

enum class ButtonKind {
    None,
    Close,
    Minimize,
    Maximize,
    Refresh,
    OpenSettings,
    Properties,
};

struct Button {
    RECT rect{};
    ButtonKind kind = ButtonKind::None;
    std::wstring label;
    std::wstring glyph;
    bool enabled = true;
    bool primary = false;
};

// One line in the middle pane. A class header, or a device under it.
struct Row {
    RECT rect{};
    int classIndex = -1;
    int deviceIndex = -1;  // -1 means the class header itself
};

struct State {
    HWND hwnd = nullptr;
    UINT dpi = 96;

    HFONT fontTitle = nullptr;
    HFONT fontHeading = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontStrong = nullptr;
    HFONT fontBold = nullptr;

    COLORREF windowColor = ui::kWindow;
    COLORREF cardColor = ui::kCard;
    COLORREF accentColor = ui::AccentColor();

    std::vector<DeviceClassInfo> classes;
    SystemSummary summary;
    HICON pcIcon = nullptr;
    HDEVINFO deviceSet = INVALID_HANDLE_VALUE;

    std::vector<Button> buttons;
    std::vector<Row> rows;

    int selectedClass = -1;
    int selectedDevice = -1;
    int hotButton = -1;
    int hotRow = -1;

    std::wstring search;
    bool searchFocused = false;
    RECT searchRect{};

    int scrollOffset = 0;
    int scrollExtent = 0;
    RECT listClip{};
};

State* g_state = nullptr;
bool g_classRegistered = false;

// -----------------------------------------------------------------------------
// Drawing helpers
// -----------------------------------------------------------------------------

// Not named DrawText: that is a macro in the Windows headers.
void DrawLabel(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
               RECT rect, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &size);
    SelectObject(dc, old);
    return size.cx;
}

void DrawSurface(HDC dc, const RECT& rect, int radius, COLORREF fill,
                 COLORREF border) {
    ui::FillRoundRect(dc, rect, radius, border);
    RECT inner = rect;
    InflateRect(&inner, -1, -1);
    ui::FillRoundRect(dc, inner, radius, fill);
}

// A chevron, drawn rather than taken from an icon font, so it cannot fall
// victim to font substitution.
void DrawChevron(HDC dc, int centerX, int centerY, int size, bool expanded,
                 COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, size / 6), color);
    HGDIOBJ old = SelectObject(dc, pen);
    POINT points[3];
    if (expanded) {
        points[0] = {centerX - size / 2, centerY - size / 4};
        points[1] = {centerX, centerY + size / 4};
        points[2] = {centerX + size / 2, centerY - size / 4};
    } else {
        points[0] = {centerX - size / 4, centerY - size / 2};
        points[1] = {centerX + size / 4, centerY};
        points[2] = {centerX - size / 4, centerY + size / 2};
    }
    Polyline(dc, points, 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawWarningDot(HDC dc, int x, int y, int size, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, x, y, x + size, y + size);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

int AddButton(State* state, RECT rect, ButtonKind kind, PCWSTR label,
              PCWSTR glyph, bool enabled, bool primary) {
    Button button;
    button.rect = rect;
    button.kind = kind;
    button.label = label ? label : L"";
    button.glyph = glyph ? glyph : L"";
    button.enabled = enabled;
    button.primary = primary;
    state->buttons.push_back(std::move(button));
    return static_cast<int>(state->buttons.size()) - 1;
}

// -----------------------------------------------------------------------------
// Filtering
// -----------------------------------------------------------------------------

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
        if (_wcsnicmp(haystack.c_str() + i, needle.c_str(), needle.size()) ==
            0) {
            return true;
        }
    }
    return false;
}

// A class survives if it matches, or if any device under it matches - so
// searching for a device name still shows which class it lives in.
bool ClassMatches(const State* state, const DeviceClassInfo& item) {
    if (state->search.empty()) {
        return true;
    }
    if (ContainsNoCase(item.name, state->search)) {
        return true;
    }
    for (const auto& device : item.devices) {
        if (ContainsNoCase(device.name, state->search)) {
            return true;
        }
    }
    return false;
}

bool DeviceMatches(const State* state, const DeviceInfo& device) {
    return state->search.empty() ||
           ContainsNoCase(device.name, state->search);
}

// -----------------------------------------------------------------------------
// Panes
// -----------------------------------------------------------------------------

void PaintTitleBar(State* state, HDC dc, const RECT& client) {
    int titleHeight = ui::Scale(ui::kTitleHeight, state->dpi);
    int buttonWidth = ui::Scale(ui::kCaptionButtonWidth, state->dpi);
    int padding = ui::Scale(ui::kPadding, state->dpi);

    RECT label{padding, 0, client.right, titleHeight};
    DrawLabel(dc, state->fontBody, ui::kTextPrimary, L"Device Manager", label,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    struct CaptionButton {
        ButtonKind kind;
        PCWSTR glyph;
    };
    // Segoe MDL2 codepoints would need that font; these are drawn from simple
    // geometry in PaintCaptionGlyph instead so nothing can substitute.
    const CaptionButton order[] = {{ButtonKind::Minimize, L""},
                                   {ButtonKind::Maximize, L""},
                                   {ButtonKind::Close, L""}};

    int right = client.right;
    for (int i = ARRAYSIZE(order) - 1; i >= 0; i--) {
        RECT rect{right - buttonWidth, 0, right, titleHeight};
        AddButton(state, rect, order[i].kind, order[i].glyph, nullptr, true,
                  false);
        right -= buttonWidth;
    }
}

void PaintCaptionGlyph(State* state, HDC dc, const Button& button, bool hot) {
    RECT rect = button.rect;
    if (hot) {
        COLORREF fill = button.kind == ButtonKind::Close ? ui::kCloseHover
                                                         : ui::kButtonHover;
        ui::FillPlain(dc, rect, fill);
    }

    int centerX = (rect.left + rect.right) / 2;
    int centerY = (rect.top + rect.bottom) / 2;
    int size = ui::Scale(10, state->dpi);
    COLORREF color = ui::kTextPrimary;
    HPEN pen = CreatePen(PS_SOLID, ui::Scale(1, state->dpi), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    switch (button.kind) {
        case ButtonKind::Minimize:
            MoveToEx(dc, centerX - size / 2, centerY, nullptr);
            LineTo(dc, centerX + size / 2, centerY);
            break;
        case ButtonKind::Maximize:
            Rectangle(dc, centerX - size / 2, centerY - size / 2,
                      centerX + size / 2, centerY + size / 2);
            break;
        case ButtonKind::Close:
            MoveToEx(dc, centerX - size / 2, centerY - size / 2, nullptr);
            LineTo(dc, centerX + size / 2 + 1, centerY + size / 2 + 1);
            MoveToEx(dc, centerX + size / 2, centerY - size / 2, nullptr);
            LineTo(dc, centerX - size / 2 - 1, centerY + size / 2 + 1);
            break;
        default:
            break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// The left pane: what machine this is, then what the window can do.
void PaintSidebar(State* state, HDC dc, const RECT& rect) {
    int padding = ui::Scale(14, state->dpi);
    int radius = ui::Scale(ui::kCardRadius, state->dpi);
    DrawSurface(dc, rect, radius, state->cardColor, ui::kCardBorder);

    int left = rect.left + padding;
    int right = rect.right - padding;
    int y = rect.top + padding;

    // The machine, pictured - the concept leads with a product shot, and the
    // shell's desktop-PC icon is the honest equivalent.
    if (state->pcIcon) {
        int size = ui::Scale(ui::kPcIconSize, state->dpi);
        DrawIconEx(dc, (rect.left + rect.right) / 2 - size / 2,
                   y + ui::Scale(6, state->dpi), state->pcIcon, size, size, 0,
                   nullptr, DI_NORMAL);
        y += size + ui::Scale(18, state->dpi);
    }

    RECT line{left, y, right, y + ui::Scale(26, state->dpi)};
    DrawLabel(dc, state->fontHeading, ui::kTextPrimary,
              state->summary.computerName.empty() ? L"This PC"
                                                  : state->summary.computerName,
              line, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
    y = line.bottom + ui::Scale(6, state->dpi);

    auto spec = [&](const std::wstring& text) {
        if (text.empty()) {
            return;
        }
        RECT row{left, y, right, y + ui::Scale(19, state->dpi)};
        DrawLabel(dc, state->fontSmall, ui::kTextSecondary, text, row,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        y = row.bottom + ui::Scale(2, state->dpi);
    };
    spec(state->summary.processor);

    // Graphics can list more than one adapter; each gets its own line.
    std::wstring graphics = state->summary.graphics;
    while (!graphics.empty()) {
        size_t split = graphics.find(L'\n');
        spec(graphics.substr(0, split));
        if (split == std::wstring::npos) {
            break;
        }
        graphics.erase(0, split + 1);
    }
    spec(state->summary.memory);

    y += ui::Scale(10, state->dpi);
    RECT divider{left, y, right, y + ui::Scale(1, state->dpi)};
    ui::FillPlain(dc, divider, ui::kDivider);
    y = divider.bottom + ui::Scale(12, state->dpi);

    if (state->summary.driveTotal > 0) {
        std::wstring caption = state->summary.driveLabel + L" - " +
                               ui::FormatSize(state->summary.driveTotal);
        RECT driveLine{left, y, right, y + ui::Scale(20, state->dpi)};
        DrawLabel(dc, state->fontBody, ui::kTextPrimary, caption, driveLine,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        y = driveLine.bottom + ui::Scale(8, state->dpi);

        ULONGLONG used = state->summary.driveTotal - state->summary.driveFree;
        int barHeight = ui::Scale(4, state->dpi);
        RECT track{left, y, right, y + barHeight};
        ui::FillRoundRect(dc, track, barHeight / 2, ui::kBarTrack);
        int fillWidth = static_cast<int>(
            (right - left) *
            static_cast<double>(used) /
            static_cast<double>(state->summary.driveTotal));
        if (fillWidth > 0) {
            RECT fill{left, y, left + std::max(fillWidth, barHeight),
                      y + barHeight};
            ui::FillRoundRect(dc, fill, barHeight / 2, state->accentColor);
        }
        y = track.bottom + ui::Scale(7, state->dpi);

        RECT usedLine{left, y, (left + right) / 2, y + ui::Scale(18, state->dpi)};
        DrawLabel(dc, state->fontSmall, ui::kTextSecondary,
                  ui::FormatSize(used) + L" used", usedLine,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        RECT freeLine{(left + right) / 2, y, right, usedLine.bottom};
        DrawLabel(dc, state->fontSmall, ui::kTextSecondary,
                  ui::FormatSize(state->summary.driveFree) + L" free", freeLine,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        y = usedLine.bottom + ui::Scale(12, state->dpi);
    }

    RECT divider2{left, y, right, y + ui::Scale(1, state->dpi)};
    ui::FillPlain(dc, divider2, ui::kDivider);
    y = divider2.bottom + ui::Scale(10, state->dpi);

    struct Action {
        ButtonKind kind;
        PCWSTR label;
    };
    const Action actions[] = {
        {ButtonKind::Refresh, L"Refresh devices list"},
        {ButtonKind::OpenSettings, L"System information in Settings"},
    };

    int rowHeight = ui::Scale(32, state->dpi);
    for (const Action& action : actions) {
        RECT row{rect.left + ui::Scale(6, state->dpi), y,
                 rect.right - ui::Scale(6, state->dpi), y + rowHeight};
        AddButton(state, row, action.kind, action.label, nullptr, true, false);
        y = row.bottom + ui::Scale(2, state->dpi);
    }
}

void PaintSidebarAction(State* state, HDC dc, const Button& button, bool hot) {
    if (hot && button.enabled) {
        ui::FillRoundRect(dc, button.rect, ui::Scale(5, state->dpi),
                          ui::kRowHover);
    }
    RECT text = button.rect;
    text.left += ui::Scale(10, state->dpi);
    text.right -= ui::Scale(8, state->dpi);
    DrawLabel(dc, state->fontBold,
              button.enabled ? ui::kTextSecondary : ui::kTextDisabled,
              button.label, text,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
}

void PaintSearchBox(State* state, HDC dc, const RECT& rect) {
    state->searchRect = rect;
    int radius = ui::Scale(5, state->dpi);
    COLORREF border = state->searchFocused ? state->accentColor
                                           : ui::kCardBorder;
    DrawSurface(dc, rect, radius, ui::MixColors(state->cardColor, RGB(0, 0, 0),
                                                18),
                border);

    RECT text = rect;
    text.left += ui::Scale(10, state->dpi);
    text.right -= ui::Scale(10, state->dpi);
    bool empty = state->search.empty();
    DrawLabel(dc, state->fontSmall,
              empty ? ui::kTextTertiary : ui::kTextPrimary,
              empty ? std::wstring(L"Search") : state->search, text,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
}

// The middle pane: device classes, expandable into their devices.
void PaintList(State* state, HDC dc, const RECT& rect) {
    int radius = ui::Scale(ui::kCardRadius, state->dpi);
    DrawSurface(dc, rect, radius, state->cardColor, ui::kCardBorder);

    int padding = ui::Scale(14, state->dpi);
    int left = rect.left + padding;
    int right = rect.right - padding;
    int y = rect.top + padding;

    RECT heading{left, y, right, y + ui::Scale(24, state->dpi)};
    DrawLabel(dc, state->fontHeading, ui::kTextPrimary, L"Device type", heading,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    int total = 0;
    for (const auto& item : state->classes) {
        total += static_cast<int>(item.devices.size());
    }
    WCHAR countText[64];
    _snwprintf(countText, ARRAYSIZE(countText) - 1, L"%d devices", total);
    countText[ARRAYSIZE(countText) - 1] = L'\0';
    DrawLabel(dc, state->fontSmall, ui::kTextTertiary, countText, heading,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    y = heading.bottom + ui::Scale(10, state->dpi);

    RECT search{left, y, right, y + ui::Scale(ui::kSearchHeight, state->dpi)};
    PaintSearchBox(state, dc, search);
    y = search.bottom + ui::Scale(10, state->dpi);

    RECT clip{rect.left + ui::Scale(4, state->dpi), y,
              rect.right - ui::Scale(4, state->dpi),
              rect.bottom - ui::Scale(8, state->dpi)};
    state->listClip = clip;

    HRGN region = CreateRectRgn(clip.left, clip.top, clip.right, clip.bottom);
    SelectClipRgn(dc, region);

    int classRowHeight = ui::Scale(ui::kClassRowHeight, state->dpi);
    int rowHeight = ui::Scale(ui::kRowHeight, state->dpi);
    int classIconSize = ui::Scale(ui::kClassIconSize, state->dpi);
    int cursor = clip.top - state->scrollOffset;
    int contentHeight = 0;

    int deviceIconSize = ui::Scale(16, state->dpi);

    for (size_t c = 0; c < state->classes.size(); c++) {
        DeviceClassInfo& item = state->classes[c];
        if (!ClassMatches(state, item)) {
            continue;
        }
        // Single chokepoint for the lazy load: whatever expanded the class -
        // click, Enter, right arrow, the setting - lands here before drawing.
        if (item.expanded) {
            EnsureDeviceIcons(state->deviceSet, item, deviceIconSize);
        }

        RECT row{clip.left + ui::Scale(4, state->dpi), cursor,
                 clip.right - ui::Scale(4, state->dpi), cursor + classRowHeight};
        bool selected = state->selectedClass == static_cast<int>(c) &&
                        state->selectedDevice < 0;
        int rowIndex = static_cast<int>(state->rows.size());
        state->rows.push_back({row, static_cast<int>(c), -1});

        if (row.bottom > clip.top && row.top < clip.bottom) {
            if (selected) {
                ui::FillRoundRect(dc, row, ui::Scale(5, state->dpi),
                                  ui::MixColors(state->cardColor,
                                                state->accentColor, 26));
            } else if (state->hotRow == rowIndex) {
                ui::FillRoundRect(dc, row, ui::Scale(5, state->dpi),
                                  ui::kRowHover);
            }

            int chevronX = row.left + ui::Scale(12, state->dpi);
            DrawChevron(dc, chevronX, (row.top + row.bottom) / 2,
                        ui::Scale(8, state->dpi), item.expanded,
                        ui::kTextSecondary);

            if (item.icon) {
                DrawIconEx(dc, row.left + ui::Scale(24, state->dpi),
                           (row.top + row.bottom) / 2 - classIconSize / 2,
                           item.icon, classIconSize, classIconSize, 0, nullptr,
                           DI_NORMAL);
            }

            RECT text = row;
            text.left += ui::Scale(24 + ui::kClassIconSize + 12, state->dpi);
            text.right -= ui::Scale(46, state->dpi);
            DrawLabel(dc, state->fontBody, ui::kTextPrimary, item.name, text,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                          DT_NOPREFIX);

            // The device count, so a class is worth expanding or not before
            // clicking it.
            WCHAR count[16];
            _snwprintf(count, ARRAYSIZE(count) - 1, L"%d",
                       static_cast<int>(item.devices.size()));
            count[ARRAYSIZE(count) - 1] = L'\0';
            RECT countRect = row;
            countRect.right -= ui::Scale(14, state->dpi);
            DrawLabel(dc, state->fontSmall, ui::kTextTertiary, count, countRect,
                      DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        cursor = row.bottom;
        contentHeight += classRowHeight;

        if (!item.expanded) {
            continue;
        }
        for (size_t d = 0; d < item.devices.size(); d++) {
            const DeviceInfo& device = item.devices[d];
            if (!DeviceMatches(state, device)) {
                continue;
            }
            RECT deviceRow{row.left, cursor, row.right, cursor + rowHeight};
            bool deviceSelected = state->selectedClass == static_cast<int>(c) &&
                                  state->selectedDevice == static_cast<int>(d);
            int deviceRowIndex = static_cast<int>(state->rows.size());
            state->rows.push_back(
                {deviceRow, static_cast<int>(c), static_cast<int>(d)});

            if (deviceRow.bottom > clip.top && deviceRow.top < clip.bottom) {
                if (deviceSelected) {
                    ui::FillRoundRect(dc, deviceRow, ui::Scale(5, state->dpi),
                                      ui::MixColors(state->cardColor,
                                                    state->accentColor, 34));
                } else if (state->hotRow == deviceRowIndex) {
                    ui::FillRoundRect(dc, deviceRow, ui::Scale(5, state->dpi),
                                      ui::kRowHover);
                }

                if (device.icon) {
                    DrawIconEx(dc,
                               deviceRow.left +
                                   ui::Scale(24 + ui::kClassIconSize + 4,
                                             state->dpi),
                               (deviceRow.top + deviceRow.bottom) / 2 -
                                   deviceIconSize / 2,
                               device.icon, deviceIconSize, deviceIconSize, 0,
                               nullptr, DI_NORMAL);
                }

                RECT text = deviceRow;
                text.left += ui::Scale(24 + ui::kClassIconSize + 26, state->dpi);
                text.right -= ui::Scale(22, state->dpi);
                COLORREF color = device.disabled ? ui::kTextDisabled
                                                 : ui::kTextSecondary;
                DrawLabel(dc, state->fontSmall, color, device.name, text,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT |
                              DT_END_ELLIPSIS | DT_NOPREFIX);

                if (device.hasProblem) {
                    int dot = ui::Scale(6, state->dpi);
                    DrawWarningDot(dc, deviceRow.right - ui::Scale(16, state->dpi),
                                   (deviceRow.top + deviceRow.bottom) / 2 -
                                       dot / 2,
                                   dot, ui::kWarning);
                }
            }
            cursor = deviceRow.bottom;
            contentHeight += rowHeight;
        }
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(region);

    int visible = clip.bottom - clip.top;
    state->scrollExtent = std::max(0, contentHeight - visible);
    if (state->scrollOffset > state->scrollExtent) {
        state->scrollOffset = state->scrollExtent;
    }

    if (state->scrollExtent > 0) {
        int barWidth = ui::Scale(ui::kScrollBarWidth, state->dpi);
        int thumbHeight = std::max(ui::Scale(28, state->dpi),
                                   visible * visible / std::max(1, contentHeight));
        int travel = visible - thumbHeight;
        int thumbTop = clip.top +
                       (travel > 0 ? travel * state->scrollOffset /
                                         state->scrollExtent
                                   : 0);
        RECT thumb{clip.right - barWidth, thumbTop, clip.right,
                   thumbTop + thumbHeight};
        ui::FillRoundRect(dc, thumb, barWidth / 2, ui::kTextTertiary);
    }
}

// The right pane: everything known about the selection, and one button.
void PaintDetails(State* state, HDC dc, const RECT& rect) {
    int radius = ui::Scale(ui::kCardRadius, state->dpi);
    DrawSurface(dc, rect, radius, state->cardColor, ui::kCardBorder);

    const DeviceInfo* device = nullptr;
    if (state->selectedClass >= 0 &&
        state->selectedClass < static_cast<int>(state->classes.size()) &&
        state->selectedDevice >= 0) {
        const auto& devices = state->classes[state->selectedClass].devices;
        if (state->selectedDevice < static_cast<int>(devices.size())) {
            device = &devices[state->selectedDevice];
        }
    }

    if (!device) {
        RECT empty = rect;
        DrawLabel(dc, state->fontSmall, ui::kTextTertiary,
                  L"Select a device to view more information", empty,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        return;
    }

    int padding = ui::Scale(22, state->dpi);
    int left = rect.left + padding;
    int right = rect.right - padding;
    int y = rect.top + padding;

    RECT nameLine{left, y, right, y + ui::Scale(30, state->dpi)};
    DrawLabel(dc, state->fontTitle, ui::kTextPrimary, device->name, nameLine,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    y = nameLine.bottom + ui::Scale(4, state->dpi);

    std::wstring status = L"This device is working properly.";
    COLORREF statusColor = ui::kTextSecondary;
    if (device->disabled) {
        status = L"This device is disabled.";
        statusColor = ui::kWarning;
    } else if (device->hasProblem) {
        WCHAR text[128];
        _snwprintf(text, ARRAYSIZE(text) - 1,
                   L"This device has a problem (code %u).",
                   static_cast<unsigned>(device->problem));
        text[ARRAYSIZE(text) - 1] = L'\0';
        status = text;
        statusColor = ui::kWarning;
    }
    RECT statusLine{left, y, right, y + ui::Scale(20, state->dpi)};
    DrawLabel(dc, state->fontSmall, statusColor, status, statusLine,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    y = statusLine.bottom + ui::Scale(16, state->dpi);

    RECT divider{left, y, right, y + ui::Scale(1, state->dpi)};
    ui::FillPlain(dc, divider, ui::kDivider);
    y = divider.bottom + ui::Scale(14, state->dpi);

    auto field = [&](PCWSTR label, const std::wstring& value) {
        if (value.empty()) {
            return;
        }
        RECT labelRect{left, y, left + ui::Scale(150, state->dpi),
                       y + ui::Scale(20, state->dpi)};
        DrawLabel(dc, state->fontSmall, ui::kTextTertiary, label, labelRect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        RECT valueRect{labelRect.right, y, right, labelRect.bottom};
        DrawLabel(dc, state->fontSmall, ui::kTextPrimary, value, valueRect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        y = labelRect.bottom + ui::Scale(6, state->dpi);
    };

    field(L"Manufacturer", device->manufacturer);
    if (state->selectedClass >= 0) {
        field(L"Device type", state->classes[state->selectedClass].name);
    }
    field(L"Driver service", device->service);
    field(L"Location", device->location);
    field(L"Device instance", device->instanceId);

    int buttonHeight = ui::Scale(32, state->dpi);
    int buttonWidth = ui::Scale(110, state->dpi);
    RECT properties{left, rect.bottom - padding - buttonHeight,
                    left + buttonWidth, rect.bottom - padding};
    AddButton(state, properties, ButtonKind::Properties, L"Properties", nullptr,
              !device->instanceId.empty(), true);
}

void PaintActionButton(State* state, HDC dc, const Button& button, bool hot) {
    COLORREF fill = button.primary ? state->accentColor
                                   : ui::MixColors(state->cardColor,
                                                   RGB(255, 255, 255), 8);
    if (!button.enabled) {
        fill = ui::MixColors(state->cardColor, RGB(255, 255, 255), 4);
    } else if (hot) {
        fill = ui::MixColors(fill, RGB(255, 255, 255), 12);
    }
    ui::FillRoundRect(dc, button.rect, ui::Scale(5, state->dpi), fill);
    DrawLabel(dc, state->fontStrong,
              button.enabled ? ui::kTextPrimary : ui::kTextDisabled,
              button.label, button.rect,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

void Paint(State* state, HDC dc, const RECT& client) {
    state->buttons.clear();
    state->rows.clear();

    ui::FillPlain(dc, client, state->windowColor);
    PaintTitleBar(state, dc, client);

    int padding = ui::Scale(ui::kPadding, state->dpi);
    int titleHeight = ui::Scale(ui::kTitleHeight, state->dpi);
    int top = titleHeight;
    int bottom = client.bottom - padding;

    int sidebarWidth = ui::Scale(ui::kSidebarWidth, state->dpi);
    int listWidth = ui::Scale(ui::kListWidth, state->dpi);

    RECT sidebar{padding, top, padding + sidebarWidth, bottom};
    RECT list{sidebar.right + padding, top,
              sidebar.right + padding + listWidth, bottom};
    RECT details{list.right + padding, top, client.right - padding, bottom};

    PaintSidebar(state, dc, sidebar);
    PaintList(state, dc, list);
    PaintDetails(state, dc, details);

    // Buttons are drawn last so hover states sit above the surfaces they are on.
    for (size_t i = 0; i < state->buttons.size(); i++) {
        const Button& button = state->buttons[i];
        bool hot = state->hotButton == static_cast<int>(i);
        switch (button.kind) {
            case ButtonKind::Close:
            case ButtonKind::Minimize:
            case ButtonKind::Maximize:
                PaintCaptionGlyph(state, dc, button, hot);
                break;
            case ButtonKind::Properties:
                PaintActionButton(state, dc, button, hot);
                break;
            default:
                PaintSidebarAction(state, dc, button, hot);
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// Interaction
// -----------------------------------------------------------------------------

void Refresh(State* state) {
    ReleaseClassIcons(state->classes);
    if (state->deviceSet != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(state->deviceSet);
        state->deviceSet = INVALID_HANDLE_VALUE;
    }
    state->classes = EnumerateDevices(&state->deviceSet);
    state->summary = ReadSystemSummary(state->classes);
    state->selectedClass = -1;
    state->selectedDevice = -1;
    state->scrollOffset = 0;
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

// Opens the mod's own properties window. The icon is re-loaded at 48px rather
// than scaling up the 16px row icon - native sizes only.
void OpenProperties(State* state, int classIndex, int deviceIndex) {
    if (classIndex < 0 ||
        classIndex >= static_cast<int>(state->classes.size())) {
        return;
    }
    DeviceClassInfo& item = state->classes[classIndex];
    if (deviceIndex < 0 ||
        deviceIndex >= static_cast<int>(item.devices.size())) {
        return;
    }
    DeviceInfo& device = item.devices[deviceIndex];

    HICON large = nullptr;
    auto loadDeviceIcon = LoadDeviceIconProc();
    int pixels = ui::Scale(48, state->dpi);
    if (loadDeviceIcon) {
        loadDeviceIcon(state->deviceSet, &device.devInfo,
                       static_cast<UINT>(pixels), static_cast<UINT>(pixels), 0,
                       &large);
    }

    bool changed = props::Show(state->hwnd, state->deviceSet, device.devInfo,
                               device, item.name, large ? large : item.icon,
                               state->cardColor, state->accentColor);
    if (large) {
        DestroyIcon(large);
    }

    if (changed) {
        Refresh(state);
    } else {
        InvalidateRect(state->hwnd, nullptr, FALSE);
    }
}

void Invoke(State* state, const Button& button) {
    if (!button.enabled) {
        return;
    }
    switch (button.kind) {
        case ButtonKind::Close:
            PostMessageW(state->hwnd, WM_CLOSE, 0, 0);
            break;
        case ButtonKind::Minimize:
            ShowWindow(state->hwnd, SW_MINIMIZE);
            break;
        case ButtonKind::Maximize:
            ShowWindow(state->hwnd,
                       IsZoomed(state->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case ButtonKind::Refresh:
            Refresh(state);
            break;
        case ButtonKind::OpenSettings:
            LaunchDetached(L"ms-settings:about", nullptr);
            break;
        case ButtonKind::Properties:
            OpenProperties(state, state->selectedClass, state->selectedDevice);
            break;
        default:
            break;
    }
}

// Moves through the flattened rows, so arrow keys walk classes and their
// expanded devices in the order they are drawn.
void MoveSelection(State* state, int delta) {
    if (state->rows.empty()) {
        return;
    }
    int current = -1;
    for (size_t i = 0; i < state->rows.size(); i++) {
        if (state->rows[i].classIndex == state->selectedClass &&
            state->rows[i].deviceIndex == state->selectedDevice) {
            current = static_cast<int>(i);
            break;
        }
    }
    int next = current < 0 ? (delta > 0 ? 0
                                        : static_cast<int>(state->rows.size()) - 1)
                           : current + delta;
    next = std::max(0, std::min(next, static_cast<int>(state->rows.size()) - 1));

    state->selectedClass = state->rows[next].classIndex;
    state->selectedDevice = state->rows[next].deviceIndex;

    // Keep the new selection inside the clip.
    const RECT& row = state->rows[next].rect;
    if (row.top < state->listClip.top) {
        state->scrollOffset -= state->listClip.top - row.top;
    } else if (row.bottom > state->listClip.bottom) {
        state->scrollOffset += row.bottom - state->listClip.bottom;
    }
    state->scrollOffset = std::max(0, state->scrollOffset);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

bool ActivateRow(State* state, const Row& row) {
    state->selectedClass = row.classIndex;
    state->selectedDevice = row.deviceIndex;
    if (row.deviceIndex < 0 &&
        row.classIndex < static_cast<int>(state->classes.size())) {
        state->classes[row.classIndex].expanded =
            !state->classes[row.classIndex].expanded;
    }
    InvalidateRect(state->hwnd, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                         LPARAM lParam) {
    State* state = g_state;
    if (!state) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) {
                return 0;  // the frame exists, but none of it is drawn
            }
            break;

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            RECT client;
            GetClientRect(hwnd, &client);

            int edge = ui::Scale(6, state->dpi);
            bool left = point.x < edge;
            bool right = point.x >= client.right - edge;
            bool top = point.y < edge;
            bool bottom = point.y >= client.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            int titleHeight = ui::Scale(ui::kTitleHeight, state->dpi);
            int captions = ui::Scale(ui::kCaptionButtonWidth, state->dpi) * 3;
            if (point.y < titleHeight && point.x < client.right - captions) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = ui::Scale(900, state->dpi);
            info->ptMinTrackSize.y = ui::Scale(520, state->dpi);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap =
                CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
            Paint(state, memory, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0,
                   SRCCOPY);
            SelectObject(memory, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int button = -1;
            for (size_t i = 0; i < state->buttons.size(); i++) {
                if (PtInRect(&state->buttons[i].rect, point)) {
                    button = static_cast<int>(i);
                    break;
                }
            }
            int row = -1;
            if (PtInRect(&state->listClip, point)) {
                for (size_t i = 0; i < state->rows.size(); i++) {
                    if (PtInRect(&state->rows[i].rect, point)) {
                        row = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (button != state->hotButton || row != state->hotRow) {
                state->hotButton = button;
                state->hotRow = row;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            state->hotButton = -1;
            state->hotRow = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            if (state->scrollExtent <= 0) {
                return 0;
            }
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            state->scrollOffset -=
                delta * ui::Scale(ui::kRowHeight, state->dpi) / WHEEL_DELTA * 3;
            state->scrollOffset =
                std::max(0, std::min(state->scrollOffset, state->scrollExtent));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetFocus(hwnd);

            state->searchFocused = PtInRect(&state->searchRect, point) != FALSE;

            for (const auto& button : state->buttons) {
                if (PtInRect(&button.rect, point)) {
                    Invoke(state, button);
                    return 0;
                }
            }
            if (PtInRect(&state->listClip, point)) {
                for (const auto& row : state->rows) {
                    if (PtInRect(&row.rect, point)) {
                        ActivateRow(state, row);
                        return 0;
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PtInRect(&state->listClip, point)) {
                for (const auto& row : state->rows) {
                    if (PtInRect(&row.rect, point) && row.deviceIndex >= 0) {
                        OpenProperties(state, row.classIndex, row.deviceIndex);
                        return 0;
                    }
                }
            }
            return 0;
        }

        case WM_CHAR: {
            WCHAR ch = static_cast<WCHAR>(wParam);
            if (ch == VK_BACK) {
                if (!state->search.empty()) {
                    state->search.pop_back();
                    state->scrollOffset = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (ch >= L' ') {
                state->search.push_back(ch);
                state->searchFocused = true;
                state->scrollOffset = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN:
            switch (wParam) {
                case VK_ESCAPE:
                    if (!state->search.empty()) {
                        state->search.clear();
                        state->scrollOffset = 0;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    }
                    return 0;
                case VK_F5:
                    Refresh(state);
                    return 0;
                case VK_DOWN:
                    MoveSelection(state, 1);
                    return 0;
                case VK_UP:
                    MoveSelection(state, -1);
                    return 0;
                case VK_RIGHT:
                    if (state->selectedClass >= 0 &&
                        state->selectedDevice < 0) {
                        state->classes[state->selectedClass].expanded = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case VK_LEFT:
                    if (state->selectedClass >= 0 &&
                        state->selectedDevice < 0) {
                        state->classes[state->selectedClass].expanded = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case VK_RETURN: {
                    if (state->selectedClass >= 0 &&
                        state->selectedDevice >= 0) {
                        OpenProperties(state, state->selectedClass,
                                       state->selectedDevice);
                    } else if (state->selectedClass >= 0) {
                        state->classes[state->selectedClass].expanded =
                            !state->classes[state->selectedClass].expanded;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                default:
                    break;
            }
            break;

        case WM_DPICHANGED:
            state->dpi = HIWORD(wParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void UnregisterWindowClassIfNeeded() {
    if (g_classRegistered) {
        UnregisterClassW(kClassName, ModuleInstance());
        g_classRegistered = false;
    }
}

bool EnsureClass() {
    if (g_classRegistered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ModuleInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc)) {
        g_classRegistered = true;
        return true;
    }
    if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        UnregisterClassW(kClassName, ModuleInstance());
        if (RegisterClassExW(&wc)) {
            g_classRegistered = true;
            return true;
        }
    }
    return false;
}

// Shows the window and runs it to completion. This owns the process: mmc.exe
// exits when the window closes.
bool Run() {
    if (!EnsureClass()) {
        return false;
    }

    State state;
    g_state = &state;

    state.hwnd = CreateWindowExW(
        0, kClassName, L"Device Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
        ModuleInstance(), nullptr);
    if (!state.hwnd) {
        g_state = nullptr;
        return false;
    }

    state.dpi = GetDpiForWindow(state.hwnd);
    state.fontTitle = MakeFont(state.dpi, 15, FW_SEMIBOLD);
    state.fontHeading = MakeFont(state.dpi, 12, FW_SEMIBOLD);
    state.fontBody = MakeFont(state.dpi, 10, FW_NORMAL);
    state.fontSmall = MakeFont(state.dpi, 9, FW_NORMAL);
    state.fontStrong = MakeFont(state.dpi, 9, FW_SEMIBOLD);
    state.fontBold = MakeFont(state.dpi, 9, FW_BOLD);
    state.accentColor = ui::AccentColor();

    if (g_settings.wallpaperTint) {
        state.windowColor = ui::WallpaperTinted(ui::kWindow, 14);
        state.cardColor = ui::WallpaperTinted(ui::kCard, 12);
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(state.hwnd, 20, &dark, sizeof(dark));
    DWORD corner = 2;
    DwmSetWindowAttribute(state.hwnd, 33, &corner, sizeof(corner));

    int width = ui::Scale(ui::kWindowWidth, state.dpi);
    int height = ui::Scale(ui::kWindowHeight, state.dpi);
    HMONITOR monitor = MonitorFromWindow(state.hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    RECT work = monitorInfo.rcWork;
    width = std::min<int>(width, work.right - work.left - ui::Scale(80, state.dpi));
    height =
        std::min<int>(height, work.bottom - work.top - ui::Scale(80, state.dpi));
    SetWindowPos(state.hwnd, nullptr,
                 work.left + ((work.right - work.left) - width) / 2,
                 work.top + ((work.bottom - work.top) - height) / 2, width,
                 height, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);

    g_classIconPixels = ui::Scale(ui::kClassIconSize, state.dpi);
    state.pcIcon = LoadPcIcon(ui::Scale(ui::kPcIconSize, state.dpi));

    state.classes = EnumerateDevices(&state.deviceSet);
    state.summary = ReadSystemSummary(state.classes);
    if (g_settings.expandFirstClass && !state.classes.empty()) {
        state.classes.front().expanded = true;
    }

    ShowWindow(state.hwnd, SW_SHOW);
    SetForegroundWindow(state.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseClassIcons(state.classes);
    if (state.deviceSet != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(state.deviceSet);
        state.deviceSet = INVALID_HANDLE_VALUE;
    }
    if (state.pcIcon) DestroyIcon(state.pcIcon);
    if (state.fontTitle) DeleteObject(state.fontTitle);
    if (state.fontHeading) DeleteObject(state.fontHeading);
    if (state.fontBody) DeleteObject(state.fontBody);
    if (state.fontSmall) DeleteObject(state.fontSmall);
    if (state.fontStrong) DeleteObject(state.fontStrong);
    if (state.fontBold) DeleteObject(state.fontBold);
    g_state = nullptr;
    return true;
}

}  // namespace devui

// -----------------------------------------------------------------------------
// Mod entry points
// -----------------------------------------------------------------------------

bool LaunchedForDeviceManager() {
    // An escape hatch: WH_DEVMGMT_CLASSIC=1 gets the original console for one
    // launch, without disabling the mod.
    WCHAR classic[8] = L"";
    if (GetEnvironmentVariableW(L"WH_DEVMGMT_CLASSIC", classic,
                                ARRAYSIZE(classic)) &&
        classic[0] == L'1') {
        return false;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    bool isDeviceManager = false;
    for (int i = 1; i < argc; i++) {
        std::wstring argument = argv[i];
        for (auto& ch : argument) {
            ch = towlower(ch);
        }
        if (argument.find(L"devmgmt.msc") != std::wstring::npos) {
            isDeviceManager = true;
            break;
        }
    }
    LocalFree(argv);
    return isDeviceManager;
}

void LoadSettings() {
    g_settings.wallpaperTint = Wh_GetIntSetting(L"wallpaperTint") != 0;
    g_settings.showHiddenDevices = Wh_GetIntSetting(L"showHiddenDevices") != 0;
    g_settings.expandFirstClass = Wh_GetIntSetting(L"expandFirstClass") != 0;
    g_settings.verboseLog = Wh_GetIntSetting(L"verboseLog") != 0;
}

// mmc's main thread, parked by the window thread below. Wh_ModInit runs on this
// thread, so it cannot suspend itself.
DWORD g_mainThreadId;

void ParkMainThread() {
    if (!g_mainThreadId) {
        return;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_mainThreadId);
    if (thread) {
        SuspendThread(thread);
        CloseHandle(thread);
    }
}

DWORD WINAPI WindowThread(LPVOID) {
    // First thing, before anything slow: stop the console from loading.
    ParkMainThread();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool shown = devui::Run();
    CoUninitialize();

    if (!shown) {
        Wh_Log(L"could not create the window; nothing is shown");
    }
    ExitProcess(0);
    return 0;
}

BOOL Wh_ModInit() {
    LoadSettings();

    if (!LaunchedForDeviceManager()) {
        return TRUE;  // some other snap-in; leave MMC alone
    }

    // Never run a message loop inside Wh_ModInit - it hangs Windhawk on
    // "Initializing...". The window runs on its own thread instead.
    g_mainThreadId = GetCurrentThreadId();
    HANDLE thread = CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    devui::UnregisterWindowClassIfNeeded();
    props::UnregisterWindowClassIfNeeded();
}
