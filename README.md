# Rugrats Adventure Game: Unofficial 64-bit Port

A native 64-bit Windows port of **Rugrats Adventure Game** (Broderbund / Mohawk "Living Books"
player 3.1, 1998). It runs the game's own scripts and media, read straight from **your original
CD-ROM**, so the game plays exactly like it did in 1998. It doesn't need the 16-bit installer,
QuickTime 3, DirectX 5 or a 256-color display mode.

Ported by **T.M. DEVELOPMENTS**. Vibe-coded with Claude (Anthropic) in October 2026.

**Unofficial port by T.M. DEVELOPMENTS. Not endorsed by or affiliated with** HMH Education
Company, Encore, Inc., Nickelodeon or Skydance Corporation. This is a noncommercial fan
project. **This port contains no game content.** It needs the files from an original CD-ROM that
you own.

---

## Requirements

| | Minimum | Recommended |
|---|---|---|
| OS | Windows 10 version 1607 (build 14393) or later, x64 | Windows 11 |
| CPU | Any x86-64 CPU (SSE2) | Dual core, 2 GHz |
| RAM | 2 GB | 4 GB |
| Graphics | Any GPU with Direct3D 11 drivers (feature level 9.1 or later; falls back to Direct3D 9, OpenGL or software) | Direct3D 11 |
| Sound | Any Windows audio device (it runs silently without one) | |
| Disk | About 1.1 GB (game data copied from the disc, about 400 MB; converted movies and sounds, about 670 MB; exe and DLLs, about 150 MB) | SSD |
| Other | An original *Rugrats Adventure Game* CD-ROM (Windows/Mac hybrid, 1998) | |

You don't need any runtimes. The C++ runtime is linked in, and Windows 10/11 already ship the
Universal CRT that the FFmpeg DLLs use. Windows 7 SP1 and 8.1 (64-bit) *may* work with
update KB2999226 and a Windows 7-compatible FFmpeg build, but they are untested. See
`docs/INSTALLER.md` for retro/XP notes.

## Installing

### With the installer (recommended)

1. Run the setup program and confirm you own an original copy.
2. Point it at your CD-ROM drive, or a folder copied from the disc. The folder must contain
   `MHK\` and `PROGRAM\`.
3. Setup copies the game's files from the disc to your computer. The game always runs from
   these local files, so the disc is only needed during setup.
4. Setup converts the movies and sounds once (`RUGRATS.EXE --prepare`). This takes a minute
   or two.

Downloads are on the project's **[GitHub Releases](https://github.com/tm-developments/rugrats_port/releases)** page (mirrored on
SourceForge). The installer isn't code-signed, so Windows SmartScreen may say "Windows protected
your PC". Click **"More info"**, then **"Run anyway"**.

### Portable use

After installing, you can copy the game folder anywhere (a USB stick, a game library) and
play it from there. To keep saves in the game folder itself, start the game with
`--portable` or create an empty file named `portable` next to `RUGRATS.EXE`. Saves,
`settings.ini` and the log then stay in that folder. If the folder is protected (for
example under Program Files), the game asks once for administrator rights and restarts.
If you decline, it uses `%APPDATA%` as usual.

## Saves and settings

The port keeps saves, settings and logs in the user's own folder:

```
%APPDATA%\Rugrats Adventure Game\
  saves\                one file per player
  PLAYFILE, GLOBALS     the game's own player and progress files
  rugrats_players.txt   the sign-in list
  settings.ini          display mode, renderer and volume (see below)
  rugrats_log.txt       log of the last run (rugrats_crash.txt / .dmp after a crash)
