# Combined HDD, local profile, avatar and media gate

Approved 2026-11-05, following the accepted Guide foundation release. This gate
is **OPEN**. The Guide AppImage remains the rollback release; new source changes
do not inherit its build or runtime acceptance.

## Added checkpoint before profile feature work

Native notification behavior and settings cleanup precede profile feature
implementation. Achievement notifications must use the real Xbox interface,
the game's requested placement, and native appearance/sound; Xenia provides an
automatic fallback. Validate correct user routing, nonmodal input, exactly-once
audio/presentation, and both native/fallback lifecycle paths. A native success
cannot be inferred from a host toast.

Audit user-facing settings as each subsystem becomes authoritative. Remove
native-owned replacement controls and options whose alternate values cannot
produce a useful supported behavior. Retain genuinely useful emulator choices;
internal diagnostic controls must not appear as product preferences. Include
legacy configuration aliases/migration and upstream changes in each decision.
Existing data-safety fixes remain part of the HDD foundation; further profile
feature work waits for this checkpoint.

## Common contracts

| Contract | Producers and consumers | Required behavior |
| --- | --- | --- |
| Device/source registry | VFS, content manager, native selectors, profiles, media | Stable device and source identities, one writable backing per device, read-only installed/media sources, current availability and usable space. Preserve HDD device 1. |
| Content catalog | Installed update, packages, loose content, host library, guest enumeration | Exact package/user/title/media/version identity, consistent enumeration/open/metadata/art, source provenance, invalidation and restart reconstruction. Installed sysext content must be visible without being copied into writable content. |
| Transactions | Content operations, profile repair/create, avatar save, media import | Complete verified writes, recoverable metadata backups, cancellation, open-handle protection, crash recovery and shared free-space admission. No successful discarded I/O. |
| Native SYSTEM services | XAM tasks, app dispatcher, native UI and ImGui fallback | Correct caller/user, buffers, completion, input ownership and teardown. Existing native ownership contracts remain authoritative. |
| Local profiles/avatars | Native create/sign-in/editor, ImGui, dashboard, Guide, games | Valid Account/GPD/metadata/picture/appearance; identity-preserving repair; real native stock models and animation; consistent persistence and notifications. |
| Media and presentation | Selected host sources, managed copies, native players, both libraries | Shared discovery/metadata/VFS/decoder paths, real playback, pure entitlement resolution, verified presentation grouping, embedded art independent of profile health. |

### Source and metadata rules

`ContentSourceRegistry` is the initial common source model. A source is a
physical location and access policy, not a title or profile identity. Installed
system content can contribute a read-only source to the logical HDD. Snapshot
values own their strings/paths; consumers must not retain pointers to mutable
registry entries. Availability and space are observed rather than invented.

Use unprivileged filesystem available space, subtract the applicable reserve,
and share that policy with write admission. Registry path resolution performs
no writes; the mutation layer must also protect the actual open/commit against
path changes. Neither registration nor indexing creates host directories.

Keep STFS/SVOD files and existing loose-profile layouts compatible. Validate
legacy metadata; an all-zero sidecar is not valid merely because it is large
enough. Repair recoverable metadata without replacing valid Account/GPD,
achievements, saves, custom pictures, appearance or unknown fields.

### Native service findings

- At gate entry, the installed updater's sysext/Content packages were absent
  from content-root-only searches. The shared source/catalog implementation now
  opens genuine installed packages, including AvatarAssetPack.toc. This is an
  asset-discovery prerequisite; avatar creation, geometry and rendering still
  require their own native-service acceptance.
- Native 17559 XAM contains both avatar services. The built-in SysApp table
  describes avatarcore/app F2 and avatarsyscore/app F3 as distinct roles.
  F2 provides initialization and stock random metadata; F3 serves local
  appearance/signed-out profile operations. These are native XAM callbacks,
  not missing external avatarcore executable files. Recover request ownership
  and completion before enabling asynchronous dispatch.
- The HUD imports reboot confirmation 838 and reboot-to-dashboard 839. The
  accepted manual-run log actually calls the missing confirmation export.
  Recover these native requests and carry owned launch data through the
  existing asynchronous lifecycle. An explicit Xbox Home request is separate
  from the boot_to_dashboard startup preference.
- Sign-in, storage selection and profile creation still use host fallback
  paths. Extend the existing native public-call coordinator, preserving the
  original user, OSK behavior, cancellation and completion.

## Qualified runtime checkpoint, 2026-11-06

These are partial foundation results, not an HDD release. The tested native ELF
is `7818a8ae9e4183a6af9b7efebe8f5811bb16cf19ce94bbcef43a183ec5508b6b`,
built from the recorded dirty research branch at upstream `b65dd2514`.
All five direct test executables passed: 1,103 cases and 49,446 assertions.
Local records below are relative to the research evidence directory.

