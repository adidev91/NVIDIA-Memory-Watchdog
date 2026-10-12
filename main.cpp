#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <dwmapi.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>
#include <process.h>
#include <cstdint>

#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

// =============================================================================
// Minimal NVML declarations.
// We load nvml.dll dynamically, so no CUDA/NVML SDK is required.
// =============================================================================

using nvmlReturn_t = int;
struct nvmlDevice_st;
using nvmlDevice_t = nvmlDevice_st*;

constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr nvmlReturn_t NVML_ERROR_NOT_SUPPORTED = 3;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;
constexpr int NVML_PSTATE_UNKNOWN = 32;
constexpr unsigned int NVML_MAX_GPU_PERF_PSTATES = 16;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0;

struct nvmlClockOffset_v1_t
{
    unsigned int version;
    int type;
    int pstate;
    int clockOffsetMHz;
    int minClockOffsetMHz;
    int maxClockOffsetMHz;
};

constexpr unsigned int NVML_CLOCK_OFFSET_V1 =
    (1u << 24) | static_cast<unsigned int>(sizeof(nvmlClockOffset_v1_t));

using PFN_nvmlInit_v2                       = nvmlReturn_t (*)();
using PFN_nvmlShutdown                      = nvmlReturn_t (*)();
using PFN_nvmlDeviceGetHandleByIndex_v2     = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using PFN_nvmlDeviceGetClockInfo            = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_nvmlDeviceGetSupportedMemoryClocks = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);
using PFN_nvmlDeviceGetSupportedPerformanceStates = nvmlReturn_t (*)(nvmlDevice_t, int*, unsigned int);
using PFN_nvmlDeviceGetMinMaxClockOfPState = nvmlReturn_t (*)(nvmlDevice_t, int, int, unsigned int*, unsigned int*);
using PFN_nvmlDeviceGetClockOffsets         = nvmlReturn_t (*)(nvmlDevice_t, nvmlClockOffset_v1_t*);
using PFN_nvmlDeviceGetMemClkVfOffset       = nvmlReturn_t (*)(nvmlDevice_t, int*);
using PFN_nvmlDeviceGetPerformanceState     = nvmlReturn_t (*)(nvmlDevice_t, int*);
using PFN_nvmlDeviceGetPowerUsage            = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using PFN_nvmlDeviceGetTemperature           = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
using PFN_nvmlDeviceSetMemoryLockedClocks   = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int);
using PFN_nvmlDeviceResetMemoryLockedClocks = nvmlReturn_t (*)(nvmlDevice_t);
using PFN_nvmlDeviceGetName                  = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using PFN_nvmlErrorString                    = const char* (*)(nvmlReturn_t);

struct Telemetry
{
    unsigned int memMHz = 0;
    unsigned int gpuMHz = 0;
    int pstate = -1;
    int memOffsetMHz = 0;
    bool memOffsetAvailable = false;
    double watts = -1.0;
    int tempC = -1;
};

struct PStateChoice
{
    int pstate = -1;
    unsigned int reportedMemMinMHz = 0;
    unsigned int reportedMemMaxMHz = 0;
    unsigned int lockMinMHz = 0;
    unsigned int lockMaxMHz = 0;
    int memOffsetMHz = 0;
    bool memOffsetAvailable = false;
    bool controllable = false;
};

class Nvml
{
public:
    bool Open(unsigned int gpuIndex, std::string& error)
    {
        Close();

        // Elevated process: only load NVML from the trusted Windows System32
        // directory. Never fall back to the application/current directory.
        dll_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

        if (!dll_)
        {
            error = "Could not load nvml.dll.";
            return false;
        }

        init_       = Load<PFN_nvmlInit_v2>("nvmlInit_v2");
        shutdown_   = Load<PFN_nvmlShutdown>("nvmlShutdown");
        getHandle_  = Load<PFN_nvmlDeviceGetHandleByIndex_v2>("nvmlDeviceGetHandleByIndex_v2");
        getClock_   = Load<PFN_nvmlDeviceGetClockInfo>("nvmlDeviceGetClockInfo");
        getSupportedMemClocks_ = Load<PFN_nvmlDeviceGetSupportedMemoryClocks>("nvmlDeviceGetSupportedMemoryClocks");
        getSupportedPstates_ = Load<PFN_nvmlDeviceGetSupportedPerformanceStates>("nvmlDeviceGetSupportedPerformanceStates");
        getMinMaxClockOfPstate_ = Load<PFN_nvmlDeviceGetMinMaxClockOfPState>("nvmlDeviceGetMinMaxClockOfPState");
        getClockOffsets_ = Load<PFN_nvmlDeviceGetClockOffsets>("nvmlDeviceGetClockOffsets");
        getMemClkVfOffset_ = Load<PFN_nvmlDeviceGetMemClkVfOffset>("nvmlDeviceGetMemClkVfOffset");
        getPstate_  = Load<PFN_nvmlDeviceGetPerformanceState>("nvmlDeviceGetPerformanceState");
        getPower_   = Load<PFN_nvmlDeviceGetPowerUsage>("nvmlDeviceGetPowerUsage");
        getTemp_    = Load<PFN_nvmlDeviceGetTemperature>("nvmlDeviceGetTemperature");
        setMem_     = Load<PFN_nvmlDeviceSetMemoryLockedClocks>("nvmlDeviceSetMemoryLockedClocks");
        resetMem_   = Load<PFN_nvmlDeviceResetMemoryLockedClocks>("nvmlDeviceResetMemoryLockedClocks");
        getName_    = Load<PFN_nvmlDeviceGetName>("nvmlDeviceGetName");
        errorString_= Load<PFN_nvmlErrorString>("nvmlErrorString");

        if (!init_ || !shutdown_ || !getHandle_ || !getClock_ || !getPstate_ ||
            !setMem_ || !resetMem_ || !errorString_)
        {
            error = "nvml.dll is missing a required function.";
            Close();
            return false;
        }

        nvmlReturn_t r = init_();
        if (r != NVML_SUCCESS)
        {
            error = "nvmlInit_v2 failed: " + Error(r);
            Close();
            return false;
        }

        initialized_ = true;

        r = getHandle_(gpuIndex, &device_);
        if (r != NVML_SUCCESS || !device_)
        {
            error = "Could not open GPU index " + std::to_string(gpuIndex) + ": " + Error(r);
            Close();
            return false;
        }

        gpuIndex_ = gpuIndex;
        gpuName_.clear();

        if (getName_)
        {
            char name[128] = {};
            if (getName_(device_, name, static_cast<unsigned int>(sizeof(name))) == NVML_SUCCESS)
                gpuName_ = name;
        }

        return true;
    }

    void Close()
    {
        device_ = nullptr;

        if (initialized_ && shutdown_)
            shutdown_();

        initialized_ = false;

        if (dll_)
        {
            FreeLibrary(dll_);
            dll_ = nullptr;
        }

        init_ = nullptr;
        shutdown_ = nullptr;
        getHandle_ = nullptr;
        getClock_ = nullptr;
        getSupportedMemClocks_ = nullptr;
        getSupportedPstates_ = nullptr;
        getMinMaxClockOfPstate_ = nullptr;
        getClockOffsets_ = nullptr;
        getMemClkVfOffset_ = nullptr;
        getPstate_ = nullptr;
        getPower_ = nullptr;
        getTemp_ = nullptr;
        setMem_ = nullptr;
        resetMem_ = nullptr;
        getName_ = nullptr;
        errorString_ = nullptr;
        gpuName_.clear();
    }

    bool IsOpen() const { return device_ != nullptr; }
    const std::string& Name() const { return gpuName_; }

    bool ReadMemory(unsigned int& memMHz, std::string& error)
    {
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        nvmlReturn_t r = getClock_(device_, NVML_CLOCK_MEM, &memMHz);
        if (r != NVML_SUCCESS)
        {
            error = "Memory clock query failed: " + Error(r);
            return false;
        }

        return true;
    }

    bool ReadPState(int& pstate, std::string& error)
    {
        pstate = -1;
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        nvmlReturn_t r = getPstate_(device_, &pstate);
        if (r != NVML_SUCCESS)
        {
            error = "P-State query failed: " + Error(r);
            return false;
        }
        return true;
    }

    bool GetSupportedPStates(std::vector<int>& pstates, std::string& error)
    {
        pstates.clear();
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }
        if (!getSupportedPstates_)
        {
            error = "Supported P-State query is not available in this NVML driver.";
            return false;
        }

        int values[NVML_MAX_GPU_PERF_PSTATES] = {};
        for (auto& value : values)
            value = NVML_PSTATE_UNKNOWN;

        nvmlReturn_t r = getSupportedPstates_(device_, values, static_cast<unsigned int>(sizeof(values)));
        if (r != NVML_SUCCESS)
        {
            error = "Supported P-State query failed: " + Error(r);
            return false;
        }

        for (int value : values)
        {
            if (value == NVML_PSTATE_UNKNOWN)
                break;
            if (value >= 0 && value <= 15)
                pstates.push_back(value);
        }

