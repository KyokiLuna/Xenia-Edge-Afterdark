# Xenia Edge 17559 research baseline

Date: 2026-11-03. Source: `b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1`.
Branch: `research/17559-baseline`, tracking `upstream/edge`.

This package establishes reproducible captures and a ranked implementation
backlog. It changes research tooling and documentation, not guest exports or
emulation behavior. GitHub publication and upstream pull requests are later work.

These observations describe the original pinned baseline. Subsequent functional
work is recorded separately in the [guide phase](17559-guide.md), following the
user's revised [implementation order](17559-roadmap.md). The baseline binaries
and scenario matrix remain historical evidence.

## Reproduction and provenance

The complete local evidence root is
`/home/kyoki/.local/state/xenia-edge-research/`. It contains personal test data
and supplied software; keep it outside Git and public attachments.

- `inventory/original-state-20261103T172734Z/`: original-state hashes, package
  identities, system-update archive inventory, source/update selections, and
  sanitized inventory summaries.
- `binaries/`: the pinned official AppImage, its extraction, and SHA-256 records.
- `seeds/`: immutable, hashed data seeds; `staging/`: their preparation inputs.
- `runs/`: one disposable data copy, command/configuration, logs, screenshots,
  and result metadata per capture.
- `matrix-cases.local.json`, `run_matrix.py`, `matrix-results.local.json`:
  exact local targets and automated startup probes.
- `evidence/build/`: native build commands, revisions, interruptions, tests,
  toolchain differences, and packaging results.
- `evidence/profile-storage-diagnosis.md`: the profile enumeration investigation.
- `social/`: pinned social API excerpts and direct-only discovery research.

The installed AppImage and official release `b65dd25` have identical SHA-256:
`c1970f48451ec940acd98e1fab1f1780408c27684f09aacf3ce2134e16a4b6d6`.
The installed filename is not evidence of a different source revision.
The extracted executable has SHA-256
`206cdd29c056fb32c1b00f661634b54a5c4fa38c846c85f90b42af161a3f7fa8`.
Packaged startup is captured separately from subsequent extracted-binary runs.

AppImageLauncher moved the downloaded research copy into Applications during
packaged startup. That research copy was returned to the evidence directory;
later captures use its extracted executable to avoid integration side effects.
The pre-existing installed executable and its desktop entry remain separate.

The first empty-data launch was manually interacted with by the user and
created a test profile. Its screenshot is not an unattended-startup result.
The resulting data was preserved as a separate seed for persistence checks.

### Isolation

[`tools/research/README.md`](../../tools/research/README.md) documents the CLI.
Every capture forces explicit absolute `storage_root`, `content_root`,
`cache_root`, and `log_file`, separate XDG directories, disabled source-title
writes, and disabled plugin loading. Every run copies a verified seed.

The copied-state seed contains system files, both existing profile directories,
configuration/library metadata, dashboard assets, and selected title updates.
Large shared DLC, original JIT caches, and installed game/app payloads were
excluded. Supplied packages are read-only launch sources. Content tests must
therefore distinguish absent seeded content from emulator failures.

Original preservation covers 361 core files / 187,433,596 bytes; metadata covers
9,821 shared-content files. The initial hash verification found zero changes.
Run metadata separately verifies its input seed after execution.

### Host and build differences

The test host is Artix Linux x86-64, AMD Custom APU 0405 / VanGogh, 4 cores /
8 threads, about 14 GiB RAM with no swap. Native work uses Clang 23.1.3 and
libstdc++ 16. The supported `--disable-lto` option avoids an unavailable system
lld. This differs from upstream CI's Clang 21, GCC 14 and ThinLTO.

Native builds use the upstream wrapper and pinned Slang 2026.8. Data repositories
are recorded separately because the wrapper fetches them:

| Repository | Captured revision |
| --- | --- |
| game-patches | `fe957a050e37f5a1e3f485ca43c2e37ee2a9c92d` |
| x360db | `43b5de9ba4f0d64eb5181d362efef2f0c58b7f11` |
| SDL_GameControllerDB | `07af1d50187e94c861c9f9f5d8a55b148f11306b` |

A local AppImage inherits this host's glibc requirements. It must not be
described as equivalent to the Ubuntu 22.04 CI compatibility floor.

