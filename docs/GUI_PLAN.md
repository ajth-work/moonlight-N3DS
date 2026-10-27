# Moonlight-N3DS — Full Graphical UI Plan

**Direction (AJ, 2026-10-26):** go all in. Every screen of this client —
before connecting, while connecting, in-stream, and on disconnect — moves
from the terminal-style text console to the graphical language proven by
the stream-profile picker (3.1.0-ui1/ui2). The picker is the design seed,
not the finish.

**Base:** `feature/stream-profile-picker` tip (3.1.0-ui2, 90c2c34).
**GUI line versioning:** `3.3.0-guiN`, version always on the bottom screen.

## Where the UI stands today (verified by code read, 2026-10-26)

- All pre-stream screens are a text console on the bottom screen built on
  one primitive, `console_selection_prompt` in `src/n3ds_main.cpp`:
  host select (`prompt_for_address`), action menu (`prompt_for_action`),
  stream settings editor (`prompt_for_stream_settings`, 14 independent
  fields), the pairing PIN + phase-trace screens (diagnostics console),
  the app-list pick, and plain-text error lines.
- The one graphical screen is the stream-profile picker: generated PNG
  pages (`3ds/gfx/stream_profile_*.png` + `make_stream_profile_pages.py`)
  embedded as BGR headers, a framebuffer-drawn selection outline
  (mapping fixed in ui2), and a single-writer footer.
- In-stream, the bottom screen already uses pre-rendered PNG plates
  (`gamepad.png`, `menu.png`, `keyboard*.png`, `touchpad.png`) — an older
  art style, driven by the touch handlers.
- Disconnect/teardown is raw text on the top-screen console
  ("Connection has been terminated gracefully…"), then back to text menus.

## The foundation: a small UI toolkit (Phase 0)

The picker pattern — one embedded PNG per screen — does not scale:
host names, app lists, PINs and error text are **dynamic**, and each
embedded full-screen plate costs ~150 KB of binary. The toolkit replaces
per-screen plates with shared parts, extracted from the picker + the
diagnostics version-overlay code:

- **One shared background plate** (the ui2 look: deep blue, soft corner
  ellipses, header band, footer band) drawn once; tiles and text are
  composed on top of it at runtime.
- **Primitives:** filled tile, list row, highlight outline (the ui2-fixed
  pixel mapping), header (title + step/context), footer
  (`Moonlight <version> · <hint>`, single writer), and **live text** —
  arbitrary strings drawn into the framebuffer, generalizing the glyph
  path `n3ds_draw_diag_version_overlay` already uses.
- **One input/redraw loop** for every screen: D-pad/circle navigation,
  A confirm, B back, touch zones; full repaint on change.
- **Open item the toolkit must close:** the flicker/tearing AJ observed
  while navigating (2026-10-26). The bottom screen runs single-buffered
  (`gfxSetDoubleBuffering(GFX_BOTTOM, false)`) yet screens swap buffers;
  the toolkit phase investigates double-buffering the UI screens or an
  equivalent tear-free redraw, and reports which it shipped.

Deliverable of Phase 0: the toolkit module (proposed `src/ui/`) with the
picker rebuilt on top of it as the proof screen — same look, same
behavior, no per-screen embedded plates for the picker pages.

## Phase 1 — Pre-connect screens

- **Host select:** graphical list of paired hosts (name/address, paired
  status marker, last-used profile summary per host) + a "New host" tile.
  Address entry keeps the system keyboard (it is already graphical),
  framed by the UI before and after.
- **Action menu:** tiles — Stream, Stream Profile, Quit Stream,
  Settings, Unpair (Unpair gets a graphical confirm page).
- **Settings editor:** graphical rows with values and steppers. Width /
  height stop being free-typed numbers everywhere in the UI — they are
  set only through the profile brackets (the 400x480 mismatch class
  stays dead). Numpad entry remains only for fields where a wrong value
  is harmless (e.g. packet size).

## Phase 2 — Connecting, pairing, app list, errors

- **Pairing screen:** big readable PIN, a phase checklist that ticks
  live, and a **working B cancel** — this phase owns the one below-the-UI
  fix in the whole program: the pairing waits must poll input (or time
  out) instead of blocking, so cancel always works. The never-stack-
  pairings rule stays visible in the flow: an abandoned session is
  cleaned up by the cancel path, as the diagnostics builds proved.
- **Connecting progress:** host-state / app-list / stream-start phases
  as graphical steps, replacing the raw trace scroll (the trace itself
  keeps logging to `moonlight.log`; a Diagnostics screen, Phase 2 or 3,
  frames it graphically — the log file remains the source of truth).
- **App list:** graphical list/tiles with real app names from the host.
- **Error pages:** graphical, plain-language, always with a working way
  back (B) — no dead-end text screens. Certificate/verify errors name
  the next step, not just the failure.

## Phase 3 — In-stream and disconnect

- **In-stream bottom plates** regenerated in the ui2 theme
  (gamepad / menu / keyboard / touchpad), same touch zones and behavior.
- **Stream-start card:** brief graphical summary of what actually
  started (resolution / fps / bitrate / profile name) — the numbers AJ
  currently reads off a trace line.
- **Disconnect screens:** graphical "Stream ended" page with the reason
  (clean exit, host quit, connection lost, error) and session length
  where known, returning to the graphical host screen. Quit-stream gets
  a confirm page like Unpair.

## Rules that hold across all phases

- Version string on the bottom screen at all times (standing rule).
- Config semantics unchanged: `moonlight.conf` and
  `stream_profiles.conf` keep their formats and meanings; per-host
  profiles keep working exactly as ui1/ui2 shipped them.
- Protocol, TLS and pairing logic are untouched **except** the
  Phase 2 pairing-wait input polling, which is scoped to making cancel
  work and must not change pairing outcomes.
- Top-screen video path (renderers, decoders) is untouched except the
  bottom-screen work named above.
- Each phase is its own bridge job + its own on-hardware photo test by
  AJ before the next phase dispatches. A phase is done when its screens
  pass on EridianOne, not when the build is green.
- Builds keep using the existing `moonlight-n3ds:latest` Docker image;
  the Dockerfile is never rebuilt (standing lesson).

## Sequencing

Phase 0 (toolkit + picker rebuilt on it) → Phase 1 → Phase 2 → Phase 3.
Branch per phase off the GUI line, pushed unmerged, merged only on AJ's
word after the hardware test. The 3D line (`feature/moonlight-3d`) is
separate and untouched by this program; whichever line merges second
rebases onto the result.
