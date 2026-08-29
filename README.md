# Corsair NVIDIA Fan Control

Small native Win32/C tool for controlling Corsair Commander Pro fan outputs
without iCUE. It uses the device's USB HID protocol directly and targets:

- Corsair Commander Pro (`VID 1B1C`, `PID 0C10`)
- Corsair Obsidian 1000D integrated Commander Pro (`VID 1B1C`, `PID 1D00`)

## Features

- Detects and initializes all supported Corsair HID fan controllers.
- Provides a live overview of fan RPM, modes, profiles, target duty, probe
  temperatures, rail voltages, firmware and connection state for every controller.
- Reads firmware, bootloader, fan modes, probe
  presence, RPM, temperatures and SATA rail voltages.
- Applies fixed duty-cycle fan speeds per channel or to all connected channels.
- Can force a fan channel mode to PWM, DC or Off when detection is wrong.
- Reads the first NVIDIA GPU core temperature through NVAPI and can run a
  multi-point GPU-temperature fan curve, edited directly on the curve graph,
  across every detected controller.
- Shows a live GPU telemetry panel (GPU clock, memory clock, fan speed,
  board power draw and power limit) read through NVML, with a power-
  utilisation meter. The voltage row is a placeholder: neither NVML nor
  NVAPI exposes the live core voltage through a public interface.
- Saves fan modes, fixed duty values, selected controller and GPU curve points
  and can reapply them automatically on startup.
- Can register itself for login autostart and run minimized in the Windows
  notification area.
- Uses a fan application/tray icon derived from Bootstrap Icons.
- Uses the common third-party synchronization mutex
  `Global\CorsairLinkReadWriteGuardMutex` around HID transactions.

## Build

Preferred LLVM Clang build for Ryzen 9 7950X3D:

```powershell
cmake --preset clang-7950x3d
cmake --build --preset clang-7950x3d-release
```

This preset uses `C:\Program Files\LLVM\bin\clang-cl.exe`, `lld-link.exe`,
`llvm-rc.exe` and `llvm-mt.exe`. Release builds are tuned for Zen 4 with
`/clang:-march=znver4`, `/clang:-mtune=znver4`, ThinLTO and linker dead-code
folding. The resulting binary is CPU-specific and should be treated as a local
build for the 7950X3D-class machine.

The optimized GUI executable is `build-clang/corsair_nvidia_fan_control.exe`;
the read-only diagnostic executable is `build-clang/corsair_nvidia_probe.exe`.

Verified direct MinGW-w64 build:

```powershell
x86_64-w64-mingw32-gcc -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0600 -mwindows src/main.c src/corsair_hid.c src/nvidia_temp.c -o build/corsair_nvidia_fan_control.exe -ladvapi32 -lcomctl32 -lhid -lshell32 -lsetupapi
```