```

The converted movies and sounds live in the game folder (made by setup), or in
`%LOCALAPPDATA%\Rugrats Adventure Game\Cache` if that folder was read-only on the first run.

It never writes to the install folder, so it runs as a standard user. It works the same when
run as administrator: there is no VirtualStore redirection, and you get the same saves either
way. The exe has an `asInvoker` manifest.

### settings.ini

Edit it with Notepad while the game is closed. The game rewrites it when you change the volume.

```
[Display]           Syntax:
mode=fullscreen     fullscreen | borderless | windowed
renderer=d3d11      d3d11 | d3d9 | opengl | software (a command-line switch overrides it)
[Volume]
level=10            0 (silent) to 10 (full)
mute=0              1 = muted
```

## Controls and switches

Everything in the game is mouse-driven, as in the original. The original's keyboard shortcuts
(from its manual and ReadMe) all work:

| Key | Action |
|---|---|
| **Alt+Enter**, **Ctrl+Enter** or **Space** | Skip a movie, where the original allows it (never the end credits). Also **File > Stop Movie** |
| **Ctrl+Q** | Quit the game (**File > Exit**) |
| **Alt+R** or **Ctrl+R** | Go to the Replay screen (**File > Go To Replay**) |
| **Ctrl+Up** / **Ctrl+Down** | Turn the volume up / down (**Options > Volume**) |
| **Ctrl+M** | Mute or unmute (**Options > Volume > Mute**) |
| **Ctrl+V** | About the game (**File > About**) |
| **Alt** (windowed) | Show or hide the menu bar (shown when the game starts in a window; hidden after F11 until you press Alt; never in full screen) |

The port adds:

| Key | Action |
|---|---|
| **F11** | Switch between full screen and a window (also **Options > Display > Zoom**) |
| **Esc** | Same as the toolbar's Quit button; during a movie, skips it like Space |
| **Enter** | Also skips a movie, like Space |
| **Alt+Tab** | In exclusive full screen the game minimizes to the taskbar and pauses; click it to return |
| **Alt+F4** | Close the game at once, in any mode (emergency exit; progress is still saved on the way out) |

Command-line switches are for special cases. Add them to the game's shortcut: right-click it,
choose **Properties**, and type them in the **Target** box after the closing quote, for example
`"C:\Program Files\Rugrats Adventure Game\RUGRATS.EXE" --borderless`.

| Command-line switch | Effect |
|---|---|
| *(none)* | Exclusive full screen at the desktop resolution, the 4:3 game scaled to fit |
| `--borderless` | Borderless full-screen window (instant Alt+Tab, works with overlays and recorders) |
| `--windowed` | Start in a window |
| `--fullscreen` | Force exclusive full screen, overriding settings.ini |
| `--data DIR` | Game data folder (the one holding `MHK\` and `PROGRAM\`), if not next to the exe |
| `--userdir DIR` | Keep saves, settings and logs in DIR |
| `--portable` | Keep saves, settings and logs in the game folder (asks for admin rights when that folder is protected) |
| `--renderer NAME` | Drawing backend, if the default gives trouble on your PC: `d3d11` (Direct3D 11, the default), `d3d9` (older GPUs and drivers), `opengl`, or `software` (no GPU at all, slowest). Shortcuts: `--d3d9`, `--opengl`, `--software` |
| `--prepare [OUTDIR]` | Build `movies\` and `sounds\` from the archives, then exit (used by setup) |

The menu bar keeps the original player's menus: File (Stop Movie, Go To Replay, About, Exit) and
Options (Display > Disable Mode Changes, Disable Warnings, Zoom; Volume > Mute, Increase,
Decrease). Unknown switches are ignored. (`--elevated` is internal: the game adds it when it
restarts itself as administrator for `--portable`.)

## What's different from the original

The port reimplements the Mohawk 3.1 player and runs the game's original page scripts.
**Gameplay, timing, dialogue, puzzles, difficulty levels, Let's Play / Let's See / Replay
and the endings are the original's.** Changes cover how it runs on a modern PC.

### Runs on today's Windows

- **Native 64-bit.** The original's InstallShield3 setup (`SETUP.EXE`, 16-bit) can't run on
  64-bit Windows at all, and the game needs QuickTime 3.0 and DirectX 5. The port needs none of
  them.
- **No 256-color display mode change.** The original switched the screen to 640x480, 256 colors.
  On Windows 8 and later that causes palette corruption, black screens and resized desktop
  windows. The port draws the 8-bit, palettized art itself in true color, so colors are always
  correct.
- **QuickTime-free movies.** The movies (Cinepak, Apple SMC, IMA ADPCM audio) are decoded with
  FFmpeg. Their timing follows the original's movie clock and voices, so lip-sync matches.
- **Sound** is mixed at 22,050 Hz, 16-bit, like the original's WaveMix setting, with no MIDI and
  no stutter on modern audio devices.

### Display

- **Scaling to any resolution.** Exclusive full screen at the monitor's own resolution (no mode
  switch, no flicker), the 640x480 canvas scaled to fit with correct 4:3 proportions and
  pillarboxing on wide screens.
- Optional **borderless full screen** and a **resizable window** with the original menu bar.
  F11 and Alt work as described above. Drawn with Direct3D 11, using
  Windows' modern flip-model presentation.
- **Per-monitor DPI-aware**, so it's sharp on 4K and laptop displays with no blurry Windows
  scaling.
- **Smooth, refresh-rate-aware turning.** When you click the left or right edge to turn the
  room, the original jumps 40 pixels at a time, about 30 times a second. The port glides the
  room between those steps at your display's own refresh rate (60, 120, 144Hz and up). The
  game's timing is untouched: each turn takes exactly as long as in the original and ends in
  the same place. The same applies to the side-to-side moves and opening pan in the chute game.
- **Smooth fades.** Fades to and from black are drawn smoothly at the display's refresh rate,
  with the same timing as the original.
- The game **pauses itself when it loses focus** (Alt+Tab/minimize) and carries on where it was.

### Install, saves and reliability

- **Runs entirely from your hard drive.** Setup copies the game data once and the disc is
  never needed to play. The original always ran its 350 MB of `MHK` archives from the CD-ROM.
- **Per-user saves** in `%APPDATA%`, never in Program Files, so you don't need admin rights,
  there's no VirtualStore, and each Windows user has their own players. Portable mode is
  optional.
- Volume is the game's own (Options > Volume, remembered in settings.ini) rather than the
  system-wide wave volume that the original changed.
- **Crash reporting**: a log of each run, plus a crash report and minidump if something goes
  wrong.
- Fast loading: movies and sounds are pre-extracted once, so pages open instantly.

### Information

- New About box and file version info with the port's credits, disclaimer and open-source
  notices.
- New high-resolution icon. The small sizes (16 to 64 px) are Tommy from the original
  `RRATADV.ICO`, and the large sizes (96 to 256 px) are the *Rugrats Adventure Game* logo.

Bugs found and fixed while play-testing the port itself aren't listed here, since the
original never had them.

## Development timeline

About six working days, all vibe-coded with Claude by Anthropic. The work covered reverse
engineering the Mohawk player and its LB v3 script bytecode, writing the engine and then
play-testing side by side against footage of the original.

| Dates (2026) | Work |
|---|---|
| Oct 13 to Oct 18 | Archive, bitmap, sprite, movie and sound support; the script engine; the house, all 11 activities, episodes, cutscenes, Let's Play / Let's See / Replay; playtesting against the original to a complete playthrough |
| Oct 23 | Final fixes (final fight, end credits, Replay, quit flow); release build: display modes, per-user saves, built-in asset preparation, icon, version info, licenses, installer plan |

## Bugs and feature requests

Please report bugs and suggest features on the project's **[GitHub Issues](https://github.com/tm-developments/rugrats_port/issues)** page.
That's the only place they're tracked. The SourceForge page is a download mirror.

A good bug report includes:

- what happened and what you expected (the original game's behavior, if you know it)
- where in the game it happened (room, activity, cutscene)
- your Windows version and display mode (fullscreen, borderless or windowed)
- `rugrats_log.txt` from `%APPDATA%\Rugrats Adventure Game` and, after a crash, `rugrats_crash.txt`
  and `rugrats_crash.dmp` from the same folder

Please don't attach or link the game's own files. They're the rights holders' property.

## Building from source

The port is C++17, using SDL2 (2.30+) and FFmpeg (libavformat, libavcodec, libavutil,
libswscale; 7.0+ or master), both found through pkg-config. It builds with CMake using
mingw-w64 GCC, either natively in MSYS2 (UCRT64) or as a cross build from Linux:

```sh
# MSYS2 UCRT64:  pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,pkgconf,SDL2,ffmpeg}
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```sh
# Linux cross build: a toolchain file for x86_64-w64-mingw32, and PKG_CONFIG_LIBDIR pointing
# at the Windows SDL2 and FFmpeg .pc files
PKG_CONFIG_LIBDIR=/path/to/ffmpeg/lib/pkgconfig:/path/to/SDL2/lib/pkgconfig \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is `build/RUGRATS.EXE`, with the C++ runtime linked in statically. Put it next to
`SDL2.dll` and the five FFmpeg DLLs. The small format tests in `test/` build alongside it.

`-DRUGRATS_DEV=ON` builds the development variant. It adds the test switches (headless runs,
scripted clicks, `--at FRAME:ACTION`) and the trace environment variables. Release builds ignore
all of them.

Source layout: `src/mohawk.*` (archives), `src/video_*` (bitmaps, sprites, movies),
`src/audio.*`, `src/prepare.*` (asset preparation), `src/lb/*` (the LB v3 script engine) and
`src/main.cpp` (window, menu, toolbar, game flow).

## Credits

- **Port:** T.M. DEVELOPMENTS, vibe-coded with Claude by Anthropic.
- **Original game:** Broderbund Software and Nickelodeon (Viacom International Inc.), 1998. *Rugrats* created by Arlene Klasky, Gabor Csupo and Paul Germain.
- **Logo artwork (icon and installer banner):** from the [LaunchBox Games Database](https://gamesdb.launchbox-app.com/) (original source unknown).
- **Installer artwork:** fan art (artist unknown) from the [LaunchBox Games Database](https://gamesdb.launchbox-app.com/). If you made it, please open an issue on GitHub (see *Bugs and feature requests*) to be credited by name or to have it removed. *Rugrats* characters are trademarks of Viacom International Inc.
- **Installer error icon:** from the [Nuvola](https://en.wikipedia.org/wiki/Nuvola) icon set by David Vignoni, via Wikimedia Commons (GNU LGPL v2.1, see `LICENSE-LGPL-2.1.TXT`).

### Open-source software used

| Component | Used for | License |
|---|---|---|
| [SDL2](https://www.libsdl.org/) 2.30.11 (Sam Lantinga and contributors) | Window, input, audio output, rendering and scaling | zlib License |
| [FFmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswscale, libswresample) | Decoding the game's QuickTime movies and their soundtracks | LGPL 2.1+ (this build: GPL v3) |
| [GCC](https://gcc.gnu.org/) libstdc++ / libgcc | C++ runtime, statically linked | GPL v3 with the GCC Runtime Library Exception |
| [mingw-w64](https://www.mingw-w64.org/) runtime and winpthreads | Windows C runtime startup and threads, statically linked | ZPL 2.1 / MIT-style |
| [ScummVM](https://www.scummvm.org/) (reference only, no code included) | Public documentation of the Mohawk / Living Books formats | GPL v3+ |

Many thanks to all of these projects. Their full license texts, copyright notices and source
locations are in `THIRD-PARTY-NOTICES.txt`, which is installed next to the game.

## License

The port's source code is free software under the **GNU General Public License v3.0 or later**.
See `LICENSE.txt`. It uses SDL2 (zlib License) and FFmpeg (LGPL 2.1+/GPL). See
`THIRD-PARTY-NOTICES.txt` for their licenses and where to get their source.

The license covers only the port's own code. The game's content (art, animation, voices, music,
scripts, text) belongs to its rights holders and isn't distributed with the port.

© 1998 Viacom International Inc. All Rights Reserved. *Rugrats* and all related titles,
logos and characters are trademarks of Viacom International Inc. *Rugrats* created by
Arlene Klasky, Gabor Csupo and Paul Germain.
