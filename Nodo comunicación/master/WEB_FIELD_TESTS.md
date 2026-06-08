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
