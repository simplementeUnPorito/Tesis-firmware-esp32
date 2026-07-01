# Master Web UI field-test checklist

This feature can be built locally, but final validation needs the ESP32 master,
slaves, and a phone connected to the master's `GeoNetwork` access point.

## Flash

1. Confirm the board flash size if the hardware batch changes.
2. Build firmware:
   `pio run`
3. Upload firmware:
   `pio run -t upload`
4. Upload LittleFS web assets:
   `pio run -t uploadfs`

## Smoke test

1. Connect the phone to `GeoNetwork` with password `geophone2026`.
2. Open `http://192.168.4.1/`.
3. Confirm the page loads and WebSocket status becomes connected.
4. Press `STATUS` and confirm READY/status packets appear in the log.

## Control path

1. Run ARM with the expected slave count.
2. Verify READY count and per-slave HELLO/MAC/fs information.
3. Change PGA, PGAvdac, and VDAC from each slave panel.
4. Confirm ACKs and lock indicators match the desktop GUI behavior.
5. Run Test, Ver, and latency probe for each slave.

## Capture path

1. Run START with a short record length.
2. Confirm live raw and filtered traces update.
3. Try FIR presets (`lp 200`, `hp 10`, `bp 10 400`, `bs 45 55`), Remove DC,
   and 50 Hz harmonic notch.
4. Compare the same captured data against the desktop GUI for numerical parity.

## Export path

1. Download a ZIP from the phone.
2. Convert on a PC:
   `python src/python/geophone_scope/zip_to_mat.py capture.zip`
3. Load the `.mat` and compare shared fields against a desktop GUI export:
   raw/filt arrays, fs, FIR command, DC/notch settings, PGA/VDAC state, MAC,
   drift/latency histories.

## RF-risk check

Run an A/B spectrum comparison before trusting field data:

1. Capture with no phone connected.
2. Capture with the phone connected but idle.
3. Capture with the phone connected and actively streaming the web UI.
4. Compare spectra for new spurious peaks beyond the known AP/beacon artifacts.

Keep the existing RF-quiet rule intact: no deliberate web traffic should be
needed during PRESTART/capture; tolerate AP/WebSocket activity only in phases
where the firmware already resumes AP beacons for dump/idle/control.

## Session log — 2026-06-08 live verification (bypass mode, hardware-in-loop)

Found and fixed two real bugs via CDP-driven live testing (Brave + raw WebSocket
CDP client from PowerShell), reflashing `uploadfs` and polling `/health` between
each fix:

1. **`config.js:43-49` — `ReferenceError: FS is not defined`**: stale references
   to the removed nominal-`FS` constant crashed ES module evaluation of
   `config.js` (first import of `app.js`), aborting the entire script — this was
   the root cause of "WS: desconectado" / empty UI / no panels. Fixed by
   replacing `DISP_SAMP = FS * 3` / `MAX_BUF = FS * MAX_BUF_S` with fixed
   placeholder constants (3000 / 30000) — they're only initial sizes;
   `syncDataBufferForFs()`/`applyDisplayWindow()` resize for real once HELLO
   reports the true Fs.

2. **`plot.js` `drawCurve` — filtered curve drawn `trimSamp` samples too far
   right**: x-position used `(xStartSamp + i)/fs` instead of
   `(xStartSamp + i - trimSamp)/fs`, double-counting the FIR's own group delay
   on top of the discard offset (net misalignment ≈ `N-1` samples instead of 0).
   This was the actual cause of the raw/filtered desync seen live (NOT a
   `(N-1)/2` vs `N-1` discard-formula issue — the discard formula `(N-1)/2` is
   correct; only the *draw* indexing was wrong). Fixed; live capture of a
   hammer-strike transient now shows raw/filtered overlapping peak-for-peak,
   zero-crossing for zero-crossing, with zero visible offset (see
   `align_spike_closeup2.png` from that session).

Also live-verified (already-implemented features, all OK): show-raw/show-filt
checkboxes (curve+legend+autoscale), click-to-zoom (left=in 1.5x anchored at
click frac, right/shift=out, correctly clamped to `zoom∈[1,128]`), and
always-CSV-in-export (downloaded a real ZIP — every node, including
disconnected ones, has `raw.csv`/`filt.csv` with `time_s` computed from the
real reported Fs, no opt-in checkbox).

### Known remaining bug — filtered curve tail clipped when zoomed

`PlotArea.update()` slices `rawTail`/`filtTail` from the *same* absolute index
range `[start, end)`, but `drawCurve` then drops the filtered curve's leading
`trimSamp` samples and re-indexes — so its last drawn point lands at
`(end - 1 - trimSamp)/fs`, i.e. `trimSamp` samples short of the raw curve's
right edge `(end - 1)/fs`. The filtered trace visibly stops short of the
window's right edge whenever zoomed in enough to notice.

