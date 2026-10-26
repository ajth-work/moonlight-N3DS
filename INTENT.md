# Current Intent — moonlight-N3DS

Updated by Rocky, 2026-10-26 (post-noon, after the Sunshine-side
investigation), at AJ's request. AJ may talk to Codex directly in
this repo; this file is the shared understanding. Codex: read this
before starting work, along with AGENTS.md.

## What AJ wants

AJ's fork of zoeyjodon's moonlight-N3DS (GPLv3), to fix the PC to
EridianOne (his New 3DS XL) streaming pain he hit on 2026-10-25:
re-pairing an already-paired host hangs forever — pairing HTTP has
no timeout — and failures give zero on-screen visibility, so a
console reboot is required to recover. Direction approved by AJ:
visibility first, features after diagnosis.

## Where it is now — READ THIS FIRST (2026-10-26 verdict)

**The pairing blocker is NOT (only) this client. Do not build a
diag6 or iterate further client TLS routes until AJ says so.**

Evidence chain, all hardware-verified by AJ (full record:
ajth-work/3ds-homebrew issue #27 and
docs/moonlight-eridianone-config.md):

- Diag builds 3.0.1-diag1 → diag5 all reproduced the same final
  failure: pairing completes in Sunshine ("Success" in the web UI),
  then the final mutual-TLS verify fails — "The client is not
  authorized. Certificate verification failed." Sunshine's log
  warning at that step is always `Client certificate identity is
  not enabled` + `/CN=NVIDIA GameStream Client -- denied`.
- Diag2/diag3 panel: the cert loaded for pairing == the cert
  loaded for TLS, fingerprint DER SHA-256 e77dc049…710084,
  byte-identical to the cert Sunshine stores for this client
  (stored-PEM SHA-256 prefix 211BB28C1680). File-level identity
  split is DEAD.
- Diag3's combined-PEM route completes the TLS handshake but
  Sunshine still denies. Diag4's in-memory blob route fails
  locally (mbedTLS -0x3D00 key parse, curl 58) and never reaches
  Sunshine — dead end, do not revive it.
- Diag5 (branch `diag/tls-file-ctx-ab`): file route primary, ctx
  callback as witnessed fallback. On hardware 2026-10-26 it ran
  the full pairing via stream.textplex.co (11:27) and was denied
  at verify (11:28:00). Panel: route=file throughout, no ctx
  fallback fired, peer slot n/a (CERTINFO not compiled in).
- **The fault then proved server-side.** On 2026-10-26 the
  known-good Android Moonlight phone (unchanged cert since 2024,
  DER a7a15745…; paired + streamed fine 2026-10-25 23:21) also
  completed pairing three times and was denied at verify every
  time, on LAN and by name.
- **Sunshine's state file is fully exonerated** (inspected
  2026-10-26 11:45–12:33, field by field):
  `C:\Program Files\Sunshine\config\sunshine_state.json` —
  20 named-device records, ALL enabled=true, only two certs
  present (15× the 3DS cert 211BB28C1680 / len 1013, incl.
  today's `3ds diag5 home 1` and `3ds diag5 new new 12`; 4× the
  phone cert 6295E33DF263 / len 1021, as `pc2` + `phone diag5
  1–3`), file written at the second of the last denial, and
  every record's uuid is a distinct per-record random ID
  (a client can never present it — not the verify key).
- **Restart does not heal it.** A fresh Sunshine process
  (v2026.914.233613, started 2026-10-26 12:01:58) still
  reported the phone NOT_PAIRED (PIN prompt on tap, 12:04) and
  denied the 3DS's final verify at 12:08:22 (~1.6 s after
  clientpairingsecret) for a pairing completed seconds earlier
  in that same process. Conclusion: the fault is inside
  Sunshine's live verification path, not its stored data and
  not a stale process.
- **Uniqueid collision (new, confirmed in the noon log):** both
  the phone and this client present the default uniqueid
  `0123456789ABCDEF`, and Sunshine keys pairing sessions by
  uniqueid. An abandoned pairing session (phone, 12:04:42)
  blocked the 3DS's next attempt with "A pairing session with
  this uniqueid already exists" until the 3DS's unpair cleanup
  cleared it. Standing hazard: always let a pairing finish or
  unpair before starting one from the other client.
- What remains genuinely open on the client side: whether this
  fork's mbedTLS stack presents a client cert in the handshake
  at all (diag3's trace never observed one —
  `client_certificate_out_record=not_observed`, and the
  Oct 25/26 log census fits "presents nothing"). That question
  is PARKED, not closed: it cannot explain the phone failing,
  so Sunshine gets fixed or cleared first.
- Branch state: the diagnostics line (diag2 `diag/cert-fingerprints`
  → diag3 `fix/tls-client-cert` → diag4 `fix/tls-inmemory-identity`
  → diag5 `diag/tls-file-ctx-ab`) is unmerged. AJ's separate
  5-commit remembered-hosts series on `diag/moonlight-logging`
  is also unmerged and still gated: merge it onto the current
  diag successor only when AJ approves, after the Sunshine
  question resolves.

## Next intended step + what done looks like

Sunshine-side, AJ's decision pending between:

1. Unfiltered Sunshine log windows (12:01 startup / state load,
   and 12:08 around the verify denial) from the log slices
   already extracted on the PC, or
2. Clean-slate experiment: back up sunshine_state.json, clear
   ALL device records in the web UI, restart Sunshine, pair
   ONLY the phone, try Connect. Phone working solo implicates
   the accumulated duplicate records; phone still failing
   convicts the Sunshine install/config wholesale.

Done for this phase = a client (phone first, as the known-good
reference) connects to the fresh Sunshine process WITHOUT
re-pairing, or the Sunshine fault is identified and
fixed/documented. Only then does client-side work resume.

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
- **No diag6 and no further client TLS-route iteration for the
  pairing bug until AJ approves the next step** — the fault is
  Sunshine-side as of 2026-10-26 (see above).
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

- Sunshine-side next step: log windows first, or the clean-slate
  phone-only experiment?
- After Sunshine clears: does the client-presentation question
  (diag3's unobserved client cert) still reproduce against the
  phone as a working reference?
- For the GUI, which stick/click options feel natural in a play test,
  and what should be included in the collapsible tray?
- Which latency measure is available and most useful to display?
- What battery and power-draw data can the 3DS reliably expose?
- What status wording should the remembered-host list use for a
  failed connection?
