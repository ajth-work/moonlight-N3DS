# GUI23.3 physical 3DS test handoff

Status: build and publish in progress; physical-console verification is pending.

## Build under test

- Version: `3.6.0-gui23.3`
- Source branch: `feature/gui23.3-log-browser-clarity`
- Exact source commit and CIA filename, size, and SHA-256: see the GUI23.3 `human-testing` comment on Moonlight N3DS issue #29.

## Check Logs browser

1. Open **Check Logs** and confirm **All**, **Host**, and **Stream** tabs are shown. Use D-pad left/right to change tabs and up/down to move between rows.
2. Confirm rows are newest first, each row has a filled status dot, and the details text explains its state: no accepted upload recorded, last attempt failed, or accepted by Sunshine.
3. Open **More Details** and confirm the full filename and session/upload IDs are available there. Compare **Local now**, **Sent source**, **Filtered log**, and **HTTP body** sizes; these measure different stages.
4. If the console has a Luma3DS `ntp_tz_offset_min` setting, confirm timestamps use that fixed UTC offset. Without a configured offset, timestamps are explicitly marked UTC. The 3DS does not automatically apply daylight-saving changes, so compare the displayed offset and relative age with the actual time.

## Upload result messages and retained copy

5. Send a saved log to the paired Sunshine host. An accepted transfer should show **UPLOAD ACCEPTED** and name Sunshine as the receiver. A failed transfer should show **LOG NOT SENT**, an actionable next step, and confirmation that the SD-card copy remains available.
6. In Check Logs, confirm the local log remains and its status dot/details match the latest recorded outcome. On Sunshine, look for the matching `Saved paired client diagnostics` receipt and compare its request byte count with **HTTP body**.

## Results AJ should send back

- Version shown on the 3DS:
- All/Host/Stream navigation and newest-first order:
- Timestamp and relative-age display:
- Status-dot colors and matching text:
- Successful upload message and matching Sunshine receipt:
- Failed-upload message/local-copy behavior (if a failure can be observed safely):
- Local/source/filtered/request byte values and Sunshine receipt comparison:
- Any screen freeze, clipped text, or other notes/photos:
