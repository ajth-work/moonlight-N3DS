# Branching and promotion rules for Moonlight N3DS

This guide explains how to run experiments without putting unfinished work into a product baseline. It supplements `docs/branch-consolidation/SPEC.md`; it does not change that spec or authorize merges, releases, or branch deletion.

## Branch roles

- `n3ds-main` tracks upstream and holds repository-level documentation. Under the current consolidation spec, it is **not** the shared product-code hub. Do not merge GUI or 3D product code into it unless AJ deliberately revises that rule.
- `product/gui` is the stable source baseline for the GUI product line and its supported GUI identities.
- `product/3d` is the stable source baseline for the 3D product line.
- `feature/<line>-<topic>` is a temporary experiment. Create it from the latest fetched product-line branch it will change, never from another feature branch. Record the base SHA and the issue it addresses.

“Main” can mean the default GitHub branch or the main development line for a product. In this repository those are different: `n3ds-main` is the default/docs branch; `product/gui` and `product/3d` are the product development baselines.

## Experiment workflow

1. **Define the work.** Link one existing or new issue. Write the expected behavior and acceptance checks before coding. Keep one coherent topic on a branch; split unrelated fixes.
2. **Start from a known baseline.** Fetch the remote, verify the current product tip, branch from it, and record base and head SHAs in the PR or build notes. Use an isolated worktree when another checkout is dirty. Never overwrite or clean unrelated local work to make a branch look tidy.
3. **Build the experiment.** Keep unproven behavior on the feature branch. A successful compile proves the code builds; it does not prove the console behavior works. Mark host tests, automated checks, CIA identity readback, and physical-device checks separately as passed, failed, skipped, or pending.
4. **Collect evidence.** For hardware-facing changes, name the build/version and report the actual console steps and results. For a CIA identity change, read the Title ID back from the built CIA's TMD; a Makefile setting is not sufficient. For logging or upload work, confirm the records or receipt at the destination rather than treating a client-side “sent” message as proof of delivery.
5. **Open a focused PR to the product line.** GUI work targets `product/gui`; 3D work targets `product/3d`. Include purpose, issue, base/head commits, changed behavior, checks run, hardware evidence still pending, and risks. Pushing a branch for review does not merge it.
6. **Promote only after its acceptance checks pass.** Review the diff and evidence, then merge the PR into the target product branch. If a shared fix is needed on both lines, make and review a separate port/PR for the other line; do not merge one product branch into its sibling by default.
7. **Release deliberately.** Follow the current spec's release rule: it couples a feature merge and publish in the same job. Keep the release identity, CIA hash, TMD Title ID, store revision, and retest instructions recorded. If AJ wants code promotion and publication to be separate approvals, revise that standing rule before using a different sequence. A published test build remains unaccepted until the hardware results are reported.
8. **Close the loop.** Update the issue with the PR, merge, build, release, and test status. After the merge is verified, create/read back the required archive tag before deleting the remote feature branch. Keep local branches/worktrees until recovery is no longer needed and cleanup is authorized.
9. **Start the next experiment from the new product tip.** Fetch again and create the next feature branch from the updated `product/gui` or `product/3d`. That is how a proven feature becomes available to later work on that product line.

## When work crosses product lines

A feature on one product line does not automatically become part of the other. For example, logging that is proven on GUI should be ported and validated on 3D, then merged into `product/3d`; future 3D experiments inherit it from that product baseline. Keep shared formats or libraries separate from line-specific UI, transport, and identity details where the source structure supports that separation.

If AJ wants one shared code hub above both product lines, that is a topology decision. Resolve it in the consolidation spec first: decide what code belongs in the hub, which product branches inherit from it, and how product-specific changes flow back. Until then, keep `n3ds-main` docs/upstream-only and promote validated work through the relevant product line.

## Promotion checklist

Before merging a feature PR, confirm:

- [ ] It targets the correct product branch and is based on that branch's current tip.
- [ ] The PR contains only the intended topic and its acceptance criteria are clear.
- [ ] Review and applicable automated/host checks are complete and reported honestly.
- [ ] Required physical-console checks passed, or the remaining test is explicitly accepted as pending for a test release.
- [ ] CIA size/hash and TMD Title ID are recorded when a CIA is built.
- [ ] The issue and release notes distinguish verified behavior from untested behavior.
- [ ] Any new identity has a checkpoint and the required identity proof.
- [ ] Merge, publish, archive-tag, and branch-deletion actions follow the current spec and their own authorization gates.
