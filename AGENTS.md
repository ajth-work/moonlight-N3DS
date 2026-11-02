# AGENTS.md — Moonlight N3DS repository rules

Read `INTENT.md`, `docs/branch-consolidation/SPEC.md`, and `docs/branch-consolidation/BRANCHING-AND-PROMOTION.md` before changing branches or promoting product work. The spec governs topology; this file gives agents the operating rules. If they conflict, stop and surface the conflict rather than silently redefining the branch model.

## Branch roles

- `n3ds-main` is the fork's default branch for upstream tracking and repository-level documentation. It is **not** the product-code hub under the current spec.
- `product/gui` is the stable development baseline for GUI products and identities.
- `product/3d` is the stable development baseline for the 3D product.
- `feature/<line>-<topic>` is a focused, temporary experiment branched directly from the latest remote tip of its target product branch. Do not stack feature branches or choose a different base without recording the reason and getting AJ's direction.

When AJ says “main,” distinguish the default branch `n3ds-main` from a product mainline such as `product/gui`. If the intended target is unclear and the operation would merge or promote code, clarify before acting.

## Experiment and PR workflow

1. Read the issue and define acceptance criteria before coding. Keep one coherent change per feature branch.
2. Fetch the remote, verify the product branch tip, and record the base SHA. Inspect dirty checkouts and use an isolated worktree when needed; preserve unrelated edits and build artifacts.
3. Keep unproven work on its feature branch. Report build, automated/host checks, CIA metadata, and physical-console checks separately. A successful build or a test release alone does not establish hardware acceptance.
4. Open a focused PR from the feature branch to its product branch: GUI to `product/gui`, 3D to `product/3d`. Include the issue, base/head SHAs, behavior changed, evidence, remaining tests, and risks.
5. Merge only after the agreed acceptance criteria and review are satisfied. For work needed on both product lines, prepare and validate a separate port/PR for the second line; do not merge sibling product branches by default.
6. Follow the current consolidation spec for release sequencing. A merge, build, publish, archive tag, and branch deletion are distinct state changes; do not assume authorization for one implies authorization for the others. The current spec specifically couples feature merge and publish in one job, so revise that rule before using a different sequence.
7. After a merge, verify the target product branch contains the intended commit. Apply the required archive tag and read it back before deleting a remote feature branch. Preserve local recovery work until cleanup is authorized.
8. Start subsequent work from the updated product branch. That is how a proven feature becomes part of the baseline inherited by later experiments on that product line.

## Getting proven work into the fork

- Proven GUI work returns to `product/gui`; proven 3D work returns to `product/3d` through a reviewed PR.
- `n3ds-main` receives upstream changes and repository docs only. Do not open or merge product-code PRs to it under the current topology.
- If AJ wants `n3ds-main` to become a shared code hub, or wants a new permanent product line, first discuss and update `docs/branch-consolidation/SPEC.md` to define ownership, inheritance, and cross-line flow. Do not create that topology by inference.
- Shared capabilities such as logging do not automatically cross product lines. Port them deliberately, test them on the destination line, and merge them through that line's PR.

For CIA work, follow the repository build and publish skills on the relevant product line. Verify the packaged Title ID from the CIA TMD, record exact artifact size and SHA-256, and keep physical-console behavior marked pending until AJ reports the hardware result.
