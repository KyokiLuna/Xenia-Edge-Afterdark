# Dashboard 17559 architecture map

Static inspection dated 2026-11-03, pinned to
[`b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1`](https://github.com/has207/xenia-edge/commit/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1).
This maps existing boundaries and later work; it does not claim runtime success.
The scenario matrix and run evidence are separate baseline artifacts. No personal
profile identifiers, user files, system binaries, or game binaries belong here.

## Observations: installed system files and HDD

- The USB update installer reads `system.manifest`, extracts flash containers,
  places shared content such as avatar assets under system extended content,
  and puts other files in versioned system extended directories. File presence
  and manifest placement are prerequisites, not proof that an app works.
  See [installer classification](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/ui/system_files_installer_wx.cc#L288-L381) and [manifest file lookup](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/system_manifest.cc#L67-L94).
- The emulator exposes the content root as
  `\Device\Harddisk0\Partition1\Content`; installed `flash`, `sysext`, and
  `sysaux` directories become guest devices. `\SystemRoot` points at flash when
  available. Some other raw HDD accesses still use `NullDevice` behavior.
  See [device setup](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/emulator.cc#L2548-L2585). This is not a complete raw HDD implementation.
- System-title preparation falls back to the title directory when system files
  are absent, links `media:` to `\SystemRoot`, and loads system `xam.xex` or
  `$flash_xam.xex` for resources. Failure to load that resource module does not
  fail the title. See [system-title preparation](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/emulator.cc#L2415-L2451).
- The configured kernel version defaults to 17559. That value alone does not
  establish the installed update's version or completeness. See [version](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/kernel_state.cc#L47).
- `mount_memory_unit=true` mounts a folder-backed `MU:` VFS alias, but the XAM
  storage-device list still contains only the dummy HDD and read-only optical
  drive. That switch alone does not provide a second writable content device
  for native copy/move operations. The device registry and content/VFS routing
  must agree. See [MU mount](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/emulator.cc#L1907)
  and [XAM device list](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_content_device.cc#L35-L77).

## Observations: XHttp and HTTPS

- `xhttp_host.h` is the host-client seam: session, connection, request,
  send/write/receive/read, raw headers, redirects, and cross-thread abort.
  Calls block on the worker side and return WinHTTP-style errors. Request
  options include timeout, redirect policy, credentials, and certificate policy.
  See [host interface](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xhttp_host.h#L26-L99).
- Guest XHttp already has per-request workers and notification delivery through
  guest-thread callbacks; its behavior must be preserved when adding a backend.
  See [worker ownership](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_xhttp.cc#L170-L222) and [notification delivery](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_xhttp.cc#L474-L558).
- Windows implements the host interface with WinHTTP, including secure requests
  and TLS configuration. POSIX creates placeholder session/connection objects;
  request operations return `X_ERROR_WINHTTP_CANNOT_CONNECT`, and reads return
  zero bytes with that error. See [Windows backend](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xhttp_host_win.cc#L239-L352) and [POSIX stub](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xhttp_host_posix.cc#L19-L62).
- Therefore Linux/macOS HTTPS requires a real host backend even before app-level
  service compatibility is evaluated. The interface routes to the server named
  by the title; it is independent of a functioning Xbox Live social service.

## Observations: profile persistence and Fake Live state

- `ProfileManager` locates profiles in the content tree, mounts them, and reads
  or writes encrypted `Account` files. Account updates refresh the cached
  profile and broadcast sign-in changes when Live eligibility changes.
  See [account loading](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/profile_manager.cc#L264-L308), [profile layout](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/profile_manager.cc#L554-L599) and [account update/write](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/profile_manager.cc#L636-L698).
- `UserProfile::WriteGpd` writes profile database data; `UserTracker` flushes the
  dashboard and active-title databases, including after setting updates.
  See [flush targets](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/user_tracker.cc#L125-L135), [GPD write](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/user_profile.cc#L184-L211) and [setting persistence](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/user_tracker.cc#L787-L810).
- A Live-enabled local profile reports `SignedInToLive`; defaults can generate
  an online XUID and populate subscription/country/language values locally.
  In contrast, XLIVEBASE logon returns disconnected and its NAT/service-info
  queries return not logged on. See [local sign-in](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/user_profile.h#L108-L125), [local defaults](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/profile_manager.cc#L701-L723),
  and [service status](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/apps/xlivebase_app.cc#L68-L100). Local identity and service connectivity are
  separate states despite these inconsistent guest-facing results.

## Observations: avatars and editor prerequisites

- Avatar initialization is disabled by default and contains an existing editor
  exception. Success there does not supply a complete avatar implementation.
  Local manifest reads use dashboard profile settings; manifest writes call
  `UpsertSetting`, which reaches profile persistence.
  See [initialization/read](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_avatar.cc#L16-L112) and [manifest write](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_avatar.cc#L164-L207).
- Asset-size queries return zero; asset retrieval, custom assets, icon/binary
  requests, and asset enumeration contain stubs. An installed-asset description
  constructs a content locator, which does not establish that assets were read
  or decoded. See [asset stubs](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_avatar.cc#L129-L162), [asset lookup](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_avatar.cc#L268-L311),
  and [enumeration](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_avatar.cc#L386-L430).
- Update installation can provide shared avatar content, but editor launch,
  resource lookup, asset access, rendering, and manifest persistence are distinct
  prerequisites. The URI resolver recognizes `avatareditor`, while the launch
  handler currently opens only the dashboard title URI.
  See [URI recognition](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_info.cc#L895-L908) and [launch restriction](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_info.cc#L974-L993).

## Observations: XNA launcher and update detection

- An XNA system launcher path already exists: when the newer XN launcher has no
  launch data, XAM selects an installed indie package and constructs launch
  data. The chooser enumerates installed marketplace content for that launcher.
  See [launch-data fallback](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_info.cc#L476-L517) and [package chooser](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_ui.cc#L1263-L1308).
- This is separate from loading a raw managed executable: `UserModule` rejects
  an MZ executable as unimplemented. Do not infer that rejection describes the
  installed system-launcher path. See [module-format handling](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/user_module.cc#L140-L164).
- Neither the chooser nor the cited launch-data path implements a missing-XNA-
  runtime/update diagnostic. An empty package list is reported as no installed
  indie games. Distinguish missing launcher/runtime files, missing game content,
  and an unsupported interface through file/import evidence; this inspection
  does not establish a complete runtime-update detection mechanism.

## Observations: XUI, native HUD/guide, and host fallback

- Exported XUI support here is explicitly limited to string tables: package
  parsing, localization fallback, lookup, and freeing. Common XAM strings are
  loaded from `huduiskin.xex`. These resources do not establish a native guide
  lifecycle, scene/control implementation, or complete exported XUI coverage.
  See [XUI boundary](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_ui.cc#L1389-L1414), [string-table loading](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_ui.cc#L1550-L1709), and [common strings](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_info.cc#L1534-L1554).
- Host fallbacks already provide sign-in, profile creation/editing, gamercard,
  and achievements dialogs through ImGui. The controller Guide button toggles
  the host context menu; the NUI guide export returns access denied after its
  check. See [dialog routing](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_ui.cc#L1123-L1261), [Guide input](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/app/emulator_window.cc#L3481-L3491), and [NUI guide](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_ui.cc#L1371-L1385).
- Dashboard rendering, native `hud.xex`/guide behavior, exported XUI, and host
  dialogs are separate investigation paths. The inspected code does not prove
  that installing HUD/skin modules routes Guide input into the native guide.

## Observations: sessions, social APIs, and direct networking

- Reuse implemented `XSession` member/state/details bookkeeping and System Link
  key registration. XGI creates sessions through that object and checks local
  Live sign-in for relevant flags. Existing System Link transport selects LAN
  broadcast/configured IPv4 tunnel or a separately configured Kai engine.
  See [session creation](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/apps/xgi_app.cc#L342-L371), [session object](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xsession.cc#L122-L339), and [transport](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_system_link.cc#L41-L74).
- Friend enumeration is always empty, friendship checks retain false, and
  presence initialization only returns success. Accepted-invite info fails.
  Friends/messages/invite UI and friend mutation names in the ordinal table
  do not establish implemented handlers. See [friends/presence/invite](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/apps/xlivebase_app.cc#L115-L218),
  [friend check](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_user.cc#L710-L758), and [social ordinal declarations](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_table.inc#L499-L518).
- Party-list calls report not in party. Party game-invite calls complete
  successfully without transmitting an invite. Voice creation denies access;
  headset/mic report absent. See [party](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_party.cc#L26-L74) and [voice](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_voice.cc#L18-L50).
- Notification listener/polling plumbing exists, but remote social state and
  correctly encoded guest event producers remain required. See [notifications](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/src/xenia/kernel/xam/xam_notify.cc#L56-L95).

## Later recommendations and unresolved dependencies

1. Preserve the baseline before feature work: identify missing resources and
   real interface failures; retain accurate errors and persistent local state.
2. Implement POSIX HTTP behind the existing host seam, preserving cancellation,
   callback ownership, redirects, certificate policy, and error behavior.
3. Trace avatar/editor, XNA, and guide calls separately. Reuse storage and
   resource mechanisms; avoid broad success stubs or new title-ID workarounds.
4. Gate network social work on automatic metadata discovery/signaling. Players
   configure no services or ports; application/game payload stays peer-to-peer;
   no relay fallback is allowed. Unreachable peers are an expected outcome.
5. Signed [DNS/Pkarr lookup](https://docs.iroh.computer/connecting/dns-address-lookup) and [Mainline DHT lookup](https://docs.iroh.computer/connecting/dht-address-lookup)
   are two discovery candidates, not selected complete signaling solutions.
   Persistent codes need address refresh; unsolicited requests and coordinated
   ICE need a return channel. No established automatic public request inbox/
   offer-answer service has been demonstrated by this bounded review.
6. [STUN](https://www.rfc-editor.org/info/rfc8489/) supplies mappings, not a complete connection solution. IPv6 and
   HTTP port numbers do not remove inbound firewall policy. Do not promise
   cross-network NAT success from single-machine checks or a local connection.
7. After that dependency is resolved, build one accepted/blocked contact state
   shared by guest APIs and UI, then presence/messages/invites, session bridging,
   party, and voice. Transport alone does not reproduce title service backends.

Follow the repository's [information-sourcing and license rules](https://github.com/has207/xenia-edge/blob/b65dd2514a5fdb92a2d4fd7ccfce5fdd4d4647d1/.github/CONTRIBUTING.md):
no XDK-derived information; review third-party licenses; express fixes through
common interfaces. This document contains no runtime validation claims.
