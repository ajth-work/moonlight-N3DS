# Branch consolidation spec — moonlight-N3DS

**Status:** proposed (AJ decision pending). Written 2026-10-29 by Rocky at AJ's request.
**Scope:** branch topology and working rules only. No code changes except Phase 3.
**How to run this:** each phase is sized to be one Codex Bridge job, or a few minutes of AJ's own time at the PC (Appendix A has the exact commands for Phases 1–2). Phases are independent gates: stop after any phase and the repo is in a valid state. Do not chain phases without a digest and AJ's go.

---

## 1. Verified current state (GitHub, 2026-10-29 12:40 EDT)

| Branch | Tip | Role today |
|---|---|---|
| `n3ds-main` | `2f006ca6` | Upstream-tracking main; holds INTENT.md. Nothing AJ runs is built from here. |
| `feature/stream-profile-picker` | `c7119ff9` | ui1/ui2 picker line. Superseded by the GUI toolkit; ancestor of the 3D line. |
| `feature/gui-toolkit` | `ad7bd4ab` | GUI line base: toolkit, GUI1–GUI5, stackdiag fixes. |
| `feature/gui-stream-moonlight-theme` | `30d53377` | Branched from gui-toolkit tip (tag `gui5-checkpoint-20261029`): GUI6–GUI10, light theme, tearing diagnostics. |
| `feature/gui-stream-gamepad` | `82667f1a` | Branched from theme tip: GUI11 "Moonlight GamePad", a fourth Title ID identity. |
| `feature/moonlight-3d` | `85455a50` | 3D line, 3d1–3d11. Branched from the picker tip; diverged from the GUI line. |
| `diag/cert-fingerprints` | `ac1ef630` | Dead diag line (pairing saga, resolved 2026-10-26). |
| `diag/moonlight-logging` | `02587cdb` | Dead diag line. |
| `diag/tls-file-ctx-ab` | `fd83a742` | Dead diag line (diag5). |
| `codex/moonlight-build-publish-skills` | `bee03599` | Contents not yet reviewed; believed to carry repo build/publish skills. Phase 0 inspects it. |

**Key structural fact:** the GUI stack is *linear* — gui-toolkit `ad7bd4ab` → theme `30d53377` → gamepad `82667f1a`, each branched from the previous tip. Collapsing it needs no merges and produces no conflicts: one new branch at the gamepad tip contains the entire GUI history. The 3D line is *not* part of that chain (it diverged at the picker), so it consolidates separately.

**Problems this solves:** (1) no branch has ever merged, so no branch means "the product"; (2) the GUI mainline is a three-deep stack whose tip must be known from chat memory, not repo shape; (3) cross-line fixes travel by hand (GUI stack fix, 3D teardown serialization, the manually ported quit combo); (4) dead branches make the branch list lie about what is alive; (5) divergence from `n3ds-main` grows weekly, raising the cost of any future upstream pull.

## 2. Target topology

- `n3ds-main` — unchanged role: upstream tracking + repo-level docs (INTENT.md, this spec).
- **`product/gui`** (new) — the GUI product mainline, created at the gamepad tip `82667f1a`. Both GUI identities (Moonlight UI `0x0004000000554900`, Moonlight GamePad `0x00040000004D1100`) live in this history.
- **`product/3d`** (new) — the 3D product mainline, created at `85455a50` (Moonlight 3D, `0x0004000000003D00`). No code change.
- Working branches: `feature/<line>-<topic>` (e.g. `feature/gui-tearing-fix`, `feature/3d-panel-pass`), always branched from a `product/*` branch — never from another feature branch.
- Archive tags: `archive/<branch-name>-2026` at a branch's tip before the branch is deleted. Tags keep the history addressable forever.

## 3. Standing rules after consolidation

