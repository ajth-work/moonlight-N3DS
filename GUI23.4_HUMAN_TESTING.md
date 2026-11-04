# GUI23.4 physical 3DS test handoff

Status: local review build prepared; publication requires AJ approval. Physical-console verification is pending.

## Changes under review

- The Check Logs header, tab row, and log rows now have separate vertical space.
- The Send Log action no longer displays the numbered tile badge.
- The top-screen footer separates control/action pairs with centered dots.
- For a log with no upload event, details say no send attempt in log; per-upload byte values remain unavailable until an upload is recorded.

## Suggested device checks

1. Open Check Logs and confirm the log count/header does not collide with All / Host / Stream tabs.
2. Move between tabs and rows; confirm all five rows remain visible and the footer still shows every control hint.
3. Open a local log with no upload attempt. Confirm details say no send attempt in log, while local file size remains available.
4. Confirm Send Log has no stray number and its label is centered and readable.
5. Send a log, then verify upload ID and byte measurements populate for the recorded attempt.
6. Capture any clipping, screen freeze, or confusing status text.
