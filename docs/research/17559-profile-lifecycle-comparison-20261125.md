# 17559 profile lifecycle compared with current Afterdark

Status: V130 system-contract review followed by the coherent V131 request and
outcome implementation. All seven V131 test runs passed; native sign-in and
shutdown still failed. The combined HDD gate remains open. This work follows
the standing [systems approach and
console fidelity rule](17559-console-fidelity-policy.md).

The initial comparison used V130 source map
`3a7c6c7a567210b67bef7b245183cb99a8002242b51cd2c6071e07216fc652a7`.
Current V131 source map is
`484392c97d89ca8e5a5ec72a7d4a9ce6fa74d654471b08255efae951ed5a70dd`
(2,430 source files). Its seven direct test runs passed under receipt
`cbe73dba7fac660411547ee2a44184490d5a138df395886dcf0f85f8d133ef0c`.
They establish their component coverage; they do not establish native profile
creation, sign-in, appearance editing or game rendering.

The primary runtime reference is the matching 17559 XAM image, SHA256
`f9e44cd6f4b04875ddc5a3b8fd519785ba2b0488b1c4622f818b740ba4064313`.
Existing executable maps and pinned contract reports supply the other body,
resource and consumer facts. Different dashboard versions retain their own
identities. No supplied executable, profile payload or console key is added to
this source document.

## Actual system boundaries

Afterdark currently combines original guest XAM/UI bodies with shared host
storage, Account, GPD and roster services. The host representation serves the
existing emulated file and HLE consumers. It must converge with the genuine
native cache and completion, rather than acting as a second independent
profile implementation.

| Lifecycle phase | Matching native contract | Current implementation and remaining obligation |
| --- | --- | --- |
| Discover and select | Native profile enumeration, actual user/device and asynchronous result ownership precede sign-in. An in-progress list count is not completed inventory. | Registered writable sources, authenticated Account snapshots, leased inventory and ordinary folder mounts are active shared services. Native and host UI must preserve the same user/source. An intermediate zero count does not establish deletion. |
| Load Account/GPD | Mounted profiles supply the selected Account and settings; successful loading is distinct from sign-in. | The active mount is a folder-backed HostPathDevice with Account service and normal GPD files/cache. This path does not require native CON raw-Header parsing. V130 preparation passed; legacy Header format is not established as its sign-in failure. |
| Create local profile | Native 230/234 has paired producer/result ownership, then a separate 20B logon request. The genuine cold metadata producer supplies 0x93D6 bytes and has directory effects before its final create call. | The active owned HLE creation recipe writes durable Account/GPD, installed stock pictures and INFO1, but still emits a legacy 0x148 aggregate Header. This is not the native cold-container producer. Preserve the distinction and all existing members during any upgrade. |
| Initial pictures/appearance | MiniCreator chooses actual appearance and later commits it through the profile services. | Installed stock tiles, preset previews and a 1,000-byte INFO1 default exist. A preview or gamer picture is not evidence of an attached, animated avatar model; native default timing/equivalence still needs acceptance. |
| Sign-in/out ingress | 20B at 0x8199C630 and 224 at 0x8199E3E0 retain original caller, flags, user slots and sync/async result policy. | PM preparation, exact Account/source comparison, original-call execution and old/new roster ownership exist. Actual V130 failed at native Schedule producer binding, after preparation. Entering or returning from the generic executor does not alone authenticate native completion. |
| Request to Task | Null-overlap 224 uses stack output bodySP+0x5C; nonnull-overlap 224 uses the genuine native request packet+0x18. E148 has its own stack producer. Tail adapter 0x81671C18 preserves SP/LR. | V131 binds the actual typed producer, retains its scalar birth receipt through native close/free/reuse and avoids later packet dereferences. Its real asynchronous initializer returned zero; later worker/protocol/publication completion remains unobserved. |
| Native publication and errors | Actual worker, native cache publication, protocol result and caller completion are distinct. Native initializer errors and canceled workers have their own paths. | V131 keeps requested inputs immutable and separately prebinds the real canceled-empty outcome, its native title context and selected read/map/retirement effects. Genuine initializer error completion does not invent a child Task. Unproved partial outcomes and initializer-error source recovery remain uncertain. The live run has not proved publication or completion. |
| Complete and release | CompleteIO 0x816A2588 atomically claims the native context, closes its actual tagged Task and frees the packet at 0x816A26E4 before returning. | The original overlap, callback, Task and source owners must remain distinct. A new host packet pin extending to Schedule/224/CompleteIO return could obstruct native free. No later packet dereference or fabricated Task birth may be justified by an earlier pointer alone. |
| MiniCreator and foreground Editor | Mapped wizard phases invoke Mini UI 6A4 or Editor launch 7DB. Mini requires an owned request/visibility/cancel/teardown lifecycle; its FE21018 queue-copy proof remains unclosed. Editor is a genuine foreground TITLE. | The paired SYSTEM 7DB handoff and TITLE-aware avatar owner now exist. Mini execution, actual title handoff, return/home and real attachment remain unaccepted; older claims that 7DB and LoadAnimation are absent are superseded. |
| Persist appearance and tiles | Mini and Editor SetManifest calls are followed by actual ProfileSync 21F; tile workers also use 2F1/2FB with their own buffer semantics. ProfileSync is a user mask. | Shared INFO1/GPD and picture transactions exist, but external 21F/2F1/2FB routing is incomplete. SAVE49/Header/PIS05/real IO-close/cache promotion is inactive. A successful SetManifest helper does not prove native save/restart. |
| Native container creation/save | Native metadata, aligned Header bytes, real descriptor/checkpoint controls, file writes/close and cache promotion are one producer/consumer chain. | Existing authenticated native CON/aligned-Header support is active. Cold creation and complete native save remain separate missing participants. Preserve actual padding, unknown fields and source identity; do not manufacture formatted state or control success. |
| Render and consume avatars | F2/F3 native engines serve initialization, appearance, assets, geometry and animation for dashboard, Editor and games. | Ten native engine/profile/animation routes exist, including actual TITLE callbacks. Remaining custom/mipmap/icon/binary/enumeration/WearNow paths still contain incomplete behavior. Actual stock models, editing, animation and Doritos consumption remain mandatory runtime checks. |
| Rename/delete | Native APIs must coordinate Account, Header, catalog, roster and storage outcomes. Profile-only deletion preserves owned game saves. | Shared host helpers exist, but external native rename/delete/save/load coverage and complete ABIs remain gaps. Host profile deletion also lacks the common package-plus-external-Header handoff and can report failure after physical removal if its cache cut is busy. These are source findings, not V130 causes. |
| Restart and recovery | Durable completion must survive restart and fresh enumeration with the same identity, settings and appearance. | Component journal replay, exact source binding and cache validation are tested. Current native save/restart and full profile lifecycle acceptance remain open. A flush, cached row or clean exit is insufficient by itself. |