The native Release executable is `build/bin/Linux/Release/xenia_edge`, SHA-256
`200a27147f0383ec330963db237a15a4b762f91d30379d10e01408e8f361d7d7`.
The upstream Linux AppDir/linuxdeploy recipe produced
`evidence/build/packaging/xenia-edge-b65dd25-local-research-no-lto.AppImage`,
SHA-256 `b8d8e367bc525dd0d408724cc46242b7a14d3fe3debae3f125fecc3be0265fcd`.
Packaging left the source executable unchanged. The unstripped native ELF is
retained for diagnostics; linuxdeploy stripped only its packaged copy.
Matched copied-state native captures reproduce dashboard home rendering and
Twitch's two missing-host-HTTP failures from the official binary. This is a
qualitative startup comparison, not full compatibility or performance parity.
The local AppImage also renders its fresh library and No Profiles dialog.
Inspected screenshots and the earlier rejected activation-based screenshot
attempt are retained in
`evidence/build/runtime/native-vs-official-comparison.shareable.json`.

### Required tests and existing guest fixtures

All four test executables were run directly from separate disposable working
directories. Full stdout/stderr and command exit codes are retained under
`evidence/build/`; all returned zero.

| Executable | Cases | Assertions | Result |
| --- | ---: | ---: | --- |
| xenia-base-tests | 84 | 2,287 | passed |
| xenia-net-tests | 124 | 11,169 | passed |
| xenia-kernel-tests | 41 | 944 | passed |
| xenia-vfs-tests | 14 | 218 | passed |

The public xamgen XHTTP corpus completed 434/434 steps and matched the unchanged
17559 fixture: **434 same, 0 different**. It checks pre-request ABI behavior;
its only SendRequest is rejected before host transport. This does not test TLS.
The content `lifecycle`/`users` subset completed all 75 selected steps:
**75 same, 0 different, 95 not run** against the unchanged full fixture. That
checks within-process save lifecycle, ownership and selected content metadata;
it cleans up its saves and does not establish cross-restart save persistence.
Commands, hashes, decoded results and checker exits are in
`evidence/guest-probes/` and their referenced run directories.

### macOS build preparation

