# GUI23.2 physical 3DS test handoff

Status: implementation is prepared; build, install, and physical-console testing are pending.

## Diagnostic upload check

1. After an approved GUI23.2 build is installed, connect to the paired Sunshine host and start then end a short stream, or use Check Logs to send a saved session log.
2. In `SD:/3ds/moonlight/moonlight.log`, find the matching `DIAG_UPLOAD_ATTEMPT` line. Record `http_status`, `response_bytes`, `response_truncated`, and the sanitized `response` summary.
3. A successful transfer requires the matching Sunshine `Saved paired client diagnostics` receipt. Record its session ID, byte count, and `truncated` value. An HTTP status alone is not proof of host receipt.
4. If the upload fails, confirm the local session log remains available and report whether the UI returns normally. Share only logs with host addresses, pins, and other private values redacted.

## Human results to send AJ

- Build/version shown on device:
- Upload route used (automatic after stream / Check Logs):
- Client HTTP status, captured bytes, truncation flag, and response summary:
- Matching Sunshine receipt (or no receipt):
- Local log retained and UI responsive after failure:
- Notes / photos: