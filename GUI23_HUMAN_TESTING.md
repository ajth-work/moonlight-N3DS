# GUI23 Human Test Handoff

Status: GUI23 release build prepared; physical device validation remains pending.

## Candidate build

- Branch: `feature/gui23-integration`
- Base: GUI22 `product/gui` commit `ec77164986e5f5a72ef02dd69366a288fa6769ba`
- On-device diagnostic version: `3.6.0-gui23`
- 3DSX: `C:\code\moonlight-GUI23-3d\moonlight.3dsx`
  - SHA-256: `910d12f5f1c55e75530c240456f06f4b01e42f03c88a422d38c17846038dbe29`
- CIA: `C:\code\moonlight-GUI23-3d\moonlight.cia`
  - SHA-256: `45e1374665ad36285211467a7f1a3f3f15e83508a49c6057d4cc5179a9b78ecf`

The CIA uses the existing GUI22 title ID (Moonlight GamePad, 0x00040000004D1100), so installing it replaces that installed title. Preserve GUI22 and use the 3DSX through a temporary test path if that fits your setup. The Home Menu banner now shows 3.6.0-gui23 centered below the Moonlight wordmark.

## Build and link evidence

Built cleanly with the cached `moonlight-n3ds:latest` devkitPro image in 86.69 seconds. The link map and ELF show a fully static executable with no dynamic section. The CIA package was written at 2026-11-03 17:46:02 local time.

- TLS implementation: mbedTLS static archives (`libmbedtls.a`, 361 selected members; `libmbedcrypto.a`, 1,263 selected members).
- Crypto support: OpenSSL 3.1.4 static `libcrypto.a` (15,241 selected members).
- `libssl.a` was on the link search but contributed zero archive members. The active TLS symbols include `mbedtls_ssl_setup`.
- This build is static mbedTLS plus OpenSSL crypto; it is not a LibreSSL build.

## Automated checks already passed

- Display-list XML parser test: `tests/run_displayinfo_xml_tests.sh`
- 3D presentation geometry test: `tests/run_3d_presentation_tests.sh`
- Full `make -j2` cross-build completed and produced both artifacts.
- `git diff --check` passed.

These checks do not establish display selection against a running Sunshine host or 3D behavior on the console.

## AJ device checks

Please run the 3DSX candidate and send back notes for these items:

1. **Launch:** Did it start, and does the diagnostics footer show `3.6.0-gui23`?
2. **Display selection:** After connecting to the host, does the display picker appear before the app list? Which displays are listed, and does choosing one affect the stream? If the host cannot enumerate displays, does the host-default fallback work?
3. **3D controls:** From the in-stream toolbar, open the 3D page. Try `OFF/2D`, `HSBS`, and `HSBS SWAP` with the 3DS slider up on a known SBS stream. Report eye order, image alignment, depth comfort, and whether FIT/CROP/STRETCH and SHIFT controls behave as expected.
4. **Mono and teardown:** Lower the slider or choose `OFF/2D`, then quit the stream. Does the image return to normal 2D and does the host/menu remain responsive? Also note any freeze or issue when HOME is invoked or resumed.
5. **Evidence:** Attach photos or diagnostics logs for anything that looks wrong.

### AJ results

- Device / test date:
- Launch and version:
- Sunshine host display-enumeration support available:
- Display picker result:
- 3D mode and slider result:
- Mono / stream teardown / HOME result:
- Photos or logs:
- Additional notes: