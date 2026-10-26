# Current Intent — moonlight-N3DS

Updated by Rocky, 2026-10-26 (evening — pairing RESOLVED), at AJ's
request. AJ may talk to Codex directly in this repo; this file is
the shared understanding. Codex: read this before starting work,
along with AGENTS.md.

## What AJ wants

AJ's fork of zoeyjodon's moonlight-N3DS (GPLv3), to fix the PC to
EridianOne (his New 3DS XL) streaming pain he hit on 2026-10-25:
re-pairing an already-paired host hangs forever — pairing HTTP has
no timeout — and failures give zero on-screen visibility, so a
console reboot is required to recover. Direction approved by AJ:
visibility first, features after diagnosis.

## Where it is now — RESOLVED 2026-10-26 (read this first)

**The pairing blocker is FIXED, and it was Sunshine-side all
along, not this client.** Both clients (Android phone and
EridianOne) stream again. Full evidence record:
ajth-work/3ds-homebrew issue #27.

### Root cause

Sunshine authorizes a client certificate **fail-closed**: a cert
is enabled only when *exactly one* named-device record matches
it (confirmed in Sunshine's own source, `is_client_enabled()`).
Every re-pair silently added another record instead of replacing
the old one, so `sunshine_state.json` ballooned to 20 records —
~16× the 3DS cert, 4× the phone cert. With duplicates present,
the final mutual-TLS verify denied BOTH clients ("The client is
not authorized. Certificate verification failed." / Sunshine log:
`Client certificate identity is not enabled`), even though every
record was enabled, byte-exact, and freshly written. That is why
diag1–diag5 all failed identically, why the known-good phone
started failing too, and why restarts never healed it.

Upstream fix: LizardByte **PR #5737** ("fix(pairing): replace
duplicate client certificates"), shipped in pre-release
**v2026.1018.5342** — a successful re-pair now deletes stale
same-cert records before storing the new one. The fail-closed
check itself is preserved, so **installing the new build alone
does not heal a duplicated state file**: dedupe (or re-pair once
per client) is still required.

### The fix, as applied 2026-10-26 evening (all verified live)

1. **Dedupe on the old build:** backed up `sunshine_state.json`,
   stopped SunshineService, removed all but the first record per
   cert (survivors: `3DS`, `pc2`) — 20 → 2 records, confirmed by
   on-disk re-read and by the web UI Clients list (the fresh
   process's in-memory list). A cur-vs-backup comparison then
   proved the edit was byte-clean: root keys identical, server
   uniqueid unchanged, surviving phone cert identical (`-ceq`,
   same length). Dedupe alone did NOT restore the phone on the
   old build (still NOT_PAIRED) — hence step 2.
2. **Installed Sunshine v2026.1018.5342** (Windows MSI, size
   53,043,200 bytes, SHA-256
   70a15df6f223c7f01c89f216e6cf51646864419320182c309669309422394a39,
   verified before install). Read-back after install:
   sunshine.exe FileVersion/ProductVersion = 2026.1018.5342,
   service Running, state file preserved at 2 records.
3. **Phone:** re-paired once on the new build → app list →
   Desktop stream live (Sunshine "Stream Started" toast,
   2026-10-26 18:44 EDT, remote from AJ's workplace).
4. **EridianOne:** needed **no re-pair at all**. Over AJ's phone
   hotspot, a fresh `stream.textplex.co` host entry went straight
   to the action menu (with "unpair" — paired state recognized),
   the app list loaded over the diag5 file route (TLS fingerprint
   e77dc049…710084, the known 3DS identity), and Desktop streamed
   live at 400x480 / 30fps / 3000kbps (19:00 EDT). **Test B (3DS
   over phone hotspot) is PASSED.** The surviving deduped `3DS`
   record was valid all along; the new build's verification
   finally honors it.

### Standing warnings (do not relearn these the hard way)

- **Never stack re-pairings.** On builds older than
  v2026.1018.5342 each pairing adds a duplicate record and
  re-breaks verification for that cert. If pairing must be
  redone on an old build, delete the same-cert records first.
- **Never leave a pairing session abandoned.** Both clients
  present the default uniqueid `0123456789ABCDEF` and Sunshine
  keys pairing sessions by uniqueid — an abandoned session from
  one client blocks the other's next attempt ("A pairing session
  with this uniqueid already exists") until it is unpaired or
  times out. Always finish a pairing or back out/unpair cleanly
  before starting one from the other client.
- **Never clear Sunshine's paired devices without a backup** of
  sunshine_state.json and AJ's explicit go.

### Client-side state after the fix

- The parked client question (does this fork present a client
  cert in the handshake at all?) is **effectively answered**:
  diag5's file route presents the identity Sunshine now accepts
  — app list and full stream work end-to-end on it. No diag6 is
  needed for the pairing bug; it is closed.
- Branch state (unchanged): the diagnostics line (diag2
  `diag/cert-fingerprints` → diag3 `fix/tls-client-cert` →
  diag4 `fix/tls-inmemory-identity` → diag5
  `diag/tls-file-ctx-ab`) is unmerged, as is AJ's 5-commit
  remembered-hosts series on `diag/moonlight-logging`. The
  Sunshine question that gated them is now resolved, so the
  merge / diag-retirement / clean-production-build discussion is
  **unblocked — AJ's call, nothing auto-starts**.

## Next intended step + what done looks like

Nothing is pending for pairing. Remaining checks are small and
non-blocking: quit-and-relaunch persistence on the 3DS (no new
PIN), and the home-LAN host entry on the new Sunshine build.

Next client work, when AJ picks it: decide the diag line's fate
(retire the diag branches; build a clean non-diag release from
the diag5 file route), the remembered-hosts merge, and the GUI
milestone below.

## Later GUI milestone idea

After diagnostics and the on-console test, AJ wants to explore a
more modern, intentional touchscreen control surface for streamed
games. The current bottom-screen controls work, but their large
square regions feel utilitarian; the redesign should make controls
and their behavior easier to understand and use, rather than only
adding features.

Ideas to explore:
- Show virtual left and right analog sticks as circular touch zones,
  with a movable inner knob that communicates direction and amount
  of movement. The right stick should offer a more comfortable,
  visible camera/right-stick control than relying on the physical
  C-stick placement.
- Make L3 and R3 clicks available without triggering them
  accidentally during ordinary stick movement. The exact interaction
  is open: prototype selectable options for separating movement from
  click (for example, a nearby mode toggle) and test what feels
  intuitive during play. Do not commit to one interaction model yet.
- Keep secondary controls in a collapsible tray, opened by a down
  chevron at the top of the bottom screen. Use compact graphical
  buttons for tray items (for example, a magnifying-glass icon for
  magnification) so secondary options take less space when hidden.
- Include settings or control options that let AJ compare alternative
  stick/click behaviors during hands-on testing; choose the final
  interaction based on what feels good in use.
- Explore a compact stream-quality display, similar in spirit to
  the Steam Deck game bar, including a latency history visualization
  for a recent window (roughly the last minute as a starting point).
  The exact latency measure, refresh rate, and presentation need
  testing.
- Explore showing the 3DS battery level while streaming, so users
  can check it without leaving the game. If reliable battery and
  power-draw data are available, consider a useful estimate of
  remaining streaming time. First verify what the hardware and APIs
  can actually report.

This is a rough direction, not a finished GUI specification.
Diagnostics and AJ's on-console test come first. Screen layout,
stick sensitivity, tray contents, and exact L3/R3 interaction remain
to be worked out and tested.

## Later host-selection UX idea

After the diagnostics milestone, make the initial server menu remember
addresses as soon as they are entered, including addresses whose
connection attempt fails. Keep those entries beside “new” so AJ can
select a failed host and retry without retyping it. Show a clear failed
status on the entry (for example, “stream.textplex.co (failed — try
again?)”). Keep the navigation hints explicit: Up/Down to select, A to
confirm or retry, B to go back.

Other possible later work: pair-status precheck, pairing timeout,
surfacing server error text, per-host profiles (Home sharp 800x480 /
Away light 400x240).

## Don't touch / standing rules

- Default branch here is `n3ds-main`, not main/master. Diagnostics
  work lives on its diag branches, unmerged until AJ approves.
- **The pairing bug is closed — no new diag builds for it.** Any
  production-build or branch-merge work starts only on AJ's go.
- Never clear Sunshine's paired devices without a backup of
  sunshine_state.json and AJ's explicit go.
- Version string on the bottom screen at all times is a requirement,
  not decoration — AJ reads the version off screenshots.
- Respect upstream and GPLv3: this is a fork of zoeyjodon's work.
- Paste = go: a pasted prompt is AJ's approval to start. No
  front-door gate; gates only at blockers, forks, destructive steps.
- Human-testing rule: anything needing AJ's hands goes on a
  repo tracker with the `human-testing` label and clear
  step-by-step instructions — never only in chat.
- AJ's console is final QA. Verifier-green is never the bar.

## Open questions for AJ

- Diag line's fate: retire the diag branches and cut a clean
  non-diag build from diag5's file route, and merge the
  remembered-hosts series onto it?
- For the GUI, which stick/click options feel natural in a play test,
  and what should be included in the collapsible tray?
- Which latency measure is available and most useful to display?
- What battery and power-draw data can the 3DS reliably expose?
- What status wording should the remembered-host list use for a
  failed connection?
