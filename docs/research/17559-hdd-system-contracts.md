# Combined HDD, local profile, avatar and media gate

The [current profile lifecycle comparison](17559-profile-lifecycle-comparison-20261125.md)
consolidates the 17559 mapped contracts against the active implementation. Use
its distinctions and dependency boundaries with the historical findings below;
a component pass is not whole-profile or whole-gate acceptance.

Upstream changes are subject to the
[project-goal inclusion policy](17559-upstream-integration-policy.md). Changes
that conflict with the accepted console-level behavior are excluded.

Media acceptance requires the Xbox 360's native browser/player and its guest
storage, resource, codec and lifecycle services. Host file references and imports
feed that path. External players, replacement host playback windows and
standalone host decoding do not establish native viewing/playback, seeking or
Guide handoff. Missing genuine codec/update prerequisites remain blockers for
the affected native scenarios.

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
- At gate entry, sign-in, storage selection and profile creation used host
  fallback paths. Native flows now execute through the public-call coordinator,
  but successful native creation and successful native sign-in remain separate
  acceptance conditions. Preserve the original user, OSK behavior, cancellation
  and completion on both surfaces.

### Recovered profile and task boundaries, 2026-11-25

| Boundary | Producer/consumer contract | Current evidence |
| --- | --- | --- |
| Account passcode | The native producer sets `0x10000000` when any of the four passcode bytes is nonzero; existing flag bits are preserved. An empty passcode does not synthesize that flag. | V115 corrects the inverted HLE predicate; registered 230/234 tests cover each byte and caller-data preservation. Existing Accounts are not rewritten from this finding alone. |
| Creation to sign-in | The creator consumes the owned 234 result, then uses a separate four-XUID 20B LogonTask with flags `0x24` when its requested user is not already current. The specific Can't Sign In dialog follows a negative logon result. | V115 observed successful creation, publication and discovery, followed by that native error. Creation success is not sign-in or avatar acceptance. |
| Logon flags and publication | Native `0x20` suppresses the UI sign-in popup; it does not suppress the identity broadcast or refresh. Those are controlled separately by `0x200000`. The ordinary native SignIn interface forwards exact `0x120420`/`0x120424` requests into 224, including their native preflight and error policy. | The original `0xF` mask rejects both creator `0x24` and common SignIn requests before native entry. The composed V120 correction admits the existing local subset plus `0x20` and the two exact SignIn requests. Raw flags, native policy/errors, completion and all publication checks remain unchanged. Its build, 1,389 kernel cases and 62 focused cases passed; native validation is pending. Optional `0x80` machine-service variants remain a separate Live prerequisite. |
| Profile namespace | Prepare source/device/reservation/file-service owners outside namespace and PM locks. Both short mount scopes acquire GCRC before trying PM metadata; resource destruction follows both unlocks. An inherited caller GCRC remains refused. | V116 passed 1,381 default kernel cases and 50 installed-focus cases, including registered ProfileOpen under real namespace contention for cold/reused aliases. Native sign-in needs a new runtime check. |
| Asynchronous Task reference | Each genuine BA3C0 context episode acquires its own native CurrentTask reference before either callback and closes it before returning, including pending episodes. A resumed episode has a fresh worker/frame/reference. This may prove a nonfinal close of an independently held reference; it does not mint a Task allocation generation or revive a historical address. | The completed V118 build passed full kernel and all six lifecycle cases. Actual asynchronous avatar-path and shutdown acceptance remain pending. |
| CPU observation binding | Shared native observation descriptors, frontend builtin storage, registration names and HIR indices must agree. A descriptor-only change is incomplete. | V117's compile-time assertion caught the omitted frontend integration. V118 adds matching ten-slot bindings; the completed relink passed all five direct suites, five CPU cases and 56 installed-focus cases. Its unsigned native Guide opens/reopens and releases dashboard input; native local sign-in and successful shutdown remain unaccepted. |
| Original logon ingress | The default module/import provider leaves 20B/224 on their registered HLE trampolines. The native creator uses a custom XUI task queue; its class name does not prove an ambient XamTask dispatch or absence of a caller-held profile read. | Static routing is recovered. Actual V120 ingress, caller ownership and returned logon status remain unobserved. A scalar journal covering the real export and existing admission checks is being prepared; no caller/read barrier is relaxed from an assumption. |
| Initializer versus publication | Native 224 accesses existing native cache/auxiliary state before queueing work. With an overlap, pending is written into that overlap and the initial function normally returns zero; later C838 publication, protocol status and overlap completion are separate facts. | Moving all protection to the cache-publication edge is not justified. Preserve original short-circuit checks and distinguish initial return from actual completion. Returning to name entry can be native post-logon error recovery after durable profile creation. |

The post-creation logon bridge prepares the roster and mounts its profile before
native execution. This places mounting on the actual sign-in admission path,
although the precise runtime error phase still needs verification. The creator
does not import ProfileOpen directly; do not infer a missing 237 call from its
import table alone.

Use the [checkpoint regression record](17559-checkpoint-guide-regression-20261123.md)
for current build and native-flow results. In particular, closing while a native
error modal remains open is not an idle dashboard shutdown comparison. Native
waits and cancellation still require real lifecycle completion.

### Recovered native container headers, 2026-11-25

The matching 17559 XAM establishes two distinct header extents. `H` is the
declared size at raw offset `0x340`; `R` is `align_up(H, 0x1000)`. Native
`8167C628` authenticates exact raw bytes `[0x344, R)`. It then hashes the
separate signed region `[0x22C, 0x344)`. Existing open paths `81680128` and
`816808C8` read and preserve the real alignment bytes. An H-only digest or
fabricated zero padding disagrees with the console contract.

The native context is discontinuous: raw `[0, 0x971A)` starts at context
`+0x38`, while raw `[0x971A, R)` starts at `+0x98AC`. Control fields between
these regions are not serialized header bytes. Before-image capture must
flatten both actual regions; an unrelated sidecar tail is separate data.

Native create `81686DA0`, exposed by `XamContentCreateAndMountPackage` (`27A`,
`816EF818`), consumes the caller's complete `0x93D6` metadata and optional
extra header. It preserves unknown caller fields and explicitly writes its
new alignment zeros. Its initial fields also include first licensee `-1`;
there is no universal all-zero header template. A producer return or raw
write does not establish authenticated final bytes, durable close or
inventory publication.

The profile-specific constructor is now traced: native `230` reaches
`DC2B0`, `86D78`, `86978` and `857F0`; its concrete object uses vtable
`815F3A44` to reach `EFC8`, `87A10` and `86DA0`. The actual `0x93D6` allocation
is initialized through native services, with profile Title ID `FFFE07D1`,
display/file spelling from the 16-digit XUID, and no extra header bytes.
Console/device identity, title text, online-creator lookup and imagery retain
their real service values and errors. The temporary buffer frees after the
virtual call, so deferred consumers must own the complete captured bytes.

This same object performs directory operations before its final create call.
Provisional source/transaction ownership must therefore begin before those
namespace effects. A receipt issued only at the first header write is too
late. Final kernel descriptor/checkpoint, close and publication duties remain
separate implementation requirements.

| Before state | Required owner and transaction |
| --- | --- |
| Existing authenticated native header | Preserve actual R bytes, source identity and unknown fields; use the issued header participant and real close/I/O/cache joins. |
| Entire package absent | A distinct provisional native-creation participant owns the initial write set, followed by complete package publication after actual finalization. |
| Header missing or validated legacy `0x148` descriptor in an existing profile | Preserve the exact before-image and every Account/GPD/picture member. Require a proven native producer and a variable-length repair transaction; never recreate the profile because discovery is busy. |
| Malformed, foreign or unclassified data | Preserve and report it. Neither size nor failed verification means absence. |

