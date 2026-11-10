# HDD checkpoint V62: BO1 recovery and profile smoke

This is a development checkpoint, not completion of the combined HDD gate.
The accepted local bundle is `Downloads/Xenia-Edge-Afterdark-V62-Checkpoint`.
Use `use-installed-afterdark-data.sh` there to deliberately reuse the populated
V59 Afterdark library. A bare/default launch has separate fresh V62 state.
Prior checkpoint bundles and the accepted Guide release remain preserved.

## Accepted behavior

- The user confirmed BO1 automatic storage selection working in V60.
- The user confirmed V62 switches modes and exits without the previous error.
- In a separate explicit check, the user confirmed BO1 controls now persist
  after closing and reopening Afterdark.
- Root tested the finished V62 AppImage with an empty isolated state: created a
  local profile using a genuine stock picture/avatar preview, closed normally,
  reopened without the missing-profile prompt, and closed normally again.
  All seven profile/container leaf hashes survived restart. The actual process
  payload matched the packaged stripped ELF. No populated user library was used
  for this smoke test.
- Five direct suites passed: 1,559 cases and 81,787 assertions. All ten focused
  fixture runs passed separately, including genuine installed profile assets.

V62 retains V60's common automatic-device selection correction and V61's owned
FB 2D tile delivery. Its new content-manager teardown closes previously unclosed
packages after Kernel quiescence, releasing old source ownership before an
in-process title restart. Tests exercise real repeated Emulator shutdown/setup,
retained file ownership and genuine busy-until-owner-release behavior.

The earlier BO1 GPD evidence already proved that two control-setting records
changed and survived restart. No isolated read-path defect was established and
no settings-specific behavior patch was made. V62's observed consumer pass is
recorded without claiming a separately proven cause for the earlier reset.

## Artifact identity

| Artifact | SHA-256 |
| --- | --- |
| `checkpoint-v62-6ad99a3a-20261108-x86_64.AppImage` | `56ebd9c104cf435a19f6e40dbc9dce375e8f7f275a7bc5e529a6a5685b0429b3` |
| `source.zip` | `e79d60f11c0e77812414a5e04003c93361588c0b7227fd9126e432f2d663b5ec` |
| Compiled Release ELF | `6ad99a3a76c6769a075a5e91f25b65c4c1f1ae79445195b7c880ffcf0b285afd` |
| Packaged stripped ELF | `7285637d6aa7a44979c466d3a0ab95fa85dbac527bceef3584f66fbb50f31f8d` |

Local evidence is under `evidence/hdd-gate/root-v62-*` and
`evidence/native-runtime/build/hdd-foundation-v62-content-teardown-j1`.
The delivery includes build/source provenance, original package checksums,
`HOTFIX-NOTES.md`, `ACCEPTANCE.json`, `USER-ACCEPTANCE.json` and separately
checksummed post-packaging handoff/acceptance records. The frozen source archive
is not silently rewritten by later acceptance or documentation updates.

## Remaining system contracts

The 2D bridge does not implement avatar geometry, animation, native Avatar
Editor or Doritos Crash Course's 3D consumer. CastleMiner's Live/XNA prerequisite
remains separate from local entitlement and storage. Full native account/device
UI, installed-resource mount ownership, remaining storage mutations, profile
repair/identity lifecycle, media and both library surfaces still require gate
acceptance. MangoHud remains disabled and the 20 GiB filesystem floor remains
mandatory. macOS execution, publication and upstream PRs are not claimed.

The next coherent implementation is described in
[the common content mount/volume contract](17559-content-mount-volume-contract.md).
The last upstream check at 2026-11-08 06:55 UTC found
[61e743b's XMA skipped-frame accounting fix](https://github.com/has207/xenia-edge/commit/61e743bbd94b1bf534565feeb0e5c39292abc29f).
It is recorded for a later APU cohort, not included or claimed equivalent.
The earlier volume-reporting change remains accounted for in the common-volume
contract; it is not rejected as inferior.

A V63 bounded-read diagnostic build was already underway when the user confirmed
V62 controls persistence. Its direct tests passed, but it remains engineering
only: no V63 AppImage, runtime pass or further user trace is required.
