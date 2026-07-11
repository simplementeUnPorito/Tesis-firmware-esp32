#!/usr/bin/env python3
"""Reconexión automática al AP del maestro (GeoNetwork) tras reprogramarlo.

Reprogramar el ESP maestro tira el WiFi/AP y Windows suele quedarse colgado de
otra red. Este script deja la PC de vuelta en GeoNetwork y verifica que el
maestro responda, sin pasos manuales:

  1. Espera a que el SSID vuelva a ser visible (el maestro tarda unos segundos
     en levantar el AP después del flasheo).
  2. `netsh wlan connect name=GeoNetwork` (requiere el perfil ya guardado, que
     esta PC tiene).
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
import subprocess
import sys
import time
import urllib.request


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, shell=False)


def ssid_visible(ssid):
    out = sh(["netsh", "wlan", "show", "networks"]).stdout
    return ssid.lower() in out.lower()


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
            return r.status == 200
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
    while time.time() < deadline:
        if connected_ssid() == args.ssid:
            if health_ok(args.host):
                print(f"[reconnect] OK: conectado a {args.ssid} y "
                      f"http://{args.host}/health responde")
                return 0
            print("[reconnect] conectado al AP, esperando /health…")
            time.sleep(2)
            continue

        if not ssid_visible(args.ssid):
            print(f"[reconnect] {args.ssid} aún no visible, esperando…")
            time.sleep(3)
            continue

        if time.time() - last_connect_try > 8:
            last_connect_try = time.time()
            print(f"[reconnect] SSID visible → netsh wlan connect {args.ssid}")
            r = sh(["netsh", "wlan", "connect", f"name={args.ssid}"])
            if r.returncode != 0:
                print(f"[reconnect] netsh: {r.stdout.strip()} {r.stderr.strip()}")
        time.sleep(2)

    print(f"[reconnect] FALLÓ: no se pudo volver a {args.ssid} con /health OK "
          f"en {args.timeout:.0f}s", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
