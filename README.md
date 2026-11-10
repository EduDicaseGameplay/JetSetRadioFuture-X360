# Jet Set Radio Future on Xbox 360

My native port of Jet Set Radio Future (original Xbox, USA version) for RGH/JTAG Xbox 360 consoles. [Leia em português](LEIAME.md).

It's not an emulator and it's not backward compatibility. The game's x86 code is translated to C++ on a PC and compiled for the 360's PowerPC CPU, with the Xbox kernel, Direct3D 8 and DirectSound rebuilt on top of the 360's own libraries. The result is a real `default.xex`, built from your own copy of the game.

**No game here.** This repository has no game files, no prebuilt XEX and no SDK. You need your own copy of the game, and you have to install the tools yourself. The generated code and the XEX contain the game's code, so please don't share them.

## Where it stands

- 720p, widescreen, at a steady 30 fps by default.
- I played the garage and all 14 stages on a real console, including a 20-minute session with 15 stage changes.
- Most stages stay at 30 fps, with short dips into the low 20s. The exceptions are the Dogenzaka Hill crowds, which drop as low as 12 fps, and 99th Street, which averages about 26 fps and drops to 18 at its busiest spot. In those places the game's own logic and drawing take longer than a frame allows.
- Movies are skipped: the original WMV decoder is far too slow on the 360.
- The save path is wired (`hdd\E\UDATA` in the game folder), but I haven't tested saving and loading on the console yet. The same goes for the story, the bosses and the ending.

## Controls added by the port

| Button | What it does |
|---|---|
| LB + RB for 1 second | Opens the stage selector. A goes, B closes |
| X, in the selector | Video mode: 720p, or the original resolution with all effects |
| Y, in the selector | Frame pacing: `30 STEADY` (default), `AUTO 60/30` or `FREE (OLD)` |
| Back + Start for 2 seconds | Back to the dashboard |

## What you need

- An Xbox 360 with RGH or JTAG.
- Your own copy of the game, USA version, extracted: `default.xbe` and the `Media` folder. The XBE's SHA-1 must be `86ffb4081ffb1a8a5da2e03d3838382ce31d7ae8`.
- Windows, 64-bit. I build on Windows 11.
- **Visual Studio 2010**. The SDK needs it. I use the Ultimate edition.
- **Xbox 360 SDK 2.0.21256** (XDK), installed after VS 2010. Its installer sets the `XEDK` variable. Check it with `echo %XEDK%`.
- **Visual Studio 2022 Build Tools**, with the "Desktop development with C++" workload. It brings MSVC, CMake and Ninja, used for the PC-side tools.
- **Git**. CMake uses it to download Zydis and toml++.
- **Python 3**. No extra packages.

## Building

1. Copy your `default.xbe` to `pc-recomp\assets\default.xbe`.

2. Open the **x64 Native Tools Command Prompt for VS 2022** and go to the project folder.

3. Build the PC tools (the XBE analyzer and the recompiler):

   ```
   cmake -S x360/tools -B x360/tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build x360/tools/build
   ```

4. Translate the game and compile it for the 360:

   ```
   python ice_loop.py
   ```

   This creates `pc-recomp\generated` from your XBE and compiles it into `x360\build\gen`. If the VS 2010 compiler hits an internal error on some function, the script sets it aside and tries again.

5. Compile the runtime:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_runtime.ps1
   ```

6. Link the XEX:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_xex.ps1
   ```

   The result is `x360\build\default.xex`. On my PC, the whole build, from step 3 to here, took about 4 minutes. Most of that time is the link.

If the link stops with `LNK1257`, run `python x360\tools\ltcg_loop.py`. It leaves out of link-time optimization the files the linker fails on.

## Installing on the console

1. Create a folder on the HDD or a USB drive with `default.xex`, `default.xbe` and `Media` side by side. The XBE has to stay there, because the port reads its data.
2. Leave about 300 MB free. On the first run, the game creates a cache in `hdd\Z`, which makes later loads faster.
3. Launch `default.xex` from Aurora, FreeStyle Dash or XeXMenu.
4. The log is saved as `jsrf360.log` in the same folder.

## Option files

Empty files in the game folder:

| File | Effect |
|---|---|
| `sem_720p.txt` | 640x480 without widescreen |
| `sem_widescreen.txt` | 720p in 4:3 |
| `com_hiz.txt` | Turns Hi-Z on (off by default) |
| `sem_pulo.txt` | No frame skipping: below the target fps, the game runs in slow motion |
| `debug_buttons.txt` | L3 saves a debug capture, Back changes gamma |

`video_modo.txt` and `fps_modo.txt` are written by the stage selector to remember your choices.

## Credits

The runtime started from my [Bloody Roar Extreme port for the Xbox 360](https://github.com/EduDicaseGameplay/BloodyRoarExtreme-X360), which uses the [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool and [BloodyRoarExtremeRecomp](https://github.com/bromurok/BloodyRoarExtremeRecomp). Several fixes follow the PC port [JSRF Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed), and the function list was seeded from the [JSRF Decompilation](https://codeberg.org/KeybadeBlox/JSRF-Decompilation). Details and licenses are in `THIRD_PARTY.txt` and `pc-recomp`.

Jet Set Radio Future belongs to SEGA. This is a fan project with no connection to SEGA.
