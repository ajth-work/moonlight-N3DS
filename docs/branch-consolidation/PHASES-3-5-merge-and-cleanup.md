# Branch hygiene phases 3–5: merge, cleanup, and later product work

**Status:** Draft for AJ review. No phase here authorizes execution. Merges, issue closures, branch deletion, builds, and publishing each require AJ's explicit go at the applicable gate.

## Phase 3 — Merge accepted issue work through PRs

For each item approved as a **Candidate PR** in Phase 2:

1. Use one focused working branch per issue, created from the matching product branch: GUI work from `product/gui`; 3D work from `product/3d`. Do not stack a new feature branch on another feature branch.
2. Open a pull request targeting that product branch. Link the issue in the PR and summarize the fix, relevant commits, checks, and any human/device acceptance still required.
3. Review the PR diff to confirm it contains only the intended issue work and is based on the correct product branch. Complete required checks and AJ's requested physical-device acceptance before merging when the issue requires it.
4. Merge only after the issue's acceptance gate is satisfied. Close the linked issue after the product-branch PR merges, with a comment linking the PR and verification evidence. GitHub's `Closes #N` automation applies when the PR targets the repository's default branch; product-branch PRs need manual issue closure.
5. Keep publishing separate from merging. A code merge does not authorize a build or release. Build and publish only under the applicable release approval and workflow.

## Phase 4 — Retire superseded branches (destructive; separate go required)

Only after accepted work is merged, relevant issues are resolved, and no open PR depends on a branch:

- Create or verify an archive tag at the exact final tip. Confirm the tag is present on GitHub before deleting the branch.
- Delete only branches explicitly approved for retirement. Never delete `n3ds-main` or a `product/*` branch as routine cleanup.
- Preserve branches with unresolved issue work or required device testing. Keep work that was classified **Needs AJ decision or testing** available until AJ decides.
- Backfill approved `.agents/skills/**/SKILL.md` files onto the product branches where they are needed. Bring over only the skill files; preserve the current `INTENT.md` and review any history or conflicts before merging.
- Update `INTENT.md` on `n3ds-main` with the final branch map and workflow after the topology is approved.

**Acceptance:** every retired branch has a verified recovery tag; active branches are limited to `n3ds-main`, product lines, and focused in-flight issue branches; unresolved work remains accessible.

## Phase 5 — Product identity, cross-line fixes, and release work (separate proposals)

- Decide whether Moonlight UI and Moonlight GamePad should share one parameterized source tree or remain separate product branches. The current GamePad commit adds behavior beyond packaging identity, so the decision must state whether that behavior is intended for both identities.
- Define source-diff, build, TMD Title ID readback, and physical-device acceptance criteria before identity code changes. Keep the published identities and shared config path unchanged unless AJ explicitly approves a change.
- Treat GUI-to-3D stack fixes and 3D-to-GUI teardown work as separate, reviewed issue PRs. Port only the changes approved for the destination product.
- A build, publish, feed change, or Codex Bridge dispatch is outside the branch-reference and cleanup phases. Each requires its own explicit authorization; any Bridge job must start only with a fully drained queue.
