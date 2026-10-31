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