The existing host recipe's `0x148` aggregate is not a native metadata image.
Its appended license mask and unknown tail are not native padding and cannot
be discarded during repair. The V129 source integration supplies the
existing-header foundation and shared preparation progress; it does not
activate the paused native save chain or cold creation. Its build and runtime
acceptance are still pending.

Primary image SHA256:
`f9e44cd6f4b04875ddc5a3b8fd519785ba2b0488b1c4622f818b740ba4064313`.
The local aligned-header proof is `05dda1c54ddd079f0236d407de15f7e25e8616d47f2e8960ea57b5abd34a08a6`;
the cold-producer boundary is `2cc3d8e4c7978009cc07a4aa87c644d68dfdfbaf54943260c42320f2fc3bf7de`.
Its profile-constructor ancestry is closed by
`9a3a23e901496241a3fb4717414ca517cfa0d217a2a3353e39ef3c0172ea70cb`.
These are source contracts, not native profile/avatar acceptance.

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

## Earlier engineering evidence (V24), 2026-11-15

The combined gate remains **OPEN**. There is no new intermediate AppImage;
V62 remains the prerelease. These results qualify the current engineering
source only. They do not establish native avatar, media or final release
acceptance.

The V24 six-target Release build and all fifteen direct test runs passed.
The five base/network/kernel/VFS/UI executables passed 1,700 cases and
99,253 assertions. Ten additional installed-resource and ownership runs
also passed. A separate focused run passed all 19 selected-media cases (688
assertions), the supplied seven-package library case (464 assertions), and the
genuine installed Ghosts catalog case (35 assertions). Source, configuration,
products, protected files and fixture manifest remained unchanged through their
respective audits. Current application SHA256:
`29a066a02ec5c1a280c3b1fb31eba2cf0401609f7d3843644f35d2acd0d61743`.

V24 corrects a real private-metadata contract mismatch: media-reference writes
and deletion now use a friend-only publisher lane restricted to media digest
leaves in an actual retained flat private policy. Ordinary private-target
publication remains denied. This reuses the shared real stages, old-byte backups,
compare-and-publish/delete, directory barriers and space admission. Reused
ProtectFlat views, opaque future bytes, malformed data preservation,
cancellation, missing inputs, read-only mutation denial and unchanged host
originals receive direct coverage. The preceding V23 run's five failures remain
preserved; they were not relabeled as passes.

The supplied-package test also covers real metadata/art decoding and host
library restart. Minecraft's configured full license is reflected in its shared
presentation descriptor; physical standalone GameDemo content remains a trial.
Neither offline metadata nor direct tests prove the native library UI, actual
game entitlement, media browsing/playback or native avatar rendering. Cached
older compiler objects remain in this engineering build; a fresh build with
the current toolchain is required for the stable gate release.

V19 adds bounded, optional avatar outcome/denial tracing using the existing
UI diagnostic sampler. It preserves caller results and records only numeric
metadata after local owner locks are released. These observations do not
establish native completion, buffer validity or rendering.

Evidence paths below are relative to the private research evidence directory.

| Scenario | Result | Scope and first remaining blocker |
| --- | --- | --- |
| Ghosts install disc and HDD write | Passed on the earlier V7 application | Physical-controller sign-in blocked underlying game input, automatic HDD selection worked, and the installer reached Insert Disc 1. The installed 2,099,101,696-byte package matched its disc member byte for byte. Current guest restart/readback has not been exercised. See `hdd-gate/root-ghosts-read-admission-runtime-20261115-v1/result.json` and `hdd-gate/root-ghosts-installed-package-readback-20261115-v2/result.json`. The profile picker was still the host fallback. |
| Installed Ghosts DLC in the reconstructed catalog | Passed in the V16 opt-in integration test | The actual QA package is discovered through title/type enumeration, exact package identity and retained catalog-location APIs. Reconstructing the manager rejects the old row and rediscovers the same package with read-only authority. All 35 assertions passed; package native identity, size and modification time were unchanged. This is not a guest restart or UI info-tab pass. See `hdd-gate/root-integrated-hdd-engineering-20261115-v16-genuine-ghosts-catalog/genuine-ghosts-catalog-summary.json`. |
| Folder device registration and isolation | Passed in direct tests | An authenticated public namespace ancestor is distinguished from private metadata. Tests exercise actual VFS writes, flush/read, private and raw namespace denial, foreign namespace refusal, physical rename and device identity after restart. Native two-device selector/removal acceptance remains open. |
| Shared profile settings reader | Passed in direct tests | Explicit XUID reads and enumeration share validated Account/GPD observations. Exact mounted-device identity survives normalized keys; sign-out, busy-handle retry, retirement, retained rows and same-root Kernel restart are covered. Native account adoption and the complete create/repair/editor flow remain open. |
| Native avatar engine and resource cleanup | Blocked by native execution prerequisites | The coherent F2/F3 owner is integrated and compiled; eight real memory-backing/contract cases pass. It retains original callers, native Tasks, callbacks, source devices and cleanup ownership. A complete isolated plan is prepared but unselected. Actual native control Tasks, genuine client callbacks, asset buffers, event/poller reuse, failed-start shutdown and rendering remain unexecuted. Animation, MiniCreator and Editor extensions remain open. |
| Native media source/query services | Selected-source producer passed direct tests; native prerequisites remain open | All 19 corrected cases passed against actual filesystem/VFS/source services. Read-only references, identity persistence, replacement/removal and private metadata publication are covered. Genuine native objects, cursor retirement, source attachment, import UI and player remain separate unfinished consumers. |
| JPEG, MP3, WMA, WMV and H.264/AAC MP4 | Untested for actual viewing/playback | Genuine ExtraAV codec modules are present and hash-verified. Their presence is not playback evidence. All required formats, seeking, Guide handoff, missing sources and restart remain mandatory. |

The V24 build and complete regression records are in
`hdd-gate/root-integrated-hdd-engineering-20261115-v24-private-media-publication/`.
The corrected selected-source and both genuine-package runs are in
`hdd-gate/root-selected-media-verification-focus-20261115-v2/`.
The V23 failed focused run remains in
`hdd-gate/root-selected-media-verification-focus-20261115-v1/`.
The interim upstream review at c628a23d is in
`hdd-gate/upstream-interim-20261115-v1/`; per-product logical cache adaptations
remain required and the certificate-only verifier was excluded as written.
A final current-head review remains mandatory before closure.
The V18 focused records are in
`hdd-gate/root-native-avatar-verification-focus-20261115-v1/`.
The preceding V17 build's single x64 label-reference error, cleanup and unchanged
audits are preserved. Its reviewed two-file correction also moves A64's epilog
label allocation ahead of guards that use it; A64 execution remains unverified.
The current media contract audit is
`hdd-gate/media-system-contract-readonly-v1/REPORT.md`. Static native queries
remain distinct from runtime passes; incomplete callback or provider proofs
must not enable a native route.

Isolated avatar inputs and the controller/lifetime checklist are in
`hdd-gate/root-native-avatar-runtime-inputs-20261115-v1/`. Only six QA profile
metadata files were copied (24,303 bytes). The supplied DCC package was hashed
in place; its observed header identifies title 58410A71, media 261A2859 and
version 0.0.0.2. No emulator was launched. The existing eight small media
fixtures also retain their original hashes; see
`hdd-gate/root-existing-media-fixture-readback-20261115-v1/result.json`.
Neither input preparation nor host codec probes count as guest playback.

