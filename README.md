# NVIDIA PowerState Watchdog

**NVIDIA PowerState Watchdog** is a native Windows utility that monitors an NVIDIA GPU's actual performance state (P-State) and automatically restores a selected state when the driver or platform moves away from it.

The selected **P-State is the target**. NVIDIA NVML memory-clock locking is used underneath as the control mechanism because NVML does not expose a public direct `SetPState(Px)` command.

> **Unofficial project:** NVIDIA PowerState Watchdog is not affiliated with, endorsed by, or supported by NVIDIA Corporation. NVIDIA, GeForce, and related marks are trademarks of their respective owners.

## What it does

- Monitors the GPU's actual NVIDIA P-State.
- Dynamically detects the P-States exposed by the selected GPU.
- Maps each usable P-State to its corresponding NVML memory-clock policy.
- Lets the user select a target P-State such as `P0`, `P2`, or `P4`.
- Automatically restores the selected P-State if the GPU moves away from it.
- Uses normal NVML recovery first.
- If the requested state cannot be restored normally, restarts only **NVIDIA Platform Controllers and Framework (NPCF)**.
- Never automatically restarts or removes the NVIDIA display adapter.
- Detects memory overclocks without treating them as a P-State failure.
- Pauses enforcement during Modern Standby, display-off, and system suspend.
- Can start automatically through Windows Startup Apps.
- Runs in the notification area with optional live telemetry on hover.
- Records recovery activity with full local date and time.

## P-State discovery

NVIDIA PowerState Watchdog does not assume a fixed set of P-States or memory clocks.

At runtime the application asks NVML which P-States are exposed by the selected GPU and determines the memory-clock policy associated with each usable state.

For example, one RTX 5090 Laptop GPU reports:

```text
P0 -> 14001 MHz
P2 -> 11001 MHz
P4 ->  9001 MHz
P5 ->   810 MHz
P8 ->   405 MHz
```

Another GPU may expose a completely different set of states and clocks.

Clearly separated deep low-power states used for idle/display operation are filtered from the normal target list rather than being presented as performance targets.

The filtering is based on the detected clock table rather than hardcoded GPU models or fixed P-State numbers.

## Overclock support

The watchdog monitors **P-State**, not whether the live memory clock exactly matches the stock clock.

For example:

```text
Target P-State:  P0
Current P-State: P0
Memory:          15801 MHz
Memory OC:       +1800 MHz
```

is considered healthy and does not trigger recovery.

If the GPU instead moves to another P-State while the target remains P0, recovery can be triggered even if the memory-clock difference itself is relatively small.

The application does not modify the user's memory overclock offset. Afterburner, NVIDIA software, or another tuning utility remains responsible for the overclock itself.

## Recovery

Recovery is intentionally conservative.

1. Detect that the actual P-State no longer matches the selected target.
2. Reapply the NVML memory policy mapped to the selected target P-State.
3. Retry normal NVML recovery first.
4. If the requested state is accepted but still cannot be restored, restart only **NVIDIA Platform Controllers and Framework (NPCF)**.
5. Verify that the selected P-State returns.

The target NVML request is applied before NPCF is restarted so the requested policy is already armed when the platform controller comes back online.

There is **no automatic NVIDIA display-adapter restart or removal path**.

## Example recovery

```text
[2026-10-12 23:46:30] Connected to NVIDIA GeForce RTX 5090 Laptop GPU.
[2026-10-12 23:46:30] Supported P-States: P0 P2 P4 P5 P8.
[2026-10-12 23:46:30] NPCF: NVIDIA Platform Controllers and Framework (ACPI\NVDA0820\NPCF).
[2026-10-12 23:46:30] Startup: P-State set to P0.
[2026-10-12 23:46:34] P-State does not match target (actual P4, target P0); attempting recovery.
[2026-10-12 23:46:36] Restarting NVIDIA Platform Controllers and Framework... (attempt 1/5).
[2026-10-12 23:46:39] P-State restored to P0.
```

The event log intentionally stays relatively sparse rather than logging every polling cycle or telemetry refresh.

## Controls

- **GPU index** — NVML device index.
- **Target P-State** — dynamically populated from the selected GPU.
- **Poll interval** — monitoring interval from 100 to 10000 ms.
- **Recovery attempts** — maximum automatic NPCF restart attempts during one continuous failure episode.
- **Automatically restore selected P-State if it changes** — enables automatic enforcement and recovery.
- **Show telemetry when hovering over tray icon** — displays live GPU telemetry.
- **Start with Windows** — registers the application with Windows Startup Apps.
- **Start minimized to tray** — starts hidden when launched with `--startup`.

## Modern Standby and display-off behavior

When the session display turns off or Windows suspends, monitoring pauses and NVML is closed so legitimate low-power GPU states do not trigger recovery.

When the session becomes active again, the application waits briefly for the NVIDIA driver, reconnects to NVML, reapplies the selected P-State policy when automatic restore is enabled, and resumes monitoring.

This also prevents the watchdog from attempting NPCF recovery during Modern Standby maintenance wakes.

## Startup Apps and elevation

**Start with Windows** uses the current user's normal Windows Startup Apps registration.

Older scheduled-task startup entries from previous NVIDIA Memory Watchdog versions are removed during migration.

The application requests Administrator privileges because NVML memory-clock policy changes and NPCF device restart require elevation on the tested system.

Normal Windows UAC policy therefore still applies when the application launches at startup.

## Safety

The recovery system deliberately avoids aggressive GPU reset methods:

- no `/remove-device` path;
- no NVIDIA display-adapter restart;
- no GPU re-enumeration;
- no forced termination of active 3D applications;
- NPCF device matching is conservative and ambiguous matches are rejected;
- `pnputil.exe` is launched from its absolute System32 path with a hard timeout;
- `nvml.dll` is loaded only from System32.

If a compatible NPCF device cannot be identified, the watchdog continues to use normal NVML recovery rather than guessing which device should be restarted.

See [`docs/PRODUCTION_AUDIT.txt`](docs/PRODUCTION_AUDIT.txt) for additional production-hardening information.

## Requirements

- Windows 10 or Windows 11 x64
- NVIDIA GPU and driver exposing the required NVML functionality
- Administrator privileges
- Visual Studio with **Desktop development with C++** when building from source

No CUDA SDK or separate NVIDIA SDK is required. The application dynamically loads the NVML DLL installed with the NVIDIA driver.

## Build

Open:

```text
NvidiaPowerStateWatchdog.sln
```

and build:

```text
Release | x64
```

or run:

```bat
BUILD_RELEASE.bat
```

The executable is produced at:

```text
x64\Release\NvidiaPowerStateWatchdog.exe
```

## Source layout

```text
NVIDIA-PowerState-Watchdog/
├── main.cpp
├── resource.h
├── app.rc
├── NvidiaPowerStateWatchdog.ico
├── NvidiaPowerStateWatchdog.sln
├── NvidiaPowerStateWatchdog.vcxproj
├── BUILD_RELEASE.bat
├── README.md
├── .gitignore
├── .gitattributes
└── docs/
    └── PRODUCTION_AUDIT.txt
```

## Support the project

If NVIDIA PowerState Watchdog is useful to you and you would like to support continued development:

**PayPal:** [paypal.me/xrplambo](https://www.paypal.com/paypalme/xrplambo)

## Disclaimer

This software changes NVIDIA GPU clock policy and may restart the NVIDIA Platform Controllers and Framework device when recovery is required.

Hardware, firmware, BIOS, driver, and OEM implementations vary. Behavior observed on one system is not guaranteed on another.

Use it at your own risk.