        return !pstates.empty();
    }

    bool GetPStateMemoryRange(int pstate, unsigned int& minMHz, unsigned int& maxMHz, std::string& error)
    {
        minMHz = 0;
        maxMHz = 0;
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }
        if (!getMinMaxClockOfPstate_)
        {
            error = "P-State clock-range query is not available in this NVML driver.";
            return false;
        }

        nvmlReturn_t r = getMinMaxClockOfPstate_(device_, NVML_CLOCK_MEM, pstate, &minMHz, &maxMHz);
        if (r != NVML_SUCCESS)
        {
            error = "P-State memory range query failed: " + Error(r);
            return false;
        }
        return true;
    }

    bool GetMemoryOffset(int pstate, int& offsetMHz, bool& supported, std::string& error)
    {
        offsetMHz = 0;
        supported = false;
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        if (getClockOffsets_)
        {
            nvmlClockOffset_v1_t info = {};
            info.version = NVML_CLOCK_OFFSET_V1;
            info.type = NVML_CLOCK_MEM;
            info.pstate = pstate;

            nvmlReturn_t r = getClockOffsets_(device_, &info);
            if (r == NVML_SUCCESS)
            {
                offsetMHz = info.clockOffsetMHz;
                supported = true;
                return true;
            }
            if (r != NVML_ERROR_NOT_SUPPORTED)
                error = "Memory clock-offset query failed: " + Error(r);
        }

        // Older drivers expose a global memory VF offset API. It is a useful
        // fallback for display/mapping when the newer per-P-State API is absent.
        if (getMemClkVfOffset_)
        {
            int legacyOffset = 0;
            nvmlReturn_t r = getMemClkVfOffset_(device_, &legacyOffset);
            if (r == NVML_SUCCESS)
            {
                offsetMHz = legacyOffset;
                supported = true;
                return true;
            }
            if (r != NVML_ERROR_NOT_SUPPORTED && error.empty())
                error = "Memory clock-offset query failed: " + Error(r);
        }

        // Unsupported is not a watchdog failure. P-State monitoring still works.
        return true;
    }

    bool ReadTelemetry(Telemetry& t, std::string& error)
    {
        t = {};

        if (!ReadMemory(t.memMHz, error))
            return false;

        unsigned int value = 0;

        if (getClock_(device_, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS)
            t.gpuMHz = value;

        int pstate = -1;
        if (getPstate_(device_, &pstate) == NVML_SUCCESS)
        {
            t.pstate = pstate;
            std::string offsetError;
            GetMemoryOffset(pstate, t.memOffsetMHz, t.memOffsetAvailable, offsetError);
        }

        if (getPower_)
        {
            value = 0;
            if (getPower_(device_, &value) == NVML_SUCCESS)
                t.watts = static_cast<double>(value) / 1000.0;
        }

        if (getTemp_)
        {
            value = 0;
            if (getTemp_(device_, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS)
                t.tempC = static_cast<int>(value);
        }

        return true;
    }

    bool GetSupportedMemoryClocks(std::vector<unsigned int>& clocksMHz, std::string& error)
    {
        clocksMHz.clear();

        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        if (!getSupportedMemClocks_)
        {
            error = "Supported memory clock query is not available in this NVML driver.";
            return false;
        }

        // Query the complete supported list. NVML reports the required count
        // when called with a null output buffer. We intentionally scan every
        // supported memory clock, then sort and use the full list internally for P-State mapping.
        unsigned int count = 0;
        nvmlReturn_t r = getSupportedMemClocks_(device_, &count, nullptr);

        if (count == 0)
        {
            error = "Supported memory clock query returned no clocks: " + Error(r);
            return false;
        }

        std::vector<unsigned int> values(count);
        r = getSupportedMemClocks_(device_, &count, values.data());
        if (r != NVML_SUCCESS)
        {
            error = "Supported memory clock query failed: " + Error(r);
            return false;
        }

        values.resize(count);
        values.erase(std::remove(values.begin(), values.end(), 0u), values.end());
        std::sort(values.begin(), values.end(), std::greater<unsigned int>());
        values.erase(std::unique(values.begin(), values.end()), values.end());

        clocksMHz = std::move(values);
        return !clocksMHz.empty();
    }

    bool SetMemoryRange(unsigned int minMHz, unsigned int maxMHz, std::string& error)
    {
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        nvmlReturn_t r = setMem_(device_, minMHz, maxMHz);
        if (r != NVML_SUCCESS)
        {
            error = Error(r);
            return false;
        }

        return true;
    }

    bool ResetMemory(std::string& error)
    {
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        nvmlReturn_t r = resetMem_(device_);
        if (r != NVML_SUCCESS)
        {
            error = Error(r);
            return false;
        }

        return true;
    }

private:
    template<typename T>
    T Load(const char* name)
    {
        return reinterpret_cast<T>(GetProcAddress(dll_, name));
    }

    std::string Error(nvmlReturn_t r) const
    {
        if (errorString_)
        {
            if (const char* s = errorString_(r))
                return s;
        }
        return "NVML error " + std::to_string(r);
    }

    HMODULE dll_ = nullptr;
    bool initialized_ = false;
    nvmlDevice_t device_ = nullptr;
    unsigned int gpuIndex_ = 0;
    std::string gpuName_;

    PFN_nvmlInit_v2 init_ = nullptr;
    PFN_nvmlShutdown shutdown_ = nullptr;
    PFN_nvmlDeviceGetHandleByIndex_v2 getHandle_ = nullptr;
    PFN_nvmlDeviceGetClockInfo getClock_ = nullptr;
    PFN_nvmlDeviceGetSupportedMemoryClocks getSupportedMemClocks_ = nullptr;
    PFN_nvmlDeviceGetSupportedPerformanceStates getSupportedPstates_ = nullptr;
    PFN_nvmlDeviceGetMinMaxClockOfPState getMinMaxClockOfPstate_ = nullptr;
    PFN_nvmlDeviceGetClockOffsets getClockOffsets_ = nullptr;
    PFN_nvmlDeviceGetMemClkVfOffset getMemClkVfOffset_ = nullptr;
    PFN_nvmlDeviceGetPerformanceState getPstate_ = nullptr;
    PFN_nvmlDeviceGetPowerUsage getPower_ = nullptr;
    PFN_nvmlDeviceGetTemperature getTemp_ = nullptr;
    PFN_nvmlDeviceSetMemoryLockedClocks setMem_ = nullptr;
    PFN_nvmlDeviceResetMemoryLockedClocks resetMem_ = nullptr;
    PFN_nvmlDeviceGetName getName_ = nullptr;
    PFN_nvmlErrorString errorString_ = nullptr;
};

// =============================================================================
// Settings
// =============================================================================

struct Settings
{
    unsigned int gpuIndex = 0;
    int targetPState = 0;
    unsigned int legacyTargetMHz = 0;
    unsigned int pollMs = 500;
    unsigned int unlockAttempts = 3;

    bool autoRestore = true;
    bool showTrayTelemetry = true;
    bool startWithWindows = false;
    bool startMinimized = true;
};

static Settings g_settings;

static std::wstring LocalAppDataPath(const wchar_t* subdir)
{
    wchar_t buf[32768] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
    std::wstring dir = (n > 0 && n < 32768) ? std::wstring(buf) : L".";
    dir += L"\\";
    dir += subdir;
    return dir;
}

static std::wstring SettingsPath()
{
    std::wstring dir = LocalAppDataPath(L"NvidiaPowerStateWatchdog");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

static std::wstring LegacySettingsPath()
{
    return LocalAppDataPath(L"NvidiaMemoryWatchdog") + L"\\settings.ini";
}

static bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void LoadSettings()
{
    const std::wstring current = SettingsPath();
    const std::wstring legacy = LegacySettingsPath();
    const bool useLegacy = !FileExists(current) && FileExists(legacy);
    const std::wstring ini = useLegacy ? legacy : current;

    g_settings.gpuIndex =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"GpuIndex", 0, ini.c_str()));

    if (useLegacy)
    {
        g_settings.targetPState = -1;
        g_settings.legacyTargetMHz =
            static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"TargetMHz", 0, ini.c_str()));
    }
    else
    {
        g_settings.targetPState =
            GetPrivateProfileIntW(L"Watchdog", L"TargetPState", 0, ini.c_str());
        g_settings.targetPState = std::clamp(g_settings.targetPState, 0, 15);
    }

    g_settings.pollMs =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"PollMs", 500, ini.c_str()));

    g_settings.unlockAttempts =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"UnlockAttempts", 3, ini.c_str()));

    g_settings.autoRestore =
        GetPrivateProfileIntW(L"Watchdog", L"AutoRestore", 1, ini.c_str()) != 0;

    g_settings.showTrayTelemetry =
        GetPrivateProfileIntW(L"Watchdog", L"ShowTrayTelemetry", 1, ini.c_str()) != 0;

    g_settings.startWithWindows =
        GetPrivateProfileIntW(L"Watchdog", L"StartWithWindows", 0, ini.c_str()) != 0;

    g_settings.startMinimized =
        GetPrivateProfileIntW(L"Watchdog", L"StartMinimized", 1, ini.c_str()) != 0;

    g_settings.pollMs = std::clamp(g_settings.pollMs, 100u, 10000u);
    g_settings.unlockAttempts = std::clamp(g_settings.unlockAttempts, 1u, 20u);
}

static void SaveSettings()
{
    const std::wstring ini = SettingsPath();

    auto PutInt = [&](const wchar_t* key, int value)
    {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%d", value);
        WritePrivateProfileStringW(L"Watchdog", key, buf, ini.c_str());
    };

    auto PutUInt = [&](const wchar_t* key, unsigned int value)
    {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%u", value);
        WritePrivateProfileStringW(L"Watchdog", key, buf, ini.c_str());
    };

    auto PutBool = [&](const wchar_t* key, bool value)
    {
        WritePrivateProfileStringW(L"Watchdog", key, value ? L"1" : L"0", ini.c_str());
    };

    PutUInt(L"GpuIndex", g_settings.gpuIndex);
    PutInt(L"TargetPState", g_settings.targetPState);
    PutUInt(L"PollMs", g_settings.pollMs);
    PutUInt(L"UnlockAttempts", g_settings.unlockAttempts);

    PutBool(L"AutoRestore", g_settings.autoRestore);
    PutBool(L"ShowTrayTelemetry", g_settings.showTrayTelemetry);
    PutBool(L"StartWithWindows", g_settings.startWithWindows);
    PutBool(L"StartMinimized", g_settings.startMinimized);
}

// =============================================================================
// Globals
// =============================================================================

static HINSTANCE g_instance = nullptr;
static HWND g_main = nullptr;
static HWND g_hover = nullptr;

static HWND g_lblGpu = nullptr;
static HWND g_lblPstateCaption = nullptr;
static HWND g_lblPstate = nullptr;
static HWND g_lblMemCaption = nullptr;
static HWND g_lblMem = nullptr;
static HWND g_lblMemOcCaption = nullptr;
static HWND g_lblMemOc = nullptr;
static HWND g_lblGpuClockCaption = nullptr;
static HWND g_lblGpuClock = nullptr;
static HWND g_lblPowerCaption = nullptr;
static HWND g_lblPower = nullptr;
static HWND g_lblTempCaption = nullptr;
static HWND g_lblTemp = nullptr;
static HWND g_lblStatusCaption = nullptr;
static HWND g_lblStatus = nullptr;

static HWND g_editGpuIndex = nullptr;
static HWND g_comboTarget = nullptr;
static HWND g_editPoll = nullptr;
static HWND g_editUnlockAttempts = nullptr;

static HWND g_chkAutoRestore = nullptr;
static HWND g_chkTrayTelemetry = nullptr;
static HWND g_chkStartup = nullptr;
static HWND g_chkStartMinimized = nullptr;

static HWND g_log = nullptr;

static HFONT g_font = nullptr;
static HFONT g_hoverFont = nullptr;
static HFONT g_hoverHeaderFont = nullptr;

static Nvml g_nvml;
static bool g_nvmlReady = false;

static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;

// Stable tray identity. Using a GUID lets a freshly launched watchdog replace
// a stale notification icon left behind by Explorer or an abnormal process exit.
static const GUID g_trayIconGuid =
{ 0x73e35752, 0xef32, 0x4e21, { 0xa5, 0x22, 0xb8, 0xc7, 0xb0, 0x6f, 0x5d, 0x91 } };

static UINT g_taskbarCreatedMsg = 0;
static UINT g_showExistingMsg = 0;

struct HoverRow
{
    std::wstring label;
    std::wstring value;
};

static bool g_hoverActive = false;
static std::vector<HoverRow> g_hoverRows;
static std::wstring g_hoverMessage;
static RECT g_hoverIconRect = {};
static bool g_hoverIconRectValid = false;
// The hover rectangle is captured once when OUR notification icon first sends
// WM_MOUSEMOVE.  It remains fixed for the lifetime of that popup, matching the
// behavior of a normal Windows tray tooltip instead of following the pointer.
static ULONGLONG g_lastHoverTelemetryTick = 0;
static int g_hoverLastX = INT_MIN;
static int g_hoverLastY = INT_MIN;
static int g_hoverLastWidth = 0;
static int g_hoverLastHeight = 0;

static HANDLE g_singletonMutex = nullptr;

static std::vector<PStateChoice> g_pstateChoices;

// GUID_SESSION_DISPLAY_STATUS: {2B84C20E-AD23-4DDF-93DB-05FFBD7EFCA5}.
// Keep a private definition so the project does not depend on a GUID library
// definition at link time.
static const GUID GUID_NMW_SESSION_DISPLAY_STATUS =
{ 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };

// Recovery is deliberately state-driven. Windows power/display/session events
// never decide whether recovery is needed; only an actual P-State mismatch can
// escalate from a normal NVML reapply to an NPCF (NVIDIA Platform Controllers
// and Framework) restart. The RTX display adapter
// itself is never restarted or removed by this build.
static bool g_watchdogTargetMismatchReported = false;
static bool g_watchdogSetFailureReported = false;
static unsigned int g_watchdogTargetMismatchRetries = 0;
static unsigned int g_stateUnlockAttemptCount = 0;
static bool g_stateUnlockLimitReported = false;
static ULONGLONG g_nextStateUnlockAllowedTick = 0;
static bool g_stateUnlockWorkerRunning = false;
static bool g_stateUnlockVerifying = false;
static ULONGLONG g_stateUnlockVerifyDeadline = 0;
static int g_stateUnlockLastObservedPState = -1;

static constexpr unsigned int NORMAL_LOCK_RETRIES_BEFORE_UNLOCK = 3;
static constexpr ULONGLONG POST_STATE_UNLOCK_GRACE_MS = 5000;
static constexpr DWORD STATE_UNLOCK_VERIFY_TIMEOUT_MS = 3000;
static constexpr UINT STATE_UNLOCK_VERIFY_INTERVAL_MS = 100;
static constexpr DWORD PNPUTIL_PROCESS_TIMEOUT_MS = 15000;
static constexpr DWORD SCHTASKS_PROCESS_TIMEOUT_MS = 15000;

// Modern Standby (S0 low-power idle) does not behave like classic S3 sleep.
// User-mode code can briefly run during maintenance/unattended wake periods while
// the session display is still off. Treat display-off as a hard watchdog pause so
// legitimate low-power memory clocks (for example 405 MHz) are never interpreted
// as a recovery failure.
static bool g_powerPaused = false;
static bool g_sessionDisplayOn = true;
static bool g_powerResumeScheduled = false;
static HPOWERNOTIFY g_suspendResumeNotify = nullptr;
static HPOWERNOTIFY g_displayStatusNotify = nullptr;
static constexpr UINT POWER_RESUME_SETTLE_MS = 500;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATE_UNLOCK_RESULT = WM_APP + 2;

constexpr UINT_PTR TIMER_WATCHDOG = 1;
constexpr UINT_PTR TIMER_TRAY_RETRY = 2;
constexpr UINT_PTR TIMER_HOVER = 3;
constexpr UINT_PTR TIMER_POWER_RESUME = 4;
constexpr UINT_PTR TIMER_STATE_UNLOCK_VERIFY = 5;

enum ControlId
{
    ID_GPU_INDEX = 1001,
    ID_TARGET,
    ID_POLL,
    ID_UNLOCK_ATTEMPTS,
    ID_AUTO_RESTORE,
    ID_TRAY_TELEMETRY,
    ID_STARTUP,
    ID_START_MINIMIZED,
    ID_SET_PSTATE,
    ID_RESET_PSTATE,
    ID_SAVE,

    ID_TRAY_OPEN = 2001,
    ID_TRAY_SETRESET,
    ID_TRAY_EXIT
};

// =============================================================================
// Utility
// =============================================================================

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return L"";

    int count = MultiByteToWideChar(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);

    if (count <= 0)
        return std::wstring(s.begin(), s.end());

    std::wstring result(static_cast<size_t>(count), L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), result.data(), count);

    return result;
}

static std::wstring ExePath()
{
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return std::wstring(buf.data(), n);
}