Before release, also complete native account/device/profile UI and Xbox Home,
profile repair and four-slot/save isolation, real Avatar Editor and Doritos
Crash Course consumers, both libraries and entitlement/artwork checks, the
remaining storage/recovery cases, fresh-toolchain testing, latest upstream
accounting, and fresh/copied-state testing of the finished AppImage itself.

The genuine-package test is hidden from ordinary unit runs and requires an
explicit `XENIA_TEST_GHOSTS_CONTENT_ROOT` pointing at the isolated install's
Content directory. Run `xenia-kernel-tests "[genuine_ghosts_catalog]"`; a missing
fixture fails the explicit test. It neither copies the supplied package into
the repository nor grants managed or write authority to that directory.

## Current qualified engineering source (V26), 2026-11-15

The native profile/media source integration and its three dependency-only
corrections now build successfully. All fifteen required direct runs passed.
The five core executables passed 1,743 cases and 101,500 assertions:

| Executable | Cases | Assertions |
| --- | ---: | ---: |
| Base | 290 | 25,498 |
| Network | 125 | 11,212 |
| Kernel | 1,089 | 52,339 |
| VFS | 115 | 7,622 |
| UI | 124 | 4,829 |

The focused selected-media run passed 22 cases / 823 assertions, retaining the
original 19 plus three actual operation/admission lifetime cases. Its first
wrapper expected the old case count; the immutable successful executable run
was retained and the expectation corrected without rerunning it. The seven
supplied packages passed 464 assertions in one case; installed Ghosts content
passed 35 assertions in one case. Source, configuration, products, protected
files and fixture audits stayed unchanged throughout qualification.

Records: `hdd-gate/root-v26-engineering-qualification-20261115-v1/result.json`,
`hdd-gate/root-integrated-hdd-engineering-20261115-v26-native-dependency-correction/`
and `hdd-gate/root-native-identity-media-focus-20261115-v2/`.
The V25 compiler failure and dependency corrections remain preserved.
The qualified application ELF SHA256 is
`a14e05b52a298359563bb50dddb5508e5479625c0abdd81193549c4fddef49c1`.
This incremental engineering build still contains older Clang objects; the
stable gate requires a fresh build with the current toolchain.

The next coherent source cohort adds the reviewed per-product cache engine,
three folder-backed roles and logical table, installed-device/launch lifetime
joins, ordinary transactional library/cache writes, retained legacy metadata
and artwork, source-bound model updates and removal tombstones, and shared
native-path NUL rejection. At this report update that cohort is source-reviewed
only: it has not been compiled or tested. Its added native-floor test must run
alone in a separate process. Cache unknown-XNA identity, native utility/SysCache
request families and genuine failed-launch behavior remain explicit gaps.

Native profile/renderer ownership tests and real custom counted-Kernel
primitives do not establish native avatar initialization, real models, Editor,
MiniCreator, DCC consumption, native account/device UI or actual media playback.
The isolated combined profile/avatar plan remains unselected. The exact Flash
Dashboard static route uses a separate FA7002B source-list request before the
24E/24F/7001C cursor path; current ordinary TITLE routing and the source-list
consumer still require actual runtime witnesses and coherent implementation.
The supplied Sysext Dashboard differs from that mapped Flash image. See
`hdd-gate/native-dash-media-consumer-route-readonly-v1/report.json`.

No checkpoint or AppImage was made. V62 remains the prerelease. The combined
HDD gate stays open until its mandatory runtime, migration, media, profile,
avatar and release scenarios pass; macOS execution remains unverified.


## Current engineering baseline and native startup failure, 2026-11-15

V29 supersedes the V26 engineering baseline above. It includes the cache,
library metadata and native-path corrections, plus two corrections exposed by
real V27/V28 qualification failures. All fifteen direct runs and four focused
runs passed. The five core executables passed 1,781 cases / 104,119 assertions:
base 313 / 27,173; network 125 / 11,212; kernel 1,098 / 53,170; VFS 121 / 7,735;
UI 124 / 4,829. The hidden native-filesystem floor case ran alone, last, in a
separate process. All cleanup, source/configuration/product, protected-file
and fixture audits passed. The failed V27/V28 runs remain preserved.

Evidence: `hdd-gate/root-v29-engineering-qualification-20261115-v1/result.json`.
Application ELF SHA256:
`6490df857b3124d2b211dcb60a628cec1741ea8d8b08dd2626f70c494b84549d`.
This remains an incremental mixed-Clang engineering build, not a release.

The first actual combined native profile/avatar launch failed with C0000001.
The exact native input auxiliary initializer at 816F44D0 returned 80004004;
startup stopped at function 4/5, before DCC and avatar initialization. The new
identity owner had no registered receipt for that legitimate bootstrap call.
The missing update-file probes in the same log have not been established as
its cause. The user closed the window; OS cleanup completed without timeout
or termination signals, but exit 1 does not prove healthy native shutdown.
All 174 supplied system files and the DCC package retained their original
hashes. Only copied QA metadata was writable. Evidence:
`hdd-gate/root-native-avatar-first-runtime-observation-20261115-v1/result.json`.

The corrective source cohort admits only that exact once-only bootstrap call
and verifies its four actual Task publications. Physical task allocations keep
their original source and birth across repeated callbacks. Finite sign-in/UI
and settings reads retain a separate read generation; only genuine joined
publication advances it. Typed raw-instruction and import evidence takes
precedence over a decompiler simplification of the optional device-stats loop.
Ordinary virtual input returns opaque zero and performs one sweep; the generic
nonzero case has no proven cancellation or drain contract.

The shared stop path distinguishes strict native drain from exact retained
ownership through actual CPU/IO/GPU stop. Entered startup calls, unreturned
executor/deleter host frames and active native modal waits remain conservative
failures. Retained ownership never supplies native Complete, Wait, Close or
free success. Callback admission seals before the bootstrap host join while
physical polling/output remains available to genuine native cleanup. Stopped
heap holders and exact old registry entries release resources only at their
factual lifecycle boundary. Unknown modal Task descendants still block profile
publication; their callback/return closure is a separate prerequisite.

At this source-cohort preparation the correction is uncompiled and untested.
Fresh build/direct qualification and another isolated actual native launch
are required. No avatar rendering, editor flow, media playback or complete
native account/device selection has passed because of this static work.
The combined HDD gate remains open; no new checkpoint or AppImage is produced.

The nullable dispatcher-domain change also covers every storage consumer.
A separate pre-existing storage lifecycle gap remains: an escaped shared
NativeContentStorageDriver can keep its old Kernel pointer and guest-path
allocations after the service drops it. Actual stopped retirement, exact
registry withdrawal and safe escaped-holder destruction need their own
coherent correction and acceptance before gate closure; these four null-safe
snapshot changes do not close that boundary.


## V31 qualification and frontend startup blocker, 2026-11-15

