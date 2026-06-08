# Handoff: Master Web UI — WebSocket relay session report

Branch: `feature/master-web-ui`. This document is a complete handoff of the work
done in this session on the ESP32 "master" web UI's WebSocket relay layer —
context, what was fixed and confirmed, what remains open, and how to keep
testing safely. Written so a fresh agent (or person) can continue without
re-deriving any of this.

---

## 1. System overview (context for everything below)

The ESP32 "master" node bridges three worlds:
- **MATLAB** over USB serial (existing, original interface)
- **ESP-NOW slaves** (geophone PSoC5 boards) over the 2.4GHz radio
- **An embedded web UI** (LittleFS + HTTP + WebSocket) served from the master's
  own WiFi access point `GeoNetwork` (`192.168.4.1`, password `geophone2026`)

The web UI is the newest addition (this branch). It mirrors the same 6-byte
binary packet protocol (`[0x56][node][type][b2][b1][b0]`) that
`MatlabTransport` already emits toward MATLAB — see `src/matlab_transport.h`
`_emit()` (line ~105), which writes to `Serial` AND calls a registered relay
callback. `web_relay.h` registers itself as that relay and mirrors every
6-byte packet to connected WS clients via `ws.binaryAll()`. Browser → master
commands are plain JSON text frames (`{"cmd":"A2","param":1}` etc.), decoded
back into `MatlabTransport::RxCmd` and pushed onto a small queue that `loop()`
drains exactly like it drains MATLAB commands — **one dispatcher, no duplicated
ARM/START/STOP/PGA/etc. logic**. This design choice (from an earlier session)
is good and should be preserved.

### Key files
- `src/main.cpp` — master firmware main logic (state machine, capture
  orchestration, beacon pause/resume, dump retries)
- `src/web_relay.h` — WS bridge: packet mirroring, JSON command decode,
  client-limit enforcement (max 1 concurrent WS client), `webRelayCloseAll()`
- `src/matlab_transport.h` — shared binary protocol encode/decode + `_emit`
- `data/js/app.js`, `data/js/config.js`, `data/js/protocol.js`,
  `data/js/signal_proc.js` — browser-side UI, command codes, FIR/notch DSP
  (all DSP — FIR filter, harmonic notch — is **pure client-side JS**, applied
  to already-received samples; there is no ESP command for it)
- `WEB_FIELD_TESTS.md` — pre-existing manual field-test checklist (phone +
  browser); still the right doc for in-browser/phone validation
- `ws_capture_test.py`, `ws_cmd_test.py`, `ws_probe.py` — WS-only Python test
  scripts written/used this session (stdlib-only, no external deps)

### State machine — `MasterState` enum (main.cpp)
```
IDLE=0, ARMING=1, ARMED=2, RUNNING=3, STOPPING=4, DUMPING=5, PRESTART=6, SCOPE_MULTI=7
```

### Command/sub-command codes (from `data/js/config.js`, mirrored in firmware)
```
CMD_HEADER       = 0xAB
CMD_DIRECTED     = 0xBD   // {"cmd":"BD","node":N,"sub":"XX","param":P} — routed to one slave
CMD_STREAM       = 0xA1
CMD_ARM          = 0xA2   // {"cmd":"A2","param":n}            -- arm n slaves
CMD_START        = 0xA3   // {"cmd":"A3","value":n}            -- start capture, 16-bit N
CMD_STOP         = 0xA4   // {"cmd":"A4","param":0}
CMD_STATUS       = 0xA5
CMD_DEBUG        = 0xA7
CMD_SET_RECLEN   = 0xAE   // {"cmd":"AE","value":n}            -- 16-bit record length (batches)
CMD_SCOPE_MULTI  = 0xB0

SUBCMD_PGA       = 0xA6   // directed: set PGA gain code
SUBCMD_PGAVDAC   = 0xA9   // directed: combined PGA+VDAC byte
SUBCMD_VDAC      = 0xAA   // directed: set VDAC byte
SUBCMD_DEBUG     = 0xA7
SUBCMD_VER       = 0xB2   // directed: trigger a "View"/scope capture on one slave
SUBCMD_LATENCY   = 0xAF   // directed: round-trip latency probe
```
Packet types seen on the wire (mirrored to WS as binary 6-byte frames):
```
0x00 DATA       0x01 HEARTBEAT (b0 = MasterState)   0x07 ACK (b2=cmd, b1:b0=value)
0xFC LATENCY    0xFD STATUS (multi-frame block)     0xFE READY (b0 = n_slaves)
```

