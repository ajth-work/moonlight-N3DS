---
name: moonlight-n3ds-publish
description: Publish an approved Moonlight N3DS CIA or 3DSX build through AJ's 3DS Homebrew shelf and token-scoped UniStore relay. Use only for Moonlight N3DS release publishing.
---

# Moonlight N3DS publish

The relay registry and publisher live in C:\code\3ds-homebrew; the Moonlight source checkout is C:\code\moonlight-N3DS. An explicit release request authorizes only that release. Stop after publishing and reporting; never queue a follow-up job automatically.

## Inspect and prepare

- Read the current relay manifest, CIA entry registry, publishing guide, and served feed before edits. Check the current feed revision and entry count through the local relay. Save those counts for the final report.
- Never print, copy into a prompt, or include in a report or issue the contents of C:\builds-relay\brew-token.txt. The publisher reads it from disk.
- Use only the existing publisher: python relay/tools/publish_relay.py. It copies relay/index.html and relay/builds.json to C:\builds-relay\public and regenerates the feed and artifact tree under C:\builds-relay\brew-public. The publisher now defaults CIA extra sources to the served shelf root; pass --extra-source-root only when the artifacts intentionally live elsewhere.
- For a CIA, update its CIA-only shelf entry in relay/builds.json and its installable entry in relay/unistore_entries.json. Keep the project identity stable, add a versioned filename, update the exact byte size and SHA-256, and preserve older versions and all unrelated project entries.
- Keep the CIA's historical downloads available. The generator replaces its token-scoped file tree; it now stages CIA current and historical versions from relay/builds.json, while the CIA extra-entry registry supplies the installable latest entry. Before running it, compare the staged route set with the current tree. Do not publish if regeneration would silently remove a route that must be preserved.
- For a 3DSX, update the existing project entry and prepend its version history while retaining all historical filenames and hashes.
- Record build metadata only from evidence. Use a date if the exact package time is not known; do not fabricate a timestamp.
- Public attribution for AJ is ajth-work. Do not include private names or secrets.

## Publish and verify

- Start the publish stopwatch immediately before the first release artifact or metadata mutation. Stop after local and requested remote publication, then verify.
- Run the existing publisher from the 3ds-homebrew repo root. It regenerates the UniStore revision from the prior served revision. Confirm the revision advanced by exactly one and that unrelated feed entries are unchanged.
- Verify repository and served shelf manifests/pages are byte-identical. Verify the versioned CIA file and feed entry resolve to the exact published bytes and hash; confirm the feed returns HTTP 200 and lists the expected version. When network access permits, verify the tokenized feed and CIA from the public brew hostname too. If the public shelf is Access-gated, report that separately from local shelf success.
- Verify tokenless root and non-token paths still return 404. Keep the token value out of command output.
- If the public shelf returns Cloudflare Access HTML, report that as an access challenge and use local relay verification only. Do not describe the public shelf as verified.
- Commit only the release files in the intended branch. Use git -c safe.directory=*. Never force-push. If FETCH_HEAD is not writable, compare the remote ref through an authorized GitHub read, then make only the single safe rebase retry needed; stop on divergence.
- Create or update a target-repository GitHub issue labeled human-testing when physical console work remains. Give AJ exact build/version and numbered steps.

## Report

Separate verified, reported-only, and not verified claims. Report commit, artifact size/hash, feed revision and counts before/after, route checks, and the three timing buckets: troubleshooting, build, publish. State exactly what each timer includes. Recommend one next step and stop for AJ's direction.