Fix direction: extend the filtered slice's right bound by `filtTrim` —
`filt.subarray(min(start, len), min(end + filtTrim, len))` — so that after
discarding the leading transient, what remains still spans the same time range
as `rawTail`. Implemented and live-tested in the follow-up log below.

## Session log — 2026-06-08 Codex follow-up

Implemented and live-tested the remaining web UI fixes:

1. **Filtered zoom tail clipping fixed**: `PlotArea.update()` now extends the
   filtered slice right bound by `filtTrim` before handing it to `drawCurve()`.
   `ChannelPlot` also auto-ranges only the actually drawn filtered samples,
   avoiding bad Y ranges when raw is hidden and the filtered buffer is shorter
   than the FIR startup trim. Hardware-in-loop check: applied `numtaps 101 lp
   10`, captured 52 batches / 1560 samples, zoomed/panned into the middle of
   the buffer, and pixel-checked `C:\tmp\plot_zoom_tail_check.png`: raw reached
   2 px from the plot right edge and filtered reached 12 px from the edge
   (well inside the old ~50-sample/large-pixel clipped gap).

2. **Layout redesign completed**: first viewport now prioritizes larger plots;
   configuration moved into a 340 px right-side tab panel (`Captura`, `Nodos`,
   `Esclavos`, `Export`, `Log`). Mobile breakpoint was checked at 390x844:
   one-column layout, no plot/sidebar overlap, WS connected, Fs = 3000 Hz,
   browser console clean.

3. **ZIP folder structure reworked**: data files are exported only for connected
   visible slaves and their folders use the slave type/alias (`hammer/`, `geo1/`,
   etc.) instead of numeric `nodeN/`. Master/global metadata is available under
   `maestro/metadata.json` and `maestro/config.json`; root `metadata.json` is
   kept for `zip_to_mat.py` compatibility. Synthetic export test confirmed one
   connected Hammer node produced only `hammer/raw_f32le.bin`,
   `hammer/filt_f32le.bin`, `hammer/raw.csv`, `hammer/filt.csv`, while a visible
   but disconnected Geo1 stayed in metadata with no data folder.

Verification performed after `uploadfs`:

- `pio run` succeeded (RAM 14.5%, Flash 66.0%).
- `pio run -t uploadfs` succeeded on COM8 and flashed LittleFS.
- `/health` returned `ok`, `littlefs=ok`, `used=159744`, `total=1441792`.
- Browser UI loaded `/js/app.js?v=field-loop-2` and `/css/style.css?v=field-loop-2`
  with no console errors/warnings; WS connected and showed Fs = 3000 Hz.
- Tabs switched correctly; ARM n=1 rendered one large plot.
- USB sniffer via PlatformIO Python (`pyserial 3.5`) saw HEARTBEAT/READY/STATUS,
  slave 1 `psoc_ok=True`, Fs = 3000 Hz, MAC parts, state ARMED.

## Session log — 2026-06-30 Codex handoff continuation

Goal: continue Claude's handoff and finish the GEO/HAMMER auto-detection path,
without reprogramming the physical PSoC as Hammer. Physical hardware available:
master ESP32 on COM8, geophone slave ESP32 on COM12, PSoC via KitProg, PC on
`GeoNetwork`.

### Implemented

1. **PSoC-driven slave type in HELLO**
   - PSoC already exposes `PSOC_HW_CLASS` through `PSOC_EVT_BOOT`.
   - Slave ESP32 now forwards that value as `MsgHello.hw_class`.
   - Master ESP32 accepts both old 5-byte HELLO and new HELLO with `hw_class`.
   - Master WebSocket relay sends subtype `0x06` for slave hardware type:
     `0 = GEO`, `1 = HAMMER`, `0xFF = unknown`.
   - Web UI applies the reported type automatically. Hammer forces offset 0 and
     hides the "Offset m" control; geophones are labeled GeoN.

2. **Dummy Hammer from PC**
   - Added HTTP simulation endpoint in the master: `/sim/hello`.
   - Added script:
     `src/esp/Nodo comunicación/simulate_hammer_dummy.py`.
   - Test command:
     `python "src/esp/Nodo comunicación/simulate_hammer_dummy.py" --host 192.168.4.1 --node 1 --type hammer --fs 2929 --psoc 1`
   - Result:
     `ok`, `node=1`, `type=HAMMER`, `fs=2929`, `mac=DE:AD:BE:EF:00:01`.
   - This simulates web telemetry for UI validation. It is not a real ESP-NOW
     slave and does not participate in capture timing.

3. **Visible slave handling**
   - Fixed the web UI so `READY n_slaves=1` with physical S2 connected does not
     show a phantom S1 placeholder.
   - Visible slaves are now based on presence signals such as MAC, PSoC state,
     reported hardware class, exact Fs, or received samples.

4. **Hammer offset visibility**
   - `slave_panel.js` now calls `_syncTypeLayout()` when alias/type is set from
     firmware.
   - The Hammer offset row uses both `hidden=true` and `display:none`; `.row`
     CSS had been forcing flex layout and could leave the row occupying space.
   - Verified in browser before the final `field-study-3` upload: Hammer panel
     had `offsetRowDisplay="none"` and `rowRect=0x0`; Geo1 kept `Offset m`
     visible.