### Hard limits / gotchas worth knowing
- `PSOC_CAPTURE_MAX_BATCHES = 512` — `clampRecordBatches()` (main.cpp ~240-246)
  silently clamps any `SET_RECLEN` above this. **Sending a larger value desyncs
  master/slave expectations and can leave the master stuck in `DUMPING`** (see
  §4 "Errors encountered" — happened once this session with `SET_RECLEN=1500`).
  Recovery: send `STOP` (`{"cmd":"A4","param":0}`) immediately followed by
  `SET_RECLEN=0` (`{"cmd":"AE","value":0}`) — clean `STOPPING→ARMED` in <1s.
- Dump retry constants: `DUMP_MAX_RETRIES=30`, `DUMP_NACK_RETRY_DELAY_MS=100`,
  `DUMP_BATCH_TIMEOUT_MS=300` → worst case ~12s per missing batch. A confused
  dump state can therefore hang for a very long time.
- **Heartbeat suppression**: `loop()` only emits heartbeats
  `if (!g_streaming && millis() - lastHbMs > 1000)`. Sending a directed VER
  command (`sub_cmd == 0xB2`) **synchronously** sets `g_state = RUNNING` AND
  `g_streaming = true` (main.cpp ~526-527). This is why no heartbeats appear
  during the ~9.5s "HOT_WAIT" handshake window after VER — it's intentional
  suppression, not a connectivity problem.

---

## 2. The bug that was fixed this session: "zombie connection" data loss

### Symptom (as found at the start of this session, inherited from a prior one)
`beacon_pause()` (called from `beginPrestart()` ~line 647 for
START/SCOPE_MULTI/START_PROBE, and from `onCfgAck` ~line 315 specifically when
a directed VER completes: `msg.sub_cmd == CMD_VIEW && msg.ok == 1 && g_state ==
RUNNING`) raises the AP `beacon_interval` from ~1024ms to 60000 TU (~61s) to
keep the radio RF-quiet during acquisition (this beacon-pause mechanism is
**pre-existing firmware behavior, not introduced by the web UI** — it protects
the geophone ADC from 10Hz RF spikes).

Problem: an already-connected WS client doesn't immediately notice its TCP
connection has gone dark — it takes ~9-10s of *passive TCP timeout* before the
browser/client realizes it's disconnected and starts reconnecting. During that
~9.6s "zombie" window (connection alive on the server, dead for the client),
**any packet emitted — including the entire dump of a short VER capture —
is silently lost.**

### Fix applied (completed this session)
`webRelayCloseAll()` (already stubbed in `web_relay.h` from a prior session,
just `ws.closeAll();`) is now wired into `beacon_pause()`:

```cpp
static void beacon_pause(void)
{
    /* Cerrar el cliente web ANTES de tocar el beacon: si esperamos a que el
     * teléfono note la asociación caída por su cuenta, tarda ~9-10 s (timeout
     * TCP pasivo) — tiempo de sobra para que una captura corta (VER) entera
     * se pierda en una conexión "zombie". Cerrar de entrada dispara su
     * reconexión ya mismo. */
    webRelayCloseAll();
    wifi_config_t ap_cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
    ap_cfg.ap.beacon_interval = 60000u;   /* max documentado TU ≈ 61 s */
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    MASTER_LOG_PRINTLN("[MASTER] beacon pausado (captura activa)");
}
```

This forces an immediate WS close, which makes the browser's own reconnect
logic (in `ws_client.js`, 1s backoff) fire right away instead of waiting out
the passive timeout.

### Build/flash (done this session, both succeeded first try)
```
pio run            → SUCCESS in 9.66s   (RAM 14.3% / Flash 65.9%)
pio run -t upload  → SUCCESS in 22.65s
```

