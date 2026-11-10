# Ranked implementation roadmap

## Current gate: combined HDD, local profiles, avatars and media

The Guide foundation's tested local Linux AppImage is accepted; see the
[release record](17559-gate-releases.md). The newly reported native Xbox Home
action is a shared launch regression to repair in this gate, not a reason to
discard the prior navigation/render/input evidence.

The user has expanded HDD work to include complete local profile creation and
repair, stock avatar creation/editing/rendering, native selectors, host media
viewing/playback (including H.264/AAC MP4), and both library surfaces. Follow the
[combined system contracts](17559-hdd-system-contracts.md). Live email/Gold/P2P,
XNA runtime and unrelated system/network applications remain later work.

Before profile feature implementation, complete native achievements/notification
routing (game placement, native sound/look, automatic Xenia fallback) and remove
the replaced/fixed notification preferences. Continue this settings audit at
every subsystem boundary so unsupported or ineffective choices do not remain
in the application UI.

The phase table and Guide task list below retain their historical baseline
scope; the combined gate supersedes their separate HDD/profile/avatar ordering.

## Historical phase ranking and baseline boundaries

Baseline source: `b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1`.
The user's implementation order is **guide → HDD/storage → profiles → avatars →
XNA → Fake Live → remaining system apps → network apps**. This supersedes the
baseline's earlier cross-subsystem ranking. Work proceeds through these phases;
shared prerequisites discovered within a phase are repaired there. Independent
feature work in later phases waits for the earlier foundation.

Fake Live retains direct-only networking, no relays or mandatory port forwarding,
metadata-only discovery/signaling helpers, persistent accepted/blocked contacts,
sender-local queued messages, guide-originated invitations, parties and voice.
System and network apps keep the supplied binaries intact where possible; optional
local adapters remain limited to demonstrated obsolete protocols. Linux is the
current runtime host; native macOS acceptance remains required later.

BO1 performance is a later track. A common GPU correctness bug is pulled forward
only if it blocks acceptance of the current system phase.

Every completed gate also requires an up-to-date upstream comparison and a
tested local Linux AppImage for downtime testing, with reasons for any changes
we defer or retain differently. The [gate release contract](17559-gate-releases.md)
defines the required artifact, provenance, upstream decisions and acceptance.
The current Guide release disposition is recorded above and in the gate policy;
the earlier keyboard-only checkpoint was not a gate release.

BO1 has a correctness gate within its track: resolve reproduced GPU memexport
readback completion failures before accepting a performance improvement.
Both no-overlay Kino samples overlap those failures. Array's empty-match sample
has no new failures in its measured interval, but loading did, so it is not a
complete correctness pass. The staging-slot eviction/submission hypothesis
needs a targeted trace and regression coverage before a fix is proposed.