static unsigned int EditUInt(HWND edit, unsigned int fallback)
{
    wchar_t buf[64] = {};
    GetWindowTextW(edit, buf, 64);

    wchar_t* end = nullptr;
    unsigned long value = wcstoul(buf, &end, 10);

    if (!buf[0] || end == buf)
        return fallback;

    return static_cast<unsigned int>(value);
}

static void SetEditUInt(HWND edit, unsigned int value)
{
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%u", value);
    SetWindowTextW(edit, buf);
}

static bool IsChecked(HWND h)
{
    return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void SetChecked(HWND h, bool value)
{
    SendMessageW(h, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void AppendLog(const std::wstring& text)
{
    if (!g_log)
        return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t prefix[64] = {};
    swprintf_s(
        prefix,
        L"[%04u-%02u-%02u %02u:%02u:%02u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring line = prefix + text + L"\r\n";

    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);

    if (GetWindowTextLengthW(g_log) > 30000)
    {
        SendMessageW(g_log, EM_SETSEL, 0, 10000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
}

// =============================================================================
// Windows startup registration
// =============================================================================

static std::wstring SystemExecutablePath(const wchar_t* exeName)
{
    wchar_t systemDir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDir, _countof(systemDir)))
        return {};

    std::wstring path = systemDir;
    path += L"\\";
    path += exeName;
    return path;
}

static bool RunHiddenProcess(
    const std::wstring& applicationPath,
    const std::wstring& arguments,
    DWORD timeoutMs,
    DWORD& exitCode,
    bool& timedOut)
{
    exitCode = ERROR_GEN_FAILURE;
    timedOut = false;

    if (applicationPath.empty())
        return false;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi = {};

    // lpApplicationName is an absolute System32 path for all elevated helper
    // processes. This prevents executable search-order hijacking. The command
    // line still includes argv[0], as CreateProcess expects.
    std::wstring commandLine = L"\"" + applicationPath + L"\"";
    if (!arguments.empty())
    {
        commandLine += L" ";
        commandLine += arguments;
    }

    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');

    if (!CreateProcessW(
            applicationPath.c_str(),
            cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi))
    {
        return false;
    }

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);

    if (wait == WAIT_TIMEOUT)
    {
        timedOut = true;
        exitCode = WAIT_TIMEOUT;
        TerminateProcess(pi.hProcess, WAIT_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    else if (wait == WAIT_OBJECT_0)
    {
        if (!GetExitCodeProcess(pi.hProcess, &exitCode))
            exitCode = GetLastError();
    }
    else
    {
        exitCode = GetLastError();
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return wait == WAIT_OBJECT_0;
}

static std::wstring UpperCopy(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return value;
}

static std::wstring ReadDeviceStringProperty(
    HDEVINFO info,
    SP_DEVINFO_DATA& dev,
    DWORD property)
{
    DWORD type = 0;
    DWORD bytes = 0;

    SetupDiGetDeviceRegistryPropertyW(
        info,
        &dev,
        property,
        &type,
        nullptr,
        0,
        &bytes);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        return {};

    std::vector<BYTE> buffer(bytes + sizeof(wchar_t) * 2, 0);

    if (!SetupDiGetDeviceRegistryPropertyW(
            info,
            &dev,
            property,
            &type,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr))
    {
        return {};
    }

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};

    return reinterpret_cast<const wchar_t*>(buffer.data());
}

static std::vector<std::wstring> ReadDeviceMultiStringProperty(
    HDEVINFO info,
    SP_DEVINFO_DATA& dev,
    DWORD property)
{
    std::vector<std::wstring> values;
    DWORD type = 0;
    DWORD bytes = 0;

    SetupDiGetDeviceRegistryPropertyW(
        info,
        &dev,
        property,
        &type,
        nullptr,
        0,
        &bytes);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        return values;

    std::vector<BYTE> buffer(bytes + sizeof(wchar_t) * 2, 0);

    if (!SetupDiGetDeviceRegistryPropertyW(
            info,
            &dev,
            property,
            &type,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr))
    {
        return values;
    }

    if (type != REG_MULTI_SZ && type != REG_SZ)
        return values;

    const wchar_t* p = reinterpret_cast<const wchar_t*>(buffer.data());
    while (*p)
    {
        std::wstring value = p;
        values.push_back(value);
        p += value.size() + 1;

        if (type == REG_SZ)
            break;
    }

    return values;
}

struct NpcfCandidate
{
    std::wstring instanceId;
    std::wstring name;
    int score = 0;
};

static bool FindNpcfInstance(std::wstring& instanceId, std::wstring& deviceName)
{
    instanceId.clear();
    deviceName.clear();

    HDEVINFO info = SetupDiGetClassDevsW(
        nullptr,
        nullptr,
        nullptr,
        DIGCF_ALLCLASSES | DIGCF_PRESENT);

    if (info == INVALID_HANDLE_VALUE)
        return false;

    std::vector<NpcfCandidate> candidates;

    for (DWORD i = 0;; ++i)
    {
        SP_DEVINFO_DATA dev = {};
        dev.cbSize = sizeof(dev);

        if (!SetupDiEnumDeviceInfo(info, i, &dev))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            continue;
        }

        wchar_t idBuffer[4096] = {};
        if (!SetupDiGetDeviceInstanceIdW(
                info,
                &dev,
                idBuffer,
                _countof(idBuffer),
                nullptr))
        {
            continue;
        }

        std::wstring id = idBuffer;
        std::wstring name = ReadDeviceStringProperty(info, dev, SPDRP_FRIENDLYNAME);
        if (name.empty())
            name = ReadDeviceStringProperty(info, dev, SPDRP_DEVICEDESC);

        const std::wstring upperId = UpperCopy(id);
        const std::wstring upperName = UpperCopy(name);

        bool knownHardwareId = false;
        const std::vector<std::wstring> hardwareIds =
            ReadDeviceMultiStringProperty(info, dev, SPDRP_HARDWAREID);

        for (const std::wstring& hardwareId : hardwareIds)
        {
            const std::wstring upperHardwareId = UpperCopy(hardwareId);

            if (upperHardwareId.find(L"VEN_NVDA&DEV_0820") != std::wstring::npos ||
                upperHardwareId.find(L"ACPI\\NVDA0820") != std::wstring::npos)
            {
                knownHardwareId = true;
                break;
            }
        }

        const bool knownInstance =
            upperId.find(L"ACPI\\NVDA0820\\") == 0 ||
            upperId.find(L"VEN_NVDA&DEV_0820") != std::wstring::npos;

        const bool knownName =
            upperName.find(L"NVIDIA PLATFORM CONTROLLERS AND FRAMEWORK") !=
            std::wstring::npos;

        // Elevated automatic recovery must not act on a name-only match.
        // Require the canonical NPCF name plus either its known hardware ID
        // or its known ACPI instance form.
        if (!knownName || (!knownHardwareId && !knownInstance))
            continue;

        int score = 0;
        if (knownName)
            score += 100;
        if (knownHardwareId)
            score += 80;
        if (knownInstance)
            score += 60;

        candidates.push_back({ id, name, score });
    }

    SetupDiDestroyDeviceInfoList(info);

    if (candidates.empty())
        return false;

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const NpcfCandidate& a, const NpcfCandidate& b)
        {
            return a.score > b.score;
        });

    // Do not guess if Windows exposes two equally strong NPCF candidates.
    if (candidates.size() > 1 &&
        candidates[0].score == candidates[1].score &&
        UpperCopy(candidates[0].instanceId) != UpperCopy(candidates[1].instanceId))
    {
        return false;
    }

    instanceId = candidates[0].instanceId;
    deviceName = candidates[0].name.empty()
        ? L"NVIDIA Platform Controllers and Framework"
        : candidates[0].name;
    return true;
}

static bool IsDeviceStarted(const std::wstring& instanceId)
{
    std::vector<wchar_t> id(instanceId.begin(), instanceId.end());
    id.push_back(L'\0');

    DEVINST devInst = 0;
    CONFIGRET cr = CM_Locate_DevNodeW(&devInst, id.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS)
        return false;

    ULONG status = 0;
    ULONG problem = 0;
    cr = CM_Get_DevNode_Status(&status, &problem, devInst, 0);

    return cr == CR_SUCCESS && (status & DN_STARTED) != 0;
}

static bool RestartNpcfWithPnpUtil(
    std::wstring& instanceId,
    std::wstring& deviceName,
    DWORD& exitCode,
    bool& deviceStarted)
{
    instanceId.clear();
    deviceName.clear();
    exitCode = ERROR_GEN_FAILURE;
    deviceStarted = false;

    if (!FindNpcfInstance(instanceId, deviceName))
        return false;

    const std::wstring pnputil = SystemExecutablePath(L"pnputil.exe");
    if (pnputil.empty())
        return false;

    const std::wstring arguments =
        L"/restart-device \"" + instanceId + L"\"";

    bool timedOut = false;
    if (!RunHiddenProcess(
            pnputil,
            arguments,
            PNPUTIL_PROCESS_TIMEOUT_MS,
            exitCode,
            timedOut) || exitCode != 0)
    {
        if (timedOut)
            exitCode = WAIT_TIMEOUT;
        return false;
    }

    // pnputil exit code 0 is the authoritative restart result. Configuration
    // Manager status can lag briefly, so DN_STARTED is diagnostic only.
    const ULONGLONG deadline = GetTickCount64() + 1500;
    do
    {
        if (IsDeviceStarted(instanceId))
        {
            deviceStarted = true;
            break;
        }
        Sleep(25);
    }
    while (GetTickCount64() < deadline);

    return true;
}

static void RemoveLegacyScheduledTasksSilently()
{
    const std::wstring schtasks = SystemExecutablePath(L"schtasks.exe");
    if (schtasks.empty())
        return;

    // Remove only task names used by older builds. Never remove the current
    // NVIDIA PowerState Watchdog task here; it is controlled by the checkbox.
    const wchar_t* taskNames[] =
    {
        L"NVIDIA VRAM Watchdog",
        L"NVIDIA Memory Watchdog"
    };

    for (const wchar_t* taskName : taskNames)
    {
        DWORD queryCode = ERROR_GEN_FAILURE;
        bool timedOut = false;
        const std::wstring queryArgs = L"/Query /TN \"" + std::wstring(taskName) + L"\"";

        if (!RunHiddenProcess(
                schtasks,
                queryArgs,
                SCHTASKS_PROCESS_TIMEOUT_MS,
                queryCode,
                timedOut) ||
            queryCode != 0)
        {
            continue;
        }

        DWORD deleteCode = ERROR_GEN_FAILURE;
        timedOut = false;
        const std::wstring deleteArgs = L"/Delete /F /TN \"" + std::wstring(taskName) + L"\"";
        RunHiddenProcess(
            schtasks,
            deleteArgs,
            SCHTASKS_PROCESS_TIMEOUT_MS,
            deleteCode,
            timedOut);
    }
}

static bool RemoveRunStartupValuesSilently()
{
    HKEY key = nullptr;
    const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        runKey,
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        &key);

    if (openResult != ERROR_SUCCESS)
        return false;

    bool currentValueExisted = false;
    DWORD type = 0;
    DWORD bytes = 0;

    if (RegQueryValueExW(
            key,
            L"NVIDIA PowerState Watchdog",
            nullptr,
            &type,
            nullptr,
            &bytes) == ERROR_SUCCESS)
    {
        currentValueExisted = true;
    }

    // The elevated watchdog cannot be launched reliably from the normal Run
    // key. Remove Run-key registrations from this and older builds.
    RegDeleteValueW(key, L"NVIDIA PowerState Watchdog");
    RegDeleteValueW(key, L"NVIDIA VRAM Watchdog");
    RegDeleteValueW(key, L"NVIDIA Memory Watchdog");

    RegCloseKey(key);
    return currentValueExisted;
}

static bool UpdateStartupTask(bool enabled)
{
    const std::wstring taskName = L"NVIDIA PowerState Watchdog";
    const std::wstring schtasks = SystemExecutablePath(L"schtasks.exe");
    if (schtasks.empty())
        return false;

    DWORD exitCode = ERROR_GEN_FAILURE;
    bool timedOut = false;

    if (!enabled)
    {
        const std::wstring deleteArguments =
            L"/Delete /F /TN \"" + taskName + L"\"";

        if (!RunHiddenProcess(
                schtasks,
                deleteArguments,
                SCHTASKS_PROCESS_TIMEOUT_MS,
                exitCode,
                timedOut))
        {
            return false;
        }

        if (exitCode == 0)
            return true;

        // If deletion returned a non-zero code, verify that the task is
        // actually absent before reporting failure.
        DWORD queryExitCode = ERROR_GEN_FAILURE;
        bool queryTimedOut = false;
        const std::wstring queryArguments =
            L"/Query /TN \"" + taskName + L"\"";

        if (!RunHiddenProcess(
                schtasks,
                queryArguments,
                SCHTASKS_PROCESS_TIMEOUT_MS,
                queryExitCode,
                queryTimedOut))
        {
            return false;
        }

        return queryExitCode != 0;
    }

    const std::wstring exe = ExePath();

    // Run at logon with highest privileges so the watchdog starts elevated
    // without relying on the normal HKCU Run startup path.
    const std::wstring arguments =
        L"/Create /F /SC ONLOGON /RL HIGHEST "
        L"/TN \"" + taskName + L"\" "
        L"/TR \"\\\"" + exe + L"\\\" --startup\"";

    if (!RunHiddenProcess(
            schtasks,
            arguments,
            SCHTASKS_PROCESS_TIMEOUT_MS,
            exitCode,
            timedOut))
    {
        return false;
    }

    return exitCode == 0;
}