### Confirmation — CONFIRMED FIXED AND WORKING
Two separate `ws_capture_test.py` runs (short VER capture, `SET_RECLEN=100`,
~1s) showed reconnect gaps of **0.02s and 0.04s**, vs. the prior ~9.6s zombie
window. This is a clear, measurable, reproducible improvement — **ship it.**

---

## 3. NEW finding this session: long-capture WiFi-association loss (UNRESOLVED, architectural)

This is the most important open item. It directly answers the user's standing
question *"el cambio a modo web no afecta en nada a la captura no?"*

**Short captures (≈1s, VER probes with small `SET_RECLEN`)**: the existing L2
WiFi association between the PC/phone and `GeoNetwork` survives the brief
beacon-pause window; the WS reconnects in the ~20-40ms range thanks to the fix
in §2. All good.

**Longer captures (tested with `SET_RECLEN=300` ≈ 30 seconds — i.e. close to
real measurement durations, which run up to ~120s)**: a materially worse
problem surfaces, *one layer below* anything the WS relay can fix:

Timeline observed:
- VER sent at t=4.6s
- WS disconnected at t=14.7s (`forcibly closed by remote host`)
- **Every single reconnect attempt then timed out** for the remaining ~170s
  of the test (`total connects=1, DATA pkts received=0`)
- `(Get-NetConnectionProfile).Name` showed `GeoNetwork` had **completely
  vanished** from Windows' visible/known network list — Windows had roamed to
  a different known network (`Flia. Martinez 5G`) instead
- `ping 192.168.4.1` → 100% packet loss / TimedOut
- Self-recovered after ~190 seconds total this time (no user action needed);
  in an earlier separate incident this session (see §5, the serial-port
  mistake) recovery required a **physical power-cycle** of the master

### Why this matters
This is **not** a WS/TCP-layer issue — it's the underlying WiFi radio
association itself dropping at L2. `beacon_pause()` keeps the AP beacon-quiet
long enough (60000 TU ≈ 61s, and it stays active for the whole capture+dump,
potentially well over a minute) that the PC's WiFi stack decides the AP is
gone and roams away. No amount of WS-layer cleverness (faster reconnects,
buffering, etc.) can fix a connection that doesn't exist at the radio level.

### What this means for the user's question
- **The acquisition itself remains completely safe** — RF-isolated on the
  slave's PSoC, no delays added, nothing about the web UI changes capture
  timing or data integrity. ✅
- **BUT the web UI will likely become fully disconnected during any real
  capture of meaningful length** (≳15-30s), and may take anywhere from a few
  minutes to a manual power-cycle to reconnect afterward. This is a serious
  caveat for *relying on the web UI to monitor a live capture* — though
  arguably it doesn't matter for the *measurement* itself, since data is
  captured and dumped server-side regardless of whether a client is watching.

### Possible directions to discuss with the user (not yet decided/explored)
- Shorten the beacon-pause duration or interval (tradeoff: more RF noise risk
  — would need an A/B spectrum comparison per the existing
  `WEB_FIELD_TESTS.md` "RF-risk check" section to validate any change)
- Periodic short beacon bursts during long captures, timed to avoid the
  geophone's sensitive sampling windows
