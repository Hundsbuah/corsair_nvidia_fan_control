# Corsair NVIDIA Fan Control

Small native Win32/C tool for controlling Corsair Commander Pro fan outputs
without iCUE. It uses the device's USB HID protocol directly and targets:

- Corsair Commander Pro (`VID 1B1C`, `PID 0C10`)
- Corsair Obsidian 1000D integrated Commander Pro (`VID 1B1C`, `PID 1D00`)

## Features

- Detects supported Corsair HID fan controllers.
- Initializes the controller and reads firmware, bootloader, fan modes, probe
  presence, RPM, temperatures and SATA rail voltages.
- Applies fixed duty-cycle fan speeds per channel or to all connected channels.
- Can force a fan channel mode to PWM, DC or Off when detection is wrong.
- Reads the first NVIDIA GPU core temperature through NVAPI and can run a
  simple two-point GPU-temperature fan curve.
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
3. Click `Scan` if needed and select a device. Selection automatically opens
   and initializes the controller.
4. Move a fan slider and click the row's `Apply`, or use `Apply all`.
5. For GPU-based control, select `NVIDIA` in a fan's mode box. The curve is a
   simple two-point interpolation from `Low C/%` to `High C/%`; defaults are
   `40 C -> 25%` and `80 C -> 100%`.
6. Click `Save` to persist all current fan modes, fixed percentages and curve
   points. If the controller is open, `Save` also applies them immediately.
7. Enable `Autostart` to create a per-user login entry. The stored command is
   `corsair_nvidia_fan_control.exe --tray`, so Windows starts it hidden in the
   notification area next to the clock. The tool also updates Windows'
   `StartupApproved` state and retries the tray icon if Explorer is not ready
   yet during login.

The Commander Pro stores hardware-side fan settings, so fixed values can keep
running after the GUI exits. Reinitialize after boot, resume or USB reconnect.
The `NVIDIA` curve is software-driven and only updates while this tool is
running. GPU temperature polling and curve updates run every 5 seconds. The GUI
saves the selected controller and GPU curve points under
`HKCU\Software\CorsairNvidiaFanControl`; fan mode selections, fixed duty values
and fan auto-apply flags are stored per physical controller under
`HKCU\Software\CorsairNvidiaFanControl\Devices\<device-key>`. Existing settings
from the older `HKCU\Software\CorsairFanControl` key are read as a compatibility
fallback. On `--tray` autostart it opens the saved controller and reapplies that
controller's saved settings automatically.

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