macOS execution and packaging remain unverified on this Linux-only host.
The pinned [build instructions](../building.md#macos) and
[CI recipe](../../.github/workflows/build.yml) provide the native path:
macOS 15 deployment target, Xcode/Metal toolchain, Python/Meson with
`mako`, `pyyaml` and `packaging`, pinned Slang 2026.8, Metal Shader Converter
4.0-beta2, and the macOS submodules including Mesa, MoltenVK and metal-cpp.
The local Linux checkout deliberately did not initialize those platform-only
submodules; a Mac must initialize them at the pinned parent revisions.
After platform prerequisites, use `./xb slang`, `./xenia-build.py msc`,
`./xb setup`, and `./xb build --config=Release --target=xenia-app`.
Pass the resulting `.app` to the same isolated capture runner. Record the Mac's
toolchain, submodules and backend separately rather than reusing Linux results.

## Confirmed observations

### Dashboard, guide, and profiles

Run `20261103T174050Z-dashboard-single-virtual-controller-_ikn6tcz` demonstrates:

1. Dashboard 2.0.17559.0 boots with the copied profile signed in.
2. RB navigates home/social/games/apps/settings. System settings and storage
   device/category screens render and accept controller navigation.
3. Guide opens Xenia's ImGui context menu; B closes it and dashboard input
   continues. This validates the host fallback, not the installed native guide.
4. Social reports disconnected from Xbox Live. Selecting Friends does not
   produce a usable friends list.
5. Settings > Profile opens the host gamercard editor. It shows a locally
   Live-enabled/Gold account while the dashboard remains disconnected.
6. Storage > Hard Drive > Profiles shows one `Corrupted Profile` entry, 0 KB.

The profile payload directories match original, seed, and run byte-for-byte.
One pre-existing sidecar is all-zero; another profile lacks a sidecar.
`ContentManager` can accept a large zero-filled sidecar as raw metadata, while
missing metadata leaves size zero. The storage scan also calls unresolved
`XamProfileLoadAccountInfo`. These are concrete interoperability leads; the
specific guest branch producing the single row remains unproven. Do not equate
the dashboard label with demonstrated Account/GPD corruption or data loss.

An earlier two-controller run opened the fallback but did not respond to later
close/navigation inputs. Retesting with only the research controller visible
to that Xenia process restored those operations. Input routing is a confound
to investigate, not evidence that the dashboard or physical pad was modified.
The research-only device-ignore environment and exact action records are saved.

A separately restarted user-created test profile reloads into local slot 0
with identical Account bytes and identity. Another capture loads both existing
profiles into distinct slots 0 and 1 without changing their Account files.
These are Account reload/sign-in passes, not dashboard container validity,
settings/save persistence or online authentication passes. See
`evidence/profile-restart/` for the before/after comparisons.

### Avatars and XNA

The Avatar Editor reaches the `Choose an avatar as a starting point` screen,
but no avatar models appear. Its log contains unresolved `XamPngDecode` and
package lookup calls; avatar asset sizing/retrieval is also stubbed in source.
Rendering the page is not a create/edit/save/render pass.

The installed `Xna_TitleLauncher.xex` renders an update prompt when launched
directly, without a managed game's launch context. All 174 system files from
a fresh installation of the supplied USB update match the original and copied
system trees by path, size and SHA-256, including the XNA/Community components.
Missing or mismatched supplied system bytes do not explain this observation.
The prompt alone does not establish a failed update-detection branch.
Normal direct STFS launches of both supplied containers fail with `C0000225`
because the default `GAME:\\default.xex` is absent; neither reaches managed
code or the installed launcher. These packages have content type `00000002`
under host title `584E07D2`, separate from the scanner's `02000000`
CommunityGame classification. A separate installed-package chooser/launch-data
path exists, and raw managed `MZ` loading is explicitly unsupported. Correct
installation and launcher context must be tested before concluding that the
installed system runtime is defective. See `evidence/xna-detection/`.

A later bounded installed-route probe stages both unchanged containers and
their original header prefixes in the supported content layout. This is a
prepared seed, not a claimed UI installation. The installed system launcher
again shows the update prompt at both 25 and 35 seconds; no game chooser appears
in those inspected captures. Thus the launcher's prerequisite gate is reproduced
with both packages present. Its exact version/package/runtime check remains
untraced, and neither managed game reaches gameplay. See
`evidence/xna-detection/installed-route-capture.json`. Ghidra's XEX loader failed
LZX decompression of this launcher and imported no memory blocks despite a zero
headless process exit, so no decompiled failure-branch evidence is claimed.

| Package | Container title | Internal GameInfo title | Virtual title |
| --- | --- | --- | --- |
| CastleMiner | `584E07D2` | `58550900` | `95332a3ac7f1405581f6427aa57e0edb` |
| CastleMiner Z | `584E07D2` | `585509DC` | `ffb7219618e24d57a9a0962c8a3ac917` |

Their managed payloads must use the XNA launch path. The shared container title
is insufficient to identify the two games; neither is an independent XBLA XEX.

### HTTP and Fake Live

The pinned POSIX `HostRequest` implementation returns `CANNOT_CONNECT` for
request operations. Its guest-side request/worker/callback machinery already
exists. A host-control HEAD request returned HTTP 200 from HTTP example.com,
verified-TLS HTTPS example.com, and HTTPS www.youtube.com on this machine.
That establishes host connectivity, not guest XHttp functionality or legacy
YouTube API compatibility. App captures identify which requests actually reach
the missing backend before proposing service adapters.

Twitch supplies the direct runtime confirmation: its POST to
`https://api.airbrake.io:443/notifier_api/v2/notices` and later HTTP avatar
request reach `no host HTTP client on this platform`. YouTube reaches a Live
Gold/sign-in gate; that capture does not demonstrate its service request or
identify HTTPS as its sole blocker. Transport, certificate, authentication,
service-protocol and media acceptance therefore remain distinct matrix rows.

Local Live sign-in eligibility, XLIVEBASE service state, discovery, social
state and title sessions are separate boundaries. Existing code has real
System Link/session bookkeeping, but empty friend enumeration, false friendship
checks, disconnected logon, failed accepted-invite lookup, no-op party invites,
and denied voice creation. See the [architecture map](17559-architecture.md).

Automatic direct P2P remains an explicit research gate: existing signed address
lookup candidates do not establish a complete public friend-request inbox and
two-way connection negotiation mechanism. No relay is permitted; reachable
HTTP ports alone do not make a firewalled peer reachable. Single-host tests
cannot establish internet traversal reliability.

## Coverage and remaining measurements

The scenario matrix uses only `passed`, `failed`, `blocked by a prerequisite`,
and `untested`. Startup, rendering, a service request and end-to-end operation
are separate rows. Deadline exit 124 is capture control, not a hang diagnosis.
Screenshots are accepted only after inspecting process-matched window captures.

See [the scenario matrix](17559-scenarios.csv) for the recorded reproduction
steps, exact evidence paths, first blockers and scope of each result.

The [controller checklist](17559-controller-checklist.md) preserves the manual
steps and acceptance criteria for native guide, profile/storage operations,
avatars, friendships, invitations, text, parties, voice and gameplay.

The completed matrix records 132 scoped scenarios: 45 passed, 11 failed,
49 blocked by prerequisites and 27 untested. A passed diagnostic or startup row
does not promote its separate end-to-end row. There are 36 finalized captures;
all input seeds remained unchanged. Nine early captures predate detailed
process-cleanup fields; the final process inventory found no running research
emulator or virtual-controller process. The final preservation recheck found
zero mismatches across the 361 hashed core files, 9,821 shared-content file
size/mtime records and original installed AppImage hash. The private bundle
index is `evidence/bundle-index.json`; do not publish its personal payloads.

The BO1 target is 60 FPS at native 1280x720 presentation in reproducible private
multiplayer/Zombies scenes. Cold shader work, steady-state frames and competing
build load must be recorded separately. There is no BO1 performance patch in
the active installed patches or the captured upstream game-patches revision.
Frame presentation time is not a substitute for separately measured CPU/GPU
execution time, and menu FPS is not a gameplay result.

BO1 reaches Zombies > Solo > Kino der Toten and renders the first-person scene
with its weapon, crosshair and 500-point HUD. The original host storage picker
did not advance under the tested inputs; the existing
`--storage_selection_dialog=false` setting allowed scene entry. The run also
uses `--mount_cache=true`, though later utility-drive accesses still fail;
`evidence/bo1-cache-diagnosis.json` records the VFS/device lifecycle hypothesis
without attributing performance to it.

Initial MangoHud captures are **confounded diagnostics**, not accepted FPS
baselines. The user reports roughly 20–30 FPS without it versus about 8 with it;
the early captures also overlap compilation. No-overlay measurements are below.
Internal resolution is 1280x720 with 1x draw scaling; host presentation
size is recorded separately. CPU/GPU execution timings remain an instrumentation
prerequisite and cannot be reconstructed from utilization percentages.

The native unstripped binary supports a lighter external measurement:
[`frame_wrapper.py` and `frame_sample.py`](../../tools/research/FRAME_SAMPLING.md)
read only its existing four-byte guest-present counter once per second.
The wrapper records separate native/wrapper hashes and enables inspection for
that research process only. No measurement layer, debugger or emulator code is
injected. These are interval averages, not individual-frame timing percentiles.
All windows below completed with sampler reason `duration`; no build was active,
MangoHud was disabled, internal resolution was 1280x720 and draw scaling was 1x.

| Native scene/window | Duration | Presents/s | New shader/pipeline events | Readback failures in window |
| --- | ---: | ---: | ---: | ---: |
| Kino starting room, initial sample | 60.001 s | 27.03 | 43 | 15,661 |
| Kino starting room, later sample | 30.001 s | 22.90 | 0 | 19,582 |
| Array, empty local Team Deathmatch, fixed spawn view | 30.001 s | 35.97 | 0 | 0 |

Kino's samples overlap `Failed to complete the submission for memexport staging
readback`, first logged at guest-present counter 8157. Array also encountered
those failures during loading, before its sampled interval. Neither establishes
complete rendering/data correctness or a healthy 60 FPS baseline. Array renders
first-person gameplay and firing changes ammunition from 15 to 14; it uses one
local player in one process, not a peer-network test. The later unattended Kino
game ends; its exact death boundary is not logged.

Full sampling results and phase/error counts are in the two
`bo1-native-*` run directories' `performance-summary.json` files. The bounded
`evidence/bo1-memexport-diagnosis.json` identifies submission/readback bookkeeping
as an investigation boundary, including a possible staging-slot eviction path.
That is a static hypothesis. Official captures have lower frame exposure than
the first failure, so a native-only regression is not established. Correctness
must precede performance tuning; CPU/GPU execution times remain unmeasured.

Guide opens the host context menu over both BO1 modes. B did not dismiss it
under the tested pure SDL virtual pad, so in-game controller continuation is
not passed. A native-modal input-routing limit and GPU progress issues must be
distinguished; the evidence does not establish either as the sole cause.

Remote-peer traversal, remote voice quality and macOS execution remain untested
because only one Linux host is available. Native guide and publisher-service
compatibility are not inferred from local networking unit tests.
No two-process title-level peer acceptance is claimed; the local networking
suite is unit coverage and does not replace isolated peer identities/processes.

The ranked follow-up work is in [17559-roadmap.md](17559-roadmap.md).
