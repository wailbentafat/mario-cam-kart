# DS64

A heavily modified native fork of [melonDS](https://melondsfork.net) that adds
webcam hand-gesture control and phone-based control for Mario Kart DS,
including real local multiplayer where each extra player drives their own
embedded game instance from their phone — all in a single window.

- **Camera mode**: tilt your hands like gripping a wheel to steer, make a
  fist to accelerate, open palm to brake, stick a thumb out to use an item.
- **Phone mode**: scan a QR code shown right in the emulator window to open a
  touch controller on your phone (steer by dragging a wheel, holding L/R
  buttons, or tilting the phone — your choice, picked on connect).
- **Local multiplayer (2-4 players)**: pick "Phone" and a player count at
  startup; every extra player's game instance is embedded as its own panel
  in the same window (not a separate OS window) and connects to the others
  over melonDS's own local wireless multiplayer emulation — real racing
  against each other, not shared/split input on one kart.

No window-focus hacks, no external bridge process — the camera tracking and
phone WebSocket server both run embedded directly inside melonDS itself.

## Quick start

1. **Get the ROM.** This repo tracks the USA Mario Kart DS `.nds` at its
   root; release builds bake it into the executable, so downloaded builds
   need no separate ROM file. If you build from source with a different
   dump, put that `.nds` in the repo root (or pass
   `-DDS64_EMBED_ROM_FILE=/path/to/game.nds` when configuring CMake).
2. **Run the launcher for your OS (source builds only):**

   | OS | Command |
   |---|---|
   | macOS / Linux | `./play.sh` |
   | Windows | `play.bat` (double-click, or run from a terminal) |

   That's it — same entrypoint on every platform. The first run will take a
   while (it builds melonDS from source and sets up a Python virtualenv with
   the camera/phone dependencies); after that, startup is fast.
3. melonDS opens with its own **"DS64 Setup"** dialog — pick **Camera** or
   **Phone**, and if Phone, how many players (1-4).
4. If you picked Phone: scan the QR code shown in the window with your
   phone's camera (same Wi-Fi network as this computer), pick a steering
   style, and play. For 2+ players, each additional player's phone shows its
   own QR code right inside their panel of the split window.
5. Press Ctrl+C in the terminal running `play.sh`/`play.bat` to stop
   everything together, or use the **"⟲ RESTART SETUP"** button drawn in the
   bottom-left of the game window to go back to the camera/phone picker
   without closing anything.

### What the launcher does for you

`play.sh` / `play.bat` are thin wrappers that just find (or install) a
system Python 3, then hand off to `play.py`, which:

- Creates a project-local Python virtualenv (`.venv`) and installs
  everything it needs into it — never touches your system Python.
- Checks for melonDS's C++ build dependencies (CMake, Qt6, SDL2, etc.) and
  installs them automatically via Homebrew (macOS) or `apt` (Linux). On
  Windows, if you don't already have a prebuilt binary and nothing's
  installed, it prints exact one-time setup steps (MSYS2) instead of
  attempting a silent multi-gigabyte unattended install — see
  `vendor/melonDS-src/BUILD.md`.
- Builds melonDS from source if it isn't already built.
- Pre-generates the QR codes, launches melonDS, serves the phone controller
  page, and cleans everything up together when you stop it.

Release binaries do all of that themselves: the C++ bootstrap in `main.cpp`
finds the Python runtime/scripts/web page packaged next to it (or inside the
macOS `.app`), sets up the same environment, generates the QR codes, and
serves the controller page. `play.py` is only needed for source builds.

## Install a prebuilt release (skip building from source)

Building from source the first time can take several minutes (longer on
Windows). If a [release](../../releases) is available for your platform,
download it instead:

1. Grab the `DS64-<platform>-<arch>` archive from the
   [Releases page](../../releases) and unzip it. The Mario Kart DS ROM,
   Python runtime, camera/phone scripts, and controller page are all inside;
   there's no separate `.nds`, virtualenv, or launcher to set up.
2. Launch the app directly:
   - **Windows:** open the extracted `DS64-windows-x86_64` folder and run
     `melonDS.exe`. Keep the folder intact — `python/`, `tools/`, and `web/`
     sit next to it and are what the executable loads.
   - **macOS:** drag `melonDS.app` wherever you like and double-click it. It
     carries `Python.framework` and its DS64 resources inside the bundle.
     Release builds are ad-hoc signed but not notarized, so Gatekeeper may
     say the app is "damaged" or from an unidentified developer. If macOS
     refuses the first launch, run
     `xattr -dr com.apple.quarantine /path/to/melonDS.app` (or right-click
     the app and choose **Open**).
   - **Linux:** make the AppImage executable (`chmod +x DS64-x86_64.AppImage`)
     and run it, or double-click it if your desktop supports AppImages.
3. `play.sh` / `play.bat` / `play.py` are not involved in release builds;
   they remain the source-checkout workflow above.

Releases are built by `.github/workflows/release.yml` on every version tag
(CI also runs the same build on every push to `master`, so build health is
checked continuously, not just at release time).

## Controls reference

| Action | Camera (gesture) | Phone |
|---|---|---|
| Steer | tilt both-hands grip left/right | drag wheel / hold L·R buttons / tilt phone |
| Accelerate | make a fist | ACCEL button |
| Brake | open palm | BRAKE button |
| Use item | stick a thumb out (either hand) | ITEM button (pulsing circle) |
| Drift / powerslide | — | DRIFT button |

Real keyboard/mouse input always takes priority — pressing any key or
clicking the screen immediately hands control back to you, and gesture/phone
control only reclaims it once both hands are visible again (camera) or a
phone reconnects.

## Known limitations

- **Windows/Linux are less battle-tested than macOS.** The native dialogs,
  split-screen embedding, and CI build steps are written correctly against
  documented platform APIs and melonDS's own `vendor/melonDS-src/BUILD.md`,
  but macOS is where this has actually been run and played on. If something
  breaks on your platform, check `.github/workflows/` CI run results first
  (same build steps you'd hit locally) — and issues/PRs are welcome.
- Mario Kart DS itself has no split-input mode — each player genuinely needs
  their own game instance talking to the others over local multiplayer, not
  one kart driven by multiple people. That's what the multi-phone embedded
  panels set up automatically.
- Gesture steering is based on hand tilt angle, not a true analog stick —
  there's no unprivileged virtual-gamepad API on most desktop OSes.

## Repo layout

- `vendor/melonDS-src/` — the melonDS fork itself (plain tracked files, not
  a submodule). The DS64-specific additions live mainly in
  `src/frontend/qt_sdl/` (`GestureController`, `PhoneController`, the DS64
  Setup dialog, and the single-window split-screen embedding) and
  `tools/gesture/` / `tools/phone/` (the embedded Python side).
- `web/controller.html` — the phone controller page.
- `play.py` / `play.sh` / `play.bat` — the cross-platform launcher.
- `requirements.txt` — Python dependencies `play.py` installs into `.venv`.
