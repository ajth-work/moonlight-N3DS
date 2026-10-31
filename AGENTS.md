## AJ fork: coordinated Moonlight and Sunshine test releases

For changes intended for AJ's paired Moonlight N3DS and Sunshine forks, do not call the result ready for 3DS testing until the matching client and host are identified and the active Sunshine runtime is verified.

- Before building, record each repository's branch, commit, and dirty paths. Preserve unrelated local changes and build artifacts.
- Ensure UniStore has the exact Moonlight CIA paired with the Sunshine build. When the Moonlight artifact changed, build and verify the fresh CIA (including packaged title/version and SHA-256), then publish through the existing `C:\code\3ds-homebrew\relay\tools\publish_relay.py` workflow. Review the live/local manifests and preserved routes first; verify the published feed entry and artifact bytes after publishing. If only Sunshine changed, verify that the existing UniStore CIA is the intended compatible client rather than republishing identical bytes.
- Before the test handoff, stop the prior Sunshine instance, launch Sunshine from the newly built/extracted package, and verify there is exactly one `sunshine.exe` process and its executable path and version/hash match the intended package. Do not infer this from the build completing or from a tray icon. If a stale instance remains or the active executable cannot be verified, report the handoff as not ready.
- Keep the Sunshine and Moonlight build identifiers paired in the report. Include source commits, artifact hashes, UniStore revision/route verification, and the running Sunshine process path/count. Mark physical 3DS acceptance as unverified until AJ confirms the device test.
- Keep release publication and runtime replacement scoped to these AJ forks. Preserve relay tokens and other credentials; never print or include them in logs, commands, or reports.

## Canonical Moonlight and Sunshine diagnostic logs

Read the diagnostic-log sections in `README.md` before troubleshooting. Use the paths below as the canonical locations, but resolve `<Sunshine appdata>` from the actual running Sunshine instance and its `log_path`; package mode and configuration can change the location.

- Sunshine runtime log: `<Sunshine appdata>/sunshine.log`; inspect rotated `sunshine.log.*` files for earlier sessions.
- Moonlight uploads received by Sunshine: `<Sunshine appdata>/moonlight-client-logs/<paired-client-id>/<upload-id>.json`. Use Sunshine's `Saved paired client diagnostics ... scope=host|stream ... session=... bytes=... truncated=... path=...` line as receipt evidence; correlate the upload's session IDs and timestamps with the runtime log. A Moonlight UI message alone is not proof of receipt.
- Moonlight's local SD-card logs, if upload is absent or incomplete: `sdmc:/3ds/moonlight/moonlight.log` and the relevant snapshots under `sdmc:/3ds/moonlight/sessions/`.
- If these host paths are outside the current agent's accessible workspace, request retrieval from the authorized host-side Codex/Bridge agent. Do not assume filesystem access, invent a path, or ask the user to reattach logs before checking the received-upload location.
- Preserve log files. Search only the relevant session window and avoid exposing credentials, pairing material, or unrelated personal data in summaries.

## Moonlight testing guidance

Before changing stream diagnostics, uploads, display selection, or teardown, inspect the existing tests and state which behavior each test actually exercises. A source-text assertion is a useful wiring tripwire, but it is not a behavioral test of runtime ordering, values, failures, or transport outcomes.

Observed test entry points on `feature/launch-display-select`:

- `python -m unittest discover -s tests -p "test_stream_diagnostics.py"` runs `tests/test_stream_diagnostics.py`. Its current checks cover selected diagnostic field/stage strings; treat them as source-level checks, not proof of runtime behavior.
- `bash tests/run_public_key_pin_tests.sh` runs the public-key pin tests in `tests/test_public_key_pin.cpp`; it requires Bash, GCC/G++, and OpenSSL. It covers pin/consent persistence, not diagnostic upload delivery or Sunshine receipt.
- `tests/test_displayinfo_xml.c` covers XML/status parsing. No repository runner was identified for it during the coverage audit; verify or add a documented runner before claiming it is part of automated test coverage.
- The audit found no behavioral test for diagnostic upload responses/receipt, shutdown ordering and timeout outcomes, or selected display ID propagation into launch. Recheck the current tree before relying on this list.

For new behavioral tests, prefer deterministic clocks, fake transports, and lifecycle recorders over real sleeps or network access. Assert success, rejection, timeout, cancellation, and cleanup paths where applicable. Keep display-list parsing tests separate from host integration evidence that a game actually opens on the selected display. Reports must name commands run and distinguish unit tests, host integration, and physical 3DS evidence. Do not describe an unrun test as passing.

## Moonlight diagnostics and testing milestones

Update these checkboxes only when the corresponding evidence has been reviewed:

- [x] Implement the Moonlight session diagnostics work and review its Builder report. Public UniStore delivery was not verified in that report; verify it separately before claiming public availability.
- [x] Audit existing Moonlight test coverage and record this repository guidance.
- [ ] Review a real 3DS session with matching Moonlight and Sunshine logs and record what the evidence explains.
- [ ] Queue a separate Sunshine logging/test job only if the paired logs leave host-side questions unanswered.
- [ ] Select one evidence-backed fix, add a focused regression test, and verify the appropriate host/device behavior.

Keep each Bridge job scoped to one repository, wait for the queue to drain, and stop for review before starting a follow-up. Physical device acceptance remains distinct from unit-test and host-integration results.