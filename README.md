# NVIDIA PowerState Watchdog

**NVIDIA PowerState Watchdog** is a native Windows utility that monitors an NVIDIA GPU's actual performance state (P-State) and automatically restores a selected state when the GPU moves away from it.

The selected **P-State is the target**. The watchdog changes the GPU's P-State indirectly by setting the memory clock associated with that state.

> **Unofficial project:** NVIDIA PowerState Watchdog is not affiliated with, endorsed by, or supported by NVIDIA Corporation. NVIDIA, GeForce, and related marks are trademarks of their respective owners.

## What it does

- Monitors the GPU's actual NVIDIA P-State.
- Dynamically detects the P-States exposed by the selected GPU.
- Determines the memory clock associated with each usable P-State.
- Lets the user select a target P-State such as `P0`, `P2`, or `P4`.
- Changes the memory clock to the value associated with the selected P-State.
- Automatically restores the selected P-State when **Automatically restore selected P-State if it changes** is enabled.
- Uses normal NVML recovery first.
- If the requested state cannot be held normally, restarts only **NVIDIA Platform Controllers and Framework (NPCF)**.
- Never automatically restarts or removes the NVIDIA display adapter.
- Detects memory overclocks without treating them as a P-State failure.
- Confirms that the selected target remains stable before declaring recovery successful.
- Pauses enforcement during Modern Standby, display-off, and system suspend.
- Can start automatically through an elevated Windows logon task.
- Runs in the notification area with optional live telemetry on hover.
- Records recovery activity with full local date and time.

## How P-State control works

NVIDIA PowerState Watchdog does not directly set the GPU/core frequency.

Instead, it changes the **memory clock** to the value associated with the selected P-State. For example, if the GPU exposes:

```text
P0 -> 14001 MHz
P2 -> 11001 MHz
P4 ->  9001 MHz
```

selecting `P0` causes the watchdog to request the `14001 MHz` memory clock, causing the GPU to enter the corresponding P-State.

The watchdog does **not** lock or directly control the GPU/core clock. Graphics frequency remains dynamically controlled by the NVIDIA driver and continues to scale normally with workload, power, voltage, and thermal conditions.

## P-State discovery

NVIDIA PowerState Watchdog does not assume a fixed set of P-States or memory clocks.

At runtime, the application detects the P-States exposed by the selected GPU and determines the memory clock associated with each usable state.

For example, one RTX 5090 Laptop GPU may report:

```text
P0 -> 14001 MHz
P2 -> 11001 MHz
P4 ->  9001 MHz
P5 ->   810 MHz
P8 ->   405 MHz
```

Another GPU may expose a different set of states and clocks.

Clearly separated deep low-power states used for idle/display operation are filtered from the normal target list rather than being presented as performance targets. The filtering is based on the detected clock table rather than hardcoded GPU models or fixed P-State numbers.

## Memory overclock support

The watchdog monitors **P-State**, not whether the live memory clock exactly matches the stock memory clock associated with that state.

For example:

```text
Target P-State:  P0
Current P-State: P0
Memory:          15801 MHz
Memory OC:       +1800 MHz
```

is considered healthy and does not trigger recovery.

The GPU is still in the requested P-State even though the live memory clock is higher because of the user's overclock.

The application does not modify the user's memory overclock offset. Afterburner, NVIDIA software, or another tuning utility remains responsible for applying the overclock itself.

## Recovery

Recovery is intentionally conservative.

1. Detect that the actual P-State no longer matches the selected target.
2. Retry the normal NVML memory-clock request first.
3. If the target still cannot be held, pre-arm the selected target request and restart only **NVIDIA Platform Controllers and Framework (NPCF)**.
4. After NPCF returns, continue observing the actual P-State.
5. Declare recovery successful only after the selected target is observed for multiple consecutive verification samples.

A single brief target-P-State sample is not treated as successful recovery. If the GPU drops away from the target during verification, the confirmation resets and the same recovery episode remains active.

There is **no automatic NVIDIA display-adapter restart or removal path**.

