# NVIDIA Memory Watchdog

**NVIDIA Memory Watchdog** is a native Windows utility for monitoring and restoring a selected NVIDIA GPU memory clock. It uses NVIDIA's NVML interface for memory-clock control and can recover a stuck platform policy state by restarting only **NVIDIA Platform Controllers and Framework (NPCF)** when normal memory-clock requests are accepted but not enforced.

The recovery path is intentionally narrower than restarting the NVIDIA display adapter, so active DirectX/3D applications can remain running while the platform-controller state is refreshed.

> **Unofficial project:** NVIDIA Memory Watchdog is not affiliated with, endorsed by, or supported by NVIDIA Corporation. NVIDIA, GeForce, and related marks are trademarks of their respective owners.

## What it does

- Connects directly to NVIDIA NVML (`nvml.dll`) shipped with the installed NVIDIA driver.
- Detects the GPU's supported memory clocks and lets you select a target.
- Monitors the actual memory clock at a configurable polling interval.
- Reapplies the requested locked memory clock when the GPU falls off target.
- If repeated normal lock requests succeed but the requested clock still does not take effect, performs **State Unlock** by restarting only the detected NPCF device.
- Verifies the requested memory clock after the NPCF restart.
- Pauses enforcement while the display is off or Windows is suspended so legitimate low-power clocks do not trigger recovery.
- Can start automatically with Windows through an elevated scheduled task.
- Runs in the notification area and can show live GPU telemetry on hover.

## Why NPCF State Unlock exists

On affected NVIDIA laptop platforms, a requested memory clock can remain stuck at a lower value even while NVML accepts the lock request. Restarting **NVIDIA Platform Controllers and Framework** causes the platform policy to be re-evaluated without restarting/removing the RTX display adapter.

The watchdog therefore uses a conservative recovery sequence:

1. Request the selected memory clock through NVML.
2. Verify the actual clock.
3. Retry normal NVML lock requests first.
4. If the target is still not enforced, restart only the verified NPCF device.
5. Re-check the actual memory clock asynchronously.
6. Continue normal retries if State Unlock reaches its configured attempt limit.

There is **no automatic RTX display-adapter restart/removal path** in this build.

## Example recovery

```text
Connected to NVIDIA GeForce RTX 5090 Laptop GPU.
Top supported memory clocks: 14001 MHz 11001 MHz 9001 MHz.
NPCF: NVIDIA Platform Controllers and Framework (ACPI\NVDA0820\NPCF).
State Unlock Mode: NPCF restart
Startup: requested 14001 MHz.
Watchdog: memory clock does not match target (actual 9001 MHz, target 14001 MHz); attempting normal lock recovery.
Watchdog recovery: memory-clocks could not be locked. Attempting unlock... (attempt 1/5).
State Unlock Successful: NVIDIA Platform Controllers and Framework (ACPI\NVDA0820\NPCF). Verifying memory clock...
State Unlock Successful: memory clock has been set to 14001 MHz.
```

## Compatibility

### Requirements

- Windows 10/11 x64
- NVIDIA GPU with a driver that exposes NVML memory-clock controls used by this application
- Administrator privileges
- Visual Studio 2022 / Build Tools with **Desktop development with C++** to build from source

### NPCF recovery

State Unlock requires a compatible **NVIDIA Platform Controllers and Framework** device. The current production build uses conservative device matching for the canonical NPCF device and known `NVDA0820` hardware/instance signatures. If a compatible NPCF device is not found, the watchdog will not guess or restart another device.

This means normal NVML monitoring/locking and NPCF recovery are separate capabilities: a system may support the former without exposing a compatible NPCF device for State Unlock.

## Controls

- **GPU index** — NVML device index.
- **Target memory clock** — select from the highest supported memory clocks reported by NVML.
- **Poll interval** — monitoring interval from 100 to 10000 ms.
- **Unlock attempts** — maximum automatic NPCF State Unlock attempts per continuous failure episode (1–20).
- **Automatically restore MemClock when off target** — enables normal enforcement and State Unlock when necessary.
- **Show telemetry when hovering over tray icon** — displays live GPU information in the tray hover panel.
- **Start with Windows** — creates/maintains an elevated logon scheduled task.
- **Start minimized to tray** — starts hidden when launched by the startup task.

## Modern Standby / display-off behavior

When the session display turns off or Windows suspends, the watchdog pauses monitoring, closes NVML, stops State Unlock verification, clears the active failure episode, and ignores low-power GPU clocks.

When the display comes back, the watchdog waits briefly for the NVIDIA driver, reopens NVML, reapplies the selected memory clock if automatic restore is enabled, and resumes monitoring after a short settle period.

## Safety behavior

The production build deliberately avoids aggressive GPU recovery:

- no `/remove-device` path;
- no NVIDIA Display-class adapter restart;
- no GPU re-enumeration;
- no forced termination of active 3D applications;
- NPCF matching is conservative and ambiguous matches are rejected;
- `pnputil.exe` and `schtasks.exe` use absolute System32 paths and hard process timeouts;
- `nvml.dll` is loaded from System32 only.

See [`docs/PRODUCTION_AUDIT.txt`](docs/PRODUCTION_AUDIT.txt) for the production-hardening audit bundled with this source snapshot.

## Build

Open `NvidiaMemoryWatchdog.sln` in Visual Studio 2022 and build **Release | x64**, or run:

```bat
BUILD_RELEASE.bat
```

The executable is produced at:

```text
x64\Release\NvidiaMemoryWatchdog.exe
```

The application requests Administrator privileges through the Visual Studio linker configuration because memory-clock control and NPCF device restart require elevation on the tested platform.

## Source layout

```text
nvmw/
├── main.cpp
├── resource.h
├── app.rc
├── NvidiaMemoryWatchdog.ico
├── NvidiaMemoryWatchdog.sln
├── NvidiaMemoryWatchdog.vcxproj
├── BUILD_RELEASE.bat
├── README.md
├── .gitignore
├── .gitattributes
└── docs/
    └── PRODUCTION_AUDIT.txt
```

## Support the project

If NVIDIA Memory Watchdog is useful to you and you would like to support continued development, you can donate through PayPal:

**PayPal:** [paypal.me/xrplambo](https://www.paypal.com/paypalme/xrplambo)

## Disclaimer

This software changes NVIDIA GPU memory-clock state and may restart the NVIDIA Platform Controllers and Framework device when recovery is required. Use it at your own risk. Hardware, firmware, BIOS, driver, and OEM implementations vary; behavior observed on one system is not guaranteed on another.