Three native terminal outcomes need separate treatment. An initializer error can
complete the original overlap without a successful worker birth, after genuine
provisional native presence changes. The C838 cancellation branch publishes
four reset rows through 0x8199DA34/return 0x8199DA38; this is an empty native
outcome, not necessarily the old or requested roster. The normal publisher can
also follow per-user adjustments. Its actual rows must match a proven outcome
before host settlement. The original requested roster and expected baseline
stay immutable; unproved partial outcomes remain explicitly uncertain.

## Coherent implementation and validation units

1. **Original identity/request lifecycle.** Cover 20B/224, native sync and async
   producer outputs, actual allocation/publication/birth, E148/C838 workers,
   initializer errors, cancellation, protocol/cache publication, CompleteIO,
   Task and packet release, and old/new source plus visible identity convergence.
   Existing original callers, read scopes and native completion remain authoritative.
   Required cases include worker completion before Schedule return and failures
   before a child Task exists. The output-location check alone is not this unit.
2. **Durable profile/container lifecycle.** Cold creation requires its actual
   metadata producer and source-issued provisional participant. Saving an existing
   authenticated native CON can proceed independently through the complete
   multi-GPD/picture/Header/IO-close/PIS05/cache-save unit and its own before-token;
   cold creation is not a universal prerequisite for that path. Missing or legacy
   Headers require their separate proven creation/repair participant. Close real
   external profile APIs from their ABIs through those shared services. Reuse
   common package/Header deletion and preserve a durable result through cache
   contention. Ordinary bridged folder reads remain valid independently of raw
   CON support.
3. **Avatar lifecycle and consumers.** Use the working identity and persistence
   contracts for MiniCreator, actual Editor attachment, remaining native asset
   operations, save/reload, dashboard/Guide and a real game consumer. Preserve
   actual native blobs, unknown fields and both body types. Tiles and component
   tests cannot substitute for models, advancing animation or restart identity.

