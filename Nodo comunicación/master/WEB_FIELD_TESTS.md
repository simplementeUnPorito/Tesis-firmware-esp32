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
