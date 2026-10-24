# Installer plan

## 1. What the original setup did (disc analysis)

The disc runs `AUTORUN\AUTORUN.EXE`. That is Broderbund's two-button "switcher": **Install** or
**Play** once installed. `SETUP.EXE` reads `SETUP.CFG` and starts `setup\is_setup.exe`, which is
**InstallShield 3** (Stirling Technologies, 16-bit, `16on16=Y`) driven by Broderbund's generic
script `SETUP\SETUP.INS` and the config file `SETUP\SETUP.INI` (format version 1.4).
**It can't run on any 64-bit Windows.**

The AutoPlay right-click menu (`AUTORUN.INF`) offers: Install, Read-Me (`notepad readme.txt`),
Electronic Registration (`ereg32\Ereg32.exe`), Install QuickTime (`qtwsetup\setup.exe`),
Install DirectX 5 (`dxsetup\dxinst.exe`) and Install Adobe Acrobat Reader
(`adobe\reader32\setup.exe`).

**Install type.** There is only one ("Typical"). The steps are welcome, destination folder, copy,
then optional prerequisites. All four components are always installed:

| Component | Source on disc | Destination | Size |
|---|---|---|---|
| Program Files | `PROGRAM\*.*` (RUGRATS.EXE, OUTLINE, GLOBALS, PLAYFILE, MOHAWK.W32, SYSTEM.MHK, `MHK\` Z01-Z09 and DETECTOR.MHK) | install dir | 48 MB |
| Ereg | `EREG32\*.*` | `<dir>\Ereg` | 0.7 MB |
| ReadMe | `README.TXT` | install dir | 4 KB (21 KB actual) |
| manual | `MANUAL\*.pdf` (MANUAL.PDF, SOLUTION.PDF) | install dir | 1 MB |

The **354 MB `MHK\` folder (episodes, activities, movies) was never copied.** The game always ran
it from the CD, located through the registry.

**Default folder.** `C:\Program Files\Broderbund Software\Rugrats Adventure Game`. The
executable is `PROGRAM\RUGRATS.EXE` on the disc, installed under the same name.

**Start Menu folder.** `Broderbund Software\Rugrats Adventure Game` contains:

- Rugrats Adventure Game
- Rugrats Adventure Game Registration
- Rugrats Adventure Game ReadMe
- Rugrats Puzzle Solutions
- Rugrats Adventure Game Uninstall

`SETUP.INI` defines `item4` twice: "Rugrats User's Manual" and then "Rugrats Puzzle Solutions".
The second overrides the first, so the manual shortcut was probably never created. This is an
original bug.

**Registry**, from the strings in `SETUP.INS` and the original `RUGRATS.EXE`:

- `HKLM\SOFTWARE\Broderbund Software\Rugrats Adventure Game\...` holds
  `$Install_FromDrive$`, `$Install_FromRoot$` (the CD path), `$Install_ToDirectory$`,
  `Install Date` and `Disable Autorun Dialog`.
- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\...` holds the `UninstallString`
  (InstallShield 3 `uninst.exe` with a log file).
- The player itself reads `Software\Broderbund Software\<product>\Configuration` and
  `Software\Broderbund Software\MOHAWK\GFX\DirectDrawProfiles\<w>:<h>`.

**Prerequisite prompts** (yes/no):

- QuickTime 3.0: required, launches `QTWSETUP\SETUP.INS`.
- DirectX 5: Express or Custom.
- Adobe Acrobat Reader 3.01: for the manual.

At the end it launches Electronic Registration (`/chained`) and opens the readme.

**System checks** (`[Hardware Reqd1-5]`):

- 586-class CPU (100 MHz Pentium)
- 16 MB RAM
- 640x480
- 256 colors
- 16-bit 22.05 kHz wave output

## 2. The port's installer

### Pages

1. **Welcome.** "Welcome to the Rugrats Adventure Game (Unofficial 64-bit Port) setup. To make
   our lawyers happy, please make sure you own an original copy of Rugrats Adventure Game
   before continuing."
2. **License agreement.** `EULA.txt`. The user must accept.
3. **Game disc.** "Where is your Rugrats Adventure Game CD-ROM?" A folder picker, pre-filled with
   the first CD drive (or any drive) that holds `MHK\EPISODE0.MHK`, `PROGRAM\OUTLINE` and
   `PROGRAM\RUGRATS.EXE`. Next stays disabled until those files are found. A folder copied from the
   disc (or a mounted ISO) works too.
4. **One install type: always local.** Setup copies `MHK\` and `PROGRAM\` (about 400 MB) from
   the disc and the game always runs from that local copy, exactly like the test builds. The
   disc is needed only during setup. About 1.1 GB in total. There is no "run from disc"
   option.
5. **Destination.**
   - Admin install: `C:\Program Files\Rugrats Adventure Game (Unofficial Port)`.
   - Per-user install (no admin, if offered):
     `%LOCALAPPDATA%\Programs\Rugrats Adventure Game (Unofficial Port)`.
6. **Start Menu folder and an optional desktop icon.**
7. **Install.**
   - Copy the port's files.
   - Copy `MHK\` and `PROGRAM\` from the disc, then clear the read-only attribute that CD
     copies carry.
   - Copy `MANUAL.PDF` and `SOLUTION.PDF` from the disc.
   - Run `RUGRATS.EXE --prepare` (into the install folder; status text "Preparing movies
     and sounds...").
8. **Finish.** Options: Play now, View ReadMe.

### Installed layout

```
<install dir>\
  RUGRATS.EXE  SDL2.dll  avcodec-*.dll avformat-*.dll avutil-*.dll swscale-*.dll swresample-*.dll
  README.TXT  LICENSE.txt  THIRD-PARTY-NOTICES.txt  EULA.txt
  MANUAL.PDF  SOLUTION.PDF          (from the disc)
  MHK\  PROGRAM\                    (from the disc)
  movies\  movies.manifest  sounds\ (made by --prepare)
%APPDATA%\Rugrats Adventure Game\   saves\, settings.ini, rugrats_log.txt   (made by the game)
```

Don't install `avdevice-*.dll`, `avfilter-*.dll`, `ffmpeg.exe`, `ffplay.exe` or `ffprobe.exe`.
The port doesn't use them.

### Shortcuts

The Start Menu folder `Rugrats Adventure Game` contains:

- Rugrats Adventure Game
- Rugrats Adventure Game (Windowed) (`--windowed`)
- Rugrats Adventure Game ReadMe
- Rugrats User's Manual
- Rugrats Puzzle Solutions
- Uninstall Rugrats Adventure Game

### Registry

The installer uses its own key, never the Broderbund one, so it can't conflict with, or be
mistaken for, an original install:

- `HKLM` (or `HKCU` for a per-user install) `\Software\Rugrats Adventure Game Unofficial Port`:
  `InstallDir` and `Version`.
- The standard Uninstall entry: DisplayName "Rugrats Adventure Game (Unofficial Port)",
  Publisher "T.M. DEVELOPMENTS", DisplayIcon, EstimatedSize and an
  `URLInfoAbout` link to the project page.

The game doesn't need any of it. It finds its data next to the exe or through `--data`.

**Uninstall** removes the install folder, including the copied data and prepared movies, and the
`%LOCALAPPDATA%` cache. It asks before deleting saved games in `%APPDATA%\Rugrats Adventure
Game` (default: keep them).

**No prerequisites.** QuickTime, DirectX and Acrobat aren't needed: the C++ runtime is static,
and Windows 10/11 include the Universal CRT.

## 3. Building it with Wise for Windows Installer (chosen tool)

The installer is built with **Wise for Windows Installer** (final version): an x64 MSI, wrapped
in a WinRAR SFX that runs with the highest privileges (section 3b). The same plan also works in
InstallAware.

**Installation type: per-machine with UAC. Don't tick "Create a Vista Standard User
Installation".** That option makes a per-user MSI: it clears *Enable User Account Control*,
marks the package as not needing elevation, and installs into the user's own profile.

- Each Windows user would then need their own 1.1 GB copy, and the shortcuts and uninstall
  entry would only exist for that user.
- It contradicts the plan: Program Files, an elevated wrapper, HKLM registration, and the
  optional Users-Modify permissions on the game folder.

Keep **Enable User Account Control** ticked and set ALLUSERS=1 (per machine), with
`[ProgramFiles64Folder]` as the destination. The *game* still never needs admin rights: its
manifest is asInvoker and it saves to `%APPDATA%`. The only exception is `--portable` in a
protected folder (section 2).

**Product details**

| Field | Value |
|---|---|
| Product name | Rugrats Adventure Game (Unofficial Port) |
| Publisher / Manufacturer | T.M. DEVELOPMENTS |
| Version | 1.0.0 |
| Platform | x64 only. MSI: Template Summary `x64;1033`, components marked 64-bit, `ProgramFiles64Folder` |
| Install scope | Per-machine: `ALLUSERS=1`, Enable User Account Control ticked, *Vista Standard User Installation* unticked |
| Upgrade code | Generate once and keep it for all versions, so later releases upgrade in place |
| Icon / ARP icon | `res\rugrats_port.ico` |
| Signing | None. The release is unsigned, so expect the SmartScreen "More info, Run anyway" prompt (see the README). If you ever sign, sign `RUGRATS.EXE`, the `.msi` and the SFX, last of all |

**Wizard pages** (section 2's order)

1. **Welcome:** the lawyer text (section 2, page 1). Use a 164x314 side banner and a 493x58 top
   banner built from the port's own art: the logo, Tommy, the T.M. DEVELOPMENTS name.
2. **License:** `EULA.txt` as RTF, with "I accept" required.
3. **Game disc:** a Browse-for-folder field `GAMEDISC`. Pre-fill it by scanning drive letters D: to
   Z: for `MHK\EPISODE0.MHK`. The Next button runs a validation action (InstallAware: *If File
   Exists* checks; Wise: a WiseScript or VBScript custom action) that requires
   `MHK\EPISODE0.MHK`, `PROGRAM\OUTLINE`, a `PROGRAM\MHK\` folder and an `.EXE` in `PROGRAM\` (`installer\GameDisc.js`), and shows "Rugrats Adventure
   Game was not found there" otherwise.
4. **Destination:** `[ProgramFiles64Folder]Rugrats Adventure Game (Unofficial Port)`.
5. **Ready / Progress / Finish:** "Launch Rugrats Adventure Game" and "View ReadMe" checkboxes.

**Files**

- From the package (everything in the repository's local `dist\` folder, the release payload): `RUGRATS.EXE`, `SDL2.dll`, `avcodec-*.dll`, `avformat-*.dll`,
  `avutil-*.dll`, `swscale-*.dll`, `swresample-*.dll`, `README.TXT` (plain-text readme), `LICENSE-LGPL-2.1.TXT`,
  `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` and `EULA.txt`. Make each file its own component,
  with the exe as the keypath of the main component.
- From the disc, at install time:
  - `[GAMEDISC]\MHK\*` to `[INSTALLDIR]MHK\`
  - `[GAMEDISC]\PROGRAM\*` to `[INSTALLDIR]PROGRAM\`
  - `[GAMEDISC]\PROGRAM\MHK\*` to `[INSTALLDIR]PROGRAM\MHK\`
  - `MANUAL\MANUAL.PDF` and `MANUAL\SOLUTION.PDF` to `[INSTALLDIR]`

  None of the game's data is inside the installer.
  - InstallAware: *Copy Files* commands that run after *Apply Install*.
  - Wise/MSI: **MoveFile table** rows with Options = 0 (copy), source folder property
    `GAMEDISC`, file name `*`. This table isn't recursive, hence one row per folder. Add
    **RemoveFile** rows (`*` in each of those folders, on uninstall) so uninstall cleans them
    up.
  - Then clear the CD read-only attribute: `attrib -R "[INSTALLDIR]*" /S /D` as a hidden,
    deferred custom action.
- **Prepare step:** run `"[INSTALLDIR]RUGRATS.EXE" --prepare` hidden and synchronous, after
  the files are copied. MSI: deferred, `msidbCustomActionTypeNoImpersonate`, sequenced before
  `InstallFinalize`. Progress text: "Preparing movies and sounds...". It takes 1 to 2 minutes,
  and a non-zero exit code should fail the install with a clear message.
- Uninstall also removes `[INSTALLDIR]movies\`, `sounds\`, `movies.manifest` (RemoveFile, or the
  InstallAware *Delete Files* command) and `[LocalAppDataFolder]Rugrats Adventure Game\Cache`.
  It then asks before removing `[AppDataFolder]Rugrats Adventure Game` (saved games;
  default: keep).

**Shortcuts and registry:** as in section 2.

**Output:**
- InstallAware: a single `RugratsAdventureGame_Setup.exe` with the web/compressed runtime
  disabled.
- Wise: `RugratsAdventureGame.msi`, plus the Wise `setup.exe` bootstrapper if you want an .exe
  front end.

**Optional "Save games in the game folder" checkbox** (off by default): when ticked, the installer
writes an empty `portable` file into `[INSTALLDIR]`. To avoid a UAC prompt on every launch from
Program Files, the installer can also grant the Users group Modify rights on `[INSTALLDIR]`
(MSI `LockPermissions` / `MsiLockPermissionsEx` table, or InstallAware *Set Folder Permissions*).
Without that, the game asks for admin rights each time it starts.

Also ship a plain **portable .zip** (exe, DLLs and docs; the user copies `MHK\` and `PROGRAM\`
in). Don't use the disc's Broderbund `DIALOG.BMP` or `SETUP\steinst.bmp` in the wizard: they are
the rights holder's artwork.

### Minimum OS check (Windows 10 version 1607 or later)

The bundled FFmpeg (`avcodec-63.dll`) imports `SetThreadDescription`, which Windows added in
Windows 10 version 1607 (build 14393). On 1507 and 1511 the DLL won't load, so that is the floor.

Don't use `VersionNT >= 1000`: Windows Installer reports Windows 10 and 11 as **VersionNT 603**,
the same as 8.1. Check the build number instead, which also excludes 8.1 (build 9600):

- **AppSearch / RegLocator (64-bit registry):** `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`,
  value `CurrentBuildNumber`, property `WINBUILD`.
- **Launch condition:** `Installed OR WINBUILD >= 14393`.
- Keep Wise's "Windows NT Version: Vista 64-bit" entry as the x64-only gate (`VersionNT64`).
- For a later Vista/7 build (with an older FFmpeg), relax it to `VersionNT64 >= 600`.

### Updates with WiseUpdate

WiseUpdate (the *WiseUpdate* page in Installation Expert) installs a small client,
`WiseUpdate Client` (WUClient), with a Start Menu shortcut "Check for Updates". It downloads
an **update information file** from a URL you set at build time, compares versions, then
downloads and runs the newer installer that the file points to.

- **Hosting.** Any plain file URL works. Use one that doesn't redirect and won't change:
  - GitHub Pages: `https://<user>.github.io/<repo>/wiseupdate.txt`
  - or raw: `https://raw.githubusercontent.com/<user>/<repo>/main/wiseupdate.txt`

  Point the download entry at the GitHub Releases asset or the archive.org file.
  - GitHub release downloads and archive.org redirect to their storage servers. The client
    uses the Windows internet stack (WinINet), which follows redirects and gets TLS 1.2 from
    Windows 10 itself.
  - Still, test the "Check for Updates" shortcut once against the real URLs: the update
    file, the Releases asset and archive.org. A 2000s-era client may not follow every redirect.
  - Use `https://` everywhere, since GitHub and archive.org no longer serve plain HTTP.
- **The update installer must be a proper upgrade:**
  - Same UpgradeCode, a higher ProductVersion (1.0.1, ...) and a new ProductCode (a major
    upgrade, Upgrade table with `RemoveExistingProducts`).
  - Watch out for the game data copied from the disc. During a major upgrade, the old
    product's RemoveFile rows would delete `MHK\`, `PROGRAM\` and the prepared movies, and the
    new install would ask for the CD again. To avoid that:
    - Delete the game data with a custom action conditioned on
      `REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE`, instead of RemoveFile rows.
    - Find existing data with an AppSearch FileSearch for `[INSTALLDIR]MHK\EPISODE0.MHK`
      (property `GAMEDATAFOUND`), and skip the disc page and the copy and `--prepare` steps
      when it is set.
  - Then a bug-fix update only replaces `RUGRATS.EXE` and the DLLs, with no disc and no
    re-preparing.
- **Make the update a small installer.** It doesn't need to be wrapped in the SFX, or it can
  be. The user's saves are untouched either way: they live in `%APPDATA%` (or the game
  folder's `saves\`, which no RemoveFile row targets).

## 3b. Outer self-extractor (SFX)

The released `setup.exe` is a self-extracting wrapper. It unpacks the InstallAware or Wise output
to a temporary folder, runs it, waits, then cleans up. The payload is only about 150 MB (the
game data comes from the user's disc), so any of the options below can carry it.

**Rules that apply to any wrapper:**

1. **Run the wrapper with the highest privileges** (WinRAR: *Advanced > Request administrative
   access*; 7-Zip SFX / NSIS: a `requireAdministrator` manifest). Then there is a single UAC
   prompt at launch, and the inner installer inherits the elevation without asking again. The
   game's saves aren't affected: they go by the user's own folders, or the game folder with
   `--portable`, never by VirtualStore. The one exception: if a standard user types *another*
   account's admin password, the "Launch the game" checkbox at the end starts the game as that
   account, so use the installer's "run as the original user" launch option where it has one, or leave that
   checkbox unticked by default.
2. **Wait for the inner installer and pass its exit code through**, then delete the
   temporary folder. Don't delete while the installer is still running: MSI chains and the
   `--prepare` step take a few minutes.
3. **Make repair and uninstall work without the wrapper.**
   - Wise / MSI: Windows caches the `.msi` without its embedded cabinets. A repair, or a
     "modify" that needs files, then asks for the original source, which is the deleted temp
     folder. Either extract to a **persistent** folder (`%ProgramData%\Rugrats Adventure Game\Setup`, removed by the MSI's uninstall) or keep the files uncompressed and
     accept a "please insert / browse to setup" prompt on repair.
   - InstallAware keeps its own cached copy, so temp extraction is fine.
4. **Brand the stub like the installer.** Set the icon (`res\rugrats_port.ico`), the version info
   (CompanyName T.M. DEVELOPMENTS, ProductName, FileDescription "Rugrats Adventure Game
   (Unofficial Port) Setup", version 1.0.0.0) and the window title. Edit the stub's resources
   first, **then sign the finished SFX last**: anything appended or edited after signing breaks
   the signature.
5. **No extra prompts.** The installer's welcome page already carries the disclaimer, so the
   wrapper should show only an "Extracting..." progress bar: no "extract to?" dialog and no
   second license.

**Wrapper options:**

| Wrapper | How to set it up | Notes |
|---|---|---|
| **7-Zip SFX** (`7zSD.sfx` "installer" module, LZMA SDK / 7-Zip Extra) | `copy /b 7zSD.sfx + config.txt + payload.7z setup.exe`. Example config below | Free (LGPL), small stub, good LZMA2 compression, extracts to `%TEMP%`, waits and deletes. Edit the stub's icon and version info *before* concatenating, then sign. |
| **WinRAR SFX** (GUI or `Default64.SFX` module) | SFX options: *Temporary mode*, *Unpack to temporary folder*, *Hide all*, Setup = `setup.exe`, *Wait and return*, custom icon and logo bitmap | The most polished wrapper (logo banner, custom icon, no visible paths). The free trial can build SFX archives too (it only shows license reminders). Add version info with a resource editor and sign last. |
| **IExpress** (built into Windows) | *Extract files and run an installation command*, Install program = `setup.exe`, *Hidden* prompt, no license, no restart | Free and already on every PC. Older-looking UI, cabinet compression, 2 GB cap. Sign the output. |
| **NSIS** (as a wrapper only) | `SetOutPath $PLUGINSDIR`, `File /r payload\*`, `ExecWait '"$PLUGINSDIR\setup.exe"' $0`, `SetErrorLevel $0`, `SilentInstall silent` + a Banner plugin | Full control over icon, version info, UAC (`RequestExecutionLevel user`) and exit code. More scripting. |
| **Inno Setup** (as a wrapper only) | `[Files]` to `{tmp}`, `[Run]` with `waituntilterminated`, all wizard pages disabled | Works, but it is a full installer wrapping another full installer. |

7-Zip SFX config (UTF-8; first and last lines exactly as shown):

```
;!@Install@!UTF-8!
Title="Rugrats Adventure Game (Unofficial Port) Setup"
Progress="yes"
ExecuteFile="setup.exe"
;!@InstallEnd@!
```

For a Wise MSI without its own `setup.exe` bootstrapper, use `ExecuteFile="msiexec.exe"` and
`ExecuteParameters="/i \"RugratsAdventureGame.msi\""` instead.

## 4. Minimum specs

| | Port (this build) | Original (1998) |
|---|---|---|
| OS | Windows 10/11 64-bit | Windows 95/98/NT 4 |
| CPU | Any x86-64 | Pentium 100 MHz |
| RAM | 2 GB system | 16 MB |
| Display | Any resolution of 640x480 or more; any color depth | 640x480, 256 colors |
| GPU | Direct3D 9 / OpenGL 2.1 (falls back to software rendering) | DirectDraw (DirectX 5) |
| Sound | Any (silent without a device) | 16-bit, 22 kHz |
| Disk | About 1.1 GB (the disc is needed only during setup) | 50 MB plus the CD |
| Runtimes | None (UCRT is built into Windows 10+) | QuickTime 3, DirectX 5 |

### Retro PCs

- **Windows 7 SP1 / 8.1 x64.** SDL2 2.30 supports them. Today's FFmpeg master DLLs import
  `bcryptprimitives.dll` (ProcessPrng) and the UCRT, so they're best-effort only. Use an FFmpeg
  7.1 LGPL build compiled for Windows 7 (`_WIN32_WINNT=0x0601`) and update KB2999226 (UCRT). The
  port's own code uses nothing newer than Windows 7, except the PerMonitorV2 DPI manifest entry,
  which older systems ignore.
- **Windows XP x64 / Vista.** Needs a separate build. See the end of this file.

## 5. Other platforms

**macOS and Linux (native).** Feasible, with no gameplay changes. The engine, script VM,
decoders, audio and asset preparation are portable C++17 on SDL2 and FFmpeg (the Windows-only code is already
kept under `#ifdef _WIN32`). The Windows-only
parts that need equivalents are:

- The Win32 menu bar (File / Options). Replace it with SDL-drawn menus, or the native macOS
  menu bar through a small Objective-C++ file.
- The crash handler and minidump. Use signals and backtraces, or Breakpad / Crashpad.
- The About box. `SDL_ShowSimpleMessageBox` is already portable.
- Paths. `SDL_GetPrefPath` already gives `~/Library/Application Support` and
  `~/.local/share`. The "LOCALAPPDATA" cache becomes `~/Library/Caches` or `~/.cache`.
- Exclusive full screen. Prefer borderless "desktop" full screen on macOS and Wayland.

Risks:

- Case-sensitive file systems. The disc's names are UPPERCASE and the scripts refer to them in
  mixed case, so lookups must be case-insensitive.
- Audio device latency differences. SDL hides most of them.

Packaging: an `.app` bundle (notarisation needs an Apple developer ID) and an AppImage or
Flatpak.

**Windows XP x64 (lowest priority).** Needs:

1. An XP-capable toolchain. mingw-w64 GCC targeting `_WIN32_WINNT=0x0502`, linked against
   `msvcrt.dll` (not UCRT), with the `win32` thread model or winpthreads, and with no
   `std::filesystem` calls that use newer APIs. The port currently uses `std::filesystem`, which
   needs checking on XP.
2. SDL2 **2.0.x up to 2.0.22** or a community XP fork. Official SDL2 dropped XP after 2.0.x.
   Its XP support relies on Direct3D 9.
3. An FFmpeg built for XP. FFmpeg 4.x or older with `--target-os=mingw32`, without bcrypt and
   without UCRT. Only the Cinepak, SMC, ADPCM and PCM decoders and the mov demuxer are needed.
   That is a small LGPL build.
4. Drop the PerMonitorV2 manifest entry (XP ignores it, but test it). Check that
   `MiniDumpWriteDump` (dbghelp 6.x on XP) and `SHGetKnownFolderPath`-style calls, if any, have
   XP fallbacks.
5. Package it with Wise for Windows Installer, targeting Windows Installer 3.1 (the MSI version
   built into XP x64 SP2). Leave out the Windows 7-only properties (`MSIINSTALLPERUSER`).

Expected result: the same game, built as a separate "legacy" exe.
