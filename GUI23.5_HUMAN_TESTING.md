# GUI23.5 physical 3DS test handoff

Status: local review build prepared; publication is not part of this handoff. Physical-console verification is pending.

## Changes under review

- Check Logs identifies its entries as Moonlight client logs stored on the 3DS. Sunshine server logs remain on the PC.
- The former Host tab is labeled Connect. Stream sessions are identified by their recorded `STREAM_BEGIN` event and appear in the Stream view.
- A Sunshine upload receipt in the global Moonlight log is matched back to its session by session ID, so an accepted upload should show green on that session row.
- Upload state remains the center LED color: red means no send attempt recorded, amber means failed or uncertain, and green means accepted. A blue ring marks the newest record in the current view; it does not replace upload state.
- Details show the selected session ID and whether the row is the latest in that view.

## Suggested device checks

1. Open Check Logs and confirm the header/details identify Moonlight client logs on the 3DS and clarify that Sunshine logs are on the PC.
2. Open Connect and Stream. Verify a recent stream appears in Stream and is the same local session file as its connection record; All should list that file once.
3. Confirm the blue ring marks only the newest record in each view, while the center LED continues to show upload state.
4. After a Sunshine-accepted upload, compare the session ID in the completion context with the selected row. Confirm that row turns green and More Details shows its receipt fields.
5. Confirm older records with no send attempt remain red and failed/uncertain attempts remain amber.
6. Check for clipping, tab/list overlap, footer separator clarity, or screen freezes.

## Build context

The build must retain static Libra SSL/OpenSSL linkage and the pinned Libra Crypto dependency. Build/link evidence is not physical-device verification.
