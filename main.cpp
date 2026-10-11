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

#include <algorithm>
#include <fstream>
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

// =============================================================================
// Minimal NVML declarations.
// We load nvml.dll dynamically, so no CUDA/NVML SDK is required.
// =============================================================================

using nvmlReturn_t = int;
struct nvmlDevice_st;
using nvmlDevice_t = nvmlDevice_st*;

constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0;

using PFN_nvmlInit_v2                       = nvmlReturn_t (*)();
using PFN_nvmlShutdown                      = nvmlReturn_t (*)();
using PFN_nvmlDeviceGetHandleByIndex_v2     = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using PFN_nvmlDeviceGetClockInfo            = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_nvmlDeviceGetSupportedMemoryClocks = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);
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
    double watts = -1.0;
    int tempC = -1;
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
            t.pstate = pstate;

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
        // supported memory clock, then sort and expose only the top three in UI.
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

    bool SetMemory(unsigned int mhz, std::string& error)
    {
        if (!device_)
        {
            error = "NVML is not connected.";
            return false;
        }

        nvmlReturn_t r = setMem_(device_, mhz, mhz);
        if (r != NVML_SUCCESS)
        {
            error = "Set memory clock failed: " + Error(r);
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
            error = "Reset memory clock failed: " + Error(r);
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
    unsigned int targetMHz = 14001;
    unsigned int targetRank = 0; // 0 = highest, 1 = second, 2 = third
    unsigned int pollMs = 500;
    unsigned int unlockAttempts = 3;

    bool autoRestore = true;
    bool showTrayTelemetry = true;
    bool startWithWindows = false;
    bool startMinimized = true;
};

static Settings g_settings;

static std::wstring SettingsPath()
{
    wchar_t buf[32768] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);

    std::wstring dir = (n > 0 && n < 32768) ? std::wstring(buf) : L".";
    dir += L"\\NvidiaMemoryWatchdog";
    CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\settings.ini";
}

static void LoadSettings()
{
    const std::wstring ini = SettingsPath();

    g_settings.gpuIndex =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"GpuIndex", 0, ini.c_str()));

    g_settings.targetMHz =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"TargetMHz", 14001, ini.c_str()));

    g_settings.targetRank =
        static_cast<unsigned int>(GetPrivateProfileIntW(L"Watchdog", L"TargetRank", 0, ini.c_str()));
    g_settings.targetRank = std::min(g_settings.targetRank, 2u);

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

    auto PutInt = [&](const wchar_t* key, unsigned int value)
    {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%u", value);
        WritePrivateProfileStringW(L"Watchdog", key, buf, ini.c_str());
    };

    auto PutBool = [&](const wchar_t* key, bool value)
    {
        WritePrivateProfileStringW(L"Watchdog", key, value ? L"1" : L"0", ini.c_str());
    };

    PutInt(L"GpuIndex", g_settings.gpuIndex);
    PutInt(L"TargetMHz", g_settings.targetMHz);
    PutInt(L"TargetRank", g_settings.targetRank);
    PutInt(L"PollMs", g_settings.pollMs);
    PutInt(L"UnlockAttempts", g_settings.unlockAttempts);

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
static HWND g_lblMem = nullptr;
static HWND g_lblGpuClock = nullptr;
static HWND g_lblPstate = nullptr;
static HWND g_lblPower = nullptr;
static HWND g_lblTemp = nullptr;
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

static bool g_hoverActive = false;
static POINT g_hoverAnchor = {};
static bool g_hoverAnchorValid = false;
static std::wstring g_hoverText = L"NVIDIA VRAM Watchdog";

static HANDLE g_singletonMutex = nullptr;

static std::vector<unsigned int> g_topMemoryClocks;

// GUID_SESSION_DISPLAY_STATUS: {2B84C20E-AD23-4DDF-93DB-05FFBD7EFCA5}.
// Keep a private definition so the project does not depend on a GUID library
// definition at link time.
static const GUID GUID_NMW_SESSION_DISPLAY_STATUS =
{ 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };

// Recovery is deliberately state-driven. Windows power/display/session events
// never decide whether recovery is needed; only actual memory-clock telemetry
// that does not match the selected target can escalate from a normal NVML re-lock to an NPCF
// (NVIDIA Platform Controllers and Framework) restart. The RTX display adapter
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
static unsigned int g_stateUnlockLastObservedMHz = 0;