The combined input/identity and shared-stop source cohort was integrated.
V30 exposed two invalid object_ref comparisons at compile time. The exact
underlying-thread pointer comparisons were corrected without changing strong
ownership; the failed build and its after-audits remain preserved. V31 then
built successfully. All fifteen direct and four focused runs passed with
complete cleanup and unchanged source/configuration/products/protected inputs
and fixtures. Core totals: 1,791 cases / 104,237 assertions (base 313 / 27,173;
network 125 / 11,212; kernel 1,108 / 53,288; VFS 121 / 7,735; UI 124 / 4,829).
The ten new cases passed. The native filesystem floor ran alone, last.
Evidence: `hdd-gate/root-v31-engineering-qualification-20261115-v1/result.json`.
ELF SHA256:
`04c3e1c660ba72f15d31de9d9bb30e75a466e394d46bad941b88e794db56108d`.

The actual isolated native retry still failed to launch, as the user confirmed.
It progressed beyond the earlier auxiliary abort, then native-system-fe-init
at 816BDE90 returned 80004004. The frontend saves the SysApp initializer's
result before loading hud.xex, so the later HUD load does not identify the
first failure. The fixed fifteen-entry initializer dispatch and the saved raw
code narrow the producer, but the first rejecting guard remains unobserved.
The final target result is C0000001 after the shared/receiver prerequisite
timeout. The run closed after 26.68 seconds with exit 1, no signals and complete
OS cleanup. This does not prove healthy native drain. Evidence:
`hdd-gate/root-native-avatar-v31-runtime-observation-20261115-v1/result.json`.
All 174 original system files, DCC and original QA seed retained their hashes;
one copied QA GPD changed. No avatar model/render/editor or game pass follows.
A prelaunch attempt was refused before Xenia started because the unused QA
root was 0755; only that isolated directory mode was corrected to 0700.

The next source cohort adds advisory owner-local first-loss and first-denial
records. Native capture uses preallocated scalar storage without locks,
allocation or logging. The exact Bootstrap host phase reports the saved line,
entry and registers before cleanup PPC; source/ELF identity binds the line.
No-cell prebound fast paths remain explicitly outside this diagnostic. It
changes no admission, native return, completion, readiness or ownership rule.

The same cohort addresses the previously recorded escaped-storage boundary:
strict source/callback drain and the factual Service stop boundary precede
in-place configuration/path-resource retirement and exact registry withdrawal.
An additional real host-resource lifecycle test covers counted-tail refusal,
known host path allocation/release, actual OS/IO joins and inert escaped
metadata. It does not fabricate native Driver installation or GPU readiness.
At source preparation this cohort and its new test are uncompiled/unrun;
actual native and release acceptance remain open.

Fresh current DCC evidence binds its stored default.xex SHA256
`6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1`
to the supplied package, with all 1,099 data leaves verified. Import record
addresses match the historical census; exact current avatar ordinals inside
the AES/LZX image remain unverified. No extracted package copy was made.
No checkpoint/AppImage is created; the combined gate remains incomplete.


## V32 qualification and coordinator diagnostic correction

V32 built and all nineteen direct/focused processes passed, including the
new real host storage lifetime case. Core totals are 1,792 cases / 104,281
assertions; kernel is 1,109 / 53,332. The other four core totals match V31.
All cleanup/source/configuration/product/protected/fixture audits passed.
ELF SHA256: `593a1b3d4f1f8c04d332a563ffa27e16547fbadaf5616663920a1d4e881a07c9`.
Evidence: `hdd-gate/root-v32-engineering-qualification-20261115-v1/result.json`.

The next actual native run still returned C0000001 after frontend 816BDE90
returned 80004004 and readiness timed out. It closed after 24.25 seconds,
exit 1, no signals, complete OS cleanup. The Bootstrap diagnostic did not
emit. That absence cannot distinguish an unreachable host turn from an empty
capture and does not identify the first rejecting guard. All original system
files, DCC and QA seed bytes, and qualified R source/configuration/products
remain exact. Evidence:
`hdd-gate/root-native-avatar-v32-runtime-observation-20261115-v1/result.json`.

The source-only successor adds an external-coordinator report at the existing
pre-title failure path. It validates the exact current Service/Kernel, keeps
strong owners while metadata locks are released, then explicitly reports
owner presence and empty/writing/complete scalar capture state. Original
capture sites and Bootstrap reporting remain unchanged; native admissions,
results and BeginStop behavior remain unchanged. This successor is uncompiled
at source preparation. First native refusal and gate acceptance remain open.


## V33 first observed listener ABI failure and coherent correction

V33 built and all nineteen qualification runs passed (1,792 core cases /
104,281 assertions), with clean source/configuration/products/protected-file
and fixture audits. ELF SHA256:
`2ee6f66997f16e81d6b0eb59642b77ab62d6171481110e35b1a2c4ae71383300`.
Evidence: `hdd-gate/root-v33-engineering-qualification-20261115-v1/result.json`.

The actual forced coordinator snapshot captured the first rejection at
native listener registration 816A8498, caller 816A8BE0, owner source line 2088:
r3=FFFFFFFF81A821FC, r4=300B1018. The guard rejects a valid sign-extended
DWORD. The authenticated native ABI also shows manager r3 / listener r4;
using r3 as a counted listener would independently fail the next check.
The run closed after 22.89 seconds, exit 1, no signals and complete OS cleanup.
Original system174/DCC/QA seed and the qualified source/configuration/products
remained exact. Native launch and graceful native drain did not pass.
Evidence: `hdd-gate/root-native-avatar-v33-runtime-observation-20261115-v1/result.json`.

The coherent source correction covers the whole quartet: register takes
manager r3 and listener r4; destruction takes listener r3; broadcast takes
DWORD event r3 and opaque DWORD data r4; GetNext takes HANDLE r3, filter r4,
ID output r5 and optional data output r6. Canonical normalization is metadata
only, never a valid-call register rewrite. GetNext binds a body only after the
actual counted ObReference succeeds, then releases its reference through the
real paired ObDereference. Exact final-reference nested destruction and real
failed-initialization cleanup before registration remain distinct from native
registration, queue retirement and own return. Observer frame tails block
stopped retirement until all local owning holders actually unwind.

Six existing code/test paths and five new regression cases are source-reviewed
only at this preparation; fresh compilation, direct checks and the same native
startup scenario remain required. No new translation unit or CMake change is
needed. Admission, references and cleanup are not fabricated from metadata.
The retained V33 loader trace separately binds all nine current DCC parsed
avatar ordinals to the supplied package and exact loader source; this proves
imports, not avatar initialization or rendering. No AppImage/checkpoint is
created and the combined gate remains open.


## V34 listener qualification and child-module ownership boundary

V34 built and all nineteen qualification processes passed: 1,797 core cases /
104,386 assertions, including five new listener ABI/lifetime regressions.
Source, configuration, products, protected files, fixtures and process cleanup
checks passed. ELF SHA256:
`a83738c6dfadbf4f0ff493a6f1942221d880921838e383ab8d2d1c6c63ee17a5`.
Evidence: `hdd-gate/root-v34-engineering-qualification-20261115-v1/result.json`.

The actual startup cleared the prior listener registration ABI rejection.
Its first observed rejection moved to XamTaskSchedule 816BA440, caller
91246948, callback FFFFFFFF91246770. The installed Xam.Community.xex loads at
91240000 and owns this caller/callback; the callback is outside the six known
profile callbacks. The old root-only caller check runs before task relevance
classification, incorrectly claiming this ordinary child-module call. The
frontend returned 8007139F and the target still failed with C0000001. The run
closed after 21.35 seconds with exit 1, no signals and complete OS cleanup;
healthy native drain remains unproved. Original system174, DCC and QA seed
hashes and qualified source/configuration/products/protected bytes stayed exact.
Evidence: `hdd-gate/root-native-avatar-v34-runtime-observation-20261115-v1/result.json`.

