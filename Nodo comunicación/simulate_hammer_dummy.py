#!/usr/bin/env python3
"""Inject a simulated Hammer HELLO into the master's web UI.

Run this from a PC connected to the GeoNetwork AP. It does not emulate ESP-NOW;
it calls the master's /sim/hello debug endpoint so the browser can verify the
Hammer UI path without reprogramming a physical GEO slave.
"""

from __future__ import annotations

import argparse
import urllib.parse
import urllib.request


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--node", type=int, default=1)
    parser.add_argument("--type", choices=("hammer", "geo"), default="hammer")
    parser.add_argument("--fs", type=int, default=2929)
    parser.add_argument("--psoc", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()

    query = urllib.parse.urlencode(
        {
            "node": args.node,
            "type": args.type,
            "fs": args.fs,
            "psoc": args.psoc,
        }
    )
    url = f"http://{args.host}/sim/hello?{query}"
    with urllib.request.urlopen(url, timeout=5) as response:
        print(response.read().decode("utf-8", errors="replace"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