| Phase | Work package | Evidence and first boundary | Acceptance for implementation |
| --- | --- | --- | --- |
| 1 | Native dashboard guide and remaining XUI paths | The fallback's matched single/two-controller menu cycle passes. The installed HUD now executes registration and two initialize/close/destroy cycles while preserving the title; unresolved gamer-card/XUI dependencies prevent functional native-guide acceptance. Follow-on controller navigation and return-home checks are tracked in the [guide report](17559-guide.md). | Complete fallback navigation/modal handoffs and trace native scene/resource loading, system graphics ownership, compositing, notifications and return to game. Complete the shared keyboard during this phase. Registration ownership and DPC context now have separate common interfaces; existing GPU work retains its legacy policy. Establish native XAM allocator/object ownership, then shared/light-device recording and system-command-buffer retirement/composition. A real guest D3D device and XUI drawing context remain prerequisites; do not reset the title ring or infer submission ownership from registration metadata. Prefer native guide when available; use the same backend state in the host fallback. |
| 2 | HDD and folder-backed storage | The dashboard enumerates HDD categories; XAM has only one writable content device. MU mounting adds a VFS alias without a matching XAM content device. | Unify device registration, identity, capacity, ownership and VFS routing for folder-backed devices; enumerate disposable content and validate native copy/move/delete plus restart persistence. Preserve existing package/profile bytes. |
| 3 | Persistent profiles, gamer pictures and saves | Local Account reload and two-user sign-in pass, but dashboard storage labels a profile Corrupted/0 KB. Zero/missing sidecars and unresolved XamProfileLoadAccountInfo provide concrete leads. | Establish the guest corrupt-label branch, validate metadata before accepting raw headers, and support valid enumeration, create/sign-in/out, settings, gamer pictures and per-user saves across restarts. Keep loose-profile compatibility and native storage operations consistent. |
| 4 | Avatar resources and common image/package APIs | Editor draws its selection screen without models; `XamPngDecode`/indexed-XEX package lookup are unresolved and avatar assets return placeholder sizes/results. | Trace actual guest calls, implement correct buffers/sizes and installed asset resolution, then create/edit/save/reload/render an avatar in editor/dashboard/game paths. Retain asynchronous completion and profile ownership; enable initialization only after underlying operations work. |
| 5 | XNA managed-package launch and runtime resolution | All 174 supplied system files match fresh/original/copied installations. Standalone launcher shows an inconclusive update prompt; both normal direct STFS launches fail at absent default.xex. An installed marketplace-package chooser/launch-data path exists; raw managed executables are rejected. | Exercise the correct installed-package route, GameInfo/virtual identity, package mounts, launch data/arguments and runtime resolution for both CastleMiner games. Establish the guest failure branch before changing update detection; reach gameplay and separate runtime, avatar, storage and networking failures. Do not bypass the prompt with fabricated success. |
| 6.1 | Coherent local Live compatibility service | Local Live eligibility coexists with disconnected XLIVEBASE; social APIs have independent placeholders. | Introduce one per-profile state model for sign-in eligibility, actual connectivity, contacts, blocks and notifications. Guest APIs and both UIs observe the same state. Service/token failures remain accurate; local identity is not represented as authenticated Microsoft service access. |
| 6.2 | Automatic direct discovery and authenticated pairing | Signed DNS/Pkarr and DHT lookup are candidates, not a verified full signaling arrangement. | A bounded two-peer spike proves entered code, fresh address lookup, request, accept/block, authenticated direct negotiation and automatic reconnect without player setup. Helpers carry metadata only; messages/voice/game packets never use a relay. Produce an explicit unavailable result when direct checks fail. If no existing permitted metadata arrangement satisfies the requirement, report that unmet prerequisite before committing to a transport/UI design. |
| 6.3 | Friends, presence and queued text | Friends enumerate empty, friendship checks remain false, and no shared remote social store exists. | Persist mutually accepted contacts and blocking; expire/recover online state; deliver typed messages directly with sender-local pending/delivered state and restart-safe deduplication. Blocked peers cannot send presence, messages or invitations. Test with two isolated identities, then independent internet peers. |
| 6.4 | Generalized sessions and game invitations | `XSession`, secure associations and System Link transport are reusable; accepted-invite info and remote publication/search are missing. | Publish/join/leave compatible sessions and carry a guide friend-list invite through notification, acceptance, title launch and join. Validate Minecraft, Cloning Clyde, Doritos Crash Course, N+, Small Arms and BO1, then both XNA games after prerequisites. Keep title-specific publisher services classified separately. Rotastic remains a local regression target. |
| 6.5 | Parties and voice | Party calls can return success without delivery; voice creation denies access and reports no microphone/headset. | Implement party membership, invites, leave/teardown, voice capture/playback, mute/block and party-versus-game routing through measured guest contracts. Exercise reconnect and privacy boundaries; remote audio quality requires a second independent peer. |
| 7 | Remaining system apps and dashboard XUI | System files install and match the USB archive; selected dashboard pages render. Full scene coverage and system-title handoff remain incomplete. | Exercise installed system utilities and remaining dashboard scenes on the completed storage/profile/avatar/XNA/Live foundations. Verify notifications, settings, resources, focus, launch/return and persistence using the common guide and services. |
| 8.1 | POSIX HTTP/HTTPS backend | `xhttp_host_posix.cc` fails request operations; host connectivity succeeds. | Implement the existing `HostRequest` contract using an upstream-compatible native dependency. Verify trusted/invalid certificates, DNS/connection/TLS errors, headers, redirects, streaming upload/read, authentication retry, cancellation and guest callback ordering on Linux; require later macOS validation. Assess libcurl before adding a new TLS stack. |
| 8.2 | Legacy app service compatibility | HTTPS transport currently blocks meaningful end-to-end service assessment. | Capture actual app endpoints/auth/media behavior after transport works. Keep supplied app binaries intact where possible; use an optional local protocol adapter only for demonstrated obsolete-service incompatibilities. Prove browsing/playback separately from transport and catalog unsupported services. |
| 9 | BO1 correctness gate and performance | Kino and private Array scene entry work. External no-overlay guest-present samples give 27.03/22.90 for Kino and 35.97 for empty Array, with the correctness limits above. Storage-picker input and utility-drive/cache lifecycle also need follow-up. No matching installed/fetched performance patch is available. | First resolve memexport readback/submission failures and preserve rendering correctness. Then repeat warmed 1280x720 scenes, measure CPU/GPU work and frame distributions without builds or injected overlays, identify the dominant bottleneck and make common emulator fixes with regression checks. Target 60 FPS; report remaining misses and avoid title-ID hacks. |

## Historical Guide phase task list

1. The host fallback's controller routing and Linux repaint repair now pass
   repeated Guide/B cycles and return-to-title input checks. Full keyboard/mouse,
   macOS and broader game coverage remain separate validation. See the
   [guide implementation record](17559-guide.md).
2. Complete ordinary shared keyboard entry and guest completion/input handoff.
   Run the installed XAM CPU-object diagnostic separately from rendering; keep
   native string-table/allocator families consistent. Follow the
   [native XUI and keyboard record](17559-native-xui.md).
3. Connect shared/light-device recording to system-command publication, actual
   GPU retirement, title-state restoration and composition. Establish a native
   guide milestone with real pixels before claiming native support.
4. Validate dashboard and game invocation, repeated open/close, user-slot routing,
   failure fallback and uninterrupted game continuation using isolated state.

A fallback repair does not complete native guide support. Profile/friend/invite
panels in the guide gain their real behavior as their underlying phases land;
phase one must route them through common interfaces and handle unavailable
services accurately.

## Contribution boundaries

- Preserve independently reproduced evidence and small, bisectable subsystem
  changes. Existing profile and game state must remain recoverable.
- Keep assets, personal logs, profile identifiers, private keys, supplied system
  binaries, game binaries and decompilation outside tracked source.
- Use public information, supplied software and existing console-derived fixtures;
  no XDK-derived information or new console observations are claimed.
- Review added dependency licenses and native packaging requirements. Windows
  development/acceptance is outside this fork's requested scope.
- Maintain this as an independent public fork. GitHub releases provide a tested
  AppImage and matching source.zip; upstream merge requests are not planned.

See [baseline evidence](17559-baseline.md), the
[architecture map](17559-architecture.md) and
[manual acceptance checklist](17559-controller-checklist.md).