The source successor establishes task claim scope before owned admission.
Ordinary transport uses neutral records solely to route exact native returns;
it receives no profile source, read ticket, task authority or readiness.
Active unknown frames still prevent retained-stop acceptance. Genuine owned
child calls retain the existing exact current-domain module execution lease,
recheck it on their own return and revoke it in place only after actual stop.
The verified slow listener quartet shares that caller proof; fast queue cuts,
counted-body bindings, sources and paired reference/destruction gates remain
unchanged. Incoming profile callback binding cannot hide behind an existing
execution-only allocation, and ordinary destructor/free nesting cannot claim
an owned task merely because a neutral routing marker exists.

This is source preparation, not a successful native launch or avatar test.
Fresh compilation and direct qualification precede the same isolated runtime
scenario. No checkpoint/AppImage is created; the combined gate remains open.


## V35 qualification and common continuation claim regression

V35 built and all nineteen qualification processes passed, including the
three new Task scope cases. Core totals are 1,800 cases / 104,459 assertions;
kernel is 1,117 / 53,510. All process cleanup, source/configuration/product,
protected-file and fixture checks passed. ELF SHA256:
`ff03d8f72e1e42813bb2372855ba051bba101e76834f6ea067d0d962da1917b2`.
Evidence: `hdd-gate/root-v35-engineering-qualification-20261115-v1/result.json`.

The actual native run failed earlier than V34, at the first common STARTUP
continuation's callback publication: 816B9BB8, caller 816BA4D8,
r3=401E6B00, r4=81696558, r5=10000, r6=1. Source line 2254 rejects the
incoming known callback because Schedule was deliberately classified neutral
when there was no profile-specific Expected. The common continuation service
already owns an actual Invocation, Admission and registry entry for this
STARTUP call. Callback address 81696558 is shared by startup, diagnostic,
media and profile continuations; its address alone cannot classify ownership.
The newly added claim check therefore regressed this existing ingress.
The prior Community caller correction is not runtime-verified by this run.

The target reported C0000001. The bounded supervisor stopped the failed run
after 125.19 seconds; it returned 143, recorded termination/cleanup signals
including SIGKILL, and reported no remaining live/unverified processes.
The underlying exit code was unavailable and leader_reaped was false.
This is failed bounded cleanup evidence, not graceful native shutdown.
An owned-window close was refused because focus escaped the test window;
a later activation was refused after process disappearance. Neither request
establishes successful UI closure. All original system174, DCC and original
six seed files, qualified source/configuration/products and protected files
retained their hashes. Only the copied QA GPD changed.
Evidence: `hdd-gate/root-native-avatar-v35-runtime-observation-20261115-v1/result.json`.

The next contract correction must connect the existing exact common
continuation receipt to the entire native Task transport and callback
lifetime. Actual bootstrap thread, domain/catalog, ticket, output and call
arguments precede Schedule; handle/allocation identity comes from native
output. A generic continuation must not acquire profile read, commit or
readiness authority by sharing a callback address. Existing identity observers
and the separate F44D0 executor receipt remain distinct. Source review,
fresh compilation and actual runtime acceptance are still required.
No intermediate checkpoint/AppImage is created; the combined gate stays open.

The continuation ABI applies to both generic and typed profile calls. Tickets
such as 10001 are valid opaque DWORDs, not aligned guest payload pointers.
The typed profile route must preserve its exact Expected/job/source and callback
read scope while using that ABI; other profile payload pointer checks remain.
The source successor covers this distinction and failure cleanup before the
outer Schedule return. New tests are metadata/contract tests; actual native
allocation, callback execution, rendering and healthy shutdown remain unverified.


## V36 qualification and resident F3/frontend startup contract

V36 built and all nineteen qualification processes passed: 1,803 core cases /
104,977 assertions, including three continuation contract cases. The kernel
suite passed 1,120 / 54,028. Source/configuration/products/protected files,
fixtures and process cleanup checks passed. ELF SHA256:
`ab95c34a84fe49428254e51e6b8ea83c74aa7d1d1d77f74ad948f34002331a9c`.
Evidence: `hdd-gate/root-v36-engineering-qualification-20261115-v1/result.json`.

The actual run cleared the prior initial shared-publication rejection and
loaded Xam.Community. It still failed frontend initialization after five of
ten functions, returning 8007139F and finally C0000001. The run closed after
16.64 seconds with exit 1, no signals and complete OS cleanup. All original
system174/DCC/seed6 bytes and qualified source/configuration/products/protected
files retained their hashes; only the copied QA GPD changed. Native avatar,
media and healthy native drain remain unverified. Evidence:
`hdd-gate/root-native-avatar-v36-runtime-observation-20261115-v1/result.json`.

The captured profile-owner refusal at source line 2348 (Close body 816B7490,
sentinel caller, task401E6B00) is secondary. InitializeFrontend closes new
allocation admission immediately after frontend failure, then verifies/releases
its retained current Task. ReadFrame requires allocation.IsAccepting even on
this existing-resource cleanup. That deterministic ordering explains a later
cleanup refusal, not the preceding frontend result. Raw context/PCR/domain
values at that refusal were not captured.

Earlier actual log events 2472/2473 show Avatar ingress denying shared setup
816BCC38 and load816BCD08 with8007139F, before HUD load and frontend return.
The avatar filter claims only F2/F3 descriptors; the fixed native frontend
initialization loop skips F2 and includes unsuppressed F3. This derives a
resident F3 ownership/order conflict from native code and actual events.
Descriptor/LR/row were not recorded dynamically, so nested initializer paths
are not excluded by those events alone. The correction must join the actual
frontend parent, exact native row/callsite, Task, inputs and original returns.
A global descriptor exemption would not establish those facts.

Storage/source/index already run before frontend initialization. The later
queued content lookup/open uses frontend transport and cannot simply be moved
before its producer. Native F3's 80000004 init path initializes a read/write
lock through81982088 and ExInitializeReadWriteLock; this path does not consume
avatar packages, profile data, geometry or asset arenas. Resident init ownership
can therefore be separate from later package/request readiness. Later actual
lookup/open and avatar source checks must remain mandatory; initialization
must be observed once without replaying descriptor setup/load in a later phase.

Existing Task release also needs a real native entry/own-return/reference-count
receipt. PolicyAny or an executed guest call alone can accept a guard's abort
value without releasing the acquired reference. New-allocation admission and
retained-resource lifetime must stay distinct, with exact generation/caller
proof, truthful failure retention, no automatic retry, and real stop/tail joins.
The source successor is still private at this preparation. No intermediate
checkpoint/AppImage is created; the combined HDD gate remains open.

The private combined successor includes two disjoint six-path components.
Avatar callbacks distinguish raw native context Task bodies from GetCurrentTask
AddRef handles tagged body|1; comparison normalization never rewrites registers
or creates references. All relevant acquired Avatar reference releases consume
the same native-close receipt. Exact retained F2/F3 Control.schedule witnesses
permit execution-only allocation observation on real TITLE/SYSTEM contexts;
stopping permits only actual unload while allocation remains open. The real
allocation output supplies generation. Known foreign TITLE lifetime operations
remain distinct from profile/read/source authority. Opaque proof holders are
revoked in place only after actual stop, including escaped aliases.

Three Avatar contract cases and four Task lifetime/receipt cases are authored
but unrun at source preparation. The existing 55-call plan remains unchanged;
later avatar roles report adopted native FE receipts with executed_now=0,
not duplicate PPC execution. Final source review, fresh guarded build, direct
checks and actual runtime acceptance remain required for this whole cohort.


## V37 compiler correction before runtime qualification

The combined twelve-code-path cohort was applied with its reviewed report.
The guarded V37 build planned31 commands and failed after93.91 seconds.
The new Avatar header incorrectly forward-declared PPCContext as a struct,
conflicting with the canonical typedef of aligned PPCContext_s. CPU and kernel
compilation reported the same root error. Source/configuration/protected-file
and process-cleanup audits passed. No completed build pin, tests, extras or
native run follows from V37; partial objects are not qualification evidence.

The narrow successor includes xenia/cpu/ppc/ppc_context.h directly and removes
that incorrect declaration. The canonical header has no Avatar dependency;
the exact twelve-path scan found no second matching declaration. There is no
runtime behavior change or new translation unit. Fresh compilation, the direct
qualification processes and native acceptance remain required. The previously
reviewed cohort's source GREEN is qualified by this compiler finding. No
checkpoint/AppImage is produced.


## V38 qualification failure and selected-source fixture staging

The corrected V38 build passed (17 commands /88.31 seconds). All fifteen
direct processes ran with complete cleanup. Fourteen passed. Kernel had
1,126 passing cases and one failed case, with54,212 passing assertions and
one failure. All seven new native-lifecycle cases passed. The failure was
content_source_test.cc1051 captured_a=false in the existing concurrent
selected-source publication test. No extras or native run was qualified.
Source/configuration/products/protected-file audits remained unchanged.

That test assumed two concurrent preparations necessarily reach a later
barrier. Native selected-file admission intentionally uses nonwaiting
HostStorageSpace::TryBegin on their shared filesystem and can refuse one
preparation with busy before its checkpoint. The actual internal errno/site
was not captured in V38; this is a source-proven invalid fixture assumption,
not a runtime claim that a specific TryBegin was observed. Increasing the
wait cannot revive a worker that already returned.

The fixture successor starts A and waits its real prepared checkpoint before
starting B; both then wait for the same release and compete concurrently at
final publication. All duplicate-device identity, VFS, exactly-one-publication
and cleanup-before-assertion checks remain. Production behavior, timeout and
registration outcome rules are unchanged. Fresh build/direct qualification
and actual native acceptance remain required; no checkpoint/AppImage exists.


## V39 qualification and observed frontend/F3 startup

V39 built (two commands /25.99 seconds) and all nineteen qualification
processes passed, including the staged selected-source publication test.
Core totals:1,810 cases /105,172 assertions; kernel1,127 /54,223. The four
focused fixture processes passed, with the native filesystem-floor case
last and alone. Source/configuration/products/protected files, fixtures and
all process cleanup checks passed. ELF SHA256:
`360ad4011b6a196dc9dd8ad0b4f5079c2979185d011ef7f870a9d9a46ca0807f`.
Evidence: `hdd-gate/root-v39-engineering-qualification-20261115-v1/result.json`.

The actual native run records frontend_resident_observed with result0,
resident_loaded1 and fe_call_succeeded1. It separately records both deferred
F3 roles adopting those original Task/SP/LR receipts with executed_now0; the
actual load returned0. This proves the prior frontend/F3 initialization
frontier advanced without replaying descriptor setup/load or synthesizing
asset readiness. Title heap and preference readback also progressed.

The target still failedC0000001 with native owned-bootstrap progress/cleanup
uncertainty. The first owner loss is source line3098, which combines an exact
return mismatch, sticky Task-ledger loss and wake loss without identifying
which operation or condition first failed. Its atomic capture is printed
later, so it may predate the observed FE/adoption reports on another worker.
The later Close denial2514 occurs after owner uncertainty and has LR816B8104,
source-authenticated to the native B80C8 borrowed getter's internal release.
It is not evidence for a new Close-first fix. Nearby undefined kernel calls
and HDD-root access refusals are observed but not uniquely causal.

The run reached its bounded supervisor stop after125.31 seconds, returned143,
with underlying exit unavailable and termination/cleanup signals recorded.
No live/unverified survivors remained; healthy native drain was not proved.
Root's later targeted stop check found the process already closed and sent
no signal. Original system174/DCC/seed6 and qualified source/configuration/
products/protected bytes remained exact; only the copied QA GPD changed.
Evidence: `hdd-gate/root-native-avatar-v39-runtime-observation-20261115-v1/result.json`.

The next cohort is diagnostic only: preallocated first failed-return metadata
and first Task/wake loss provenance, armed from owner creation. Safe existing
Bootstrap/coordinator turns emit the scalars; capture must not allocate, log
or introduce locks under native/global critical regions. Native admission,
return values, source/read/ready/stop behavior remain unchanged. This will
identify the first actual broken invariant before a functional correction.
No checkpoint/AppImage is produced; combined gate acceptance remains open.

The frozen diagnostic cohort covers every existing ObserveReturn owner-loss
cut. Full metadata capture stays under the existing State mutex; unlocked
frame/exception cuts explicitly record partial scalars. A one-writer0/1/2
publication permits safe host reporters to read only complete payloads.
Tasks57/Wake13 existing loss sites retain their first method/line provenance;
two focused tests check that later errors cannot overwrite the first failure.
All prior24 task/wake cases remain. No new translation units, readiness,
admission, return-value or lifecycle behavior changes are introduced.
Source review is distinct from the still-required build, tests and actual
native run. Freeze: `native-profile-first-return-diagnostic-private-v1`.


## V40 startup observation and first-failure publication contract

The diagnostic-only V40 build and all19 qualification processes passed.
Core totals are1,812 cases/105,205 assertions, including both added first-
failure preservation cases. ELF SHA256:
`08da954d07a4f668cf458cc9a8753de30e7ac620729c7f32a797f9e5e88b4418`.
Evidence: `hdd-gate/root-v40-engineering-qualification-20261115-v1/result.json`.

Its actual isolated run again observed FE/F3 initialization and adoption of
both original receipts. This run did not reproduce V39's own-return loss:
both safe-host reports had loss0 and return payload state0. The first denial
was the already-stopping listener cutoff at owner line2630, from81A3F890.
The coordinator reported its combined transport/backend/startup-stop branch,
which does not distinguish the individual predicates; the12s deadline error
was absent. These observations do not establish a GPU or transport fault.
The run reached bounded125.15s/143 termination with signals, no live or
unverified survivors, and no proof of healthy native drain. Original
system174/DCC/seed6 and qualified source/config/products/protected bytes were
unchanged. Only the isolated QA GPD changed.
Evidence: `hdd-gate/root-native-avatar-v40-runtime-observation-20261115-v1/result.json`.

Source review found a concrete first-cause publication gap: Bootstrap's
fail(text) calls stop_owned_on_bootstrap, which sets stopping and begins
identity cleanup, before Poison(text) publishes the original failure text.
Cleanup may wait or execute guest work in between. The launch coordinator
can therefore see stopping with an empty reason and emit a generic error.
The successor must publish/preserve that first text before the unchanged
cleanup tail, and report the already-fetched coordinator predicate tuple.
This changes failure visibility without relaxing native readiness, task
admission, ownership, or drain obligations. Native workflows and the combined
HDD gate remain incomplete; no AppImage/checkpoint is produced.