// =============================================================================
// NVML / telemetry
// =============================================================================

static bool ConnectNvml(bool log)
{
    if (g_powerPaused)
        return false;

    std::string error;

    g_nvmlReady = g_nvml.Open(g_settings.gpuIndex, error);

    if (g_nvmlReady)
    {
        std::wstring name = g_nvml.Name().empty()
            ? (L"GPU " + std::to_wstring(g_settings.gpuIndex))
            : Utf8ToWide(g_nvml.Name());

        if (g_lblGpu)
            SetWindowTextW(g_lblGpu, name.c_str());

        if (log)
            AppendLog(L"Connected to " + name + L".");

        return true;
    }

    if (g_lblGpu)
        SetWindowTextW(g_lblGpu, L"Unavailable");

    if (log)
        AppendLog(L"NVML connection failed: " + Utf8ToWide(error));

    return false;
}

static bool EnsureNvml()
{
    // Never wake/reopen NVML while the session is in Modern Standby/display-off
    // handling. This gate protects every background telemetry/recovery caller,
    // not only the main watchdog timer.
    if (g_powerPaused)
        return false;

    if (g_nvmlReady && g_nvml.IsOpen())
        return true;

    return ConnectNvml(false);
}

static const PStateChoice* FindPStateChoice(int pstate)
{
    for (const auto& choice : g_pstateChoices)
    {
        if (choice.pstate == pstate)
            return &choice;
    }
    return nullptr;
}

static void PopulateTargetComboFromCurrentList()
{
    if (!g_comboTarget)
        return;

    SendMessageW(g_comboTarget, CB_RESETCONTENT, 0, 0);

    if (g_pstateChoices.empty())
    {
        const std::wstring fallback = L"P" + std::to_wstring(g_settings.targetPState >= 0 ? g_settings.targetPState : 0);
        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fallback.c_str()));
        SendMessageW(g_comboTarget, CB_SETCURSEL, 0, 0);
        return;
    }

    size_t selected = 0;
    bool foundTarget = false;

    for (size_t i = 0; i < g_pstateChoices.size(); ++i)
    {
        const auto& choice = g_pstateChoices[i];
        std::wstring label = L"P" + std::to_wstring(choice.pstate);
        if (!choice.controllable)
            label += L" (read-only)";

        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));

        if (choice.pstate == g_settings.targetPState)
        {
            selected = i;
            foundTarget = true;
        }
    }

    // Migrate an old memory-clock target to the P-State whose lock range
    // contains it. This happens only on the first run after upgrading.
    if (!foundTarget && g_settings.legacyTargetMHz != 0)
    {
        for (size_t i = 0; i < g_pstateChoices.size(); ++i)
        {
            const auto& choice = g_pstateChoices[i];
            if (choice.controllable &&
                g_settings.legacyTargetMHz >= choice.lockMinMHz &&
                g_settings.legacyTargetMHz <= choice.lockMaxMHz)
            {
                selected = i;
                foundTarget = true;
                break;
            }
        }
    }

    if (!foundTarget)
    {
        for (size_t i = 0; i < g_pstateChoices.size(); ++i)
        {
            if (g_pstateChoices[i].controllable)
            {
                selected = i;
                break;
            }
        }
    }

    g_settings.targetPState = g_pstateChoices[selected].pstate;
    g_settings.legacyTargetMHz = 0;
    SendMessageW(g_comboTarget, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    SaveSettings();
}

static void FilterDeepLowPowerPStates(std::vector<PStateChoice>& choices)
{
    // Some GPUs expose deep idle/display-power P-States alongside normal
    // performance P-States. They may have valid NVML memory clock entries but
    // cannot be held while the GPU is actively driving displays or rendering.
    // Detect a clearly separated low-clock cluster instead of hard-coding P5,
    // P8, or any model-specific P-State numbers.
    std::vector<unsigned int> clocks;
    for (const auto& choice : choices)
    {
        if (choice.controllable && choice.lockMaxMHz > 0)
            clocks.push_back(choice.lockMaxMHz);
    }

    std::sort(clocks.begin(), clocks.end(), std::greater<unsigned int>());
    clocks.erase(std::unique(clocks.begin(), clocks.end()), clocks.end());

    if (clocks.size() < 2)
        return;

    size_t splitIndex = clocks.size();
    double largestRatio = 1.0;

    for (size_t i = 1; i < clocks.size(); ++i)
    {
        if (clocks[i] == 0)
            continue;

        const double ratio = static_cast<double>(clocks[i - 1]) /
                             static_cast<double>(clocks[i]);
        if (ratio > largestRatio)
        {
            largestRatio = ratio;
            splitIndex = i;
        }
    }

    // Only classify a lower cluster as deep-low-power when there is a very
    // large discontinuity and the lower cluster is at most one quarter of the
    // highest performance memory clock. On the RTX 5090 Laptop this cleanly
    // separates 14001/11001/9001 from 810/405 without knowing the GPU model.
    if (splitIndex >= clocks.size() ||
        largestRatio < 4.0 ||
        static_cast<unsigned long long>(clocks[splitIndex]) * 4ull > clocks.front())
    {
        return;
    }

    const unsigned int firstDeepLowPowerClock = clocks[splitIndex];

    choices.erase(
        std::remove_if(
            choices.begin(),
            choices.end(),
            [&](const PStateChoice& choice)
            {
                return choice.controllable &&
                       choice.lockMaxMHz > 0 &&
                       choice.lockMaxMHz <= firstDeepLowPowerClock;
            }),
        choices.end());
}

static bool RefreshPStateChoices(bool log)
{
    g_pstateChoices.clear();

    if (!EnsureNvml())
    {
        PopulateTargetComboFromCurrentList();
        return false;
    }

    std::vector<int> pstates;
    std::vector<unsigned int> supportedMemClocks;
    std::string error;

    if (!g_nvml.GetSupportedPStates(pstates, error) || pstates.empty())
    {
        // Some driver branches expose the per-P-State range API without the
        // supported-state list API. Probe P0..P15 read-only as a fallback.
        pstates.clear();
        for (int pstate = 0; pstate <= 15; ++pstate)
        {
            unsigned int minMHz = 0;
            unsigned int maxMHz = 0;
            std::string probeError;
            if (g_nvml.GetPStateMemoryRange(pstate, minMHz, maxMHz, probeError))
                pstates.push_back(pstate);
        }

        if (pstates.empty())
        {
            if (log)
                AppendLog(L"P-State discovery failed: " + Utf8ToWide(error));
            PopulateTargetComboFromCurrentList();
            return false;
        }
    }

    std::sort(pstates.begin(), pstates.end());
    pstates.erase(std::unique(pstates.begin(), pstates.end()), pstates.end());

    std::string clocksError;
    g_nvml.GetSupportedMemoryClocks(supportedMemClocks, clocksError);
    std::sort(supportedMemClocks.begin(), supportedMemClocks.end());

    for (int pstate : pstates)
    {
        PStateChoice choice;
        choice.pstate = pstate;

        std::string rangeError;
        if (!g_nvml.GetPStateMemoryRange(
                pstate,
                choice.reportedMemMinMHz,
                choice.reportedMemMaxMHz,
                rangeError))
        {
            g_pstateChoices.push_back(choice);
            continue;
        }

        std::string offsetError;
        g_nvml.GetMemoryOffset(
            pstate,
            choice.memOffsetMHz,
            choice.memOffsetAvailable,
            offsetError);

        auto clocksInside = [&](long long rangeMin, long long rangeMax)
        {
            std::vector<unsigned int> inside;
            if (rangeMin < 0) rangeMin = 0;
            if (rangeMax < 0) rangeMax = 0;
            if (rangeMin > rangeMax) std::swap(rangeMin, rangeMax);

            for (unsigned int clock : supportedMemClocks)
            {
                if (clock >= static_cast<unsigned int>(rangeMin) &&
                    clock <= static_cast<unsigned int>(rangeMax))
                {
                    inside.push_back(clock);
                }
            }
            return inside;
        };

        // Different driver generations have reported P-State clock ranges with
        // and without the active VF offset folded into them. First match the
        // raw range. Only if that fails, normalize by the reported OC offset.
        long long baseMin = static_cast<long long>(choice.reportedMemMinMHz);
        long long baseMax = static_cast<long long>(choice.reportedMemMaxMHz);
        std::vector<unsigned int> inside = clocksInside(baseMin, baseMax);

        if (inside.empty() && choice.memOffsetAvailable && choice.memOffsetMHz != 0)
        {
            baseMin = static_cast<long long>(choice.reportedMemMinMHz) - choice.memOffsetMHz;
            baseMax = static_cast<long long>(choice.reportedMemMaxMHz) - choice.memOffsetMHz;
            inside = clocksInside(baseMin, baseMax);
        }

        if (!inside.empty())
        {
            choice.lockMinMHz = inside.front();
            choice.lockMaxMHz = inside.back();
            choice.controllable = true;
        }
        else if (baseMin > 0 && baseMax > 0)
        {
            // Some drivers expose a valid P-State range without repeating each
            // endpoint in GetSupportedMemoryClocks. Keep the range as a safe
            // fallback; NVML will reject it at apply-time if it is not legal.
            if (baseMin > baseMax) std::swap(baseMin, baseMax);
            choice.lockMinMHz = static_cast<unsigned int>(baseMin);
            choice.lockMaxMHz = static_cast<unsigned int>(baseMax);
            choice.controllable = true;
        }

        g_pstateChoices.push_back(choice);
    }

    FilterDeepLowPowerPStates(g_pstateChoices);

    // The target dropdown is for states the watchdog can actually request.
    // Read-only states remain observable through live telemetry but are not
    // useful target choices.
    g_pstateChoices.erase(
        std::remove_if(
            g_pstateChoices.begin(),
            g_pstateChoices.end(),
            [](const PStateChoice& choice) { return !choice.controllable; }),
        g_pstateChoices.end());

    PopulateTargetComboFromCurrentList();

    if (log)
    {
        std::wstring message = L"Supported P-States:";
        for (const auto& choice : g_pstateChoices)
            message += L" P" + std::to_wstring(choice.pstate);
        AppendLog(message + L".");

        int currentPstate = -1;
        std::string pstateError;
        if (g_nvml.ReadPState(currentPstate, pstateError))
        {
            int offset = 0;
            bool offsetSupported = false;
            std::string offsetError;
            if (g_nvml.GetMemoryOffset(currentPstate, offset, offsetSupported, offsetError) &&
                offsetSupported && offset != 0)
            {
                if (offset > 0)
                    AppendLog(L"Memory overclock detected: +" + std::to_wstring(offset) + L" MHz.");
                else
                    AppendLog(L"Memory underclock detected: " + std::to_wstring(offset) + L" MHz.");
            }
        }
    }

    return !g_pstateChoices.empty();
}

static int SelectedPStateFromUi()
{
    if (!g_comboTarget || g_pstateChoices.empty())
        return g_settings.targetPState;

    const LRESULT selection = SendMessageW(g_comboTarget, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR)
        return g_settings.targetPState;

    const size_t index = static_cast<size_t>(selection);
    if (index >= g_pstateChoices.size())
        return g_settings.targetPState;

    return g_pstateChoices[index].pstate;
}

static bool SetPStateNow(const wchar_t* source, bool logSuccess = true, bool logFailure = true)
{
    if (!EnsureNvml())
    {
        if (logFailure)
            AppendLog(L"Set P-State failed: NVML unavailable.");
        return false;
    }

    const PStateChoice* choice = FindPStateChoice(g_settings.targetPState);
    if (!choice || !choice->controllable)
    {
        if (logFailure)
            AppendLog(L"Unable to control P" + std::to_wstring(g_settings.targetPState) + L" on this GPU.");
        return false;
    }

    std::string error;
    if (!g_nvml.SetMemoryRange(choice->lockMinMHz, choice->lockMaxMHz, error))
    {
        if (logFailure)
            AppendLog(L"Set P-State failed: " + Utf8ToWide(error));
        return false;
    }

    if (logSuccess)
    {
        const std::wstring prefix = (source && wcscmp(source, L"Startup") == 0) ? L"Startup: " : L"";
        AppendLog(prefix + L"P-State set to P" + std::to_wstring(g_settings.targetPState) + L".");
    }

    return true;
}

static bool ResetPStateNow()
{
    if (!EnsureNvml())
    {
        AppendLog(L"Reset P-State failed: NVML unavailable.");
        return false;
    }

    std::string error;
    if (!g_nvml.ResetMemory(error))
    {
        AppendLog(L"Reset P-State failed: " + Utf8ToWide(error));
        return false;
    }

    AppendLog(L"P-State lock cleared.");
    return true;
}

