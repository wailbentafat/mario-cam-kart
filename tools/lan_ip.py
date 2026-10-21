"""
Picks the IPv4 address a phone on the same Wi-Fi can actually reach —
shared by play.py and tools/make_controller_qr.py so the QR codes and the
printed URL always agree.

The old approach ("which local address would route to 8.8.8.8?") returns
whatever interface carries the default route: a VPN/Tailscale tunnel
address the phone can't reach, and it raised outright on a LAN with no
internet route. Instead, enumerate interfaces and prefer a private LAN
address on a physical-looking adapter.

Override with DS64_HOST=<ip> when the guess is wrong.
"""

import ipaddress
import os
import socket

import psutil

# Virtual adapters a phone can't reach us on (VPNs, containers, VMs,
# bridges). Short Unix-style names are matched as prefixes only — as a
# substring "lo" would also drop "wlo1", a common Linux Wi-Fi name — and
# Windows' descriptive names ("vEthernet (WSL)") as substrings.
_VIRTUAL_IFACE_PREFIXES = (
    "lo", "docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tun", "tap",
    "utun", "wg", "tailscale", "zt", "ppp", "ipsec", "awdl", "llw", "bridge",
    "cni", "flannel", "kube",
)
_VIRTUAL_IFACE_SUBSTRINGS = (
    "virtualbox", "vmware", "vethernet", "hyper-v", "tailscale", "wireguard",
    "vpn", "loopback",
)


def _is_virtual(iface: str) -> bool:
    name = iface.lower()
    return name.startswith(_VIRTUAL_IFACE_PREFIXES) or any(s in name for s in _VIRTUAL_IFACE_SUBSTRINGS)

_CGNAT = ipaddress.ip_network("100.64.0.0/10")  # Tailscale and carrier NAT


def _default_route_ip() -> str | None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))  # UDP: no packet is sent
        return s.getsockname()[0]
    except OSError:
        return None  # no default route (offline LAN / hotspot without data)
    finally:
        s.close()


def candidates() -> list[tuple[str, str]]:
    """(interface, ip) pairs a phone could plausibly reach, best first."""
    stats = psutil.net_if_stats()
    found = []
    for iface, addrs in psutil.net_if_addrs().items():
        if iface in stats and not stats[iface].isup:
            continue
        if _is_virtual(iface):
            continue
        for a in addrs:
            if a.family != socket.AF_INET:
                continue
            ip = ipaddress.ip_address(a.address)
            if ip.is_loopback or ip.is_link_local or ip in _CGNAT or not ip.is_private:
                continue
            found.append((iface, a.address))

    route_ip = _default_route_ip()

    def rank(entry):
        ip = entry[1]
        return (
            ip != route_ip,               # the default-route NIC, if it qualified
            not ip.startswith("192.168."),  # then typical home Wi-Fi ranges
            not ip.startswith("10."),
        )

    return sorted(found, key=rank)


def lan_ip() -> str:
    override = os.environ.get("DS64_HOST", "").strip()
    if override:
        return override
    found = candidates()
    if found:
        return found[0][1]
    return "127.0.0.1"  # nothing reachable — callers warn about this


if __name__ == "__main__":
    print("chosen:", lan_ip())
    for iface, ip in candidates():
        print(f"  candidate {ip} ({iface})")
