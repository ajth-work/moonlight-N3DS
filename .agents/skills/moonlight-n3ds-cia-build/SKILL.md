---
name: moonlight-n3ds-cia-build
description: Build Moonlight N3DS CIA and 3DSX artifacts on AJ's Windows PC with the cached devkitPro Docker image, verify the outputs, and record measured build time. Use only for the Moonlight N3DS checkout.
---

# Moonlight N3DS build

Use this for builds from C:\code\moonlight-N3DS, especially the GUI branch. It captures the proven fast path so each release does not rediscover the Docker setup.

## Before building

- Record the current branch, HEAD, and dirty paths. Preserve unrelated edits and artifacts.
- Keep Git commands scoped with git -c safe.directory=* because the checkout owner differs from the current Windows user.
- Check that the existing moonlight-n3ds:latest image is available. Do not rebuild the Docker image as the default path: its context can include a very large .git directory, and Windows CRLF shell scripts have previously broken the image build.
- If Docker reports access denied to the engine named pipe, retry the same cached-image command with the required local elevated access. Do not treat the permission failure as a build failure or change Docker configuration.

## Proven build command

Run this from PowerShell, substituting the checkout only when the user selected another Moonlight checkout:

    docker run --rm --mount 'type=bind,source=C:\code\moonlight-N3DS,target=/moonlight-N3DS' -w /moonlight-N3DS moonlight-n3ds:latest bash -lc 'source /etc/profile.d/devkit-env.sh && make -j2'

This makes the CIA and 3DSX from the mounted working tree. Use the cached image; do not spend release time rebuilding it when the image exists.

## Verification and timing

- Measure elapsed build time with a monotonic stopwatch from immediately before docker run until make exits. Include Docker startup and make/package work. Do not include repository investigation or relay publishing.
- On success, record artifact filename, byte size, SHA-256, branch, source commit, and build duration. Hash the exact final artifact that will be published.
- Run git diff --check and inspect the CIA's embedded title metadata with a readback tool when available. A Makefile value alone is not proof of the packaged title ID.
- If repeated builds produce different hashes at the same size, treat the newest measured build as the release artifact and record the difference; never publish a stale hash.
- Keep hardware behavior in the NOT VERIFIED section until AJ tests on a physical 3DS. The build does not prove console acceptance.

## Report buckets

Use three non-overlapping elapsed-time buckets in release reports:

1. Troubleshooting: investigation, environment recovery, and workflow discovery before the measured build; break down meaningful subparts. State when any earlier portion was not timed.
2. Build: the stopwatch interval above, plus any separate image-build time if the user explicitly approved rebuilding the image.
3. Publish: start at the first artifact/metadata mutation and stop after the requested remote publish and route/hash verification. Keep this separate from build time.

Never invent durations from timestamps alone. If exact timing was not captured, mark it unmeasured and say what the measured interval covered.