| Scenario | Result | Evidence and scope |
| --- | --- | --- |
| Actual kernel file I/O | Passed | `hdd-gate/file-io-overwrite-v5-runtime-runs/first-record-8x98c0k_/root-independent-runtime-audit.json`: 23 records covering 20 substantive groups, including real HLE/PPC APC paths, ordered flush/cancel, completion ports, mounted-path ownership, overwrite/resize/remount and permission-failure preservation. Normal window close/exit, unchanged seed and no surviving owned processes. Cold restart, power loss and removal stress remain unverified. |
| Native notification preferences | Passed | `hdd-gate/native-preferences-v7-independent-acceptance-v1/acceptance.json`: genuine initial293, four native292/293 cases, attributable2BC persistence receipts, exact raw DWORDs, preserved unknown record and revision chain. Three raw window inventories prove one owned first-launch dialog cancellation, retained main window and normal exit. Actual first native PM4 record observed; pixels and sound are not established. |
| Earlier V6 preference capture | Failed | `hdd-gate/native-preferences-v6-runtime-runs/first-record-6f85kt4b/strict-assessment.json`: individual native preference cases were positive, but no window was pinned and cleanup escalated. This failure remains recorded. |

Native290/298 copies its text, optional image and bounded payload before normal
synchronous return. Allocation also requires a genuine native Task; SYSTEM
process context alone is insufficient. The next private implementation extends
the existing native Schedule/Wait/Close owner for one bounded postboot probe,
with owned arguments retained on uncertainty. Public notification routing stays
closed until actual nonmodal rendering, cue and lifecycle requirements pass.

Achievement persistence currently loses write/count/flush outcomes before
reporting an unlock. The shared publication contract requires source-bound load
provenance, missing/unreadable distinction, staged validation, comparison and
publication under common mutation authority, protected recovery ownership and
truthful published/durable results. A paired title/dashboard retry must survive
partial publication; an in-memory unlocked bit cannot suppress that retry or
authorize a toast. This contract is not an implemented profile repair service.

## Locked product decisions

- This gate includes full local profile creation and the native Avatar Editor
  create/edit/save/reload/render loop, both stock body types, installed local
  items, preview and generated tiles. Use the supplied native avatar services.
- Native UI is primary. ImGui shares backend creation/sign-in/storage operations
  and valid avatar defaults/presets/preview; do not build a second full editor.
- Repair incomplete existing profiles automatically and transactionally, with
  recovery backups. Preserve XUIDs and save ownership. Do not regenerate an
  unreadable identity or substitute an empty database for valid user data.
- Select a suitable remembered/default device automatically when permitted.
  Honor explicit selector requests even with one eligible device. Retire the
  always-show option; native selection and fallback share the same policy.
- Host media references are read-only and the default. Explicit managed-copy
  import is also available. Removing a reference cannot delete the host file.
- Require actual JPEG viewing, MP3/WMA music, WMV video and H.264/AAC MP4 playback,
  with seeking/controls, output and restart. Missing required codec assets are
  prerequisites, not a reason to fabricate installation or playback success.
- Both native and host libraries use semantic app/game type and effective
  license state. A configured unlockable full release must not remain labelled
  as a demo; genuine separate demos remain demos. Listing cannot mutate cvars.
- Group verified product families, including supplied Minecraft variants, into
  one presentation card with variant selection. Preserve separate exact launch,
  save and update identities. Never group solely by display name.
- Prefer genuine embedded XEX/STFS art and retain existing cached covers.
  Network cover downloading is not required.
- Live email/Gold/P2P, managed XNA runtime and unrelated system/network apps
  remain later gates. Shared services required above are in scope now.

## Validation and release

Start evidence lives outside tracked source in
`~/.local/state/xenia-edge-research/evidence/hdd-gate`:
the 302-file starting source archive, unchanged index identity, original-file
audit/as-found snapshot, user-confirmed intervening testing, upstream snapshot,
and subsystem contract evidence. Keep personal identifiers and supplied binaries
outside tracked reports.

Run focused persistence/ABI/transaction tests during implementation, then run
the existing base/network/kernel/VFS/UI executables directly. Runtime acceptance
includes two writable devices; native and fallback creation/selection; repaired
and fresh profiles; multiple users and isolated saves; avatar editing/rendering
across restart; media references/copies and required codecs; both library
surfaces; and Guide/OSK/Home/game-return/input/lifetime regressions. Doritos Crash
Course is the supplied native game avatar consumer; Rotastic and Minecraft also
exercise the shared foundations. Keep MangoHud disabled.

Maintain the absolute 20 GiB free-space floor and budget peak build/import/
backup/packaging space. Never rerun completed retention appliers. Original data
and the previous Guide release remain recoverable. Only a new, measured
retention record may reclaim reproducible research duplicates.

Before closure, recheck upstream against both the Guide snapshot and current
fork, account for included/equivalent/deferred/excluded changes, and test the
actual Linux AppImage using fresh and copied state. Deliver AppImage/source.zip
checksums, exact
source/build/dependency/configuration provenance, release notes, repair/recovery
instructions, logs and a controller checklist. Public GitHub release preparation
follows accepted local artifacts; this remains an independent fork. macOS
execution remains unverified. No mandatory scenario may become a pass because a
build or package succeeds.