5. **FIR status before Fs**
   - Recompiling a saved FIR before HELLO/Fs no longer reports permanent
     `Invalid: fs must be positive`.
   - The panel now waits for Fs and shows a pending state instead.

6. **GeoN ordering by offset**
   - `reorderGeosByOffset()` now sorts only present geophone nodes, saves the
     resulting alias, renders affected rows, and refreshes presentation order.
   - Offset input now dispatches on `input` and `change`.
   - A simulated S3 GEO revealed the old relabel did not fire reliably from UI
     input; code was adjusted in `field-study-3`. Final visual retest was not
     repeated because the in-app browser blocked reload/navigation to
     `192.168.4.1`; direct HTTP/WS tests below were used instead.

### Build and flash checks

- `pio run -e esp32dev` in `master`: success after changes.
  - RAM: 15.4%, Flash: 68.1%.
- `pio run -e slave2` in `slave`: success after changes.
  - RAM: 13.5%, Flash: 57.0%.
- Master LittleFS upload on COM8: success.
  - Served HTML contains `field-study-3`.
  - `/health` after reconnecting to `GeoNetwork`: `ok`, `ap_ip=192.168.4.1`,
    `littlefs=ok`, `used=237568`, `total=1441792`.
- Earlier in the same continuation:
  - Master firmware upload on COM8 succeeded.
  - Slave2 firmware upload on COM12 succeeded.
  - User later recompiled/reconnected the master after a reset/connectivity
    hiccup.

### Live verification

1. **Physical S2 GEO type from PSoC**
   - Web/WS logs showed:
     - `HELLO slave=2 psoc_ok=true fs=2900Hz`
     - `HELLO slave=2 fs_exact=2929Hz`
     - `HELLO slave=2 tipo=GEO`
     - `HELLO slave=2 MAC=C8:2E:18:68:5F:6C`
   - This confirms the physical PSoC on COM12 is programmed as GEO and the type
     is now propagated through slave -> master -> web.

2. **Physical capture through direct WebSocket**
   - Commands sent to `ws://192.168.4.1/ws`:
     - `{"cmd":"A5","param":0}` STATUS
     - `{"cmd":"A2","param":1}` ARM one slave
     - `{"cmd":"A3","value":293}` START about 3 seconds at 2929 Hz
   - Result:
     - READY sequence included `n=1`.
     - ACKs: `cmd=0xA2 val=0`, `cmd=0xA3 val=1`.
     - HELLO repeated `node=2 psoc=True`, `fsExact=2929`, `type=GEO`.
     - Latency packet: `node=2 4653us`.
     - Data received: node 2 had exactly `8790` samples.
     - First sample raw int24: `10747`; last sample raw int24: `9012`.

3. **Dummy Hammer**
   - `/sim/hello` with node 1 Hammer returned `ok`.
   - UI before the final reload block showed `Hammer (S1)` and `Geo1 (S2)`.
   - Hammer `Offset m` row was confirmed hidden after the `display:none` fix.

4. **Existing controls**
   - `Sin offset DC` checkbox toggled on/off.
   - `Espectro` checkbox replaced time plot and returned to time plot.
   - Per-node `Offset Y (mV)` and `Invertir señal` controls toggled without
     breaking capture; final waveform-level numerical verification is still
     useful when doing a real hammer hit.

5. **S2 calibration result**
   - Directed WS command sent:
     `{"cmd":"BD","node":2,"sub":"B5","param":1}`.
   - S2 stayed connected and continued reporting `psoc=True`, `fsExact=2929`,
     `type=GEO`.
   - Calibration progress ACKs received:
     `2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 2, 2, 5, 6`.
   - Final ACK: `node=2 cmd=0xB5 val=0`.
   - Conclusion: the red calibration dot is a real calibration failure on S2,
     not just an 8-second UI timeout. Next fix should add the per-stage
     calibration result packet/table so the failing stage has measured DAC,
     measured voltage, target, and error.

### Errors and notes kept from Claude's handoff

- **COM12 serial monitor silent**: accepted as expected when boot logs are
  missed and PSoC calibration logs are disabled. The slave has no periodic
  human-readable output unless data/diagnostics flow.
- **Calibration red dot**: reproduced on S2. The command reached progress
  stages through value `6` and ended with final `val=0`. Need per-stage
  calibration result telemetry to see which stage/target failed.
- **Browser reload blocked**: after the final `field-study-3` upload, the
  Codex in-app browser rejected reloading `192.168.4.1` by policy. Direct HTTP
  and direct WebSocket tests succeeded, so hardware/network were good.
- **Connectivity hiccup after reset**: PC kept IP `192.168.4.2` but no ARP/ping
  to `192.168.4.1`; user recompiled/reconnected to `GeoNetwork`, after which
  `/health` and WS tests succeeded.
