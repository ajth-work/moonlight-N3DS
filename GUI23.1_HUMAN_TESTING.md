# GUI23.1 physical 3DS test

Status: release build complete; physical console acceptance is pending.

## Artifact identity

- Version: `3.6.0-gui23.1`
- App: Moonlight GamePad
- CIA: `moonlight-gamepad-3.6.0-gui23.1.cia` — 4,405,184 bytes, SHA-256 `7D0BD169CF40292A002586CA4DD45D3176D03CB81ED2786EF5A90BD5F231974B`
- 3DSX test file: `moonlight.3dsx` — 7,951,820 bytes, SHA-256 `7B3E8A5F8AA4155DCB0723E5F0F85EC606972E51D6202E42E6641E2AFA9A0D85`
- Title ID: `0x00040000004D1100` (same Moonlight GamePad title as GUI23; CIA install replaces it)
- Home Menu banner: `3.6.0-gui23.1`, stamped below the Moonlight wordmark using the existing banner template.
- Source branch: `feature/gui23.1-diagnostics`

GUI23.1 corrects the malformed diagnostic-upload metadata and adds input/renderer telemetry for the reported 3D presentation problem. It does not claim the top-screen stereo presentation is fixed; that behavior remains part of this human test.

## Presentation check

1. Start a live stream and open the 3D presentation page.
2. Raise the physical 3D slider and cycle Mode until the panel reads HSBS.
3. Check whether the top image changes and whether the two eyes show the expected left/right halves. Record the exact mode, slider position, aspect, and shift.
4. Cycle Aspect and Shift while the stream is moving. Note whether the top image updates and whether both screens remain responsive.
5. If either screen freezes, record whether touch/buttons still respond and whether the stream can be exited through the panel.

## Upload check

1. End the stream normally and observe the upload result.
2. In `SD:/3ds/moonlight/moonlight.log`, look for `DIAG_UPLOAD_ATTEMPT` with HTTP status and bounded server-response summary. In the session file, check the final `DIAG_UPLOAD` state.
3. Success is HTTP 200 with `response=saved`, `DIAG_UPLOAD state=accepted`, and a Sunshine `Saved paired client diagnostics` receipt.
4. If upload fails, attach `SD:/3ds/moonlight/sessions/session-<id>.log` and `SD:/3ds/moonlight/moonlight.log`. The logs remain on SD even when transfer fails.

## Human results

- Presentation: pending
- Stream freeze / exit behavior: pending
- Upload status and Sunshine receipt: pending
- Notes / photos: pending