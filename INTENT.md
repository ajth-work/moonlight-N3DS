# Current Intent — moonlight-N3DS

Updated by Rocky, 2026-10-26, at AJ's request. AJ may talk to Codex
directly in this repo; this file is the shared understanding. Codex:
read this before starting work, along with AGENTS.md.

## What AJ wants

AJ's fork of zoeyjodon's moonlight-N3DS (GPLv3), to fix the PC to
EridianOne (his New 3DS XL) streaming pain he hit on 2026-10-25:
re-pairing an already-paired host hangs forever — pairing HTTP has
no timeout — and failures give zero on-screen visibility, so a
console reboot is required to recover. Direction approved by AJ:
visibility first, features after diagnosis.

## Where it is now

- Milestone 1 spec lives in `docs/DIAGNOSTICS.md` and as issue #1.
- Reported (not yet digested): branch `diag/moonlight-logging` was
  pushed by Codex on 2026-10-25, with commits "Add Moonlight N3DS
  diagnostics build" and "Keep diagnostics version visible on
  bottom screen". The bridge job's report / return leg is stranded
  (see codex-bridge), so treat the branch as reported, not verified.
- Not verified: on-hardware behavior cannot be verified off-console.
  AJ's on-console test is the acceptance.

## Next intended step + what done looks like

Milestone 1 — diagnostics build:
- On-screen phase trace during pair / connect.
- Persistent, per-line-flushed log at
  `sd:/3ds/moonlight/moonlight.log` — timestamps, phase transitions,
  HTTP status, free-memory readings per phase.
- Watchdog with B-to-cancel.
- Version string `3.0.1-diag1` visible on the bottom screen at
  all times.
Done looks like AJ running it on EridianOne and the log / trace
showing where a re-pair hang actually stalls.

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
- Explore a compact stream-quality display, similar in spirit to the
  Steam Deck game bar, including a latency history visualization for
  a recent window (roughly the last minute as a starting point). The
  exact latency measure, refresh rate, and presentation need testing.

This is a rough direction, not a finished GUI specification.
Diagnostics and AJ's on-console test come first. Screen layout,
stick sensitivity, tray contents, and exact L3/R3 interaction remain
to be worked out and tested.

Other possible later work: pair-status precheck, pairing timeout,
surfacing server error text, per-host profiles (Home sharp 800x480 /
Away light 400x240).

## Don't touch / standing rules

- Default branch here is `n3ds-main`, not main/master. Milestone 1
  work lives on `diag/moonlight-logging`, unmerged until AJ approves.
- Version string on the bottom screen at all times is a requirement,
  not decoration — AJ reads the version off screenshots.
- Respect upstream and GPLv3: this is a fork of zoeyjodon's work.
- Paste = go: a pasted prompt is AJ's approval to start. No
  front-door gate; gates only at blockers, forks, destructive steps.
- Human-testing rule: anything needing AJ's hands goes on this
  repo's tracker with the `human-testing` label and clear
  step-by-step instructions — never only in chat.
- AJ's console is final QA. Verifier-green is never the bar.

## Open questions for AJ

- Diagnostics build on-console: does the trace/log capture the hang?
- For the GUI, which stick/click options feel natural in a play test,
  and what should be included in the collapsible tray?
- Which latency measure is available and most useful to display?