## Example recovery

```text
[2026-10-12 05:57:07] Connected to NVIDIA GeForce RTX 5090 Laptop GPU.
[2026-10-12 05:57:07] Supported P-States: P0 P2 P4.
[2026-10-12 05:57:07] NPCF: NVIDIA Platform Controllers and Framework (ACPI\NVDA0820\NPCF).
[2026-10-12 05:57:07] Startup: target P-State requested: P0.
[2026-10-12 05:57:07] P-State does not match target (actual P4, target P0); attempting recovery.
[2026-10-12 05:57:08] Restarting NVIDIA Platform Controllers and Framework... (attempt 1/3).
[2026-10-12 05:57:10] P-State locked to selected target: P0.
```

The event log intentionally stays relatively sparse rather than logging every polling cycle or telemetry refresh.

## Controls

- **GPU index** — NVML device index.
- **Target P-State** — dynamically populated from the selected GPU.
- **Poll interval** — monitoring interval from 100 to 10000 ms.
- **Recovery attempts** — maximum automatic NPCF restart attempts during one continuous failure episode.
- **Automatically restore selected P-State if it changes** — enables automatic enforcement and recovery. **Off by default on a fresh install.**
- **Show telemetry when hovering over tray icon** — displays the custom live telemetry card. When disabled, no custom hover window is shown and the normal tray tooltip displays `NVIDIA PowerState Watchdog`.
- **Start with Windows** — immediately creates or removes the elevated logon task when the checkbox is changed. **Off by default.**
- **Start minimized to tray** — starts hidden when launched with `--startup`.

All checkbox options and Target P-State changes are persisted immediately. **Save Settings** is only needed for the editable numeric settings.

## Modern Standby and display-off behavior

When the session display turns off or Windows suspends, monitoring pauses and NVML is closed so legitimate low-power GPU states do not trigger recovery.

When the session becomes active again, the application waits briefly for the NVIDIA driver, reconnects to NVML, rebuilds P-State mappings if necessary, requests the selected target when automatic restore is enabled, and resumes monitoring.

Recovery state from before the power transition is invalidated so a late NPCF worker result cannot alter the new session state.

## Start with Windows and elevation

The application requests Administrator privileges because NVML memory-clock changes and NPCF device restart require elevation on the tested system.

**Start with Windows** uses an `ONLOGON` scheduled task configured with **highest privileges** and launches the same executable with `--startup`.

The checkbox itself creates or deletes the task immediately. **Save Settings does not create, update, or delete the startup task.**

On launch, the application queries Task Scheduler for the `NVIDIA PowerState Watchdog` task and uses its presence as the **Start with Windows** state. Checking the box recreates the task with the current executable path, `--startup`, an `ONLOGON` trigger, and highest privileges.

Legacy task names from older NVIDIA VRAM Watchdog / NVIDIA Memory Watchdog builds are removed silently.

## Safety

The recovery system deliberately avoids aggressive GPU reset methods:

- no `/remove-device` path;
- no NVIDIA display-adapter restart;
- no GPU re-enumeration;
- no forced termination of active 3D applications;
- NPCF device matching requires the canonical NVIDIA Platform Controllers and Framework name plus NVIDIA ACPI/PCI identity;
- ambiguous NPCF matches are rejected;
- `pnputil.exe` is launched from its absolute System32 path with a hard timeout;
- `nvml.dll` is loaded only from System32.

If a compatible NPCF device cannot be identified, the watchdog continues to use normal NVML recovery rather than guessing which device should be restarted.

See [`docs/PRODUCTION_AUDIT.txt`](docs/PRODUCTION_AUDIT.txt) for additional implementation notes.

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

This software changes the NVIDIA GPU's **P-State by changing its memory clock** and may restart the NVIDIA Platform Controllers and Framework device when recovery is required.

It does **not** lock or directly control the GPU/core clock.

Hardware, firmware, BIOS, driver, and OEM implementations vary. Behavior observed on one system is not guaranteed on another.

Use it at your own risk.