1. New work branches off a `product/*` branch. No new stacks: a feature branch's base is a product branch, full stop.
2. A feature branch merges back into its product branch in the same job that publishes its build, and is deleted after merge. Released builds get a checkpoint tag first (`<line>-checkpoint-YYYYMMDD`).
3. Before branching off for a *new identity or line*, cut a checkpoint tag on the product branch and verify the new Title ID from a built CIA's TMD readback — never from the Makefile alone.
4. Source commit + push is part of build + publish (existing rule; the stackdiag dirty-worktree era is why).
5. `n3ds-main` receives only upstream merges and repo-level docs.
6. The branch list should only ever show: `n3ds-main`, `product/*`, and currently active `feature/*` branches. Anything else gets an archive tag and is deleted.
7. Repo skills (`.agents/skills/**/SKILL.md`) must live on the product branches where jobs actually run, per the architect repo-skills rule (bridge job 20261028-1200).

**Adding a new line later (AJ's "new branch tomorrow" case):** pick the closest product branch as base → checkpoint tag → `feature/<line>-<topic>` → build with TMD-verified identity → publish → merge back to the product branch (or promote to `product/<line>` if it becomes a standing product) → delete the feature branch. The new line never becomes a base for further stacking.

## 4. Phases

### Phase 0 — Verify and inspect (read-only; Codex job or AJ, ~15 min)
- Re-list branches and tips; confirm they still match §1 (if a tip moved, note the new SHA and proceed — topology, not SHAs, is what matters; flag any *new* branch not in §1).
- Inspect `codex/moonlight-build-publish-skills`: what it contains, its base, whether its skills duplicate or differ from any `.agents/skills` already on the GUI/3D branches.
- Confirm the PC checkout has no uncommitted work that exists only locally (the known untracked `build-*` dirs and `docs/GUI_MENU_AUDIT.md` are expected and are not source).
- Confirm the UniStore feed state (rev 46 at spec time) and that the bridge queue is drained before any later phase runs.
- **Gate:** report findings; AJ's go for Phase 1.

### Phase 1 — Create the product branches (mechanical, no code change; one small job or AJ by hand)
- Create `product/gui` at `82667f1a` and `product/3d` at `85455a50`; push both.
- Create safety tags *before* anything else moves: `archive/gui-toolkit-2026` at `ad7bd4ab`, `archive/gui-stream-moonlight-theme-2026` at `30d53377`, `archive/gui-stream-gamepad-2026` at `82667f1a`, `archive/moonlight-3d-2026` at `85455a50`. Push tags.
- Verify: both product branches' SHAs match the table; `git log` on `product/gui` shows the GUI5/GUI10/GUI11 commits in its history.
- **No build, no publish, no retest:** the trees are identical to today's branch tips, so the published CIAs and feed are untouched by construction.
- **Gate:** digest to AJ. Rollback = delete the two product branches and the new tags; nothing else has changed.

### Phase 2 — Retire the old branches (destructive step; requires AJ's explicit go, separately from Phase 1)
- Archive-tag and delete: `diag/cert-fingerprints`, `diag/moonlight-logging`, `diag/tls-file-ctx-ab`, `feature/stream-profile-picker` (its history survives inside `product/3d`'s ancestry regardless).
- Delete the superseded feature branches now that product branches + archive tags exist: `feature/gui-toolkit`, `feature/gui-stream-moonlight-theme`, `feature/gui-stream-gamepad`, `feature/moonlight-3d`.
- `codex/moonlight-build-publish-skills`: per Phase 0 findings — if it carries skills, first land them on `product/gui` and `product/3d` (cherry-pick or merge, as a normal source commit), then archive-tag and delete this branch too.
- Update INTENT.md (on `n3ds-main`) with the new branch map and the §3 rules, so a fresh architect session learns the topology from the repo, not from chat.
- **Gate:** AJ's explicit go before any deletion. Rollback = recreate any branch from its archive tag (`git branch <name> archive/<name>-2026`).

### Phase 3 — One tree, two GUI identities (the only code-touching phase; one Codex job)
Today the Moonlight UI and GamePad identities are Makefile states on different branches. On `product/gui` they must become a build-time choice:
- Parameterize the identity (SMDH title, product code, UNIQUE_ID) as a build variable with the two published presets pinned: UI (`Moonlight UI`, CTR-P-MLUI, 0x5549 → TMD `0x0004000000554900`) and GamePad (`Moonlight GamePad`, CTR-P-MLGP, 0x4D11 → TMD `0x00040000004D1100`).
- Proof gate, no publish: build **both** variants from the same `product/gui` tree and read back **both** TMD Title IDs from the built CIAs. Both must match the published values exactly, and each variant's non-identity code must be unchanged from its published build (diff the source, not the binary).
- **Gate:** AJ's go before any build from `product/gui` is published. Until this phase passes, the last published UI and GamePad builds remain the installable ones — nothing about the feed changes.
- *Fallback (Option B) if parameterization proves messy:* keep two leaf branches, `product/gui-ui` (at `30d53377`) and `product/gui-gamepad` (at `82667f1a`), with a written rule that shared fixes land on `gui-ui` first and merge down to `gui-gamepad` in the same job. Simpler, but it keeps a two-deep stack by design; only choose it on evidence from the Phase 3 attempt.

### Phase 4 — Cross-pollination (separate jobs, each build + publish + retest; schedule at AJ's convenience)
- **Job X1 — stack fix to the 3D line.** Port the GUI line's connection-stack work (http_init frame diet 8,328 → 136 bytes; static-scratch diagnostic formatting from stackdiag2/3) into `product/3d`. Build + publish a 3D build. Retest: the GUI crash repro paths (Connect via A and touchscreen tile) plus the standard 3D legs (quit-to-menu timing, HOME backs).
- **Job X2 — teardown audit for the GUI line.** Audit first, port only what applies: compare the 3D line's serialized teardown / quit-to-menu path against the GUI line's stream-exit path. The GUI line already received the quit combo by manual port (GUI11), so this job's deliverable may legitimately be "already equivalent, here's the comparison" — that is a valid outcome, not a failed job. Port what's missing; build + publish only if code changed.
- These two jobs are independent of Phases 1–3 and of each other; they can run before or after consolidation. Consolidation just makes their targets unambiguous (`product/3d`, `product/gui`).

## 5. What this spec deliberately does not do
- No merging of the 3D line into the GUI line (or vice versa). They are sibling products with diverged renderers; a forced unification is a project, not a cleanup.
- No upstream rebase/merge from zoeyjodon's repo. Consolidation makes that *possible* later by giving it exactly two target branches; it is not scheduled here.
- No feed or relay changes. The UniStore feed stays the integration point throughout; published artifacts are never rebuilt as part of Phases 1–2.

## Appendix A — Phase 1–2 commands (for AJ at the PC, in the moonlight-N3DS clone)

Phase 1 (safe, additive):
```
git fetch origin
git tag archive/gui-toolkit-2026 ad7bd4ab
git tag archive/gui-stream-moonlight-theme-2026 30d53377
git tag archive/gui-stream-gamepad-2026 82667f1a
git tag archive/moonlight-3d-2026 85455a50
git branch product/gui 82667f1a
git branch product/3d 85455a50
git push origin --tags product/gui product/3d
```

Phase 2 deletions (only after AJ's separate go):
```
git tag archive/cert-fingerprints-2026 ac1ef630
git tag archive/moonlight-logging-2026 02587cdb
git tag archive/tls-file-ctx-ab-2026 fd83a742
git tag archive/stream-profile-picker-2026 c7119ff9
git push origin --tags
git push origin --delete diag/cert-fingerprints diag/moonlight-logging diag/tls-file-ctx-ab feature/stream-profile-picker feature/gui-toolkit feature/gui-stream-moonlight-theme feature/gui-stream-gamepad feature/moonlight-3d
```
(`codex/moonlight-build-publish-skills` is deleted only after its Phase 0/2 skills landing; not in the batch above on purpose.)

## Appendix B — Acceptance checklist
- [ ] `product/gui` and `product/3d` exist at the §1 tips (or their verified successors)
- [ ] Archive tags exist for every retired branch before its deletion
- [ ] Branch list shows only `n3ds-main`, `product/*`, active `feature/*`
- [ ] INTENT.md documents the branch map and §3 rules
- [ ] Phase 3: both GUI identities build from `product/gui` with TMD-verified Title IDs
- [ ] Feed untouched by Phases 1–2 (still rev 46 lineage, all routes hash-verified)