- Accept this as a known limitation of the RF-quiet design and document it
  prominently in the UI ("you'll lose connection during capture — this is
  expected; your data is safe, reconnect after the capture completes")
- Investigate whether forcing the *same* AP channel/static behavior (vs.
  letting it idle-roam) reduces Windows' eagerness to abandon the association

**This has already been reported to the user in conversation** — they're aware
and the conversation is ongoing. No action needed to "break the news" again,
but any follow-up firmware change here needs their sign-off given the RF-risk
tradeoffs.

---

## 4. Errors encountered and how they were resolved (this session)

1. **`SET_RECLEN=1500` → silently clamped to 512 → stuck `DUMPING` state.**
   Sent during an early test iteration; the master hung with
   `total connects=1, DATA pkts received=0` for the full 180s test window, and
   remained in `DUMPING` for 45+ seconds after a `STOP`.
   **Fix**: `STOP` (`{"cmd":"A4","param":0}`) immediately followed by
   `SET_RECLEN=0` (`{"cmd":"AE","value":0}`) → clean `STOPPING→ARMED` in 0.2s.
   Lesson: **never send `SET_RECLEN` above 512** in test scripts.

2. **No compile/runtime errors at all.** `pio run` and `pio run -t upload`
   both succeeded cleanly on the first attempt.

---

## 5. CRITICAL — do not open the COM8 serial port from scripts

Mid-session, the user suggested adding COM8 serial monitoring alongside the WS
tests for better diagnostics (a reasonable-sounding idea). **This was tried
and it broke the device:**

- A script (`ws_serial_combo_test.py`, since deleted) opened `COM8` via
  `pyserial.Serial(...)`. The CP210x USB-UART chip's auto-reset circuit
  (DTR/RTS-driven) reset the ESP32, and it came up stuck in
  bootloader/download mode — **the AP stopped broadcasting entirely.**
- User confirmed alarm: *"Ya no veo que aparezca GeoNetwork entre las opciones
  de mi wifi, algo mataste"* / *"Igualmente no aparece geoNetwork visible
  aunque sae"*.
- Windows-side recovery attempts (`netsh wlan connect/disconnect`,
  `Set-Service`/registry edits for Location Services) all failed — blocked by
  admin-only `lfsvc` service / "Access is denied" on `HKLM:` paths.
- **Actual fix required physically power-cycling the master ESP32** (the user
  did this; confirmed recovered via `(Get-NetConnectionProfile).Name` showing
  `GeoNetwork` again and `ping 192.168.4.1` returning 3ms RTT / 0% loss).

**Researched afterward** (this session, see WebSearch results): there IS a
known mitigation for this class of bug — set `dtr=False`/`rts=False` *before*
calling `.open()` (what `esptool --before no_reset` does internally) — but
[pyserial issue #124](https://github.com/pyserial/pyserial/issues/124)
documents that **Windows + the CP210x driver toggles these lines at the OS
level the instant the port handle opens**, before pySerial's settings can take
effect. So the standard mitigation is specifically known to be unreliable on
exactly our Windows+CP210x combination — there's a real chance of repeating
this incident even being careful. The clean fixes are hardware-level (cut/cap
the EN-line auto-reset trace) — not something to attempt from a script.

**Rule going forward: WS-only diagnostics. Never open the serial port (COM8)
from any script or tool while the device is in the field / can't be easily
power-cycled.** If deep serial debugging is ever truly needed, do it on a
different OS (Linux, where the DTR/RTS mitigation is known to work) or in
person with the physical reset button held during port-open.

---

## 6. What's been tested and confirmed working (this session)

### `ws_cmd_test.py` (NEW — short command/ACK round-trip probe, 90s, zero captures)
Exercises every "button" that doesn't trigger `beacon_pause()`. Result:
**zero disconnects across 90 seconds**, all commands ACKed promptly:

| Command | Result |
|---|---|
| STATUS (0xA5) | ✅ relayed, full 4-frame status block |
| ARM n=1 (0xA2) | ✅ ACK @ 3.34s |
| PGA set, node 1 (directed 0xA6, param=3) | ✅ ACK @ 6.27s |
| VDAC set, node 1 (directed 0xAA, param=128) | ✅ ACK @ 8.28s |
| PGAVDAC set, node 1 (directed 0xA9, param=0x73) | ✅ ACK @ 10.27s |
| LATENCY probe, node 1 (directed 0xAF) | ✅ ACK @ 11.76s + LATENCY reading (raw 0x2FAE ≈ 12.2ms RTT) @ 11.78s |
| STATUS again | ✅ relayed correctly |
| STOP (0xA4) | ✅ ACK @ 16.29s, clean `STOPPING→ARMED` in **40ms** |

Heartbeats (`[state=ARMED]`) kept flowing at ~1Hz throughout — confirming
`g_streaming` stayed false and the connection was perfectly healthy and idle
between commands. **This is solid evidence the entire control-path command
surface (config, arm, directed slave commands, status, stop) works correctly
through the web relay with no edge cases.**

Note one curiosity (NOT a confirmed bug, just unexplained): the ACK "value"
field for directed commands (PGA/VDAC/PGAVDAC/LATENCY) showed values like
`256` that don't obviously match the param sent (e.g. PGA param=3 → ack
val=256). This might just mean the `(b1<<8)|b0` interpretation in the test
script's `describe()` doesn't match the real semantic meaning of directed-ACK
payloads (vs. standard-cmd ACKs like ARM/STOP which correctly showed `val=0`).
Worth a quick look at how `onCfgAck`/`sendAck` populate those bytes for
directed sub-commands if anyone wants to chase it — but it didn't block or
break anything; ACKs arrived correctly and promptly either way.

### `ws_capture_test.py` (from a prior session, reused/modified this session)
Used to confirm the zombie-connection fix (§2) and to discover the long-capture
WiFi-association-loss issue (§3). **Caution**: keep `SET_RECLEN` ≤ ~100 for
quick zombie-fix verification; values ≳150 risk triggering the long-outage
issue in §3, and values > 512 risk the stuck-DUMPING issue in §4.1.

### `ws_probe.py` (from a prior session)
Minimal idle-listener — connects, logs every frame + gaps, useful for raw
"is the link healthy when nothing is happening" checks. Untouched this
session.

### Confirmed NOT independently testable via WS (client-side only)
FIR filter and harmonic notch (`signal_proc.js`: `firFilter`, `harmonicNotch`,
`compileFirCmd`) are pure browser-side DSP applied to already-received sample
arrays — there's no ESP command for them. They can only be validated visually
in-browser with real signal data, per `WEB_FIELD_TESTS.md` §"Capture path"
(FIR presets `lp 200`, `hp 10`, `bp 10 400`, `bs 45 55`, Remove DC, 50Hz notch).
Same for "target V" (a VDAC-calculation helper, also client-side).

---

## 7. Recommended next steps (priority order)

1. **Discuss §3 (long-capture WiFi-association loss) with the user** and agree
   on a direction before making any further firmware changes — this is an
   architectural tradeoff (RF-quiet vs. connectivity) that needs their input,
   not another autonomous test cycle. Options are listed in §3.
2. **Do NOT run more long-capture WS tests** (`SET_RECLEN` > ~100) until a
   mitigation for §3 is in place or the user explicitly accepts the risk —
   each one risks another multi-minute outage that may need physical
   intervention.
3. **In-browser/phone field testing** (per `WEB_FIELD_TESTS.md`) is the
   logical next phase — FIR/notch, live trace rendering, export/ZIP→.mat
   round trip, and the RF-risk A/B spectrum check all require a real browser
   and can't be scripted from WS alone. The user said they want to do this
   themselves once I'm done (*"cuando estes dejame probar a mí"*).
4. If pursuing a §3 mitigation in firmware, remember: any change to
   `beacon_interval` / pause duration MUST be validated with the existing
   "RF-risk check" A/B spectrum comparison in `WEB_FIELD_TESTS.md` before
   trusting field data — the whole point of `beacon_pause()` is protecting
   measurement integrity, so don't regress that to fix connectivity.
5. **Never** open the COM8 serial port from any script (§5) — WS-only
   diagnostics from here on.

---

## 8. Quick-reference: how to run the test scripts

All are stdlib-only Python (no pip installs needed), run from
`src/esp/Nodo comunicación/master/`:

```
python ws_cmd_test.py       # 90s — safe, short commands only, no captures
python ws_probe.py          # 90s — idle listener, just watches the link
python ws_capture_test.py   # 180s — runs a VER capture; EDIT SET_RECLEN first!
                            #   ≤100  → safe zombie-fix check (~1s capture)
                            #   >150  → risks long WiFi outage (§3)
                            #   >512  → risks stuck DUMPING state (§4.1)
```
Pre-flight check: `ping -n 1 192.168.4.1` should show ~3ms RTT before running
anything. If it fails, check `(Get-NetConnectionProfile).Name` for
`GeoNetwork` — it may just be mid-recovery from a previous test (wait ~30-60s
and recheck before assuming something is broken).
