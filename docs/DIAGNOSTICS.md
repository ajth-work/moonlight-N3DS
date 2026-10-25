# Milestone 1 — Diagnostics build (visibility first)

## Why

On 2026-10-25, stock Moonlight-N3DS v3.0.1 repeatedly **hard-hung during re-pairing** with Sunshine: first-time pairing works and streams in that session, but in a later session the client treats the already-paired host as unpaired, runs the pairing flow again, Sunshine accepts the PIN ("Success"), and the 3DS never proceeds — the console stalls hard enough to need a **reboot**. Reproduced on home Wi-Fi and on a phone hotspot, including a real away attempt. Full field notes: `docs/moonlight-eridianone-config.md` in ajth-work/3ds-homebrew.

We have **zero client-side visibility** — no log, no phase readout, no memory figures — so we cannot tell whether the hang is a memory failure/leak, a blocked network call, or a deadlock. Upstream's pairing HTTP calls have no timeout ("Limits HTTP timeout to 1min for all but pairing"), so any unexpected response can wait forever.

## This milestone: diagnostics only

Make the client able to say what it's doing. No behavior fixes in this milestone (beyond what the watchdog/cancel needs).

- [ ] **Persistent log** at `sd:/3ds/moonlight/moonlight.log`, flushed per line so it survives the forced reboot: timestamp, event, detail.
- [ ] **Instrument the pair/connect flow**: log every phase transition, each HTTP request (method, URL, status), and each phase's outcome.
- [ ] **Free-memory reading at each phase**, logged alongside the phase — a leak drains phase over phase; a stall stays flat with one call never returning. This distinction is the whole point of the build.
- [ ] **On-screen trace**: during pair/connect, the bottom screen shows the current phase + the last few log lines live, instead of a static PIN screen.
- [ ] **Watchdog**: if a phase overruns a limit, log state + free memory, show the state on screen, and offer **B to cancel** back to the host list — a hang must never again take the whole console down.
- [ ] **Version on the bottom screen** at all times (standing rule for all prototypes): this build identifies as `3.0.1-diag1`.
- [ ] Also log app start, host selection, stream start/stop, and clean exit, for context in later milestones.

## Acceptance

- Branch `diag/moonlight-logging`; builds clean via the repo Dockerfile and/or the inherited `build-cia.yml` GitHub Actions workflow; report which produced the artifact (name, size, SHA-256).
- AJ installs the CIA, reproduces a pairing attempt, and the resulting `moonlight.log` shows the phase sequence up to the hang with memory readings — that log is the deliverable this milestone exists for.
- Not in scope: fixing the re-pair hang itself (that's Milestone 2, driven by what this log shows), pair-status precheck, per-host profiles, dual-address hosts.

Upstream: zoeyjodon/moonlight-N3DS (GPLv3).

---
Tracking note: forks have GitHub Issues disabled by default, so this milestone lives as a doc until/unless Issues are enabled in the fork's settings.