static constexpr unsigned int NORMAL_LOCK_RETRIES_BEFORE_UNLOCK = 3;
static constexpr unsigned int TARGET_TOLERANCE_MHZ = 1;
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
    ID_SET_MEMCLOCK,
    ID_RESET_MEMCLOCK,
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

    wchar_t prefix[32] = {};
    swprintf_s(prefix, L"[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);

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
// Windows startup task
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

static bool UpdateStartupTask(bool enabled)
{
    const std::wstring taskName = L"NVIDIA VRAM Watchdog";
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

        // schtasks returns a generic non-zero code when the task is already
        // absent. Verify absence instead of treating every delete failure as
        // success and potentially leaving an unwanted startup task behind.
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

        return queryExitCode != 0; // non-zero query => task is absent
    }

    const std::wstring exe = ExePath();

    // Task launches this exact executable at user logon. --startup tells the
    // app to honor the StartMinimized setting.
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

static void PopulateTargetComboFromCurrentList()
{
    if (!g_comboTarget)
        return;

    SendMessageW(g_comboTarget, CB_RESETCONTENT, 0, 0);

    if (g_topMemoryClocks.empty())
    {
        const std::wstring fallback = std::to_wstring(g_settings.targetMHz) + L" MHz";
        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fallback.c_str()));
        SendMessageW(g_comboTarget, CB_SETCURSEL, 0, 0);
        return;
    }

    for (size_t i = 0; i < g_topMemoryClocks.size(); ++i)
    {
        std::wstring label = std::to_wstring(g_topMemoryClocks[i]) + L" MHz";
        if (i == 0)
            label += L" (Highest)";
        else if (i == 1)
            label += L" (2nd)";
        else if (i == 2)
            label += L" (3rd)";

        SendMessageW(g_comboTarget, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    // The app always scans the COMPLETE supported memory-clock list, sorts it,
    // and exposes the top three. Rank 0 is the automatic default. A user's
    // explicit #2/#3 choice persists as a rank across driver sessions and app restarts.
    const size_t selected = std::min<size_t>(
        g_settings.targetRank,
        g_topMemoryClocks.size() - 1);

    g_settings.targetRank = static_cast<unsigned int>(selected);
    g_settings.targetMHz = g_topMemoryClocks[selected];

    SendMessageW(g_comboTarget, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    SaveSettings();
}

static bool RefreshMemoryClockChoices(bool log)
{
    g_topMemoryClocks.clear();

    if (!EnsureNvml())
    {
        PopulateTargetComboFromCurrentList();
        return false;
    }

    std::vector<unsigned int> clocks;
    std::string error;

    if (!g_nvml.GetSupportedMemoryClocks(clocks, error) || clocks.empty())
    {
        if (log)
            AppendLog(L"Supported memory-clock scan failed: " + Utf8ToWide(error));

        PopulateTargetComboFromCurrentList();
        return false;
    }

    const size_t count = std::min<size_t>(3, clocks.size());
    g_topMemoryClocks.assign(clocks.begin(), clocks.begin() + count);

    PopulateTargetComboFromCurrentList();

    if (log)
    {
        std::wstring message = L"Top supported memory clocks:";
        for (unsigned int mhz : g_topMemoryClocks)
            message += L" " + std::to_wstring(mhz) + L" MHz";
        AppendLog(message + L".");
    }

    return true;
}

static unsigned int SelectedTargetFromUi()
{
    if (!g_comboTarget || g_topMemoryClocks.empty())
        return g_settings.targetMHz;

    const LRESULT selection = SendMessageW(g_comboTarget, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR)
        return g_settings.targetMHz;

    const size_t index = static_cast<size_t>(selection);
    if (index >= g_topMemoryClocks.size())
        return g_settings.targetMHz;

    g_settings.targetRank = static_cast<unsigned int>(index);
    return g_topMemoryClocks[index];
}

static bool SetMemClockNow(const wchar_t* source, bool logSuccess = true, bool logFailure = true)
{
    if (!EnsureNvml())
    {
        if (logFailure)
            AppendLog(std::wstring(source) + L": NVML unavailable.");
        return false;
    }

    std::string error;

    if (!g_nvml.SetMemory(g_settings.targetMHz, error))
    {
        if (logFailure)
            AppendLog(std::wstring(source) + L": " + Utf8ToWide(error));
        return false;
    }

    if (logSuccess)
    {
        AppendLog(
            std::wstring(source) +
            L": requested " +
            std::to_wstring(g_settings.targetMHz) +
            L" MHz.");
    }

    return true;
}

static bool ResetMemClockNow(const wchar_t* source)
{
    if (!EnsureNvml())
    {
        AppendLog(std::wstring(source) + L": NVML unavailable.");
        return false;
    }

    std::string error;

    if (!g_nvml.ResetMemory(error))
    {
        AppendLog(std::wstring(source) + L": " + Utf8ToWide(error));
        return false;
    }

    AppendLog(std::wstring(source) + L": memory clock lock reset.");
    return true;
}

static bool TargetMatches(unsigned int actualMHz, unsigned int targetMHz)
{
    const unsigned int diff =
        actualMHz > targetMHz
            ? actualMHz - targetMHz
            : targetMHz - actualMHz;
    return diff <= TARGET_TOLERANCE_MHZ;
}

static bool CurrentTargetReached()
{
    if (!EnsureNvml())
        return false;

    unsigned int memMHz = 0;
    std::string error;

    if (!g_nvml.ReadMemory(memMHz, error))
        return false;

    return TargetMatches(memMHz, g_settings.targetMHz);
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

    AppendLog(
        L"Watchdog recovery: state unlocked has reached maximum attempt limit. "
        L"Continuing normal memory-lock retries; the GPU will not be restarted.");
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

    // Proven sequence: make sure the requested target is accepted first, then
    // restart NPCF so the platform stack re-evaluates that existing request.
    if (!SetMemClockNow(L"State Unlock", false))
    {
        AppendLog(
            L"State Unlock Failed: memory-clock request could not be completed.");
        return false;
    }

    // If the final request became effective by itself, no controller restart is
    // necessary. The next normal watchdog sample will clear the failure episode.
    if (CurrentTargetReached())
        return false;

    const unsigned int attempt = g_stateUnlockAttemptCount + 1;

    AppendLog(
        L"Watchdog recovery: memory-clocks could not be locked. Attempting unlock... (attempt " +
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
        AppendLog(L"State Unlock failed: recovery worker could not be started.");
        return false;
    }

    CloseHandle(reinterpret_cast<HANDLE>(thread));
    g_stateUnlockWorkerRunning = true;
    ++g_stateUnlockAttemptCount;

    // Start the global cooldown immediately. Completion extends it again, so a
    // fast failure cannot cause a tight restart loop.
    g_nextStateUnlockAllowedTick =
        GetTickCount64() + POST_STATE_UNLOCK_GRACE_MS;

    return true;
}

static void HandleStateUnlockResult(StateUnlockWorkerResult* result)
{
    g_stateUnlockWorkerRunning = false;

    if (!result)
        return;

    // Every attempted restart, successful or not, gets a cooldown. This global
    // throttle intentionally survives a later successful recovery.
    g_nextStateUnlockAllowedTick =
        GetTickCount64() + POST_STATE_UNLOCK_GRACE_MS;

    if (!result->restarted)
    {
        if (result->instanceId.empty())
        {
            AppendLog(L"State Unlock failed: NPCF not found.");
        }
        else
        {
            AppendLog(
                L"State Unlock failed: " + result->deviceName +
                L" (" + result->instanceId + L"); pnputil exit code " +
                std::to_wstring(result->exitCode) + L".");
        }

        ReportStateUnlockLimitIfNeeded();
        return;
    }

    if (g_powerPaused || !g_settings.autoRestore)
    {
        // The restart completed, but power/configuration changed while the
        // worker was running. Do not begin stale verification.
        return;
    }

    AppendLog(
        L"State Unlock Successful: " + result->deviceName +
        L" (" + result->instanceId + L"). Verifying memory clock...");

    if (!result->deviceStarted)
    {
        AppendLog(
            L"State Unlock Diagnostic: pnputil reported success, but DN_STARTED had not yet appeared; "
            L"memory-clock telemetry will decide success.");
    }

    g_stateUnlockVerifying = true;
    g_stateUnlockVerifyDeadline =
        GetTickCount64() + STATE_UNLOCK_VERIFY_TIMEOUT_MS;
    g_stateUnlockLastObservedMHz = 0;

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
        unsigned int memMHz = 0;

        if (g_nvml.ReadMemory(memMHz, error))
        {
            g_stateUnlockLastObservedMHz = memMHz;

            if (TargetMatches(memMHz, g_settings.targetMHz))
            {
                g_stateUnlockVerifying = false;
                KillTimer(g_main, TIMER_STATE_UNLOCK_VERIFY);

                AppendLog(
                    L"State Unlock Successful: memory clock has been set to " +
                    std::to_wstring(memMHz) + L" MHz.");

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

        AppendLog(
            L"State Unlock completed, but the requested memory clock was not verified within " +
            std::to_wstring(STATE_UNLOCK_VERIFY_TIMEOUT_MS) + L" ms" +
            (g_stateUnlockLastObservedMHz > 0
                ? L" (last observed " +
                    std::to_wstring(g_stateUnlockLastObservedMHz) + L" MHz)."
                : L"."));

        ReportStateUnlockLimitIfNeeded();
        UpdateMainTelemetry();
    }
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
        SetWindowTextW(g_lblStatus, L"State Unlock in progress");
    else if (g_stateUnlockVerifying)
        SetWindowTextW(g_lblStatus, L"Verifying memory clock");
    else if (TargetMatches(t.memMHz, g_settings.targetMHz))
        SetWindowTextW(g_lblStatus, L"Target reached");
    else if (g_settings.autoRestore)
        SetWindowTextW(g_lblStatus, L"Target mismatch - watchdog active");
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

    // NPCF restart itself runs on a worker thread and verification has its own
    // 100 ms timer. Do not issue competing normal lock requests during either
    // phase; keep the UI responsive and telemetry independent.
    if (g_stateUnlockWorkerRunning || g_stateUnlockVerifying)
    {
        UpdateMainTelemetry();
        return;
    }

    if (!EnsureNvml())
        return;

    unsigned int memMHz = 0;
    std::string error;

    if (!g_nvml.ReadMemory(memMHz, error))
    {
        g_nvmlReady = false;
        return;
    }

    const bool targetReached = TargetMatches(memMHz, g_settings.targetMHz);

    if (g_settings.autoRestore && !targetReached)
    {
        if (!g_watchdogTargetMismatchReported)
        {
            AppendLog(
                L"Watchdog: memory clock does not match target (actual " +
                std::to_wstring(memMHz) + L" MHz, target " +
                std::to_wstring(g_settings.targetMHz) +
                L" MHz); attempting normal lock recovery.");
            g_watchdogTargetMismatchReported = true;
        }

        // Only count a normal-recovery request if NVML actually accepted it.
        // State Unlock is specifically for the accepted-but-not-enforced case.
        // Repeated identical NVML setter failures are logged once per episode.
        if (SetMemClockNow(L"Watchdog", false, !g_watchdogSetFailureReported))
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
                {
                    StartStateUnlock();
                }
                else
                {
                    ReportStateUnlockLimitIfNeeded();
                }
            }
        }
    }
    else if (targetReached)
    {
        if (g_watchdogTargetMismatchReported)
        {
            AppendLog(
                L"Watchdog: target restored at " +
                std::to_wstring(memMHz) + L" MHz.");
        }

        ResetWatchdogFailureEpisode();
    }
    else
    {
        // Auto Restore is disabled. Do not retain stale failure counters that
        // could trigger recovery when the option is enabled later.
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
        AppendLog(std::wstring(reason) + L": watchdog paused; low-power GPU clocks will be ignored.");
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
        AppendLog(std::wstring(reason) + L": waiting briefly for NVIDIA driver before resuming watchdog.");
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
        requested = SetMemClockNow(L"Wake", false);

    if (g_main)
        SetTimer(g_main, TIMER_WATCHDOG, g_settings.pollMs, nullptr);

    if (connected)
    {
        if (g_settings.autoRestore && requested)
            AppendLog(L"Watchdog resumed after display-on; selected memory clock reapplied.");
        else
            AppendLog(L"Watchdog resumed after display-on.");
    }
    else
    {
        AppendLog(L"Watchdog resumed after display-on; NVML is not ready yet and will be retried normally.");
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
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    g_nid.guidItem = g_trayIconGuid;
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"NVIDIA VRAM Watchdog");

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
        targetReached ? L"Reset MemClock" : L"Set MemClock");

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

static bool MouseStillNearTrayIcon()
{
    POINT pt = {};
    if (!GetCursorPos(&pt))
        return false;

    RECT rc = {};

    if (TrayRect(rc))
    {
        InflateRect(&rc, 10, 10);
        if (PtInRect(&rc, pt))
            return true;
    }

    if (g_hoverAnchorValid)
    {
        LONG dx = pt.x - g_hoverAnchor.x;
        LONG dy = pt.y - g_hoverAnchor.y;

        if (dx >= -40 && dx <= 40 && dy >= -40 && dy <= 40)
            return true;
    }

    return false;
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
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client = {};
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(GetSysColor(COLOR_INFOBK));
        FillRect(dc, &client, background);
        DeleteObject(background);

        HBRUSH border = CreateSolidBrush(GetSysColor(COLOR_WINDOWFRAME));
        FrameRect(dc, &client, border);
        DeleteObject(border);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_INFOTEXT));

        HFONT oldFont = nullptr;
        if (g_font)
            oldFont = static_cast<HFONT>(SelectObject(dc, g_font));

        RECT textRect = client;
        InflateRect(&textRect, -10, -8);

        DrawTextW(
            dc,
            g_hoverText.c_str(),
            -1,
            &textRect,
            DT_LEFT | DT_TOP | DT_NOPREFIX);

        if (oldFont)
            SelectObject(dc, oldFont);

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
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"NMW_Fresh_Hover",
        L"",
        WS_POPUP,
        0, 0, 10, 10,
        nullptr,
        nullptr,
        g_instance,
        nullptr);
}