static bool CurrentTargetReached()
{
    if (!EnsureNvml())
        return false;

    int pstate = -1;
    std::string error;
    if (!g_nvml.ReadPState(pstate, error))
        return false;

    return pstate == g_settings.targetPState;
}

static void ResetWatchdogFailureEpisode();
static void UpdateMainTelemetry();

struct StateUnlockWorkerResult
{
    std::wstring instanceId;
    std::wstring deviceName;
    DWORD exitCode = ERROR_GEN_FAILURE;
    bool restarted = false;
    bool deviceStarted = false;
};

static unsigned __stdcall StateUnlockWorkerThread(void* parameter)
{
    const HWND notifyWindow = reinterpret_cast<HWND>(parameter);
    auto* result = new StateUnlockWorkerResult();

    result->restarted = RestartNpcfWithPnpUtil(
        result->instanceId,
        result->deviceName,
        result->exitCode,
        result->deviceStarted);

    if (!PostMessageW(
            notifyWindow,
            WM_STATE_UNLOCK_RESULT,
            0,
            reinterpret_cast<LPARAM>(result)))
    {
        delete result;
    }

    return 0;
}

static void ReportStateUnlockLimitIfNeeded()
{
    if (g_stateUnlockLimitReported ||
        g_stateUnlockAttemptCount < g_settings.unlockAttempts)
    {
        return;
    }

    AppendLog(L"Recovery limit reached.");
    g_stateUnlockLimitReported = true;
}

static bool StartStateUnlock()
{
    if (g_powerPaused ||
        !g_settings.autoRestore ||
        g_stateUnlockWorkerRunning ||
        g_stateUnlockVerifying ||
        g_stateUnlockAttemptCount >= g_settings.unlockAttempts)
    {
        return false;
    }

    // Keep the proven sequence: pre-arm the target request before restarting
    // NPCF so the platform stack can re-evaluate the already-accepted request.
    if (!SetPStateNow(L"Recovery", false, true))
        return false;

    if (CurrentTargetReached())
        return false;

    const unsigned int attempt = g_stateUnlockAttemptCount + 1;
    AppendLog(
        L"Restarting NVIDIA Platform Controllers and Framework... (attempt " +
        std::to_wstring(attempt) + L"/" +
        std::to_wstring(g_settings.unlockAttempts) + L").");

    uintptr_t thread = _beginthreadex(
        nullptr,
        0,
        StateUnlockWorkerThread,
        reinterpret_cast<void*>(g_main),
        0,
        nullptr);

    if (thread == 0)
    {
        AppendLog(L"NPCF restart failed.");
        return false;
    }

    CloseHandle(reinterpret_cast<HANDLE>(thread));
    g_stateUnlockWorkerRunning = true;
    ++g_stateUnlockAttemptCount;
    g_nextStateUnlockAllowedTick = GetTickCount64() + POST_STATE_UNLOCK_GRACE_MS;
    return true;
}

static void HandleStateUnlockResult(StateUnlockWorkerResult* result)
{
    g_stateUnlockWorkerRunning = false;
    if (!result)
        return;

    g_nextStateUnlockAllowedTick = GetTickCount64() + POST_STATE_UNLOCK_GRACE_MS;

    if (!result->restarted)
    {
        if (result->instanceId.empty())
            AppendLog(L"NPCF not found; recovery unavailable.");
        else if (result->exitCode == WAIT_TIMEOUT)
            AppendLog(L"NPCF restart timed out.");
        else
            AppendLog(L"NPCF restart failed.");

        ReportStateUnlockLimitIfNeeded();
        return;
    }

    if (g_powerPaused || !g_settings.autoRestore)
        return;

    g_stateUnlockVerifying = true;
    g_stateUnlockVerifyDeadline = GetTickCount64() + STATE_UNLOCK_VERIFY_TIMEOUT_MS;
    g_stateUnlockLastObservedPState = -1;

    SetTimer(
        g_main,
        TIMER_STATE_UNLOCK_VERIFY,
        STATE_UNLOCK_VERIFY_INTERVAL_MS,
        nullptr);
}

static void StateUnlockVerifyTick()
{
    if (!g_stateUnlockVerifying)
    {
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
        return;
    }

    if (g_powerPaused || !g_settings.autoRestore)
    {
        g_stateUnlockVerifying = false;
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
        return;
    }

    if (EnsureNvml())
    {
        std::string error;
        int pstate = -1;

        if (g_nvml.ReadPState(pstate, error))
        {
            g_stateUnlockLastObservedPState = pstate;

            if (pstate == g_settings.targetPState)
            {
                g_stateUnlockVerifying = false;
                KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
                AppendLog(L"P-State restored to P" + std::to_wstring(g_settings.targetPState) + L".");
                ResetWatchdogFailureEpisode();
                UpdateMainTelemetry();
                return;
            }
        }
        else
        {
            g_nvmlReady = false;
        }
    }

    if (GetTickCount64() >= g_stateUnlockVerifyDeadline)
    {
        g_stateUnlockVerifying = false;
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);

        if (g_stateUnlockLastObservedPState >= 0)
        {
            AppendLog(
                L"Recovery failed; P-State remains P" +
                std::to_wstring(g_stateUnlockLastObservedPState) + L".");
        }
        else
        {
            AppendLog(L"Recovery failed.");
        }

        ReportStateUnlockLimitIfNeeded();
        UpdateMainTelemetry();
    }
}

static void LayoutTelemetryRows(bool showMemoryOffset)
{
    // Normal layout has no empty row between Memory and Power. When an OC/UV
    // offset exists, insert only that one conditional row underneath Memory.
    const int powerY = showMemoryOffset ? 122 : 102;
    const int statusY = showMemoryOffset ? 146 : 130;

    if (g_lblMemOcCaption)
        SetWindowPos(g_lblMemOcCaption, nullptr, 18, 98, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_lblMemOc)
        SetWindowPos(g_lblMemOc, nullptr, 120, 98, 120, 20, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_lblPowerCaption)
        SetWindowPos(g_lblPowerCaption, nullptr, 18, powerY, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_lblPower)
        SetWindowPos(g_lblPower, nullptr, 120, powerY, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_lblTempCaption)
        SetWindowPos(g_lblTempCaption, nullptr, 250, powerY, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_lblTemp)
        SetWindowPos(g_lblTemp, nullptr, 350, powerY, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_lblStatusCaption)
        SetWindowPos(g_lblStatusCaption, nullptr, 18, statusY, 90, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_lblStatus)
        SetWindowPos(g_lblStatus, nullptr, 120, statusY, 430, 20, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void UpdateMainTelemetry()
{
    if (!IsWindowVisible(g_main))
        return;

    if (g_powerPaused)
    {
        SetWindowTextW(g_lblStatus, L"Paused - display off / standby");
        return;
    }

    if (!EnsureNvml())
    {
        SetWindowTextW(g_lblStatus, L"NVML unavailable");
        return;
    }

    Telemetry t;
    std::string error;
    if (!g_nvml.ReadTelemetry(t, error))
    {
        SetWindowTextW(g_lblStatus, L"Telemetry query failed");
        return;
    }

    SetWindowTextW(g_lblMem, (std::to_wstring(t.memMHz) + L" MHz").c_str());
    SetWindowTextW(g_lblGpuClock, (std::to_wstring(t.gpuMHz) + L" MHz").c_str());

    if (g_lblMemOcCaption && g_lblMemOc)
    {
        const bool showOffset = t.memOffsetAvailable && t.memOffsetMHz != 0;
        LayoutTelemetryRows(showOffset);
        ShowWindow(g_lblMemOcCaption, showOffset ? SW_SHOWNA : SW_HIDE);
        ShowWindow(g_lblMemOc, showOffset ? SW_SHOWNA : SW_HIDE);
        if (showOffset)
        {
            const std::wstring value =
                (t.memOffsetMHz > 0 ? L"+" : L"") +
                std::to_wstring(t.memOffsetMHz) + L" MHz";
            SetWindowTextW(g_lblMemOc, value.c_str());
        }
    }

    if (t.pstate >= 0)
        SetWindowTextW(g_lblPstate, (L"P" + std::to_wstring(t.pstate)).c_str());
    else
        SetWindowTextW(g_lblPstate, L"--");

    if (t.watts >= 0.0)
    {
        wchar_t watts[64] = {};
        swprintf_s(watts, L"%.1f W", t.watts);
        SetWindowTextW(g_lblPower, watts);
    }
    else
    {
        SetWindowTextW(g_lblPower, L"--");
    }

    if (t.tempC >= 0)
        SetWindowTextW(g_lblTemp, (std::to_wstring(t.tempC) + L" C").c_str());
    else
        SetWindowTextW(g_lblTemp, L"--");

    if (g_stateUnlockWorkerRunning)
        SetWindowTextW(g_lblStatus, L"Recovering P-State");
    else if (g_stateUnlockVerifying)
        SetWindowTextW(g_lblStatus, L"Verifying P-State");
    else if (t.pstate == g_settings.targetPState)
        SetWindowTextW(g_lblStatus, L"Target reached");
    else if (g_settings.autoRestore)
        SetWindowTextW(g_lblStatus, L"Target mismatch - monitoring active");
    else
        SetWindowTextW(g_lblStatus, L"Target mismatch");
}

static void ResetWatchdogFailureEpisode()
{
    g_watchdogTargetMismatchReported = false;
    g_watchdogSetFailureReported = false;
    g_watchdogTargetMismatchRetries = 0;
    g_stateUnlockAttemptCount = 0;
    g_stateUnlockLimitReported = false;

    // Intentionally DO NOT clear g_nextStateUnlockAllowedTick here. The global
    // cooldown must survive a successful recovery so a brief subsequent drop
    // cannot immediately cause another NPCF restart.
}

static void ResetWatchdogStateForConfigurationChange()
{
    ResetWatchdogFailureEpisode();
    g_nextStateUnlockAllowedTick = 0;

    if (g_stateUnlockVerifying)
    {
        g_stateUnlockVerifying = false;
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
    }
}

static void WatchdogTick()
{
    if (g_powerPaused)
        return;

    if (g_stateUnlockWorkerRunning || g_stateUnlockVerifying)
    {
        UpdateMainTelemetry();
        return;
    }

    if (!EnsureNvml())
        return;

    int currentPstate = -1;
    std::string error;
    if (!g_nvml.ReadPState(currentPstate, error))
    {
        g_nvmlReady = false;
        return;
    }

    const bool targetReached = currentPstate == g_settings.targetPState;

    if (g_settings.autoRestore && !targetReached)
    {
        if (!g_watchdogTargetMismatchReported)
        {
            AppendLog(
                L"P-State does not match target (actual P" +
                std::to_wstring(currentPstate) + L", target P" +
                std::to_wstring(g_settings.targetPState) +
                L"); attempting recovery.");
            g_watchdogTargetMismatchReported = true;
        }

        if (SetPStateNow(L"Recovery", false, !g_watchdogSetFailureReported))
        {
            g_watchdogSetFailureReported = false;
            ++g_watchdogTargetMismatchRetries;
        }
        else
        {
            g_watchdogSetFailureReported = true;
        }

        if (g_watchdogTargetMismatchRetries >= NORMAL_LOCK_RETRIES_BEFORE_UNLOCK)
        {
            g_watchdogTargetMismatchRetries = 0;
            const ULONGLONG now = GetTickCount64();

            if (now >= g_nextStateUnlockAllowedTick)
            {
                if (g_stateUnlockAttemptCount < g_settings.unlockAttempts)
                    StartStateUnlock();
                else
                    ReportStateUnlockLimitIfNeeded();
            }
        }
    }
    else if (targetReached)
    {
        if (g_watchdogTargetMismatchReported)
            AppendLog(L"P-State restored to P" + std::to_wstring(g_settings.targetPState) + L".");

        ResetWatchdogFailureEpisode();
    }
    else
    {
        ResetWatchdogFailureEpisode();
    }

    UpdateMainTelemetry();
}

static void PauseWatchdogForPower(const wchar_t* reason)
{
    const bool wasPaused = g_powerPaused;
    g_powerPaused = true;
    g_powerResumeScheduled = false;

    if (g_main)
    {
        KillTimer(g_main, TIMER_WATCHDOG);
        KillTimer(g_main, TIMER_POWER_RESUME);
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
    }

    // Never retain a device handle across Modern Standby / system sleep.
    g_nvml.Close();
    g_nvmlReady = false;

    // Low clocks observed while the display is off must not count toward normal
    // retries or the NPCF escalation threshold after the user wakes the machine.
    ResetWatchdogStateForConfigurationChange();

    if (g_lblStatus)
        SetWindowTextW(g_lblStatus, L"Paused - display off / standby");

    if (!wasPaused && reason)
        AppendLog(std::wstring(reason) + L": monitoring paused; low-power P-States will be ignored.");
}

static void ScheduleWatchdogResume(const wchar_t* reason)
{
    if (g_powerResumeScheduled)
        return;

    // Stay logically paused during the short settle window so no other
    // UI/background path can reopen NVML early.
    g_powerPaused = true;
    g_powerResumeScheduled = true;

    if (g_main)
    {
        KillTimer(g_main, TIMER_WATCHDOG);
        KillTimer(g_main, TIMER_POWER_RESUME);
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);
        SetTimer(g_main, TIMER_POWER_RESUME, POWER_RESUME_SETTLE_MS, nullptr);
    }

    g_nvml.Close();
    g_nvmlReady = false;
    ResetWatchdogStateForConfigurationChange();

    if (g_lblStatus)
        SetWindowTextW(g_lblStatus, L"Display on - waiting for NVIDIA driver");

    if (reason)
        AppendLog(std::wstring(reason) + L": waiting briefly for NVIDIA driver.");
}

static void CompleteWatchdogResume()
{
    if (g_main)
        KillTimer(g_main, TIMER_POWER_RESUME);

    g_powerResumeScheduled = false;

    // An unattended Modern Standby wake can return to sleep before the settle
    // timer expires. Never resume if the session display is still known to be off.
    if (!g_sessionDisplayOn)
        return;

    g_powerPaused = false;
    ResetWatchdogFailureEpisode();

    // During the first few seconds after display-on, normal NVML lock attempts
    // are allowed but automatic NPCF escalation is suppressed. This lets the
    // NVIDIA driver finish wake initialization without risking a restart just
    // because readback is temporarily low or unavailable.
    g_nextStateUnlockAllowedTick = GetTickCount64() + POST_STATE_UNLOCK_GRACE_MS;

    // Reopen NVML only after the settle delay. If Auto Restore is enabled, make
    // one normal lock request; the regular watchdog owns all later verification.
    g_nvml.Close();
    g_nvmlReady = false;

    const bool connected = ConnectNvml(false);
    bool requested = false;

    if (connected && g_settings.autoRestore)
        requested = SetPStateNow(L"Wake", false);

    if (g_main)
        SetTimer(g_main, TIMER_WATCHDOG, g_settings.pollMs, nullptr);

    if (connected)
    {
        if (g_settings.autoRestore && requested)
            AppendLog(L"Monitoring resumed; selected P-State reapplied.");
        else
            AppendLog(L"Monitoring resumed.");
    }
    else
    {
        AppendLog(L"Monitoring resumed; NVIDIA driver is not ready yet.");
    }

    UpdateMainTelemetry();
}

// =============================================================================
// Tray icon
// =============================================================================

static bool TryAddTrayIcon()
{
    if (g_trayAdded)
        return true;

    // Remove any stale GUID-based icon from a previous watchdog process.
    // This also recovers cleanly if Explorer or a prior process exited before
    // it could remove its notification icon.
    NOTIFYICONDATAW stale = {};
    stale.cbSize = sizeof(stale);
    stale.uFlags = NIF_GUID;
    stale.guidItem = g_trayIconGuid;
    Shell_NotifyIconW(NIM_DELETE, &stale);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_GUID;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    g_nid.guidItem = g_trayIconGuid;

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        return false;

    g_trayAdded = true;
    KillTimer(g_main, TIMER_TRAY_RETRY);
    return true;
}

static void EnsureTrayIcon()
{
    if (!TryAddTrayIcon())
        SetTimer(g_main, TIMER_TRAY_RETRY, 1500, nullptr);
}

static void RemoveTrayIcon()
{
    KillTimer(g_main, TIMER_TRAY_RETRY);

    if (g_trayAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void ShowMainWindow()
{
    ShowWindow(g_main, SW_RESTORE);
    BringWindowToTop(g_main);
    SetForegroundWindow(g_main);
    UpdateMainTelemetry();
}

static void HideMainWindow()
{
    ShowWindow(g_main, SW_HIDE);
}

static void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open");

    const bool targetReached = CurrentTargetReached();

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_SETRESET,
        targetReached ? L"Reset P-State" : L"Set P-State");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);

    SetForegroundWindow(g_main);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x,
        pt.y,
        0,
        g_main,
        nullptr);

    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// =============================================================================
