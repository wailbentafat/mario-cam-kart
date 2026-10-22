<h1 align="center">DS64</h1>

<p align="center">
  <b>Play Mario Kart DS with your hands or your phone.</b><br>
  A <a href="https://github.com/melonDS-emu/melonDS">melonDS</a> fork with webcam gesture steering, a phone touch controller, and up to 4-player local multiplayer in one window.
</p>

<p align="center">
  <a href="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-ubuntu.yml"><img src="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-ubuntu.yml/badge.svg" alt="Ubuntu build"></a>
  <a href="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-macos.yml"><img src="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-macos.yml/badge.svg" alt="macOS build"></a>
  <a href="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-windows.yml"><img src="https://github.com/wailbentafat/mario-cam-kart/actions/workflows/build-windows.yml/badge.svg" alt="Windows build"></a>
  <a href="https://github.com/wailbentafat/mario-cam-kart/releases"><img src="https://img.shields.io/github/v/release/wailbentafat/mario-cam-kart?include_prereleases" alt="Latest release"></a>
  <a href="vendor/melonDS-src/LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="License: GPL-3.0"></a>
</p>

---

## Table of contents

- [Features](#features)
- [How it works](#how-it-works)
- [Install](#install)
- [Build from source](#build-from-source)
- [Playing](#playing)
- [Controls](#controls)
- [Project layout](#project-layout)
- [Known limitations](#known-limitations)
- [Contributing](#contributing)
- [License and legal](#license-and-legal)
- [Acknowledgements](#acknowledgements)

## Features

- **Camera mode.** Hold your hands as if gripping a wheel and tilt them to
  steer. Make a fist to accelerate, open your palm to brake, and stick a thumb
  out to use an item. Hand tracking runs on [MediaPipe](https://github.com/google-ai-edge/mediapipe)
  inside the emulator process.
- **Phone mode.** Scan the QR code shown in the emulator window to open a
  touch controller in your phone's browser. You pick a steering style when
  you connect: a draggable wheel, L/R buttons, or tilting the phone (gyroscope).
- **Local multiplayer, 2 to 4 players.** Each extra player gets their own
  game instance, embedded as a panel in the same window. The instances talk
  to each other through melonDS's local wireless emulation, so every player
  drives their own kart in a real race.
- **No helper apps.** Camera tracking and the phone WebSocket server are
  built into melonDS through embedded CPython. There are no focus tricks,
  synthetic key presses, or separate bridge processes.
- **Keyboard still works.** Any real key press or click takes control back
  right away.
- **Cross-platform.** Linux (AppImage), Windows and macOS (Intel and Apple
  Silicon), all built and smoke-tested on every push by GitHub Actions.

## How it works

```
 ┌──────────── melonDS (DS64) ────────────┐
 │                                        │
 │  GestureController ──► embedded Python ─┼─► webcam → MediaPipe hands
 │         │                              │
 │         ▼                              │
 │   EmuInstance key mask / touch input   │
 │         ▲                              │
 │  PhoneController  ──► embedded Python ─┼─► WebSocket :8765+N ◄── phone browser
 │                                        │        ▲
 │  built-in HTTP server :8080 ───────────┼────────┘ serves web/controller.html
 └────────────────────────────────────────┘
```

- `GestureController` and `PhoneController` (C++, in
  `vendor/melonDS-src/src/frontend/qt_sdl/`) host the Python scripts in
  `vendor/melonDS-src/tools/gesture/` and `tools/phone/` through the CPython
  C API. They write the result straight into the emulator's input state.
- In phone mode, player *N* connects on WebSocket port `8765 + N - 1`. The
  controller page is served on port `8080`, and `tools/make_controller_qr.py`
  generates a QR code with your LAN address for each player.
- In multiplayer, player 1's window embeds every other instance's screen
  panel, so you get split-screen in a single window.

## Install

### Prebuilt releases

Download the build for your platform from the
[Releases page](https://github.com/wailbentafat/mario-cam-kart/releases).
Each one is self-contained: it bundles Python, the camera and phone scripts,
and the controller page, so nothing else needs to be installed.

| Platform | Asset | How to run |
|---|---|---|
| Linux x86_64 | `DS64-x86_64.AppImage` | `chmod +x DS64-x86_64.AppImage && ./DS64-x86_64.AppImage` |
| Windows x86_64 | `DS64-windows-x86_64.zip` | Extract it and run `melonDS.exe`. Keep the folder intact; `python/`, `tools/` and `web/` must stay next to the exe. |
| macOS Apple Silicon | `DS64-macos-arm64.zip` | Unzip and open `melonDS.app` |
| macOS Intel | `DS64-macos-x86_64.zip` | Unzip and open `melonDS.app` |

> **macOS Gatekeeper:** builds are ad-hoc signed but not notarized. If macOS
> says the app is damaged or from an unidentified developer, right-click it
> and choose **Open**, or run:
>
> ```bash
> xattr -dr com.apple.quarantine /path/to/melonDS.app
> ```

The Linux AppImage needs glibc 2.35 or newer (Ubuntu 22.04, Fedora 36, or
anything more recent).

## Build from source

You'll need **Python 3** (3.12 recommended, since camera mode needs
`mediapipe==0.10.14`, which has no wheels for 3.13 or later) and a
**Mario Kart DS (USA) `.nds` ROM** dumped from a cartridge you own. Put the
ROM in the repository root.

```bash
git clone https://github.com/wailbentafat/mario-cam-kart.git
cd mario-cam-kart
```

| OS | Command |
|---|---|
| Linux / macOS | `./play.sh` |
| Windows | `play.bat` (double-click it or run it from a terminal) |

The first run takes a few minutes. `play.sh` and `play.bat` find or install
Python 3, then hand off to `play.py`, which:

1. creates a local virtualenv in `.venv/` and installs `requirements.txt`
   (your system Python is left alone),
2. installs melonDS's C++ build dependencies (CMake, Qt 6, SDL2, …) with
   Homebrew on macOS or `apt` on Debian/Ubuntu. On Fedora, Arch and other
   distros, install them yourself first using the commands in `BUILD.md`.
   On Windows it prints the
   one-time MSYS2 setup steps instead; see
   [`vendor/melonDS-src/BUILD.md`](vendor/melonDS-src/BUILD.md),
3. builds melonDS if it isn't built yet,
4. generates the QR codes, launches the emulator, serves the controller
   page, and stops everything together when you press Ctrl+C.

Later runs start in seconds.

<details>
<summary><b>Building the release packages yourself</b></summary>

The CI workflows in [`.github/workflows/`](.github/workflows) are the
reference build steps. To build the Linux AppImage locally (in Docker, from
the repo root):

```bash
docker run --rm -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) -v "$PWD":/src:ro -v "$PWD/dist":/out -v "$HOME/.cache/ds64-appimage":/cache ubuntu:22.04 bash /src/packaging/linux/build-appimage.sh
```

The AppImage lands in `dist/`. Windows and macOS bundles are put together by
`packaging/windows/make-bundle.ps1` and `packaging/macos/make-bundle.sh`, and
`packaging/smoke_test.py` checks any of the three packages.

The CMake option `-DDS64_EMBED_ROM=ON` (with optional
`-DDS64_EMBED_ROM_FILE=/path/to/game.nds`) compiles a ROM into the executable
for a personal single-file build.

</details>

## Playing

1. Start DS64. A **DS64 Setup** dialog asks you to choose **Camera** or
   **Phone**, and for Phone, how many players (1–4).
2. **Camera:** sit where the webcam can see both hands and grab an imaginary
   wheel.
3. **Phone:** make sure the phone is on the same Wi-Fi network as the
   computer. Scan the QR code in the window, pick a steering style, and
   drive. With 2 or more players, each player's QR code appears inside their
   own panel.
4. The **⟲ RESTART SETUP** button in the bottom-left corner takes you back to
   the mode picker without closing anything. When running from source,
   Ctrl+C in the terminal shuts everything down.

> Phones can't connect? Check that your firewall allows incoming connections
> on TCP `8080` and `8765`–`8768`, and that the phone isn't on a guest
> network that isolates clients.

For scripted launches, `DS64_AUTO_MODE=camera|phone` and
`DS64_AUTO_PLAYERS=1..4` skip the setup dialog.

## Controls

| Action | Camera (gesture) | Phone |
|---|---|---|
| Steer | Tilt both hands left/right | Drag the wheel, hold L/R, or tilt the phone |
| Accelerate | Fist | **ACCEL** |
| Brake | Open palm | **BRAKE** |
| Use item | Thumb out (either hand) | **L · ITEM** |
| Drift / powerslide | — | **R · DRIFT** |

Real keyboard or mouse input always wins. Gesture control takes over again
once both hands are back in view, and phone control once a phone reconnects.

## Project layout

```
.
├── vendor/melonDS-src/        # the melonDS fork (tracked directly, not a submodule)
│   ├── src/frontend/qt_sdl/   #   DS64 code: GestureController, PhoneController,
│   │                          #   setup dialog, single-window split-screen
│   └── tools/{gesture,phone}/ #   embedded Python: hand tracking, phone WebSocket bridge
├── web/controller.html        # phone controller page
├── tools/                     # QR code + LAN IP helpers
├── packaging/                 # AppImage / Windows / macOS bundling + smoke test
├── server/, vendor/*.py       # earlier standalone prototype (not used by the emulator build)
├── play.py, play.sh, play.bat # source-checkout launcher
├── requirements.txt           # Python deps installed into .venv by play.py
└── .github/workflows/         # CI builds and tag-triggered releases
```

## Known limitations

- **macOS gets the most testing.** Linux and Windows build and pass smoke
  tests in CI on every push, but they've seen less real play. Bug reports are
  very welcome.
- **Gesture steering is digital.** Hand tilt maps to left/right with a dead
  zone, not an analog stick, because most desktop OSes have no unprivileged
  virtual-gamepad API.
- **Multiplayer runs one emulator instance per player.** Mario Kart DS has
  no shared-console split-screen, so each player needs their own instance,
  and CPU use goes up with player count. Multiplayer panels use software
  rendering.
- **Camera mode needs Python ≤ 3.12** in source builds (a `mediapipe`
  constraint). Phone mode works on newer Pythons.

## Contributing

Issues and pull requests are welcome.

1. Fork the repo and create a branch from `master`.
2. Make your change. Please keep the existing code style; the melonDS
   upstream style guide is in
   [`vendor/melonDS-src/CONTRIBUTING.md`](vendor/melonDS-src/CONTRIBUTING.md).
3. Make sure the CI workflows (Ubuntu, macOS, Windows) pass on your PR.
   Each one builds the package and runs `packaging/smoke_test.py`.
4. Describe what you changed and how you tested it, ideally on real
   hardware: webcam, phone, and number of players.

To cut a release, push a `v*` tag. [`release.yml`](.github/workflows/release.yml)
builds every platform and publishes a GitHub Release. Tags containing a `-`
(e.g. `v0.2.0-rc1`) produce a private draft pre-release for testing.

## License and legal

DS64 is a fork of melonDS and is distributed under the
**[GNU General Public License v3.0](vendor/melonDS-src/LICENSE)**, like
upstream melonDS. Any redistribution or modified version must also be GPL-3.0
and ship its source code.

DS64 is an independent fan project. It is **not affiliated with, endorsed
by, or sponsored by Nintendo**. *Nintendo DS* and *Mario Kart* are
trademarks of Nintendo. The game is © Nintendo. Only play games you own.

## Acknowledgements

- [melonDS](https://github.com/melonDS-emu/melonDS) by Arisotura and
  contributors: the emulator all of this is built on.
- [MediaPipe](https://github.com/google-ai-edge/mediapipe) for hand tracking,
  and [OpenCV](https://opencv.org/) for camera capture.
- [websockets](https://github.com/python-websockets/websockets) and
  [python-qrcode](https://github.com/lincolnloop/python-qrcode).
- [python-build-standalone](https://github.com/astral-sh/python-build-standalone),
  [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and
  [appimagetool](https://github.com/AppImage/appimagetool) for the Linux
  packaging.