static void PositionHoverWindow()
{
    if (!g_hover)
        return;

    RECT tray = {};
    POINT cursor = {};
    GetCursorPos(&cursor);

    if (!TrayRect(tray))
    {
        tray.left = cursor.x - 16;
        tray.right = cursor.x + 16;
        tray.top = cursor.y - 16;
        tray.bottom = cursor.y + 16;
    }

    HDC dc = GetDC(g_hover);

    HFONT oldFont = nullptr;
    if (g_font)
        oldFont = static_cast<HFONT>(SelectObject(dc, g_font));

    RECT measured = {0, 0, 520, 0};

    DrawTextW(
        dc,
        g_hoverText.c_str(),
        -1,
        &measured,
        DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);

    if (oldFont)
        SelectObject(dc, oldFont);

    ReleaseDC(g_hover, dc);

    const int measuredWidth = static_cast<int>(measured.right - measured.left);
    const int measuredHeight = static_cast<int>(measured.bottom - measured.top);

    int width = std::max(210, measuredWidth + 20);
    int height = std::max(40, measuredHeight + 16);

    HMONITOR monitor = MonitorFromRect(&tray, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);

    int x = ((tray.left + tray.right) / 2) - (width / 2);
    int y = tray.top - height - 8;

    if (y < mi.rcWork.top)
        y = tray.bottom + 8;

    const int workLeft = static_cast<int>(mi.rcWork.left);
    const int workTop = static_cast<int>(mi.rcWork.top);
    const int workRight = static_cast<int>(mi.rcWork.right);
    const int workBottom = static_cast<int>(mi.rcWork.bottom);

    x = std::max(workLeft, std::min(x, workRight - width));
    y = std::max(workTop, std::min(y, workBottom - height));

    SetWindowPos(
        g_hover,
        HWND_TOPMOST,
        x, y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    InvalidateRect(g_hover, nullptr, TRUE);
    UpdateWindow(g_hover);
}

static void RefreshHoverTelemetry()
{
    if (!g_hoverActive)
        return;

    if (g_powerPaused)
    {
        g_hoverText = L"NVIDIA VRAM Watchdog\nPaused - display off / standby";
        PositionHoverWindow();
        return;
    }

    std::wostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(1);

    ss << L"NVIDIA VRAM Watchdog";

    if (g_settings.showTrayTelemetry && EnsureNvml())
    {
        Telemetry t;
        std::string error;

        if (g_nvml.ReadTelemetry(t, error))
        {
            ss << L"\n";
            ss << L"Mem " << t.memMHz << L" MHz | GPU " << t.gpuMHz << L" MHz";
            ss << L"\n";

            if (t.pstate >= 0)
                ss << L"P" << t.pstate;
            else
                ss << L"P?";

            ss << L" | ";

            if (t.watts >= 0.0)
                ss << t.watts << L" W";
            else
                ss << L"-- W";

            ss << L" | ";

            if (t.tempC >= 0)
                ss << t.tempC << L" C";
            else
                ss << L"-- C";
        }
    }

    g_hoverText = ss.str();

    EnsureHoverWindow();
    PositionHoverWindow();
}

static void BeginHover()
{
    POINT pt = {};
    if (GetCursorPos(&pt))
    {
        g_hoverAnchor = pt;
        g_hoverAnchorValid = true;
    }

    if (!g_hoverActive)
    {
        g_hoverActive = true;
        RefreshHoverTelemetry();
        SetTimer(g_main, TIMER_HOVER, 250, nullptr);
    }
}

static void EndHover()
{
    g_hoverActive = false;
    g_hoverAnchorValid = false;
    KillTimer(g_main, TIMER_HOVER);

    if (g_hover)
        ShowWindow(g_hover, SW_HIDE);
}

static void HoverTick()
{
    if (!g_hoverActive)
        return;

    if (!MouseStillNearTrayIcon())
    {
        EndHover();
        return;
    }

    RefreshHoverTelemetry();
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
    g_settings.targetMHz = SelectedTargetFromUi();
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

    MakeControl(L"STATIC", L"Memory:", SS_LEFT, 18, 46, 90, 20);
    g_lblMem = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 46, 110, 20);

    MakeControl(L"STATIC", L"GPU clock:", SS_LEFT, 250, 46, 90, 20);
    g_lblGpuClock = MakeControl(L"STATIC", L"--", SS_LEFT, 350, 46, 120, 20);

    MakeControl(L"STATIC", L"P-state:", SS_LEFT, 18, 74, 90, 20);
    g_lblPstate = MakeControl(L"STATIC", L"--", SS_LEFT, 120, 74, 70, 20);

    MakeControl(L"STATIC", L"Power:", SS_LEFT, 210, 74, 60, 20);
    g_lblPower = MakeControl(L"STATIC", L"--", SS_LEFT, 275, 74, 85, 20);

    MakeControl(L"STATIC", L"Temp:", SS_LEFT, 380, 74, 55, 20);
    g_lblTemp = MakeControl(L"STATIC", L"--", SS_LEFT, 440, 74, 85, 20);

    MakeControl(L"STATIC", L"Status:", SS_LEFT, 18, 102, 90, 20);
    g_lblStatus = MakeControl(L"STATIC", L"Starting...", SS_LEFT, 120, 102, 430, 20);

    MakeControl(L"STATIC", L"GPU index", SS_LEFT, 18, 144, 100, 20);
    g_editGpuIndex = MakeControl(
        L"EDIT", L"0",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        125, 140, 90, 24,
        ID_GPU_INDEX,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Target memory clock", SS_LEFT, 235, 144, 135, 20);
    g_comboTarget = MakeControl(
        L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL,
        375, 138, 165, 120,
        ID_TARGET,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Poll interval ms", SS_LEFT, 18, 178, 100, 20);
    g_editPoll = MakeControl(
        L"EDIT", L"500",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        125, 174, 90, 24,
        ID_POLL,
        WS_EX_CLIENTEDGE);

    MakeControl(L"STATIC", L"Unlock attempts", SS_LEFT, 235, 178, 105, 20);
    g_editUnlockAttempts = MakeControl(
        L"EDIT", L"3",
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        350, 174, 90, 24,
        ID_UNLOCK_ATTEMPTS,
        WS_EX_CLIENTEDGE);

    g_chkAutoRestore = MakeControl(
        L"BUTTON",
        L"Automatically restore MemClock when off target",
        BS_AUTOCHECKBOX,
        18, 208, 400, 24,
        ID_AUTO_RESTORE);

    g_chkTrayTelemetry = MakeControl(
        L"BUTTON",
        L"Show telemetry when hovering over tray icon",
        BS_AUTOCHECKBOX,
        18, 244, 300, 24,
        ID_TRAY_TELEMETRY);

    g_chkStartup = MakeControl(
        L"BUTTON",
        L"Start with Windows",
        BS_AUTOCHECKBOX,
        18, 278, 160, 24,
        ID_STARTUP);

    g_chkStartMinimized = MakeControl(
        L"BUTTON",
        L"Start minimized to tray",
        BS_AUTOCHECKBOX,
        200, 278, 190, 24,
        ID_START_MINIMIZED);

    MakeControl(
        L"BUTTON", L"Set MemClock",
        BS_PUSHBUTTON,
        18, 320, 165, 32,
        ID_SET_MEMCLOCK);

    MakeControl(
        L"BUTTON", L"Reset MemClock",
        BS_PUSHBUTTON,
        193, 320, 165, 32,
        ID_RESET_MEMCLOCK);

    MakeControl(
        L"BUTTON", L"Save Settings",
        BS_PUSHBUTTON,
        368, 320, 180, 32,
        ID_SAVE);

    MakeControl(L"STATIC", L"Event log", SS_LEFT, 18, 368, 90, 20);

    g_log = MakeControl(
        L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        18, 392, 530, 190,
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
        case ID_SET_MEMCLOCK:
            PullSettingsFromUi();
            SaveSettings();
            ResetWatchdogStateForConfigurationChange();

            if (SetMemClockNow(L"UI"))
            {
                Sleep(100);
                UpdateMainTelemetry();
            }
            return 0;

        case ID_RESET_MEMCLOCK:
            // The Auto Restore checkbox is a live control. Sync it before the
            // reset so an unchecked box cannot be ignored by a stale in-memory
            // setting and immediately re-lock the memory clock on the next tick.
            SyncAutoRestoreFromUi(true);
            ResetWatchdogStateForConfigurationChange();

            if (ResetMemClockNow(L"UI"))
            {
                Sleep(100);
                UpdateMainTelemetry();
            }
            return 0;

        case ID_TARGET:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                g_settings.targetMHz = SelectedTargetFromUi();
                SaveSettings();

                AppendLog(
                    L"Target memory clock selected: " +
                    std::to_wstring(g_settings.targetMHz) +
                    L" MHz.");

                ResetWatchdogStateForConfigurationChange();

                if (g_settings.autoRestore && !CurrentTargetReached())
                    SetMemClockNow(L"Target selection");

                UpdateMainTelemetry();
            }
            return 0;

        case ID_AUTO_RESTORE:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                SyncAutoRestoreFromUi(true);

                AppendLog(
                    g_settings.autoRestore
                        ? L"Auto Restore enabled."
                        : L"Auto Restore disabled.");

                ResetWatchdogStateForConfigurationChange();

                // When enabling it, let the option behave immediately instead
                // of waiting for the next watchdog tick.
                if (g_settings.autoRestore && !CurrentTargetReached())
                    SetMemClockNow(L"Auto Restore");

                UpdateMainTelemetry();
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
                    RefreshMemoryClockChoices(true);
                }
            }

            if (!UpdateStartupTask(g_settings.startWithWindows))
                AppendLog(L"Could not update the Windows startup task.");
            else
                AppendLog(L"Settings saved.");

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
                ResetMemClockNow(L"Tray");
            }
            else
            {
                SetMemClockNow(L"Tray");
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
        // We intentionally use legacy notification-icon callback semantics:
        // lParam is the mouse/window message.
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

    g_showExistingMsg = RegisterWindowMessageW(L"NvidiaMemoryWatchdog.Fresh.Show");
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Single instance.
    g_singletonMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Local\\NvidiaMemoryWatchdog.Fresh.Singleton");

    if (g_singletonMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        PostMessageW(HWND_BROADCAST, g_showExistingMsg, 0, 0);
        CloseHandle(g_singletonMutex);
        return 0;
    }

    LoadSettings();

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
    mainClass.lpszClassName = L"NMW_Fresh_Main";

    if (!RegisterClassExW(&mainClass))
        return 1;

    WNDCLASSEXW hoverClass = {};
    hoverClass.cbSize = sizeof(hoverClass);
    hoverClass.lpfnWndProc = HoverProc;
    hoverClass.hInstance = hInstance;
    hoverClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hoverClass.hbrBackground = nullptr;
    hoverClass.lpszClassName = L"NMW_Fresh_Hover";

    if (!RegisterClassExW(&hoverClass))
        return 1;

    g_main = CreateWindowExW(
        0,
        L"NMW_Fresh_Main",
        L"NVIDIA VRAM Watchdog",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        590,
        640,
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
    RefreshMemoryClockChoices(true);

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
                L"NPCF not found; NVML lock retries remain available.");
        }
    }

    AppendLog(L"State Unlock Mode: NPCF restart");

    if (g_settings.autoRestore)
    {
        // Request the selected target immediately. The regular watchdog verifies
        // actual telemetry and, if the request is accepted but not enforced,
        // escalates only to an NPCF restart. The RTX GPU is never reset.
        SetMemClockNow(L"Startup");
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
