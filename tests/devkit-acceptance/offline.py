"""Collect evidence of Windows network isolation before applying the Blueprint."""

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import socket
import subprocess


def _network_token():
    # GetCurrentProcessToken() is the pseudo-handle -4. No token is opened.
    token = wintypes.HANDLE(-4)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    app_container = wintypes.DWORD()
    size = wintypes.DWORD()
    if not security.GetTokenInformation(token, 29, ctypes.byref(app_container),
                                        ctypes.sizeof(app_container), ctypes.byref(size)):
        raise RuntimeError(f"GetTokenInformation(TokenIsAppContainer): {ctypes.get_last_error()}")
    capabilities = []
    if app_container.value:
        for kind, name in ((85, "internetClient"), (86, "internetClientServer"),
                           (87, "privateNetworkClientServer")):
            sid = ctypes.create_string_buffer(68)  # SECURITY_MAX_SID_SIZE
            size = wintypes.DWORD(len(sid))
            if not security.CreateWellKnownSid(kind, None, sid, ctypes.byref(size)):
                raise RuntimeError(f"CreateWellKnownSid({name}): {ctypes.get_last_error()}")
            enabled = wintypes.BOOL()
            if not kernel.CheckTokenCapability(token, sid, ctypes.byref(enabled)):
                raise RuntimeError(f"CheckTokenCapability({name}): {ctypes.get_last_error()}")
            if enabled.value:
                capabilities.append(name)
    return bool(app_container.value), capabilities


def verify_offline():
    """Return facts for the current process, whose brw children inherit its token."""
    if os.name != "nt":
        raise RuntimeError("Windows is required for devkit acceptance")
    container, capabilities = _network_token()
    evidence = {"appContainer": container, "networkCapabilities": capabilities, "tcpProbes": []}
    if not container or capabilities:
        powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        command = (
            "$ErrorActionPreference='Stop'; "
            "@([Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | "
            "Where-Object { $_.OperationalStatus -eq 'Up' -and "
            "$_.NetworkInterfaceType -ne 'Loopback' }).Count"
        )
        try:
            result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-Command", command],
                                    capture_output=True, text=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RuntimeError(f"Cannot inspect Windows network adapters: {error}") from error
        if result.returncode or result.stdout.strip() != "0":
            raise RuntimeError("Run with network adapters disconnected or in an "
                               "AppContainer without network capabilities")
        evidence["upNonLoopbackAdapters"] = 0

    for family, address in ((socket.AF_INET, "192.0.2.1"), (socket.AF_INET6, "2001:db8::1")):
        try:
            with socket.socket(family, socket.SOCK_STREAM) as probe:
                probe.settimeout(2)
                probe.connect((address, 443))
        except OSError as error:
            # Access denied, protocol family unavailable, no local address,
            # network down or unreachable. A timeout could come from a live route.
            code = getattr(error, "winerror", None)
            if code not in {10013, 10047, 10049, 10050, 10051}:
                raise RuntimeError(f"Offline TCP probe inconclusive at {address}: {error}") from error
            evidence["tcpProbes"].append({"address": address, "port": 443, "winError": code})
        else:
            raise RuntimeError(f"Offline TCP probe unexpectedly connected to {address}")
    return evidence