These are dependencies within the existing combined HDD gate, not new gates or
permission to postpone its mandatory scenarios. Independent units can be
implemented in parallel after their shared interfaces are established. A
checkpoint requires an actual usable dashboard/sign-in/game-save/Guide/exit
path and a test of the packaged AppImage itself; the current V131 run does not
satisfy that requirement.

## V131 integration and actual consumer result

The nine-file implementation covers typed producer output, Task/birth lifetime,
initializer return/error ownership, actual requested or canceled publication,
old-model writer drainage and final resource retirement together. Twelve new
cases passed alongside all existing selected cases. Direct runs passed base
331/28,245 assertions, network 125/11,212, kernel 1,448/91,190, VFS 136/9,736,
UI 124/4,863, focused CPU 5/130 and focused installed-system 179/21,427. The
installed selection explicitly included three hidden cases. Focused runs overlap
full-suite coverage; the CPU instruction corpus was not run in full.

In the actual isolated ELF run, unsigned dashboard and native Guide/profile
list rendered. Selecting disposable local123 requested slot 1. The actual native
initializer returned zero without the former producer-binding denial or
execution-uncertain result. A genuine C838 Task/read/commit record was observed;
protocol, roster publication and owned CompleteIO were not. The dashboard stayed
unsigned, and later Guide input did not reopen the overlay. This is a failed
sign-in lifecycle test, despite the successful initializer and component tests.
The next review follows the whole worker/wake/publication/completion chain.

An ordinary owned WM close was requested. The process exited one after 425.05
seconds, with complete reaping, no forced signals and checked inputs preserved.
Successful host cleanup is not successful native shutdown. Avatar Editor,
game/save, actual packaged AppImage and combined gate acceptance remain open.
MangoHud and GameMode were disabled; original user data was not used.

| V131 record | SHA256 |
| --- | --- |
| Coherent source freeze | `2b4ed71a97b20eb387ce1402dd0e97ca7035e0c5e95f53e9c876c471a42c3102` |
| Source application | `ce437e2b25af79fedfac657eb5493a95da785055bb9d8aa791c4d92f3dd064cf` |
| Build | `b980534af4c408c4d76f6535fa23ca872c3567408c67632a0c6115d557ddf09f` |
| Seven direct runs | `cbe73dba7fac660411547ee2a44184490d5a138df395886dcf0f85f8d133ef0c` |
| ELF | `e116016a6ff865a4de3a0aa5ba660b52bf92807935c93e5ccb2d3b27606ac378` |
| Actual runtime result | `bc26d56c7110a0adb40068f0063bc9767b5dd82930da5cd046d2044d284b751d` |
| Root visual/cleanup assessment | `f660e124ca1a43ca1257c607a3a930223a222bc6248b06a59d47164b612d7eb6` |

## Evidence

Detailed local evidence is retained under `evidence/hdd-gate` in the research
state, with raw runtime data kept private. The following source-bound reports
back this comparison:

| Record | SHA256 |
| --- | --- |
| Current storage lifecycle comparison | `217b5b6bb0dfdea54e09c6c559a431e61275b8cf3701f6883474bf0be1515260` |
| Current creation/avatar lifecycle comparison | `b994e38c78f2d73a2c6b6a7b551fc03d763f9f3e5201241e44bb203af2a73b4d` |
| Independent native producer output/free contract | `9e9ac738300ff00b3dd47c444331533f35721194053a6f1fa0eb9a2ca426af24` |
| Original identity/request lifecycle comparison | `2b9ea61efc760088c256d54094726162429cc03d303172d5a7040bde4f6b4277` |
| Native terminal-outcome supplement | `2d36552571fd3869031cb5312646c8d5afcbe66c0826ec7b376c5958a9e6711f` |
| Actual V130 original-ingress observation | `2679120661c6ee6ef5ebab3a1a5da48bc998fdf50ead367b301565ae3995b933` |

The V130 runtime displayed the unsigned dashboard/Guide and native profile list.
Its first observed denial was Owner8226 at 0x816BA440. The exact failed
subpredicate was not recorded. No authenticated native own-return, protocol,
CompleteIO or roster publication followed. Manual WM close exited zero after
411.18 seconds, fully reaped without forced signals. This observed result and
the static lifecycle findings above remain explicitly separate.