// Hover telemetry popup
// =============================================================================

static void EndHover();

static bool TrayRect(RECT& rc)
{
    if (!g_trayAdded)
        return false;

    NOTIFYICONIDENTIFIER id = {};
    id.cbSize = sizeof(id);
    id.hWnd = g_main;
    id.uID = 1;
    id.guidItem = g_trayIconGuid;

    return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rc));
}

static void UpdateHoverIconRectFromTrayCallback()
{
    POINT pt = {};
    if (!GetCursorPos(&pt))
        return;

    RECT exact = {};
    if (TrayRect(exact))
    {
        const LONG rawWidth = exact.right - exact.left;
        const LONG rawHeight = exact.bottom - exact.top;

        // If Explorer returned a rectangle in the same coordinate space as the
        // cursor, use it verbatim.  If DPI virtualization makes the rectangle
        // miss the cursor, preserve its reported size but translate it to the
        // callback point.  Either way the rectangle is captured once and never
        // follows subsequent mouse movement.
        if (rawWidth > 0 && rawHeight > 0 && PtInRect(&exact, pt))
        {
            g_hoverIconRect = exact;
            g_hoverIconRectValid = true;
            return;
        }

        const LONG width = std::clamp<LONG>(rawWidth > 0 ? rawWidth : 28, 16, 48);
        const LONG height = std::clamp<LONG>(rawHeight > 0 ? rawHeight : 28, 16, 48);
        g_hoverIconRect.left = pt.x - width / 2;
        g_hoverIconRect.right = g_hoverIconRect.left + width;
        g_hoverIconRect.top = pt.y - height / 2;
        g_hoverIconRect.bottom = g_hoverIconRect.top + height;
        g_hoverIconRectValid = true;
        return;
    }

    // Explorer can temporarily refuse Shell_NotifyIconGetRect (for example
    // while rebuilding the tray).  A small fixed rectangle around the callback
    // point is a safe fallback because WM_MOUSEMOVE came from OUR icon.
    constexpr LONG width = 28;
    constexpr LONG height = 28;
    g_hoverIconRect.left = pt.x - width / 2;
    g_hoverIconRect.right = g_hoverIconRect.left + width;
    g_hoverIconRect.top = pt.y - height / 2;
    g_hoverIconRect.bottom = g_hoverIconRect.top + height;
    g_hoverIconRectValid = true;
}

static bool MouseOverTrayIcon()
{
    if (!g_hoverIconRectValid)
        return false;

    POINT pt = {};
    if (!GetCursorPos(&pt))
        return false;

    return PtInRect(&g_hoverIconRect, pt) != FALSE;
}

