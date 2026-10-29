# Branch hygiene phases 0–2: inventory and setup

**Status:** Draft for AJ review. These phases describe work; they do not authorize branch creation, deletion, merging, builds, or publishing. Each state-changing step needs AJ's explicit go.

## Phase 0 — Inventory and issue map (read-only)

1. Fetch the repository and record current local and GitHub branch tips. Report branches that are missing remotely, diverged, or ahead/behind; do not infer that a local branch name means its commits are unpublished.
2. For every candidate product line, compare commits and ancestry against the proposed product tips. Classify each issue's work as **already included**, **candidate for a PR**, **unfinished / needs human acceptance**, or **archive / do not merge**.
3. Link relevant commits and branches to GitHub issues and pull requests. Record each issue's acceptance criteria and whether physical-device evidence is still needed.
4. Inspect the build/publish skills branch and compare its files with the product branches. Preserve current `INTENT.md`; do not replace it with an older copy from the skills branch.
5. Record the current worktree state and expected local-only artifacts. Preserve all unrelated work.
6. Before any future Codex Bridge dispatch, verify that the GitHub queue is fully drained. This discussion and these documents do not inspect or modify the UniStore feed or relay.

**Phase 0 deliverable:** a branch/commit → issue → intended product table, with evidence and unresolved decisions. No code or refs change.

## Phase 1 — Establish product branch references (state-changing; separate go required)

After AJ reviews Phase 0 and explicitly approves:

- Create `product/gui` at the verified GUI product tip (the reviewed proposal used `82667f1a`).
- Create `product/3d` at the verified 3D product tip (the reviewed proposal used `85455a50`).
- Create archive tags for the source branch tips before any retirement. Verify each tag resolves to the intended SHA and exists on GitHub before relying on it for recovery.
- Do not merge source histories, change source files, build, publish, or touch the feed as part of this phase.

**Acceptance:** both product branches and the safety tags resolve to the reviewed SHAs; `product/gui` contains the GUI toolkit → theme → GamePad ancestry; `product/3d` retains the 3D line. Report actual SHAs if a tip moved since the proposal.

## Phase 2 — Decide the disposition of existing issue work (read-only; no merges)

For each old branch or commit, record one of these outcomes:

- **Already included:** commit is an ancestor of the appropriate product branch; no duplicate merge or PR is needed.
- **Candidate PR:** wanted work is absent from the product branch and has a clear issue, scope, and acceptance evidence.
- **Needs AJ decision or testing:** intent, behavior, or physical-device acceptance is unresolved.
- **Archive / do not merge:** obsolete diagnostics, superseded experiments, or work that does not belong in either product line.

Do not merge a whole branch just because it has issue-related commits. Review the actual commit range and exclude unrelated or obsolete changes. Keep GUI and 3D histories separate.

**Gate:** AJ reviews and approves the per-issue disposition before any PR creation, merge, issue closure, or branch deletion.