The two-path source correction also records the first Bootstrap-owned progress
stage (identity, media, or missing startup/bootstrap) without changing native
call order or short-circuit behavior. No new test is claimed: existing
continuation tests do not exercise the real Service/Bootstrap ordering, and
no meaningful Service fixture currently exists. Exact source inverse/order
checks supplement required build/direct tests and the next actual runtime.


## V41 first notification-listener completion failure

The first-failure publication correction compiled (eight commands/46.53s)
and passed all19 qualification runs, with1,812 core cases/105,205 assertions.
The actual isolated launch again observed genuine FE/F3 success and adoption
of both original receipts. Its original identity-step failure now reached
the coordinator before native cleanup completed, validating the corrected
failure visibility. ELF SHA256:
`c7216d2945b6def9fd48b65c19ec0288bbc5a8d16465fe2e11075b2322cbf4f3`.
Evidence: `hdd-gate/root-v41-engineering-qualification-20261115-v1/result.json`.

The first recorded return failure is Broadcast816A93C8, LR818CA068,
SP7083F820, thread12/PCR30044000, phase1, flags00020000 and prior owner loss0.
It has ordinary source1/noTask birth; current read source2 and prepared job3
exist separately, with native-entered flag absent. The original notification
is02000003 (LiveLinkStateChanged), not a profile publication proof. The
owner becomes uncertain at3193 only after this captured failure; later
Close denial2581 is downstream. Tasks/Wake had no first-failure reason.
The sampled graphics capacity/immutable flags were true, epochs2 agreed,
stop proof was not stopped and transport was not stopping or retired. This
is not native graphics/rendering acceptance.

Broadcast's returned R3=FFFFFFFF81A82204 is manager-lock residue: native
caller and bridge both ignore it. Authenticated f9 instruction/contracts
prove a static zero-without-enqueue path in8328 only when the incoming ID
has its high bit set and area+10 is0. The actual selected normalized ID and
area flag were not captured, so this path is not established as the cause.
The first failing bridge cell/frame/inner/lost/CAS or ledger predicate is
still unknown. No return-ABI change or authority relaxation is justified.

The run stopped at its125.29s bound with supervisor143/native exit-9 and
signals; the leader was reaped and no live/unverified survivors remained.
Healthy native drain remains unproved. Original system174/DCC/seed6 and
qualified source/config/products/protected bytes stayed exact; only copied
QA profile metadata changed. Evidence:
`hdd-gate/root-native-avatar-v41-runtime-observation-20261115-v1/result.json`.
Native contracts: `native-v41-broadcast-return-contract-readonly-v1` and
`native-v41-broadcast-listener-contract-readonly-v1`.

The next source cohort is diagnostic only across the whole ListenerBridge
and queue-origin lifecycle. It must preserve the first actual failure site
and prebound cell/selected-ID/area/inner/linearization scalars without adding
allocation, IO, native memory reads or locks under guest critical regions.
Only existing safe host cuts may report complete published snapshots.
Native semantics and the combined HDD acceptance remain unchanged/open.

The frozen diagnostic covers all54 original Bridge and18 Ledger failure
sites plus invalid-root construction. First Ledger provenance is published
before its original lost store; direct Bridge scalars are complete before
that delegated loss becomes visible. Scalar availability distinguishes
invalid-context partial captures from validated current-cell state and
clears completed/per-iteration data. Two metadata regression cases preserve
all7 earlier cases and exercise original-loss retention, partial fields,
reuse within one Broadcast and a bounded concurrent lost observer. These
are host metadata checks, with no native compatibility success implied.


## V42 measured native enqueue word-operand mismatch

V42 compiled (six commands/31s) and all19 qualification processes passed:
1,814 core cases/105,282 assertions. Both diagnostic metadata regressions
passed. ELF SHA256:
`57dde092889fdaca7f78fc3c1894e7eead43c150d577524594e3435caf896426`.
Evidence: `hdd-gate/root-v42-engineering-qualification-20261115-v1/result.json`.

The actual first Bridge failure is FastBeforeUnlock466/reason16 at native
LR816A8410. The same active Broadcast cell/inner8328/frame is validated;
SP7083F6B0 equals innerSP7083F730 minus80, r31 equals area300D3008,
r28 equals data0, and lock address300D2034 equals listener300D2018+1C.
The first failing predicate is the canonical-DWORD check on R29's full64-bit
value0200000302000003. Prior ledger provenance is0: this is the direct
Bridge failure, not queue-capacity/orphan loss. Native unsigned range
checks in the bridge are short-circuited here and were not observed.

Authenticated native8354/8360 and the existing wrapping rlwinm emitter/test
goldens explain the exact64-bit value: wrapping word-mask semantics preserve
the duplicated high half. Native8364/8370 compare only the low word; native
word indexing/store operations use that32-bit key. The decoder/emitter is
consistent with the captured register and its existing goldens. Native
area minimum/maximum offsets+4/+8 match CaptureAreas header[1]/header[2].
Pointer, frame, handle, opaque-data and API parameter checks have separate
contracts; no blanket canonical-value exemption is justified.

The coherent correction derives one typed32-bit normalized queue ID at this
native word-consumption seam and uses it for the same unsigned bounds and
ledger replacement key. It leaves CPU behavior, raw registers, pointer/frame
validation, native queue instructions and source/ownership authority intact.
A production metadata regression must cover both captured and unequal-half
legal wrapped values, invalid low-ID bounds and forged pointer high halves,
plus queue-origin transfer/whole-work retention. Host metadata tests do not
substitute for the next actual native startup/profile/avatar run.

V42 ended at125.22s bounded stop/supervisor143 with signals, no remaining
live/unverified process, and no healthy native drain proof. Original system174,
DCC andseed6 plus qualified source/config/products/protected bytes stayed
exact. Only copied QA GPD metadata changed. Actual evidence:
`hdd-gate/root-native-avatar-v42-runtime-observation-20261115-v1/result.json`.
Read-only ABI proof: `native-v42-enqueue-word-abi-proof-readonly-v1` and
`native-listener-word-operand-contract-readonly-v1`. Gate acceptance stays
open; no checkpoint/AppImage is produced.


## V43 actual launch milestone and sign-in/avatar frontier

The typed native queue-word correction built (six commands/30.34s) and all19
qualification processes passed, including the four-mode production metadata
regression:1,815 core cases/105,346 assertions. ELF SHA256:
`3619afc29ff7806a8c203ae33cd738c9ca1cf05f1505d2b20fbb3d9be4d1f3e7`.
The actual native run established pre-foreground startup prerequisites at
backend epoch2, then DCC executed thousands of frames. The user confirmed:
"it reaches game but sign in is looping". Captured listener state/ledger and
owner loss were0; the prior false canonical-word rejection was cleared.
This is native startup/foreground progress, not complete identity or avatar
acceptance. Evidence: `hdd-gate/root-native-avatar-v43-runtime-observation-20261115-v1/startup-milestone-and-open-blockers.json`.

DCC's admitted AvatarInitialize1500 returned80070005 at frame2; shutdown1501
then refused the failed owner state. The actual F2 TOC Open importEFD80 is
bound to HLE ContentOpenFile/Avatar asset admission, so the guest-only
EE500 denial branch is not the executed path. First negative Prepare/Start/
overlap/asset-open stage and actual adapter-versus-ContentManager result
remain unmeasured. Later TaskClose advisory5 lacks issuer/timing and does
not establish the source of the error.