With CMake, use Visual Studio or a MinGW environment where the generator and
compiler use compatible path syntax, for example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
cmake --build build
```

The executable will be `build/corsair_nvidia_fan_control.exe` for that non-preset
example.

For read-only hardware diagnostics, build and run `corsair_nvidia_probe.exe`.
It lists supported devices and reads firmware, fan modes, RPM, temperatures and
voltages without changing fan settings. It also reports the NVIDIA GPU core
temperature when `nvapi64.dll` is available from the installed NVIDIA driver.

## Use

1. Close Corsair iCUE and other tools that talk to the same controller.
2. Start `corsair_nvidia_fan_control.exe`.
3. Click `Scan` if needed. All detected controllers are opened and initialized;
   the device selection only chooses which controller is displayed and edited.
4. Click `Overview` to see the cached live values from every controller in one
   resizable window. The tables repaint with the five-second background poll;
   hardware values themselves refresh every second refresh tick (~10 seconds,
   see below).
5. Move a fan slider and click the row's `Apply`, or use `Apply all`.
6. For GPU-based control, select `NVIDIA` in a fan's mode box. The curve is
   edited directly on the `GPU CURVE` graph, where both temperature and fan
   duty range from 0 to 100: right-click adds a point, dragging moves it,
   and double-click (or right-click without dragging) removes a point;
   at least two points are required. Defaults are `25 C -> 30%` and
   `80 C -> 100%`.
7. Click `Save` to persist all current fan modes, fixed percentages and curve
   points. If the controller is open, `Save` also applies them immediately.
8. Enable `Autostart` to create a per-user login entry. The stored command is
   `corsair_nvidia_fan_control.exe --tray`, so Windows starts it hidden in the
   notification area next to the clock. The tool also updates Windows'
   `StartupApproved` state and retries the tray icon if Explorer is not ready
   yet during login.

The Commander Pro stores hardware-side fan settings, so fixed values can keep
running after the GUI exits. Reinitialize after boot, resume or USB reconnect.
The `NVIDIA` curve is software-driven and only updates while this tool is
running. The background poll fires every 5 seconds and repaints the UI with
the latest values; GPU temperature polling also runs every 5 seconds. HID
sensor reads and NVIDIA curve updates are rate-limited to every second
refresh tick (~10 seconds) for all detected controllers, independently of the
controller selected in the GUI, to halve the worst-case HID I/O on the UI
thread.
The GUI saves the selected controller and GPU curve points under
`HKCU\Software\CorsairNvidiaFanControl`; fan mode selections, fixed duty values
and fan auto-apply flags are stored per physical controller under
`HKCU\Software\CorsairNvidiaFanControl\Devices\<device-key>`. Existing settings
from the older `HKCU\Software\CorsairFanControl` key are read as a compatibility
fallback. On `--tray` autostart it opens every detected controller and reapplies
each controller's own saved settings automatically.

Minimizing or closing the window hides it to the tray. Double-click the tray
icon to restore it, or right-click the icon and choose `Exit` to stop the
program.

## Notes and sources

This implementation is based on public reverse-engineered and kernel-driver
documentation:

- Linux kernel hwmon documentation for `corsair-cpro`: supported devices,
  sysfs model, and exposed fan/temp/voltage capabilities:
  https://docs.kernel.org/hwmon/corsair-cpro.html
- Linux `corsair-cpro` driver by Marius Zachmann: command IDs, response status
  bytes, and HID buffer behavior:
  https://github.com/MisterZ42/corsair-cpro
- `liquidctl` Commander Pro guide and driver: initialization, status reads,
  fixed duty-cycle control, fan modes, and the Obsidian 1000D PID:
  https://github.com/liquidctl/liquidctl/blob/main/docs/corsair-commander-guide.md
  https://github.com/liquidctl/liquidctl/blob/main/liquidctl/driver/commander_pro.py
- Corsair Commander Pro quick-start guide: physical capabilities, six DC/PWM
  fan channels and four thermistors:
  https://assets.corsair.com/image/upload/corsairmedia/sys_master/productcontent/WW_CommanderPRO_QSG_Web_AE.pdf
- Corsair Obsidian 1000D product page: integrated Commander Pro controller:
  https://www.corsair.com/us/en/s/obsidian-1000d-case
- FanControl CorsairLink plugin: current Windows ecosystem notes and the shared
  mutex used by compatible third-party tools:
  https://github.com/EvanMulawski/FanControl.CorsairLink
- NVIDIA NVAPI documentation: official Windows API and thermal sensor function
  used for the GPU core temperature:
  https://developer.nvidia.com/nvapi/get-started
  https://docs.nvidia.com/nvapi/group__gputhermal.html
  https://github.com/NVIDIA/nvapi
- Application icon: Bootstrap Icons `fan`, MIT licensed, adapted as a white fan
  on a blue circle for better tray visibility:
  https://icons.getbootstrap.com/icons/fan/
  https://github.com/twbs/icons/blob/main/LICENSE

Known limitation: iCUE does not coordinate through the third-party mutex, so it
can still interfere. For reliable control, keep iCUE closed.
