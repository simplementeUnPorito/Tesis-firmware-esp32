#!/usr/bin/env python3
"""Reconexión automática al AP del maestro (GeoNetwork) tras reprogramarlo.

Reprogramar el ESP maestro tira el WiFi/AP y Windows suele quedarse colgado de
otra red. Este script deja la PC de vuelta en GeoNetwork y verifica que el
maestro responda, sin pasos manuales:

  1. Solicita el perfil guardado GeoNetwork mediante WlanConnect por ``ctypes``,
     sin escanear SSIDs (``dot11_BSS_type_any=3``).
  2. Reintenta mientras el maestro vuelve a levantar el AP y Windows obtiene
     conectividad.
  3. Verifica HTTP contra http://192.168.4.1/health hasta que conteste.

Uso:
    python reconnect_geonetwork.py            # espera hasta 120 s en total
    python reconnect_geonetwork.py --timeout 300
    python reconnect_geonetwork.py --ssid OtraRed --host 192.168.4.1

Pensado para encadenar con el flasheo:
    pio run -e esp32dev -t upload && python reconnect_geonetwork.py

Código de salida 0 si el maestro respondió /health, 1 si no.
"""
import argparse
import ctypes
import subprocess
import sys
import time
import urllib.request
from ctypes import wintypes


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, shell=False)


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class WLAN_INTERFACE_INFO(ctypes.Structure):
    _fields_ = [
        ("InterfaceGuid", GUID),
        ("strInterfaceDescription", wintypes.WCHAR * 256),
        ("isState", wintypes.DWORD),
    ]


class WLAN_INTERFACE_INFO_LIST(ctypes.Structure):
    _fields_ = [
        ("dwNumberOfItems", wintypes.DWORD),
        ("dwIndex", wintypes.DWORD),
        ("InterfaceInfo", WLAN_INTERFACE_INFO * 1),
    ]


class WLAN_CONNECTION_PARAMETERS(ctypes.Structure):
    _fields_ = [
        ("wlanConnectionMode", wintypes.DWORD),
        ("strProfile", wintypes.LPCWSTR),
        ("pDot11Ssid", wintypes.LPVOID),
        ("pDesiredBssidList", wintypes.LPVOID),
        ("dot11BssType", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
    ]


def connect_profile_native(profile):
    """Conecta un perfil guardado sin escanear SSIDs.

    Windows 11 bloquea ``netsh wlan show networks/connect`` cuando el acceso a
    ubicación está deshabilitado. WlanConnect no necesita enumerar redes y es
    justamente la operación que necesitamos para un perfil conocido.
    """
    if sys.platform != "win32":
        result = sh(["netsh", "wlan", "connect", f"name={profile}"])
        return result.returncode == 0, result.stderr.strip() or result.stdout.strip()

    wlan = ctypes.WinDLL("wlanapi.dll", use_last_error=True)
    handle = wintypes.HANDLE()
    negotiated = wintypes.DWORD()
    interfaces = ctypes.POINTER(WLAN_INTERFACE_INFO_LIST)()

    wlan.WlanOpenHandle.argtypes = [
        wintypes.DWORD, wintypes.LPVOID,
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.HANDLE),
    ]
    wlan.WlanOpenHandle.restype = wintypes.DWORD
    wlan.WlanEnumInterfaces.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID,
        ctypes.POINTER(ctypes.POINTER(WLAN_INTERFACE_INFO_LIST)),
    ]
    wlan.WlanEnumInterfaces.restype = wintypes.DWORD
    wlan.WlanConnect.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(GUID),
        ctypes.POINTER(WLAN_CONNECTION_PARAMETERS), wintypes.LPVOID,
    ]
    wlan.WlanConnect.restype = wintypes.DWORD
    wlan.WlanFreeMemory.argtypes = [wintypes.LPVOID]
    wlan.WlanCloseHandle.argtypes = [wintypes.HANDLE, wintypes.LPVOID]

    rc = wlan.WlanOpenHandle(2, None, ctypes.byref(negotiated), ctypes.byref(handle))
    if rc:
        return False, f"WlanOpenHandle error={rc}"

    errors = []
    try:
        rc = wlan.WlanEnumInterfaces(handle, None, ctypes.byref(interfaces))
        if rc:
            return False, f"WlanEnumInterfaces error={rc}"
        if not interfaces or interfaces.contents.dwNumberOfItems == 0:
            return False, "no hay interfaces WLAN"

        base = ctypes.addressof(interfaces.contents.InterfaceInfo)
        params = WLAN_CONNECTION_PARAMETERS(
            0,       # wlan_connection_mode_profile
            profile,
            None,
            None,
            3,       # dot11_BSS_type_any
            0,
        )
        for index in range(interfaces.contents.dwNumberOfItems):
            info = ctypes.cast(
                base + index * ctypes.sizeof(WLAN_INTERFACE_INFO),
                ctypes.POINTER(WLAN_INTERFACE_INFO),
            ).contents
            rc = wlan.WlanConnect(
                handle, ctypes.byref(info.InterfaceGuid), ctypes.byref(params), None
            )
            if rc == 0:
                return True, info.strInterfaceDescription
            errors.append(f"{info.strInterfaceDescription}: error={rc}")
        return False, "; ".join(errors)
    finally:
        if interfaces:
            wlan.WlanFreeMemory(interfaces)
        wlan.WlanCloseHandle(handle, None)


def connected_ssid():
    out = sh(["netsh", "wlan", "show", "interfaces"]).stdout
    for line in out.splitlines():
        stripped = line.strip()
        # "SSID : GeoNetwork" (evitar BSSID)
        if stripped.startswith("SSID") and "BSSID" not in stripped and ":" in stripped:
            return stripped.split(":", 1)[1].strip()
    return None


def health_ok(host, timeout_s=3):
    try:
        with urllib.request.urlopen(f"http://{host}/health", timeout=timeout_s) as r:
            body = r.read().decode("utf-8", "replace")
            lines = {line.strip() for line in body.splitlines()}
            return r.status == 200 and "ok" in lines and "littlefs=ok" in lines
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ssid", default="GeoNetwork")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="tiempo total máximo en segundos (default 120)")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    print(f"[reconnect] objetivo: SSID={args.ssid} host={args.host} "
          f"timeout={args.timeout:.0f}s")

    last_connect_try = 0.0
    connect_announced = False
    while time.time() < deadline:
        if health_ok(args.host):
            print(f"[reconnect] OK: conectado a {args.ssid}; "
                  f"http://{args.host}/health responde con littlefs=ok")
            return 0

        if time.time() - last_connect_try > 8:
            last_connect_try = time.time()
            ok, detail = connect_profile_native(args.ssid)
            if ok:
                if not connect_announced:
                    print(f"[reconnect] WlanConnect({args.ssid}) solicitado "
                          f"en {detail}; esperando DHCP/health…")
                    connect_announced = True
            else:
                print(f"[reconnect] no se pudo solicitar conexión: {detail}")
        time.sleep(2)

    print(f"[reconnect] FALLÓ: no se pudo volver a {args.ssid} con /health OK "
          f"en {args.timeout:.0f}s", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
