"""
Generates one QR code per possible player slot (1-4), each encoding the
phone controller page URL with that player's own WebSocket port baked in
(?ws=...), so scanning skips typing the address entirely.

melonDS's native PhoneController listens directly on these ports (one
per EmuInstance in a multi-phone session) — no external bridge process
involved. Each instance's own QR overlay (see Window.cpp) loads its file
by a predictable path: <system temp dir>/ds64_qr_p<N>.png — the same
directory Qt's QDir::tempPath() resolves to on Windows/Linux/macOS, so
this and Window.cpp always agree without either side hardcoding an
OS-specific path like "/tmp" (which doesn't exist on Windows).

Usage: python tools/make_controller_qr.py [web_port] [base_ws_port]
"""

import os
import sys
import tempfile
from pathlib import Path

import qrcode

from lan_ip import lan_ip

MAX_PLAYERS = 4
REPO_ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    web_port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    base_ws_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765

    ip = lan_ip()
    if ip == "127.0.0.1":
        print("WARNING: no LAN address found — phones won't be able to connect. "
              "Join a Wi-Fi network or set DS64_HOST=<this computer's IP>.")
    tmp_dir = Path(tempfile.gettempdir())

    for i in range(MAX_PLAYERS):
        player_num = i + 1
        ws_port = base_ws_port + i
        ws_addr = f"ws://{ip}:{ws_port}"
        page_url = f"http://{ip}:{web_port}/controller.html?ws={ws_addr}"

        img = qrcode.make(page_url, border=2)
        out_path = tmp_dir / f"ds64_qr_p{player_num}.png"
        img.save(str(out_path))
        print(f"Player {player_num}: {page_url} -> {out_path}")

    # Keep a copy of player 1's QR in web/ for scan.html / manual sharing.
    # Release bundles are signed/read-only and serving the page does not need
    # this file, so only write it for source/play.py runs. The per-player
    # copies above (in the temp dir) are what the in-game overlay loads.
    if not os.environ.get("DS64_BUNDLE_DIR"):
        try:
            qrcode.make(
                f"http://{ip}:{web_port}/controller.html?ws=ws://{ip}:{base_ws_port}",
                border=2,
            ).save(str(REPO_ROOT / "web" / "qr.png"))
            print(f"Scan page: http://{ip}:{web_port}/scan.html")
        except OSError:
            pass


if __name__ == "__main__":
    main()