Source contracts expose two independent sign-in gaps: pre-foreground
readiness does not include stable identity convergence, and pending native
publication closes ordinary unscoped TITLE roster reads. Imported2BC is
also routed only to a legacy host dialog, whose lease refuses active native
input ownership. No actual getter/job/2BC call tuple was captured, so these
conditional gaps do not uniquely establish the user's loop cause. Waiting
for every profile/render join before foreground has a possible genuine
Running-input dependency and must not be introduced without its contract.
Contracts: `native-v43-initial-profile-projection-audit-readonly-v1` and
`native-v43-avatar-signin-shared-contract-readonly-v1`.

The native2BC/2BE entries are recovered from the actual f9 export table:
2BC16B tails2BE80B with userFF, which submits20B{0,user,users,flags,0} through
816C1868. Its actual AppRequestLoadEx299 admission reserves UI ownership,
then sends FE message21001. The result adapter maps request acceptance,
not selected-account completion. Original caller/process and native modal
lifetime must be preserved when this route is implemented. Evidence:
`root-native-signin-public-contract-20261115-v1`.

V43 ended at125.19s bounded stop/supervisor143 with signals, no live or
unverified survivors and no healthy native drain proof. Original system174,
DCC/seed6 and qualified source/config/products/protected bytes stayed exact.
The next diagnostic cohort correlates actual initial profile phase/read
projection, getter/UI dispatch and first negative avatar asset admission
without changing native routing/readiness/authority or fabricating success.
The combined gate stays open; no checkpoint/AppImage is produced.

The frozen11-path diagnostic preserves original guest behavior. Its host PM
reporter takes no profile lock, because adding a reporter lock could make
existing try-lock getters return signed out. Mirrors are scalar atomics from
original mutation/read cuts and prebound publication; cross-cut tuples are
explicitly observational. First projections are per valid user, queries per
ordinal/user plus an invalid bucket, including original XUID type-mask.
Owner job joined bit13 is unavailable rather than reading an unsynchronized
shared pointer. Avatar phases distinguish guard, HRESULT and Win32 outcomes
and asset admission predicates using original evaluated operands. No new TU
or test is claimed: no real live TITLE/PM/Avatar fixture exists, and a mock
state would not establish these native ordering facts. Existing direct tests
and the next actual native run remain required.


## V44 pre-title refusal and V45 sign-in/alignment correction

V44 passed all19 direct/focused qualification processes (1,815 core cases,
105,346 assertions), but the actual native run failed before foreground at
`identity-step`. PM operation2 was canceled before an owner job was published;
no TITLE query or first listener failure was captured. The precise original
Prepare/Enqueue refusal is unknown. Stopping was observed after failure and
is not established as its cause. Original system174, DCC and seed6 plus the
qualified source/config/products/protected files stayed exact. Evidence:
`hdd-gate/root-native-avatar-v44-runtime-observation-20261115-v1/startup-frontier-and-open-blockers.json`.

The V45 source cohort corrects two independently established contracts. Initial
profile alignment retries only actual pre-native contention within a bounded
budget, retaining the exact failed PM attempt until cancellation completes.
Both native descendant-closure checks remain. Unpublished event producers are
retired before another attempt; stale authority, unsupported data and capacity
failures remain truthful failures. Fatal startup does not prevent factual stop
cleanup. First refusal/fatal records capture original stage, error and evaluated
predicates rather than assigning causality from a later stopping snapshot.

Recovered 2BC/2BE sign-in calls now use the existing original-caller native
creating-UI broker. A private prebound ThreadState observer records the exact
helper/sender frames without locks, guest reads or IO. Authenticated normal
public return supplies explicitly derived continuation evidence where a native
tail restore bypasses an own-return hook; actual raw receipts stay distinct.
A real no-UI0 releases only temporary creation ownership, while an accepted
FE request0 retains normal native pending ownership. Missing or contradictory
observations retain the uncertain operation. Exact HRESULT adaptation is
checked. Selected host fallback retains its issued lease and dispatch-owned
completion/input isolation. Ex/Nui routes remain unchanged.

The user clarified V43: the picker appeared, and selecting QA62 returned to
the sign-in prompt. The old fallback can accept an already selected host XUID
after void LoginMultiple discards an adoption failure; that is not proof of a
native commit. Native profile convergence, fallback truthfulness and the actual
DCC/avatar result remain open. New57-call plan `cf0a9fbd...` preserves all55
previous entries and the14 phases/11 bridges; consumed `a949...` is untouched.

Six focused cases are authored across three existing test translation units,
with existing case bodies preserved. Source review is not build, test, native
or gate acceptance. The next integrated build must run all required direct
processes and an actual isolated consumer test. No checkpoint/AppImage or
release is produced; macOS execution remains unverified.


## V46 qualification and native Task/Avatar frontier

The V45 build passed304 commands, but a new test consumed its cancellation
Fence twice. The V46 test-only correction keeps the dispatch run as the sole
waiter. All19 processes passed:1,821 core cases/106,579 assertions. Qualified
engineering ELF SHA256 is33d11afb5018bb34348fc38cbac20bb8632e57fa7c6c81f11ac55948cb317946.
Source/config/products/protected and fixtures stayed exact. Evidence:
`hdd-gate/root-v46-engineering-qualification-20261115-v1/result.json`.

Fresh QA14 reached DCC. Actual2BC returned0 with complete helper/sender/
withdrawal receipts223, queued decision2 and native UI visibility1. TITLE
slot0 reported local sign-in1 with XUID E030000003E5BE18. Alignment remained
Prepared/ordinary1, so the prior post-publication data-read hypothesis was
not reached. Native UI delivery triggered the first manager Query and old
source-drain transition; completed account selection remains unverified.

The user reported a guest crash and frozen input. At816B9CAC, native Task
publication reaches lwarx with a null/tag-zero scheduling object plus4,
before callback C838 executes. Host address100000004 is membase100000000
plus guest EA4. The original options/provider result is unobserved. BAAD0000
also exists in the native constructor; nearby missing XeKeys exports do not
establish causality. The first Avatar asset-open denial is Win325, phase14,
evaluated3/failed2: owner uncertainty predates unevaluated source/Task/core
and ContentManager checks. Existing Control receipts precede their validation
tails. Source and actual evidence:
`hdd-gate/root-native-avatar-v46-runtime-observation-20261115-v1/`,
`native-v46-task-publication-fault-readonly-v1` and
`native-v46-profile-avatar-frontier-readonly-v1`.

QA14 exited0 after166.84s without signals or surviving owned processes.
Original system174, DCC, seed6 and qualified R stayed exact. GUI Cheap-skate
quick_exit can bypass teardown and its reports; native drain remains
unverified. The next diagnostic-only cohort correlates actual provider/
options/native Task operands and the FIRST Avatar uncertainty setter, with
exact Control short-circuit masks. A safe live host snapshot before GUI exit
must preserve these records if bootstrap is waiting. It grants no shutdown,
native completion or stopped-CPU claim. Existing native admission, results,
profile authority and lifetime rules remain intact; whole erasure restores
V46. No new TU/mock-native test is claimed. Build/direct19/actual-run results
remain required after integration.

The user authorized scripted controller input and careful screenshots while
unavailable. A dedicated idle pad was observed through its own helper/sysfs
identity and the actual SDL library; SDL presents it as Xbox360 Controller.
The transient slot_bindings_passthrough argument will select its observed
GUID in guest slot0 and the physical pad in slot1. Metadata alone does not
prove guest routing: verify the real owned window, Controllers mapping and
permitted native sampling before inputs. Preserve installed data and the
keyboard/Guide policy. This combined gate stays OPEN; no checkpoint/AppImage
or release is produced.