static LRESULT CALLBACK HoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps = {};
        HDC targetDc = BeginPaint(hwnd, &ps);

        RECT client = {};
        GetClientRect(hwnd, &client);

        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);

        // Draw the whole card into an off-screen bitmap, then present it in one
        // BitBlt. This avoids visible GDI redraw/erase flicker while telemetry
        // updates in place.
        HDC dc = CreateCompatibleDC(targetDc);
        HBITMAP bitmap = CreateCompatibleBitmap(targetDc, width, height);
        HGDIOBJ oldBitmap = nullptr;
        if (dc && bitmap)
            oldBitmap = SelectObject(dc, bitmap);

        HDC drawDc = (dc && bitmap) ? dc : targetDc;

        const COLORREF bgColor = RGB(45, 45, 48);
        const COLORREF headerColor = RGB(39, 39, 42);
        const COLORREF borderColor = RGB(72, 72, 78);
        const COLORREF dividerColor = RGB(66, 66, 71);
        const COLORREF textColor = RGB(242, 242, 242);
        const COLORREF labelColor = RGB(205, 205, 210);

        HBRUSH background = CreateSolidBrush(bgColor);
        FillRect(drawDc, &client, background);
        DeleteObject(background);

        constexpr int headerHeight = 31;
        RECT headerRect = client;
        headerRect.bottom = std::min<LONG>(client.bottom, static_cast<LONG>(headerHeight));
        HBRUSH headerBrush = CreateSolidBrush(headerColor);
        FillRect(drawDc, &headerRect, headerBrush);
        DeleteObject(headerBrush);

        HPEN dividerPen = CreatePen(PS_SOLID, 1, dividerColor);
        HGDIOBJ oldPen = SelectObject(drawDc, dividerPen);
        MoveToEx(drawDc, 10, headerHeight, nullptr);
        LineTo(drawDc, std::max<LONG>(10, client.right - 10), headerHeight);
        SelectObject(drawDc, oldPen);
        DeleteObject(dividerPen);

        HPEN borderPen = CreatePen(PS_SOLID, 1, borderColor);
        oldPen = SelectObject(drawDc, borderPen);
        HGDIOBJ oldBrush = SelectObject(drawDc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(drawDc, 0, 0, client.right, client.bottom, 12, 12);
        SelectObject(drawDc, oldBrush);
        SelectObject(drawDc, oldPen);
        DeleteObject(borderPen);

        SetBkMode(drawDc, TRANSPARENT);
        SetTextColor(drawDc, textColor);

        HFONT oldFont = nullptr;
        HFONT headerFont = g_hoverHeaderFont ? g_hoverHeaderFont : (g_hoverFont ? g_hoverFont : g_font);
        if (headerFont)
            oldFont = static_cast<HFONT>(SelectObject(drawDc, headerFont));

        RECT titleRect = {12, 6, client.right - 12, headerHeight - 4};
        DrawTextW(drawDc, L"NVIDIA PowerState Watchdog", -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (oldFont)
            SelectObject(drawDc, oldFont);

        HFONT bodyFont = g_hoverFont ? g_hoverFont : g_font;
        oldFont = nullptr;
        if (bodyFont)
            oldFont = static_cast<HFONT>(SelectObject(drawDc, bodyFont));

        constexpr int left = 12;
        constexpr int valueLeft = 104;
        constexpr int rowHeight = 18;
        int y = headerHeight + 8;

        if (!g_hoverMessage.empty())
        {
            SetTextColor(drawDc, labelColor);
            RECT msgRect = {left, y, client.right - 12, client.bottom - 8};
            DrawTextW(drawDc, g_hoverMessage.c_str(), -1, &msgRect,
                      DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
        }
        else
        {
            for (const HoverRow& row : g_hoverRows)
            {
                SetTextColor(drawDc, labelColor);
                RECT labelRect = {left, y, valueLeft - 6, y + rowHeight};
                DrawTextW(drawDc, row.label.c_str(), -1, &labelRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                SetTextColor(drawDc, textColor);
                RECT valueRect = {valueLeft, y, client.right - 12, y + rowHeight};
                DrawTextW(drawDc, row.value.c_str(), -1, &valueRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                y += rowHeight;
            }
        }

        if (oldFont)
            SelectObject(drawDc, oldFont);

        if (dc && bitmap)
        {
            BitBlt(targetDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
            if (oldBitmap)
                SelectObject(dc, oldBitmap);
        }

        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureHoverWindow()
{
    if (g_hover)
        return;

    g_hover = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"NPSW_Hover",
        L"",
        WS_POPUP,
        0, 0, 10, 10,
        nullptr,
        nullptr,
        g_instance,
        nullptr);

    if (!g_hover)
        return;

    if (!g_hoverFont)
    {
        // Compact body typography. The popup should be readable without visually
        // dominating the notification area.
        g_hoverFont = CreateFontW(
            -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    if (!g_hoverHeaderFont)
    {
        g_hoverHeaderFont = CreateFontW(
            -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    // Windows 11-style dark rounded card when supported. The GDI rounded region
    // below remains the fallback on older Windows builds.
    const BOOL dark = TRUE;
    const DWORD darkModeAttribute = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(g_hover, darkModeAttribute, &dark, sizeof(dark));

    const DWORD cornerAttribute = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    const int roundedPreference = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(
        g_hover,
        cornerAttribute,
        &roundedPreference,
        sizeof(roundedPreference));
}

static void PositionHoverWindow()
{
    if (!g_hover || !g_hoverIconRectValid)
        return;

    // Size from the structured rows rather than a raw multiline DrawText block.
    constexpr int headerHeight = 31;
    constexpr int rowHeight = 18;
    constexpr int verticalPadding = 18;
    constexpr int minWidth = 228;

    int width = minWidth;
    int bodyHeight = 0;

    HDC dc = GetDC(g_hover);
    if (dc)
    {
        HFONT bodyFont = g_hoverFont ? g_hoverFont : g_font;
        HFONT oldFont = nullptr;
        if (bodyFont)
            oldFont = static_cast<HFONT>(SelectObject(dc, bodyFont));

        SIZE size = {};
        for (const HoverRow& row : g_hoverRows)
        {
            std::wstring line = row.label + L"    " + row.value;
            if (GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &size))
                width = std::max(width, static_cast<int>(size.cx) + 28);
        }

        if (!g_hoverMessage.empty())
        {
            RECT measured = {0, 0, 260, 0};
            DrawTextW(dc, g_hoverMessage.c_str(), -1, &measured,
                      DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);
            width = std::max(width, static_cast<int>(measured.right - measured.left) + 28);
            bodyHeight = std::max(rowHeight, static_cast<int>(measured.bottom - measured.top));
        }
        else
        {
            bodyHeight = std::max(rowHeight, static_cast<int>(g_hoverRows.size()) * rowHeight);
        }

        if (oldFont)
            SelectObject(dc, oldFont);
        ReleaseDC(g_hover, dc);
    }
    else
    {
        bodyHeight = std::max(rowHeight, static_cast<int>(g_hoverRows.size()) * rowHeight);
    }

    width = std::min(width, 320);
    const int height = headerHeight + verticalPadding + bodyHeight;

    // Behave like a native notification-area tooltip: anchor to the icon itself,
    // not the cursor, and place the card immediately outside the taskbar edge.
    // This leaves the icon completely unobstructed for left/right clicks.
    const RECT icon = g_hoverIconRect;
    POINT iconCenter = {
        (icon.left + icon.right) / 2,
        (icon.top + icon.bottom) / 2
    };

    HMONITOR monitor = MonitorFromPoint(iconCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi))
        return;

    const int monitorWidth = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monitorHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int relX = iconCenter.x - mi.rcMonitor.left;
    const int relY = iconCenter.y - mi.rcMonitor.top;

    const int distLeft = relX;
    const int distRight = monitorWidth - relX;
    const int distTop = relY;
    const int distBottom = monitorHeight - relY;

    enum class TaskbarEdge { Bottom, Top, Left, Right };
    TaskbarEdge edge = TaskbarEdge::Bottom;
    int best = distBottom;
    if (distTop < best) { best = distTop; edge = TaskbarEdge::Top; }
    if (distLeft < best) { best = distLeft; edge = TaskbarEdge::Left; }
    if (distRight < best) { edge = TaskbarEdge::Right; }

    constexpr int gap = 8;
    constexpr int margin = 8;

    int x = iconCenter.x - width / 2;
    int y = icon.top - height - gap;

    switch (edge)
    {
    case TaskbarEdge::Bottom:
        x = iconCenter.x - width / 2;
        y = icon.top - height - gap;
        break;
    case TaskbarEdge::Top:
        x = iconCenter.x - width / 2;
        y = icon.bottom + gap;
        break;
    case TaskbarEdge::Left:
        x = icon.right + gap;
        y = iconCenter.y - height / 2;
        break;
    case TaskbarEdge::Right:
        x = icon.left - width - gap;
        y = iconCenter.y - height / 2;
        break;
    }

    // Clamp only to the monitor edges.  Do not clamp the popup back toward the
    // taskbar work area; its position relative to the icon above is authoritative.
    const int minX = static_cast<int>(mi.rcMonitor.left) + margin;
    const int maxX = std::max(minX, static_cast<int>(mi.rcMonitor.right) - width - margin);
    const int minY = static_cast<int>(mi.rcMonitor.top) + margin;
    const int maxY = std::max(minY, static_cast<int>(mi.rcMonitor.bottom) - height - margin);

    x = std::max(minX, std::min(x, maxX));
    y = std::max(minY, std::min(y, maxY));

    const bool geometryChanged =
        x != g_hoverLastX || y != g_hoverLastY ||
        width != g_hoverLastWidth || height != g_hoverLastHeight;

    if (geometryChanged)
    {
        HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
        if (rounded && !SetWindowRgn(g_hover, rounded, FALSE))
            DeleteObject(rounded);

        g_hoverLastX = x;
        g_hoverLastY = y;
        g_hoverLastWidth = width;
        g_hoverLastHeight = height;

        SetWindowPos(
            g_hover,
            HWND_TOPMOST,
            x, y, width, height,
            SWP_NOACTIVATE | (IsWindowVisible(g_hover) ? 0 : SWP_SHOWWINDOW));
    }
    else if (!IsWindowVisible(g_hover))
    {
        SetWindowPos(
            g_hover,
            HWND_TOPMOST,
            x, y, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    InvalidateRect(g_hover, nullptr, FALSE);
}

static void RefreshHoverTelemetry()
{
    if (!g_hoverActive)
        return;

    g_hoverRows.clear();
    g_hoverMessage.clear();

    if (g_powerPaused)
    {
        g_hoverMessage = L"Paused - display off / standby";
    }
    else if (g_settings.showTrayTelemetry && EnsureNvml())
    {
        Telemetry t;
        std::string error;
        if (g_nvml.ReadTelemetry(t, error))
        {
            HoverRow pstate;
            pstate.label = L"P-State";
            pstate.value = (t.pstate >= 0) ? (L"P" + std::to_wstring(t.pstate)) : L"--";
            g_hoverRows.push_back(pstate);

            g_hoverRows.push_back({L"Memory", std::to_wstring(t.memMHz) + L" MHz"});

            if (t.memOffsetAvailable && t.memOffsetMHz != 0)
            {
                std::wstring oc;
                if (t.memOffsetMHz > 0)
                    oc += L"+";
                oc += std::to_wstring(t.memOffsetMHz) + L" MHz";
                g_hoverRows.push_back({L"Memory OC", oc});
            }

            g_hoverRows.push_back({L"GPU Clock", std::to_wstring(t.gpuMHz) + L" MHz"});

            std::wostringstream power;
            power.setf(std::ios::fixed);
            power.precision(1);
            if (t.watts >= 0.0)
                power << t.watts << L" W";
            else
                power << L"--";
            g_hoverRows.push_back({L"Power", power.str()});

            std::wstring temp = (t.tempC >= 0) ? (std::to_wstring(t.tempC) + L" C") : L"--";
            g_hoverRows.push_back({L"Temperature", temp});
        }
        else
        {
            g_hoverMessage = L"Telemetry unavailable";
        }
    }

    EnsureHoverWindow();
    PositionHoverWindow();
}

static void BeginHover()
{
    if (g_hoverActive)
        return;

    UpdateHoverIconRectFromTrayCallback();
    if (!g_hoverIconRectValid)
        return;

    g_hoverActive = true;
    g_lastHoverTelemetryTick = GetTickCount64();
    RefreshHoverTelemetry();
    // Keep hover-exit detection responsive, but telemetry itself refreshes at
    // 4 Hz.  The captured icon rectangle stays fixed until the popup closes.
    SetTimer(g_main, TIMER_HOVER, 100, nullptr);
}

static void EndHover()
{
    g_hoverActive = false;
    g_hoverIconRectValid = false;
    g_lastHoverTelemetryTick = 0;
    KillTimer(g_main, TIMER_HOVER);

    if (g_hover)
        ShowWindow(g_hover, SW_HIDE);
}

static void HoverTick()
{
    if (!g_hoverActive)
        return;

    if (!MouseOverTrayIcon())
    {
        EndHover();
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastHoverTelemetryTick >= 250)
    {
        g_lastHoverTelemetryTick = now;
        RefreshHoverTelemetry();
    }
}

// =============================================================================
// UI
// =============================================================================

static HWND MakeControl(
    const wchar_t* cls,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id = 0,
    DWORD exStyle = 0)
{
    HWND h = CreateWindowExW(
        exStyle,
        cls,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x, y, width, height,
        g_main,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance,
        nullptr);

    if (h && g_font)
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

    return h;
}

static void PushSettingsToUi()
{
    SetEditUInt(g_editGpuIndex, g_settings.gpuIndex);
    PopulateTargetComboFromCurrentList();
    SetEditUInt(g_editPoll, g_settings.pollMs);
    SetEditUInt(g_editUnlockAttempts, g_settings.unlockAttempts);

    SetChecked(g_chkAutoRestore, g_settings.autoRestore);
    SetChecked(g_chkTrayTelemetry, g_settings.showTrayTelemetry);
    SetChecked(g_chkStartup, g_settings.startWithWindows);
    SetChecked(g_chkStartMinimized, g_settings.startMinimized);
}

static void SyncAutoRestoreFromUi(bool persist)
{
    if (!g_chkAutoRestore)
        return;

    g_settings.autoRestore = IsChecked(g_chkAutoRestore);

    if (persist)
        SaveSettings();
}

static void PullSettingsFromUi()
{
    g_settings.gpuIndex = EditUInt(g_editGpuIndex, g_settings.gpuIndex);
    g_settings.targetPState = SelectedPStateFromUi();
    g_settings.pollMs = EditUInt(g_editPoll, g_settings.pollMs);
    g_settings.pollMs = std::clamp(g_settings.pollMs, 100u, 10000u);

    g_settings.unlockAttempts = EditUInt(g_editUnlockAttempts, g_settings.unlockAttempts);
    g_settings.unlockAttempts = std::clamp(g_settings.unlockAttempts, 1u, 20u);

    g_settings.autoRestore = IsChecked(g_chkAutoRestore);
    g_settings.showTrayTelemetry = IsChecked(g_chkTrayTelemetry);
    g_settings.startWithWindows = IsChecked(g_chkStartup);
    g_settings.startMinimized = IsChecked(g_chkStartMinimized);

    SetEditUInt(g_editPoll, g_settings.pollMs);
    SetEditUInt(g_editUnlockAttempts, g_settings.unlockAttempts);
}

static void BuildUi()
{
    g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    MakeControl(L"STATIC", L"GPU:", SS_LEFT, 18, 18, 90, 20);
    g_lblGpu = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 18, 430, 20);

    g_lblPstateCaption = MakeControl(L"STATIC", L"P-State:", SS_LEFT, 18, 46, 90, 20);
    g_lblPstate = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 46, 90, 20);

    g_lblMemCaption = MakeControl(L"STATIC", L"Memory:", SS_LEFT, 18, 74, 90, 20);
    g_lblMem = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 74, 120, 20);

    g_lblGpuClockCaption = MakeControl(L"STATIC", L"GPU clock:", SS_LEFT, 250, 74, 90, 20);
    g_lblGpuClock = MakeControl(L"STATIC", L"--", SS_LEFT, 350, 74, 120, 20);

    g_lblMemOcCaption = MakeControl(L"STATIC", L"Memory OC:", SS_LEFT, 18, 98, 90, 20);
    g_lblMemOc = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 98, 120, 20);
    ShowWindow(g_lblMemOcCaption, SW_HIDE);
    ShowWindow(g_lblMemOc, SW_HIDE);

    g_lblPowerCaption = MakeControl(L"STATIC", L"Power:", SS_LEFT, 18, 102, 90, 20);
    g_lblPower = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 102, 90, 20);

    g_lblTempCaption = MakeControl(L"STATIC", L"Temp:", SS_LEFT, 250, 102, 90, 20);
    g_lblTemp = MakeControl(L"STATIC", L"--", SS_LEFT, 350, 102, 90, 20);

    g_lblStatusCaption = MakeControl(L"STATIC", L"Status:", SS_LEFT, 18, 130, 90, 20);
    g_lblStatus = MakeControl(L"STATIC", L"Starting...", SS_LEFT, 120, 130, 430, 20);

    MakeControl(L"STATIC", L"GPU index", SS_LEFT, 18, 172, 100, 20);
    g_editGpuIndex = MakeControl(
        L"EDIT", L"0",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        125, 168, 90, 24,
        ID_GPU_INDEX,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Target P-State", SS_LEFT, 235, 172, 135, 20);
    g_comboTarget = MakeControl(
        L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL,
        375, 166, 165, 180,
        ID_TARGET,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Poll interval ms", SS_LEFT, 18, 206, 100, 20);
    g_editPoll = MakeControl(
        L"EDIT", L"500",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        125, 202, 90, 24,
        ID_POLL,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Recovery attempts", SS_LEFT, 235, 206, 105, 20);
    g_editUnlockAttempts = MakeControl(
        L"EDIT", L"3",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        350, 202, 90, 24,
        ID_UNLOCK_ATTEMPTS,
        WS_EX_CLIENTEDGE);

    g_chkAutoRestore = MakeControl(
        L"BUTTON",
        L"Automatically restore selected P-State if it changes",
        BS_AUTOCHECKBOX,
        18, 236, 420, 24,
        ID_AUTO_RESTORE);

    g_chkTrayTelemetry = MakeControl(
        L"BUTTON",
        L"Show telemetry when hovering over tray icon",
        BS_AUTOCHECKBOX,
        18, 272, 320, 24,
        ID_TRAY_TELEMETRY);

    g_chkStartup = MakeControl(
        L"BUTTON",
        L"Start with Windows",
        BS_AUTOCHECKBOX,
        18, 306, 160, 24,
        ID_STARTUP);

    g_chkStartMinimized = MakeControl(
        L"BUTTON",
        L"Start minimized to tray",
        BS_AUTOCHECKBOX,
        200, 306, 190, 24,
        ID_START_MINIMIZED);

    MakeControl(
        L"BUTTON", L"Set P-State",
        BS_PUSHBUTTON,
        18, 348, 165, 32,
        ID_SET_PSTATE);

    MakeControl(
        L"BUTTON", L"Reset P-State",
        BS_PUSHBUTTON,
        193, 348, 165, 32,
        ID_RESET_PSTATE);

    MakeControl(
        L"BUTTON", L"Save Settings",
        BS_PUSHBUTTON,
        368, 348, 180, 32,
        ID_SAVE);

    MakeControl(L"STATIC", L"Event log", SS_LEFT, 18, 396, 90, 20);

    g_log = MakeControl(
        L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        18, 420, 530, 190,
        0,
        WS_EX_CLIENTEDGE);

    PushSettingsToUi();
}

// =============================================================================
// Window procedure
// =============================================================================

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg)
    {
        // Explorer restarted / taskbar was recreated.
        g_trayAdded = false;
        EndHover();
        EnsureTrayIcon();
        return 0;
    }

    if (g_showExistingMsg != 0 && msg == g_showExistingMsg)
    {
        ShowMainWindow();
        return 0;
    }

    switch (msg)
    {
    case WM_TIMER:
        if (wParam == TIMER_WATCHDOG)
        {
            WatchdogTick();
            return 0;
        }

        if (wParam == TIMER_TRAY_RETRY)
        {
            TryAddTrayIcon();
            return 0;
        }

        if (wParam == TIMER_HOVER)
        {
            HoverTick();
            return 0;
        }

        if (wParam == TIMER_POWER_RESUME)
        {
            CompleteWatchdogResume();
            return 0;
        }

        if (wParam == TIMER_STATE_UNLOCK_VERIFY)
        {
            StateUnlockVerifyTick();
            return 0;
        }
        break;

    case WM_STATE_UNLOCK_RESULT:
    {
        auto* result = reinterpret_cast<StateUnlockWorkerResult*>(lParam);
        HandleStateUnlockResult(result);
        delete result;
        UpdateMainTelemetry();
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));

        // Labels/telemetry/checkbox text blend into the white parent instead of
        // drawing the old gray control rectangles. The read-only event log uses
        // an opaque white background so text scrolling remains artifact-free.
        SetBkMode(dc, control == g_log ? OPAQUE : TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE)
        {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);

            if (setting &&
                IsEqualGUID(setting->PowerSetting, GUID_NMW_SESSION_DISPLAY_STATUS) &&
                setting->DataLength >= sizeof(DWORD))
            {
                DWORD state = 0;
                CopyMemory(&state, setting->Data, sizeof(state));

                if (state == 0) // PowerMonitorOff
                {
                    g_sessionDisplayOn = false;
                    PauseWatchdogForPower(L"Session display off");
                }
                else if (state == 1) // PowerMonitorOn
                {
                    const bool needsResume = g_powerPaused || !g_sessionDisplayOn;
                    g_sessionDisplayOn = true;

                    if (needsResume)
                        ScheduleWatchdogResume(L"Session display on");
                }
                // PowerMonitorDim (2) remains an active state. Do not pause.
            }

            return TRUE;
        }

        if (wParam == PBT_APMSUSPEND)
        {
            PauseWatchdogForPower(L"System suspend");
            return TRUE;
        }

        if (wParam == PBT_APMRESUMEAUTOMATIC ||
            wParam == PBT_APMRESUMECRITICAL)
        {
            // This can be an unattended Modern Standby maintenance wake. Keep
            // recovery disabled until the session display actually turns on.
            g_nvml.Close();
            g_nvmlReady = false;
            return TRUE;
        }

        if (wParam == PBT_APMRESUMESUSPEND)
        {
            // User-interactive resume. Normally GUID_NMW_SESSION_DISPLAY_STATUS=On
            // will drive resume. This is only a fallback for classic sleep on
            // systems where the display-status event is missing.
            if (g_sessionDisplayOn && g_powerPaused)
                ScheduleWatchdogResume(L"User resume");
            return TRUE;
        }

        return TRUE;

    case WM_DISPLAYCHANGE:
        // Informational only. Do not set clocks or trigger recovery here.
        UpdateMainTelemetry();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_SET_PSTATE:
            PullSettingsFromUi();
            SaveSettings();
            ResetWatchdogStateForConfigurationChange();

            if (SetPStateNow(L"UI"))
            {
                Sleep(100);
                UpdateMainTelemetry();
            }
            return 0;

        case ID_RESET_PSTATE:
            // The Auto Restore checkbox is a live control. Sync it before the
            // reset so an unchecked box cannot be ignored by a stale in-memory
            // setting and immediately reapply the P-State on the next tick.
            SyncAutoRestoreFromUi(true);
            ResetWatchdogStateForConfigurationChange();

            if (ResetPStateNow())
            {
                Sleep(100);
                UpdateMainTelemetry();
            }
            return 0;

        case ID_TARGET:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                g_settings.targetPState = SelectedPStateFromUi();
                SaveSettings();

                ResetWatchdogStateForConfigurationChange();

                if (g_settings.autoRestore && !CurrentTargetReached())
                    SetPStateNow(L"Target selection", false);

                UpdateMainTelemetry();
            }
            return 0;

        case ID_AUTO_RESTORE:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                SyncAutoRestoreFromUi(true);

                ResetWatchdogStateForConfigurationChange();

                // When enabling it, let the option behave immediately instead
                // of waiting for the next watchdog tick.
                if (g_settings.autoRestore && !CurrentTargetReached())
                    SetPStateNow(L"Auto Restore", false);

                UpdateMainTelemetry();
            }
            return 0;

        case ID_STARTUP:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                const bool previous = g_settings.startWithWindows;
                const bool requested = IsChecked(g_chkStartup);

                if (UpdateStartupTask(requested))
                {
                    g_settings.startWithWindows = requested;
                    SaveSettings();

                    AppendLog(
                        requested
                            ? L"Start with Windows enabled."
                            : L"Start with Windows disabled.");
                }
                else
                {
                    // Keep the UI and saved setting truthful if task creation
                    // or deletion fails.
                    SetChecked(g_chkStartup, previous);

                    AppendLog(
                        requested
                            ? L"Could not create the Windows startup task."
                            : L"Could not remove the Windows startup task.");
                }
            }
            return 0;

        case ID_SAVE:
        {
            const unsigned int oldGpuIndex = g_settings.gpuIndex;

            PullSettingsFromUi();
            SaveSettings();
            ResetWatchdogStateForConfigurationChange();

            KillTimer(g_main, TIMER_WATCHDOG);
            if (!g_powerPaused)
                SetTimer(g_main, TIMER_WATCHDOG, g_settings.pollMs, nullptr);

            if (oldGpuIndex != g_settings.gpuIndex)
            {
                g_nvml.Close();
                g_nvmlReady = false;

                if (!g_powerPaused)
                {
                    ConnectNvml(true);
                    RefreshPStateChoices(true);
                }
            }

            UpdateMainTelemetry();
            return 0;
        }

        case ID_TRAY_OPEN:
            ShowMainWindow();
            return 0;

        case ID_TRAY_SETRESET:
            EndHover();
            SyncAutoRestoreFromUi(true);
            ResetWatchdogStateForConfigurationChange();

            if (CurrentTargetReached())
            {
                ResetPStateNow();
            }
            else
            {
                SetPStateNow(L"Tray");
            }

            Sleep(100);
            UpdateMainTelemetry();
            return 0;

        case ID_TRAY_EXIT:
            DestroyWindow(g_main);
            return 0;
        }
        break;

    case WM_TRAY:
        // A WM_MOUSEMOVE delivered for this icon is authoritative. The first
        // callback captures a fixed icon rectangle so the telemetry card behaves
        // like a normal tray tooltip and never follows the cursor.
        if (lParam == WM_MOUSEMOVE)
        {
            BeginHover();
            return 0;
        }

        if (lParam == WM_LBUTTONDBLCLK)
        {
            EndHover();
            ShowMainWindow();
            return 0;
        }

        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            EndHover();
            ShowTrayMenu();
            return 0;
        }
        break;

    case WM_CLOSE:
        // X never exits the watchdog. It only hides the window.
        HideMainWindow();
        return 0;

    case WM_DESTROY:
        KillTimer(g_main, TIMER_WATCHDOG);
        KillTimer(g_main, TIMER_TRAY_RETRY);
        KillTimer(g_main, TIMER_HOVER);
        KillTimer(g_main, TIMER_POWER_RESUME);
        KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);

        if (g_displayStatusNotify)
        {
            UnregisterPowerSettingNotification(g_displayStatusNotify);
            g_displayStatusNotify = nullptr;
        }

        if (g_suspendResumeNotify)
        {
            UnregisterSuspendResumeNotification(g_suspendResumeNotify);
            g_suspendResumeNotify = nullptr;
        }

        EndHover();
        RemoveTrayIcon();

        if (g_hover)
        {
            DestroyWindow(g_hover);
            g_hover = nullptr;
        }

        if (g_hoverFont)
        {
            DeleteObject(g_hoverFont);
            g_hoverFont = nullptr;
        }
        if (g_hoverHeaderFont)
        {
            DeleteObject(g_hoverHeaderFont);
            g_hoverHeaderFont = nullptr;
        }

        g_nvml.Close();

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================================================================
// Entry point
// =============================================================================

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR cmdLine, int nCmdShow)
{
    g_instance = hInstance;

    // Normal watchdog process. Recovery is performed in-process by restarting
    // NPCF only; there is no helper/relaunch path and no RTX GPU reset path.

    g_showExistingMsg = RegisterWindowMessageW(L"NvidiaPowerStateWatchdog.Fresh.Show");
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Single instance.
    g_singletonMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Local\\NvidiaPowerStateWatchdog.Fresh.Singleton");

    if (g_singletonMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        PostMessageW(HWND_BROADCAST, g_showExistingMsg, 0, 0);
        CloseHandle(g_singletonMutex);
        return 0;
    }

    LoadSettings();

    // Migrate the short-lived Run-key implementation to the elevated scheduled
    // task once. After migration, the Start with Windows checkbox exclusively
    // creates or deletes the current task.
    const bool hadRunStartup = RemoveRunStartupValuesSilently();
    RemoveLegacyScheduledTasksSilently();

    if (hadRunStartup && g_settings.startWithWindows)
        UpdateStartupTask(true);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW mainClass = {};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = hInstance;
    mainClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    mainClass.hIconSm = static_cast<HICON>(
        LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            16,
            16,
            LR_DEFAULTCOLOR));
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    mainClass.lpszClassName = L"NPSW_Main";

    if (!RegisterClassExW(&mainClass))
        return 1;

    WNDCLASSEXW hoverClass = {};
    hoverClass.cbSize = sizeof(hoverClass);
    hoverClass.lpfnWndProc = HoverProc;
    hoverClass.hInstance = hInstance;
    hoverClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hoverClass.hbrBackground = nullptr;
    hoverClass.lpszClassName = L"NPSW_Hover";

    if (!RegisterClassExW(&hoverClass))
        return 1;

    g_main = CreateWindowExW(
        0,
        L"NPSW_Main",
        L"NVIDIA PowerState Watchdog",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        590,
        670,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!g_main)
        return 1;

    BuildUi();

    // Explicit Modern Standby / display-state notifications. Modern Standby is
    // S0 low-power idle, so classic PBT_APMSUSPEND alone is not sufficient.
    g_suspendResumeNotify = RegisterSuspendResumeNotification(
        g_main,
        DEVICE_NOTIFY_WINDOW_HANDLE);

    g_displayStatusNotify = RegisterPowerSettingNotification(
        g_main,
        &GUID_NMW_SESSION_DISPLAY_STATUS,
        DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!g_suspendResumeNotify)
        AppendLog(L"Warning: could not register suspend/resume notifications.");

    if (!g_displayStatusNotify)
        AppendLog(L"Warning: could not register session display-status notifications; Modern Standby protection may be incomplete.");

    ConnectNvml(true);
    RefreshPStateChoices(true);

    {
        std::wstring npcfInstanceId;
        std::wstring npcfDeviceName;

        if (FindNpcfInstance(npcfInstanceId, npcfDeviceName))
        {
            AppendLog(
                L"NPCF: " + npcfDeviceName +
                L" (" + npcfInstanceId + L").");
        }
        else
        {
            AppendLog(
                L"NPCF not found; recovery unavailable.");
        }
    }

    if (g_settings.autoRestore)
    {
        // Request the selected target immediately. The regular watchdog verifies
        // actual telemetry and, if the request is accepted but not enforced,
        // escalates only to an NPCF restart. The RTX GPU is never reset.
        SetPStateNow(L"Startup");
    }

    EnsureTrayIcon();

    if (!g_powerPaused)
        SetTimer(g_main, TIMER_WATCHDOG, g_settings.pollMs, nullptr);

    const bool startupLaunch = wcsstr(cmdLine, L"--startup") != nullptr;

    if (startupLaunch && g_settings.startMinimized)
        ShowWindow(g_main, SW_HIDE);
    else
        ShowWindow(g_main, nCmdShow);

    UpdateWindow(g_main);
    UpdateMainTelemetry();

    MSG msg = {};

    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_singletonMutex)
    {
        ReleaseMutex(g_singletonMutex);
        CloseHandle(g_singletonMutex);
        g_singletonMutex = nullptr;
    }

    return static_cast<int>(msg.wParam);
}
