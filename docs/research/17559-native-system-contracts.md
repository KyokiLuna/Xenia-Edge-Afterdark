# Native SYSTEM UI contracts for dashboard 17559

Established 2026-11-04 from current source and the supplied, independently decoded
17559 XAM/HUD/VK images. This is the integration contract for the remaining
Guide/XUI work. Native execution acceptance is separate. HDD/profile creation
remain after Guide/XUI; their services are not reimplemented in this package.

## Closed Guide foundation cohort and release boundary

Guide/XUI/shared XAM is **CLOSED for the tested local Linux cohort**, subject to
explicit later HDD/Profile/avatar/XNA/Live/app-service prerequisites. Actual
manual AppImage acceptance is user-reported Rotastic gameplay, native Guide
navigation/close/reopen/control return, Alt-Tab recovery and Guide-open window
close. Keep that report separate from the automated NoSwap, keyboard, native
render/input/lifetime proofs and their narrower limits.

AppImage85fbdd4b/payload9a2d84a2 preserves the accepted05f0 cohort:824cases/37,686
assertions, CPU and302-source proof, with no new C++ for packaging/fresh GUI.
The final839e upstream decision table covers all123 paths and the22:57:43UTC
refresh confirms unchanged074d3. Final preservation matches10,182 originals
against the user-confirmed baseline. Use the release's normal `start-guide.sh`
with isolated manual data and external console/game/profile files. The payload
requiresGLIBC_2.44 and was tested on host glibc2.44; other distributions, macOS,
FUSE, broader IME/multiunit text and other-user endpoints remain unverified.

Fresh V2 passes only frontend startup: decline the expected no-profile modal,
observe normal empty library, close naturally with runner/container0, no signals
or faults, unchanged seed and all four observed PID paths absent. Its30 artifacts
and exact retained static extraction are authenticated separately; no profile,
title/native request or FUSE acceptance is inferred. Fresh V1's modal prerequisite
was mishandled by automation, not a proved backend defect. Both that failed gate
and `ag8_02h3`'s invalid-command/false gates remain preserved. Retained static
content, no live consumer and no associated mount are distinct from deletion.

## Architecture decision

Use the installed native XAM task scheduler and FE UI manager for UI policy.
XAM already contains request scheduling, task context, dynamic HUD/VK loading,
app save/restore, navigation, scene timing, and request/event/APC completion.
The emulator supplies the execution environment, scoped module/import binding,
input transport, GPU transport, presentation and safe resource retirement.

The host must not build a second Guide/keyboard manager beside the native one.
The earlier one-shot probes establish reusable primitives and evidence; their
manual scene construction and retained FF registration are not the production
lifecycle. Existing native-manager behavior must be traced before replacing it.

| Authority | Native implementation | Emulator responsibility |
| --- | --- | --- |
| Task scheduling and task handles | XAM Task family, real task/TLS chain | Correct SYSTEM process/thread/TLS, waits, events, kernel handles and cancellation primitives |
| UI requests and app policy | Native FE manager and dynamic FF app | Admit the native domain, preserve original caller context, bind imports/loaders consistently |
| HUD/keyboard switching | Native save/close/load/secondary-init/restore | Preserve module/code/data lifetimes; observe transitions without duplicating private state |
| Layout, focus, navigation, animation and painting | Native XAM/XUI/HUD/VK | Supply genuine input and rendering progress in the native execution context |
| Request result and completion | Native request/task/event/APC machinery | Correct kernel delivery, original caller/overlap lifetime, generation-safe teardown |
| Command execution and pixels | Guest recording plus host GPU backend | Checked immutable source, real submission, source fence, independent image publication |
| Screen visibility | Native UI policy | Independently revocable presentation plane and repaint, including when the title stalls |

## One native domain, consistent boundaries

A native UI domain is one exact Kernel/XAM module generation, with a compatible
native binding profile for its UI modules. Ordinary titles and unrelated HLE
services keep their existing routes. This is not a process-wide redirect of
all XAM exports.

The domain's native Task, XMsg, SysApp registration, resource and allocator/free
families must have a coherent call graph. Native intra-image branches bypass
HLE exports. Routing an external call through a different HLE registry, task
handle implementation or allocator can therefore split one logical operation
between incompatible owners. An API name or a successful zero return is not
proof of correct routing.

The scoped SYSTEM UI provider now claims the related native Task, XMsg,
notification, input, SysApp, resource and allocator families together. The
ordinary title's HLE routing remains available. Installing this profile is
not native service readiness: the service must initialize and observe its
native manager before admitting UI requests. Any retained host registry
metadata protects/observes the native domain rather than controlling native
FF replacement.

Scoped loader/resolver integration now propagates the native domain through
child loads and resolves its static and dynamic imports consistently.
Runtime/execution leases intentionally refuse unsafe unload. NULL/zero-module
lookups still resolve the actual title executable; unrelated read-only module
lookups keep their existing behavior. Native compatibility checks apply when
loading/reusing the claimed root or a domain member.
Establish required process/module TLS and thread attach/detach behavior without
confusing the native XAM task TLS chain with PCR/KTHREAD or PE module TLS.
Resolve this bridge as part of the lifecycle; do not relax unload checks or
temporarily change the foreground executable.

## Ownership scopes

| Scope | Identity and end boundary |
| --- | --- |
| Native system domain | Kernel epoch plus exact XAM/module generation; shared native initialization and manager state; ends at Kernel retirement |
| Foreground | Exact TitleExecutionState generation and owning Kernel, including preparing/stopping transitions; controls visibility and caller admission |
| Native app/window | Native descriptor generation, module/code lease, actual instance/root and borrowed shared DC; native manager controls replacement |
| Native task | Actual native task reference and TLS/current-app/dispatch context; SYSTEM thread type alone is insufficient |
| UI request | Native request plus original caller, packet/string/output/overlap and event/APC context; ends at native completion/cancellation drain |
| Recorded source | Session owner and nonreused Source serial; mutable update through frozen/pinned recording to actual native GPU completion |
| Presentation image | Backend/owner epoch and immutable image version; independent of Source and per-frame Request lifetime |
| Presentation consumer | Actual paint submission and consumer lease; ends at its fence or a proven GPU stop after publication has been withdrawn |

Current relaunch destroys/recreates KernelState. System-domain persistence
therefore means persistence across Guide close/reopen inside that Kernel,
not persistence across relaunch. A new Kernel must initialize a fresh native
generation after old tasks, publications and GPU resources have retired.

## Native startup and readiness

GuestRuntime Ready currently proves execution of the specific supplied plan.
The existing heap/CRT plan does not prove a complete task/UI-manager boot.
Establish a compatible native startup cut with verified signatures, ordering,
initial-state requirements and genuine native readbacks. Never manufacture
native manager, heap, lock, task or TLS fields.

Static evidence identifies the native task implementation and UI-manager
constructor/startup/state loop. The manager constructs native tasks, initializes
the shared renderer/DC and switches its task callback to the UI state loop.
Its request worker owns FF loading and restoration. This eliminates the need
for a separate host app-switch controller or a hand-written resident painter
when the native loop can be executed through supported boundaries.

The full system startup routine is not an acceptable narrow initializer: it
also mounts storage and eventually launches a title. Identify and validate its
actual subsystem initializers and dependency order. Function addresses alone
are not verified call signatures or permission to execute a boot routine.

The supplied 17559 evidence maps these native boundaries (addresses are image
identities for research, not production constants or a ready-to-run plan):

| Boundary | Verified native path | Still required before activation |
| --- | --- | --- |
| Task subsystem | 81696968 → wrapper 816BA650 → no-argument initializer 816BA120 | Selective prerequisites, worker stop/drain and kernel primitive conformance |
| Native worker | Creator 816B9748; entry 816B9668 sets real task TLS | SYSTEM/suspend/KTHREAD-return behavior for flags 0x83 and worker/queue lifetimes |
| Native registry/FE | Initializer 816BDE90 installs FE callback 816BD930 | Preserve caller's initialization predicate and coherent native registry/resource bindings |
| UI manager | Constructor 816CEBD8 → startup task 816CE8B0 → state loop 816CE108 | Transport installed before tasks run; genuine readiness and native teardown |
| UI request | FE21005 → loader 816CDD58 → worker 816CD988 | Original caller capture, native import/load domain and request cancellation lifetime |
| Dynamic FF | Install 816BD038 / restore 816BD0A0; close 816BD1C0 → 816BCF60 | Actual module/recording consumers drained; no competing host FF replacement |
| Native completion | 816A2588, pending cleanup 816C95B8 | Kernel event/APC/object lifetime conformance and cancellation/race acceptance |

The recovered selective order is the actual DLL self-module setter, heap/CRT,
TaskInit, shared graphics init 816C9EA0, configuration service init 816A17B8,
notification init 816A95A0, and input auxiliary init 816F44D0. The notification
initializer is a void, no-argument bootstrap call, before frontend execution;
its actual mask, comparer, vector capacity and all area/version counts must pass
readback. Stable phase IDs are preserved with notifications appended as phase 8;
bootstrap versus native-Task execution is an explicit stage, not a numeric range.
The actual update-media predicate, FE registry init and conditional
UI-manager construction then run together on ONE real native XAM Task.
FE changes that Task's current-app association; an ordinary SYSTEM thread or
separate Task for each function does not satisfy this contract. The registry
boolean comes from the actual native predicate. Configuration itself starts
a thread/task and joins the same lifetime contract. Constructor
success and B0/B4 bits alone do not prove healthy asynchronous startup: native
code can publish those bits after earlier failures, and TaskInit does not check
every pool-worker creation. Require real worker/context/resource postconditions.

The original main startup body at 81696558 eventually launches a title and
stays outside the selective execution contract. On the private, hash-validated
root only, its quiescent entry may instead carry the host continuation: the
CPU verifies its exact .pdata extent, full body, and actual nine-instruction
native TaskSchedule registration witness. Native Schedule/Wait/Close still
own execution and completion; the original main body is never invoked.
Protected witness/scheduler intervals cannot be replaced by later bridges.
The earlier provisional attribution of 816BACC0 to
task initialization was corrected: that function mounts storage. The task
initializer chain above is supported by raw callsite instructions.

Install native graphics/input/loader transport before allowing native manager
tasks to use it. The new native-manager startup profile must be exclusive with
the manual one-shot engine/controller initialization in the same generation.
A poisoned partially initialized generation is not retried against its globals.

The allocation cohort, separate early/frontend startup transactions, verified
CPU task bridge, real-task continuation, service coordinator, notification
transport and independent graphics safe points are compiled in `df1219b5`.
The five direct executables pass 648 cases / 26,967 assertions. Native Home
presentation/readback passes across two full in-process relaunches, preserving
the existing manual scene's incomplete composition/footer layout. These tests
do not establish actual service startup, Guide requests or keyboard acceptance.
The owning service must seal and join its whole SYSTEM bootstrap host thread
before draining continuations, closing allocation admission or reclaiming
scheduler fibers; callback counters alone do not prove that join.

The first real service launches exposed two assembly gaps: the fresh XEX must
complete the existing loader stage before plan validation, and the plan must
invoke the native DLL-entry's actual self-module setter before selective startup.
The V4 plan uses the typed `module_handle` operand and checks the real
cell afterward. An offline helper runs the unchanged production plan parser with
the supplied image's actual XEX security descriptors and export table; the old
empty-phase plan is retained as a negative case.

`8b8e3c20` passes 670 direct cases / 27,668 assertions and observes actual startup
through the update predicate. FE initialization executes but returns `8007000E`
from the static-app loader; subsequent HUD load is unconditional and does not
identify the failed callback. The loaded HUD's native import bindings have been
independently enumerated against the supplied export table. A scoped null-unload
host crash was fixed and its failing native startup reproduced without a host
fault. None of these observations admits full Guide requests or native keyboard
acceptance.

The earlier `7d95866f` shared-contract integration passes 685 direct cases /
27,849 assertions. Its exact `55088ac6` plan passes the actual C++ parser with
four initializer calls and 30 catalog entries. The real first-record launch
observes phases `1,2,3,8,4,5`, including genuine notification readiness, then times
out before frontend completion/graphics registration. A scheduler watchdog
samples worker `0000000B` running with LR `818A6E58`; the dependency at that point
requires investigation. The earlier FC error is not returned in this run, but
FC completion has not been established. No host/guest faults occur, owned cleanup
completes, and the 10,182 recorded original files remain unchanged. Production
Source/visibility/input and activation changes are compiled; this diagnostic
explicitly disables production activation and proves no native pixels or Guide
interaction. The same-ELF successful startup/first-record prerequisite for
production interaction testing remains unmet.

The next narrow build, `5d5cbe83`, replaces only `XeCryptRandom` and adds four
actual-export regressions; all five suites pass 689 cases / 28,043 assertions.
The supplied native sampler has a verified constant-`FD` rejection invariant
for its actual immutable bound. The replacement uses checked OS entropy into
bounded host scratch, ordinary guest writes preserving physical write watches,
zero-length no-access, the original void ABI, and explicit failure termination.
Linux export/bounds/write-watch tests pass; macOS and Windows execution remain
unverified. The startup plan is unchanged. Actual startup now reaches new worker
creation and HUD binding, then times out with repeated unsupported dispatcher
type `0x81` at kernel object resolution. The full dispatcher-header/initialization/
wait contract must be recovered before changing type interpretation. No genuine
manager Set or complete native Guide interaction is established. The failed
run's baseline capture metadata is incomplete after runner termination; a
separate full seed-tree hash and recorded-original audit establish preservation.

`d87c8291` adds the common `ObGetWaitableObject` conversion; all five suites pass
695 cases / 28,204 assertions. Actual native evidence identifies outer listener
`300D5018`, type factory `81A821E0`, offset `0xC` and returned event `300D5024`.
The type-129 flood is gone without changing event decoding. The FE call passes
its return policy but fails the next ownership observer; the failure wrapper
does not retain its raw return. The original FE body proves `81AA0B5C` is the
`hud.xex` load-output cell, distinct from XAM self-module cell `81AA3EE0`.
At that checkpoint, both host ownership predicates required the wrong equality.
The subsequent correction validates root callback ownership and genuine loaded
HUD identity/domain/cohort separately. Original-state and copied-seed checks pass;
native Guide boot and first manager frame remain unverified.

`1f6e37b9` passes all native CPU phases `1,2,3,8,4,5,6,7` and all five direct
suites (697 cases / 28,434 assertions). The HUD ownership correction is observed
with all checks passing: root `3002E000`/load generation2 and child `300E5000`/
generation7, both in native domain1, with the expected source identity. Native
Task status preserves exact `E_PENDING` for the active startup task; completion
is not required of its resident state loop. POSIX Query now observes event
type/state without consuming it.

The original SYSTEM Guide304 invocation executes and returns raw0, but no
manager record appears within15s. The recovered native message path returns0
after scheduling and ignores later UI-handler failure. The handler rejects
before readiness flags2190/2194 are set (or when21BC requests shutdown); that probe
only waited for foreground Running. Early flags/DC are0, but their actual
request-time values and raw recording-hook reach/rejection are not logged.
Native asynchronous readiness and the first recording boundary therefore
remained unresolved in that capture. No renderer, pixels, physical input or keyboard acceptance
is inferred from completed CPU startup. The failed capture preserves all
recorded originals and the copied seed, with no host/guest faults.

### Historical observed boundary: 00722aee

`00722aee` (`native-first-record-readiness-v2`) passes 697 direct cases /
28,434 assertions. The original UI invocation exited0 but its interleaved
summary failed the harness parser; that result is preserved. A UI-only direct
rerun with dedicated Catch output supplies the accepted55/560 result; no full
suite rerun or suppressed test is claimed.

The unchanged `55088ac6` plan now produces a passing first-record witness in
`first-record-qbxb4yld`. All native phases `1,2,3,8,4,5,6,7` and pre-request
readiness `0x7FF` are observed. The original SYSTEM Guide304 call returns raw0;
the genuine renderer Task supplies 2,602 dwords / 10,408 bytes through the real
Set boundary, with command/output ownership observed. This PM4 contains a
color/depth draw and copy; shader/content is unknown. Source freeze, GPU work,
ACK, visible Guide content and physical input are not accepted by this CPU
capture. Observation after the public request does not establish Guide causality.

The separate same-ELF production attempt, `production-i0sm9zry`, stops before
foreground Running or physical action. Actual flags/event/shared-graph checks
and native AddRefs precede the failure in
`NativeGraphicsDeviceBinding::CreateForNativeSystem`: its ordinal `0x1BF`
`VdGlobalXamDevice` export name/type/value predicate rejects. The exact failing
conjunct/value is not recorded. This error is not evidence that asynchronous
shared device/DC readiness was absent.

Separate static native evidence shows the LightDevice recording path skips
normal-device kernel-global publication. The kernel global needs explicit
publication ownership of the actual catalog device and a checked lifetime;
neither identity nor ownership can be assumed from the other cell.
The observed descriptor's 852x480 extent also lies outside the current
640x480-only output profile; this is a later static boundary, not the cause
observed in the earlier production guard. Close the real binding and
snapshot-derived layout contracts before GPU pixels, Guide navigation,
close/reopen, title continuation and native keyboard acceptance.

Current evidence: `evidence/native-runtime/guide-first-native-record-00722aee-v2.json`
and its two original run results. The full post-production original-state audit
checks all 10,182 recorded files without differences/failures; both runs have
zero guest/host-fiber faults, complete owned cleanup and unchanged copied seeds.
The superseded v1 report remains preserved with its PM4 interpretation correction.

### Historical observed boundary: 0b955c56

`0b955c56` (`native-shared-device-output-v1`) passes all five direct executables:
702 cases / 28,714 assertions, with separate Catch output files. The unchanged
`55088ac6` startup plan captures the same 10,408-byte native CPU record in
`first-record-6avw4_vg` (5.773 seconds); all phases and readiness `0x7FF` pass.
The record contains a color/depth draw and copy; shader/content is unknown.

The early shared-device contract now has actual evidence in both diagnostic and
production modes. The actual LightDevice is validated, one native AddRef returns
2, and an owned CAS publishes it into the resolved kernel-global cell from zero,
with serial1 and the same actual pointer afterward. The later native DC joins
that same staged binding. Production pre-resume checks establish
`source_frame_ready`, transport readiness and receiver bridges before foreground
Running; these are prerequisite predicates, not proof of a frozen Source or GPU
execution. The former `00722aee` guard failure remains preserved above.

`production-ct7n5x2g` reaches Running and completes a clean 37.151-second capture.
Two Guide and two B pulses have post-sink uinput down/up records; native user,
slot and 506 receipt are not observed. All 12 window captures were checked:
images00/01 show startup, and02–11 show Dashboard without visible Guide.
No native SYS_UI visibility, production Source/GPU work, Guide pixels or native
keyboard acceptance follows from clean capture or command emission. Bounded
pipeline observations are the next step; full navigation/focus/close/reopen and
HUD→VK→HUD acceptance remain outstanding.

The new dynamic layout contract has five passing real Memory/Session/snapshot
regressions and keeps source/destination pitches distinct. Its production GPU
path is unverified against the actual 852x480 record. No host geometry or content
is invented to bypass exact sealed-rectangle validation.

Evidence: `evidence/native-runtime/guide-shared-device-output-0b955c56.json`
plus its first-record, production and direct-suite artifacts. The full post-run
preservation audit checks all 10,182 originals without differences; both runs
preserve copied seeds and have no guest/host-fiber faults or surviving owned
processes. Later UIActive/trace source and private 31-call helpers are not part of
this compiled/runtime result.

### Historical observed boundary: c3e26f90

`c3e26f90` (`native-ui-pipeline-trace-v1`) passes all five direct suites:
703 cases / 28,870 assertions. The `923ec6d7` 31-call plan adds the native
UIActive query. Its CPU-only first-record witness passes in 5.348 seconds with
all phases/readiness `0x7FF` and the genuine 10,408-byte command record; production
and trace execution remain disabled in that diagnostic.

The production run `production-e8fj4z17` reaches Running, records 100 valid
bounded trace envelopes and completes a clean 36.166-second capture. Prepared
transport/event/flags are observed. Two actual host Guide edges on slot1 carry
raw/filtered `0x400`; each fails native-input capture with reason4, missing
device. Only kind2 SYSTEM callback `816F09E8` is recorded. Native user-binding
and device-config registrations (kinds0/1), binding output, read-state and506
execution are not observed. This establishes a concrete registration/device
prerequisite before requests reach native UI policy. The existing five-argument
binding callback argument map must not be replaced with a guessed argument fix.

Original TITLE native public samples return `0x2EC=0`, `0x191=1167` and
`0x198=4306`. No SYS_UI9, production Source/GPU or presentation event appears in
the captured trace. All 12 owned images were checked: startup then Dashboard
without visible Guide. Native assigned user, successful506, opening/navigation/
focus/reopen and HUD→VK→HUD acceptance remain unverified. Host samples and source
trace reasons are evidence; sampled absence is not a universal execution proof.

Evidence: `evidence/native-runtime/guide-pipeline-trace-c3e26f90.json` and its
actual run/test artifacts. Both runs preserve their copied seeds with no host or
guest faults and complete owned cleanup. The full post-run audit,
`preservation-pipeline-c3e26f90-after.json`, checks 10,182 originals with zero
differences/failures. Earlier 0b/00722 results retain their original scopes.

### Historical observed boundary: 5d742233

All five direct suites pass717 cases /29,125 assertions, including seven real
Memory data cases and seven actual UI-queue notification cases. Whole-callback
rundown now covers the strong Driver frame before its inner Poll, with stopped
queue invalidation and self/ancestor wait refusal. The earlier ad13 CPU failure
is retained as failed; the subsequent one-line import-library normalization fix
matches the loader's extension-free `xboxkrnl`, without bypassing native policy.

The unchanged923 plan's CPU witness passes5.136s with the genuine10,408-byte
record. Actual production callbacks bind host slots0/1 to returned native users
0/1 and settle the initial masks3/3. Physical Guide cookie3 on slot1 executes
native506 for observed user1/raw0, then native SYS_UI9 delivers visible1. Input,
native notification and Source/fence identifiers remain separate namespaces;
these events do not establish rendered Guide or focus.

Production fails at the first Set's stage23/reason24: request preparation returns
null before an admitted Source/GPU submission. The observed identity is2602
DWORDs/852x480/format6; production bytes and the internal rejection are not yet
captured. Later input refresh/protocol reason1 is secondary to that first Set
failure. The two images precede the Guide action and show startup only, so no
post-notification pixel claim is available. Actual navigation/reopen/keyboard,
production GPU work and extended hotplug/failure cases remain unverified.

Evidence: `evidence/native-runtime/guide-input-binding-5d742233.json`, actual
CPU/production run results and the verified717-case outputs. Both captures have
no host/guest faults, complete owned cleanup and unchanged copied seeds. The
full `preservation-input-5d742233-after.json` audit checks10,182 originals with
zero differences/failures. The next concrete contract is bounded request-builder
failure telemetry; no guessed snapshot or geometry bypass is authorized.

### Historical accepted foundation: 05f0 common Host/native route completion

`05f0788a` passes all five direct suites, 824 cases / 37,686 assertions, the
same-plan CPU witness and checked 302-file source archive. A common ImGui mapper
change restores fresh-controller activation modality before NewFrame, preserving
selection and held-edge behavior. Actual-widget baseline failure and positive
83-case/861-assertion runs cover defaults, menu handoff, mouse focus, selected
buttons and text entry; there is no per-widget acceptance bypass. The old
runtime's unlogged cursor state remains unknown.

The identical nine-step NoSwap fixture/actions now pass. Host lease1 releases
before message lease2, which releases before keyboard lease3. First completed
TitleInput7/fence1 is published inside lease3 after its actual Host request,
during the real Swap bootstrap. That later prerequisite does not change the selected Host
backend. Guarded message choice0 and Host keyboard `keep` plus NUL complete via
public1FB; their post/release finishes before native sealing/next request997.
The native keyboard completes Cancel1627/1223 with untouched output and guards.
All nine public results pass; Source273's full B-selected input/output capture
passes separately. Owned Host/native UI and Title-return images do not equate
native submission, UI paint, image version and screenshot identities.

The independent nine-step audit and frozen verifier retain the 47.378s run
with zero host/guest/startup faults, clean owned closure and 10,182 preserved
current-baseline originals.
The blocked 74b attempt remains historical, including absent guest results and
its missing post-creation close input. Full Guide stays OPEN/no gate AppImage;
real-game gameplay continuation, manual focus, broader lifecycle coverage,
final upstream review and delivered-package acceptance remain independent gates.

At the same ELF, supervised `production-_ozry1rp` observes populated native Home,
Right/Media, B/title return and populated Home reopened. Two actual Guide calls
return0 for the adopted slot1/native user1; SYSUI visibility is1/0/1. Final Guide
Down stays held across the existing pinned owned WM_DELETE until verified owned
processes are absent. The live worker's complete3,319B record prefix is identical
before close and after owned absence; release/closed1 follows, with worker0,
runner0 and no escalation. Capture and held-close gates pass in168.561s with zero
faults. This accepts that normal owned-window path, not all C++ destructor or
shutdown/relaunch paths, gameplay or screenshot/submission identity.

The gameplay prerequisite is setup: existing profile B occupies guest0 and the
controller's guest1 has no selected account. A collapsed Slot1 profile selector
does not show an empty account list or a profile-backend failure. The pinned
`native-rotastic-existing-profile-readiness-20261105T220400Z` contract verifies
both Account bytes against historical two-slot evidence and shows ordinary UI
can select existing profile A for guest1 without profile creation. That selection
was not attempted in this run. Continue gameplay
acceptance only on the candidate AppImage in isolated copied data, not another
native-ELF retry. The independent `fe0d08ba` held-close audit preserves the actual
controls, all29 artifacts and1,000 raw envelopes; no broader gate is closed.

### Historical observed boundary: 74b NoSwap route integration

`74b179cd` passes 818 cases / 37,647 assertions across all five direct suites,
the same-plan CPU witness and checked 301-file source archive. Actual request
routing remains distinct from UI completion and native rendering acceptance.

In `production-9vz070c7`, physical Guide before any Title Swap selects Host
lease1 and B closes it. That lease releases before Host message lease2, which
releases before Host keyboard lease3. The first completed real TitleInput
outcome7/fence1 is published inside lease3; a later GPU prerequisite does not
change the backend of the already-selected Host request.

The message completion advances only after the final B press, following the
earlier scheduled A and Start presses. Host keyboard creation therefore has no
remaining close input. Main reaches its public1FB completion wait, but no guest
result file, full guarded outputs, native keyboard creation/SYSUI9, Source or
B-selected export follows within the capture bound. The capture gate passes
and the separate output gate fails, with zero faults and complete owned cleanup.
No specific ImGui focus, UI-factory or paint failure is established by these
records; screenshots retained before final B are not post-release object state.

The two independent 74b audits retain all33 declared artifacts and107 raw trace
rows. Preservation matches10,182 originals against the user-confirmed current
baseline. The integrated transition remains **blocked**; prior e702 native UI,
held-input and complete-bundle acceptance stays historical. Full Guide remains
OPEN with no gate AppImage.

### Historical observed boundary: e702 input handoff and immutable evidence

`e7023c2f` passes 808 cases / 37,521 assertions, same-plan CPU startup and frozen
296-file source verification. The retained v1 failures concern two new fixture
backend epochs; their v2 fix changes tests only. Historical failures remain.

Held physical input is quarantined across native UI ownership changes until an
actual known controller release. Keyboard-only success or a neutral peer cannot
stand in for an unknown/disconnected controller. Native UI reads retain raw B
for native closing; filtered Title/HLE reads suppress the consumed held button.
The eight-step real-queue/runtime case observes Cancel, fresh A without held B,
then a newly pressed B after host-observed Up without an intervening Title poll.
All result guards pass; this is one bounded controller/user1 handoff.

The one-shot B export copies the exact strong Source primary and every immutable
native block, selected shader copy and optional typed Title input before retirement.
Source273 validates 25 native blocks/231 shader copies/Title input totaling708,356
bytes. Identity, coverage, inline/external correspondence and byte hashes pass;
no later guest RAM read or guessed texture footprint fills missing evidence.
Loader generation, runtime generation, native submission and UI paint fences stay
separate. B selection and native image association do not prove animation phase
or actual UI screenshot selection.

The earlier eight-step direct-Swap fixture omitted genuine KRNL1B1 display/scaler
setup and produced NaN first-background inputs. A separate nine-step derivative
adds exactly one ordinary public kind1 call with a canonical64B scaler packet,
after the unchanged twelve-Swap bootstrap. All80 guarded bytes and actual zero
return are captured, then the original eight calls/results still pass. Same-Source274
immutable inputs contain finite full-viewport first-background vertices, with raw
VK output over the actual four-stripe Title image. The fixture prerequisite is
solved without production C++ repair, direct native-global writes or host clear. Public
status0 alone remains insufficient: registered native readiness and finite captured
geometry are independent acceptance requirements. The original blocked fixture
and older clock artifacts remain historical evidence, not a general renderer fix.
Rotastic's same-ELF Source163 also has finite background geometry and a populated
raw Guide. Early gray opening shots0.117s/0.364s after Guide do not establish
persistent blank rendering; a populated second owned-window menu remains unobserved.

The separate four-tab run observes owned Home/Media/Settings and raw Games &
Apps under physical Right/Left navigation, with no A or menu-action invocation.
Clock7:48→7:49 remains readable in the retained samples. SYSUI shows/hides and
both capture gates pass; screenshot capacity ends before B, so this run adds no
postclose intro image. Raw output and owned images remain separate evidence.

The independent held-B/full-input and Rotastic audits plus frozen e702 verifier
retain exact artifacts, raw trace rows, clean closure and no faults. Preservation
passes10,182 user-confirmed originals. Full Guide remains OPEN/no gate AppImage;
focus, broader real-game continuation, no-first-Swap fallback, other audiences,
HUD-owned nested keyboard and transient composition remain separate gates.

### Historical checkpoint: 3997 native desktop text contracts

`3997176e` passes 796 cases / 35,548 assertions, same-plan CPU startup and frozen
296-file source verification. Query bootstrap owner refresh runs only after
busy retirement/capacity gates and before a usable frame decision, outside
StateMutex. Native277 still revalidates the real resident UI Task, current
scene/parent/native user, adopted class2 endpoint and audience epoch; visibility
or SYSTEM process type alone grants no user authority. Matched successful
`keepa` runs log 723 versus 67 native image publications before this ordering
change. This is a bounded cost/progress signal, not FPS or a benchmark.

The same wxGTK IM context is observed after Show/realization. The earlier 96d8
no-availability/no-typing prerequisite remains recorded. Actual 1c35/3997 short
`a` delivery reaches SYSTEM277/user1 and the ordinary Title receives guarded
`keepa` plus NUL after native Done. A separate false-passthrough held7 fixture
preserves the original six calls and appends guarded user0 GetState. The complete
320-byte result was read while XQueryKeymap still reported key38 pressed:
`keepaaa` plus NUL, success overlap0, user1 fresh A/packet4, and user0 success/
packet2 with all gamepad fields zero and canaries intact. Native group0/user0 and
group1/user1 bindings are actual; keyboard group0 membership is source-derived.
This checks one held-key close handoff, not all users or input transfers.

Paired desktop `a`, Backspace, `b`, Return also complete exact guarded `keepb`
plus NUL, overlap0 and later fresh Title A/packet2. Native hide precedes that
controller A; the run sends no controller Start/Guide/B fallback. Host receipts
alone are insufficient: actual native read observations and guest outputs are
retained. FocusNone could not hold focus away from Xenia; the owned managed-Park
attempt also could not switch within its budget and sent no keys. These are
WM prerequisite negatives, not emulator failures or native focus acceptance.
A manual Alt-Tab/held-release/regain check remains outstanding.

The historical 1c35 Rotastic run proves native Guide over the animated intro and
B return to its continuing intro. SYSUI/class2 show-hide pairs pass, but blank
gray initial/second-show panels remain observed composition limits. It proves
no gameplay, fresh Start, storage or save contract. Other users/devices/layouts,
IME/composed/supplementary input, Windows/macOS/Wayland text, broader focus/modal/
teardown handoffs, fresh mapped-key reuse after release, no-initial-Swap fallback
and HUD-owned nested keyboard remain open. The Guide gate is OPEN/no gate
AppImage. Actual evidence is in the named independent typing, held7, edit and
Rotastic audits plus `verify-3997-native-physical-text.json`; the recorded 10,182
originals pass against the user-confirmed current settings baseline, preserving
all historical comparisons.

### Historical observed boundary: CBB native admission and sequential keyboard

The checked1AB/4A7 native loader queries and exact upstream wait/qword slice are
built in cbbefe1e. Five suites pass777 cases /34,954 assertions; CPU startup
control passes5.548s. A Title2C1 request while Guide is active returns5 before
keyboard dispatch, as the genuine native guard requires. Its nonnull overlap
does not make this an absent native Task or a request-scheduling failure.

One-Kernel production u2klqpzs instead closes Guide through physical B before
the original Title requests2C1. Actual SYS_UI9 hide at raw line11191 precedes
request997 at14068, followed by VK show/hide. All six guest steps complete:
`keep` plus NUL, preserved unused bytes/canaries, success overlap0, immediate
GetState1167/neutral, then fresh Title A with status0/packet8/buttons0x1000.
The independent audit matches all649 trace rows and owned Guide/VK/Title images;
closure is clean with no host/guest/startup faults. A separate same-build
Unicode default-string run preserves exact `日本🙂` plus NUL, canaries and
fresh Title A. The owned image shows `日本`; the smiley glyph remains
unverified. This is a data-copy result, not physical typing or IME acceptance.
The desktop-keyboard policy is to follow the actual Guide/OSK opener while
preventing simultaneous keyboard-as-gamepad actions; its class2 binding and
repeat-audience contracts still require implementation. This proves the sequential
Guide-close/ordinary-Title-keyboard contract; HUD-owned nesting/restoration
remains unproven. Desktop text, ordinary games, no-initial-Swap/fallback,
transient composition/clock artifacts and held teardown are still open.

The48-step upstream17559 Xapi comparison is a semantic pass with exact finite421
return pointers and INFINITE no-write. Its936-byte output is complete, but the
harness ends by SIG2/15/9/leader-9, not graceful termination. Upstream console
observations belong to the fixture author; our emulator/unit evidence remains
separate. Snapshot review does not close the Guide gate or produce its required
AppImage. See the [gate-close policy](17559-gate-releases.md).

Current facts, independent audit and verifier are pinned by
`evidence/native-runtime/guide-native-keyboard-cbbefe1e.json`. The10,182-file
preservation pass uses the user-confirmed current settings snapshot. The three
older settings/library-metadata differences and their failed comparisons remain
preserved; no old-baseline identity or restoration is claimed.

### Historical observed boundary: 8a0a05cb native keyboard accept/cancel

All five direct suites pass774 cases /33,280 assertions; the unchanged-plan CPU
witness i7cf74js passes5.335s. Genuine per-Title heap initialization precedes
public UI admission. The shared SYSTEM460 string import now executes the
supplied native implementation. Default Done2cx4qgcg returns `keep` plus NUL;
edited Done ml_g4vq0 returns `kee1` plus NUL. Both preserve unused bytes/canaries
and complete the original overlap0 after request997. Cancel5bk0q5kv returns
1627/extended1223/length0 with untouched guarded output. Fresh Title A after edit
and Cancel reaches native GetState0/buttons0x1000; these are bounded original
fixture results, not ordinary-game acceptance.

Immediate GetState1167 with neutral output precedes native hide. Preserve that
native policy: overlap completion does not itself restore input, and the legacy
checker's both-status-zero expectation is not claimed to pass. The fixture
performs initial genuine Title swaps and then idles. Prior9f2 qlyyd_mz remains
a failed output contract despite success completion; prior9f2 p0iia0jg retains
bounded four-tab/Settings-focus/Dashboard-return evidence. The048 float-cache
correction fixes its observed geometry defects without asserting cross-run
command or screenshot-version identity.

Guide-first y_2ps7yi fails before Main step0/VK request with one guest fault in
native UI thread01000104 at81787760/LR81784E30 (fault-log READ0x100000000).
The native PNG/resource path is unresolved; undefined loader imports are leads,
not a closed cause or an authorized synthetic image. No HUD→VK→HUD restoration
is proven. Cancel's transient nested rectangles and composition/clock artifacts
remain unfinished XUI. Ordinary-game continuation, no-initial-Swap operation,
held teardown and fallback regression remain open; full Guide/OSK is incomplete.

The two independent8a0 native Done/edit/Cancel audits, original guest outputs,
Guide-first failure, exact-ELF suites/CPU witness and artifact verifier preserve
these separate outcomes. Standalone acceptance captures have no host/guest or
startup faults and complete owned closure; Guide-first retains its guest fault.
The full8a0 original audit matches10,182 files with zero differences/failures.

### Historical observed boundary: 66af0d78

This checkpoint first proved bounded native Guide open/B-dismiss/reopen/return
and separate producer/UI-consumer completion on its own timelines. It passed
760 cases /32,234 assertions but retained visible tab/footer overlap and a gray
strip. These failures and `guide-native-rendering-66af0d78.json` remain intact;
no earlier event can stand in for a current frame, fence or keyboard result.

### Historical observed boundary: c0f5d180

The real Title input producer now captures GPU bytes after its actual copy fence;
sampled outcome7/fence1 records identify backend2, foreground1 and the actual
Title module/load generation. Input is1280x720 tiled storage,3,768,320 bytes,
with the original and normalized fetch words retained. This closes a Title input
prerequisite, not native Guide GPU execution. The same923 CPU first-record
witness passes5.574s; real user1 native506/raw0 and SYS_UI9 visible1 repeat.

All3 shader metadata analyses, both draw proofs, primary comparison and complete
input snapshot Seal pass. Request construction then rejects the exact geometry:
the sealed rectangle is856x480 but the copied native descriptor declares852x480.
The observed boundary is before Session minting/final ResourcesValid; no admitted
request, native Guide GPU submission or pixels follow. Recover the logical
visible extent versus aligned resolve contract without substituting guessed
geometry or weakening byte/owner coverage.

Accepted direct coverage is742 cases /30,832 assertions, including21 new cases.
The initial net failure is preserved; its exact case and a separate full net
rerun pass unchanged. Production rejects7.469s with no host/guest faults, unchanged
seed and complete closure. Both images precede Guide and show black/near-black
startup only. Evidence: `evidence/native-runtime/guide-title-input-c0f5d180.json`
and actual CPU/production/test records. The full
`preservation-title-input-c0f5d180-after.json` audit matches10,182 originals with
zero differences/failures. Earlier c46 owned bytes and d5 metadata refusal retain
their scopes; complete Guide/keyboard lifecycle acceptance remains outstanding.

### Historical observed boundary: d5b17738

All five direct suites pass721 cases /29,705 assertions; the same923 CPU witness
passes5.757s. The separate physical capture again observes actual user binding,
user1 native506/raw0 and native SYS_UI9 visible1. Its complete packet-profile
check now passes62 packets/3 shader loads with no packet error. Source FREEZE,
record/output pins and writability pass. Matching built control flow proves the
initial input capture, record read and primary equality check precede the
observed shader failure; this does not prove complete input sealing.

The first shader load refuses `unsupported_control_flow` at DWORD3, reported as
`shader_metadata_incomplete`. Production ends7.962s at stage23/reason24 before
Seal, final request admission or GPU/presentation. Recover the actual owned
shader bytes and control-flow contract; retain strict coverage/ownership checks.
Later input protocol reason1 is secondary. Both pictures precede Guide, so no
post-notification pixels or Guide/OSK lifecycle acceptance are established.
Evidence: `evidence/native-runtime/guide-shader-profile-d5b17738.json` and actual
run/test artifacts. The full `preservation-terminal-window-d5b17738-after.json`
audit matches10,182 originals with zero differences/failures; seeds, fault-free
closure and all previous negative checkpoint evidence are preserved.

### Historical checkpoint: 34dcc354

All five direct suites pass717 cases /29,125 assertions; the same923 CPU witness
passes5.145s. Actual binding/native506/user1/SYS_UI9visible1 repeat in production,
which rejects its first Set after7.984s. Bounded telemetry reports
`complete_packet_profile_refused`: register8320 (`0x2080`), DWORDoffset2588.

Source FREEZE and record/output pins/writability pass. The immutable-mode
`primary_matches` marker does not compare recaptured RAM bytes. Input capture,
Seal and final request validation have not run because complete-packet analysis
rejects first; zero partial-report fields are not shader/draw absence evidence.
No admitted request, GPU or rendered Guide is established. Both pictures precede
the Guide action; later input protocol reason1 is secondary to this rejection.

Close the actual register/backend contract with strict packet validation. The
observed error does not justify a snapshot/layout bypass or weaken native caller,
owner, pin, Source or submission lifetimes. Evidence is
`evidence/native-runtime/guide-packet-profile-34dcc354.json`, actual raw artifacts,
and the full `preservation-frame-prepare-34dcc354-after.json` audit:10,182 originals,
zero differences/failures, unchanged seeds and clean fault-free owned closure.

## Request and app contracts

The native SYSTEM heap/CRT boot leaves per-Title heap0 disabled. Before any
public UI request or prepared Title Main, the service executes the verified
native initializer816E6870 once for the exact foreground/Title module/load/Main
identity. It checks pristine heap0 and the real executable-module import, calls
the supplied body without host metadata locks, then validates actual owned
reserve/commit/RW backing and native descriptor postconditions. The Title
workspace header40201 and native allocation policy remain authoritative;
failure or uncertain execution retains the once/lifetime record until ordered
stop, with no lazy2C1 retry or host-written private heap fields.

Public UI requests begin on their original guest caller. In particular, native
XMsgStartIORequest must capture the original caller process/thread/APC state
and any existing native task context before SYSTEM execution. Native task lookup
is SYSTEM-specific; do not manufacture a TITLE task. Moving a keyboard packet to a SYSTEM host thread first would change
that context. Frontend controller Guide intents have no TITLE completion caller;
preserve their initiating local user and enter the verified native request path.

The verified native Guide request reaches FE message 21015. Ordinary keyboard
requests reach FE message 21005 and the native loader for vk.xex. The ordinary
keyboard packet is 32 bytes, eight big-endian DWORDs; its extended variant is
a distinct contract. HUD and VK both register dynamic FF, and VK borrows the
shared DC/common XUI. Native manager save/close/reload/restore is authoritative.
Keeping a probe HUD permanently registered in FF is incompatible with this flow.

Native completion already has task, overlap, event and APC handling. Prefer
that implementation to the HLE immediate-completion helper, which uses the
current thread. Do not substitute frame completion, window disappearance,
handle close or a timeout for request completion. Preserve native acceptance,
user cancellation and external shutdown result distinctions. Shadow buffers
are an adapter only if an actual address/lifetime boundary requires them; they
are not a replacement for the native request protocol.

A successful completion is distinct from a correct returned string. In the
ordinary VK Key.OK action, current edit text is copied into the original packet's
output with capacity including NUL, then native completion follows. NULL text
or zero capacity can skip the copy, so overlap success alone is insufficient.
Every participating SYSTEM module must resolve the shared native string import
consistently; qlyyd_mz demonstrated a real missing460 boundary despite success
completion. Cancel uses the separate native result1627 / extended1223 / length0
path and leaves output untouched. External shutdown remains a different result.

No old-generation completion may write newly reused caller memory, signal a
replacement event or restore input over a newer UI owner. Keep caller memory
and object tables valid while native requests cancel and drain. Normal Guide
hide, keyboard completion, native app replacement and Kernel shutdown are
different operations.

## Input contract

Native XAM owns UI input selection, focus, navigation and dismissal policy.
The recovered physical seams are KRNL XInputdReadState (1E6), RawState (1E9),
ReadTextKeystroke (277), FFDeviceControl (286) and DrvSetSysReqCallback (20C).
The ordinary virtual-controller profile now has opaque device handles,
device-stats/enumeration, native binding callbacks and generation-scoped
notification rundown. Optional/raw lanes remain outside that proven profile.
Native GetState/HUD/keystroke routines retain counters, caches, translations and
gamepad repeat policy; do not inject a second SDL-generated gamepad stroke
stream or use host slot integers as native handles.

Native XamInputSendXenonButtonPress (506) already queues Guide intent through its
real owner/task. Once that owner and callback path are ready, route one physical
Guide edge with its initiating user through the native intent, or use fallback
when native admission is unavailable. Do not also send ShowGuide or open the
host Guide; native worker policy owns the resulting request.

An admitted native SYSTEM consumer must receive device state/keystrokes while
TITLE input is neutralized. A caller-supplied system flag alone is not authority:
require the live native domain, actual process/context and current input claim.
The native route must still respect higher-priority host modal ownership.

Observed keyboard completion may immediately return native GetState1167 with
neutral output before the native hide notification, then later status0. Edit
and Cancel fixtures separately prove a fresh physical A at that later Title
poll; default Done only observed neutral state. Preserve the native policy
instead of requiring both statuses to be zero. Overlap completion, visibility,
input restoration and fresh input receipt are separate boundaries.

The installed physical adapter is below native191/196 and does not require a
visible plane. Native per-user policy selects the audience; host modal
suppression remains separate. Ordinary binding/read/Guide delivery now has
runtime evidence; unsupported driver lanes do not inherit that acceptance.

The HLE InputSystem path neutralizes GetState for SystemUI-active or a host blocker,
even for XAM's system flag. Blocked GetKeystroke also discards driver strokes;
merely adding a native raw reader would still lose input when the title polls.
These are HLE-path findings. Native intra-image input calls may bypass them:
trace the actual consumer to kernel HID or a verified trusted native bridge.
An HLE XamInputGetState change alone is not established as sufficient.
Establish per-consumer routing/queue ownership together. Preserve connected vs
empty-slot results, title packet-number behavior, and held-button suppression
when input finally returns to the title. A HUD-to-keyboard transition must not
briefly release its input claim to the game.

Native notifications require the same authority discipline. Native UI emits
notification 9 with data 1/0 for show/hide, while native listener objects and HLE
XNotifyListener handles retain distinct queues/types. The checked SYS_UI9
observer preserves native broadcasts and explicitly transports show/hide to
TITLE HLE listeners and the native plane under generation ownership. This does
not claim every notification family or physical-device lane. Do not cast native
handles as HLE listener objects or replace real broadcasts with a host UI flag.
Reuse existing counted guest-defined object primitives where their ABI fits.

The separate KRNL graphics-notification receiver now has a scoped transport
for its verified payload-copy cases 1, 3 and 9, with original caller admission,
copied payloads, an actual SYSTEM callback worker and explicit drain. Its kind
9 describes an allocation; it is not the XNotify SYS_UI visibility event.
Unregister invalidates queued calls, while entered calls still require the
coordinator's caller/dispatcher join before native state is destroyed.

## Native render and presentation contracts

The genuine native task/manager loop drives timers, layout, input, app changes
and rendering. Bind frame ownership to that actual producer context. Reusing
the one-shot draw callback would repeat initialization and is prohibited.

Native shared graphics/command buffers are allocated before the later render
Task executes. Preserve a real native-domain allocation owner and prove its
delegation to the actual render Task/invocation. Attaching the probe's fresh
single-thread Session later does not establish ownership of earlier buffers.
Task affinity/migration and the shared device's creator provenance must remain
explicit; neither any SYSTEM thread nor a rewritten creator field is authority.

The recovered native producer is the system-command-buffer path: the state
loop checks VdQuerySystemCommandBuffer(mode 0), records through its native
worker, and publishes through VdSetSystemCommandBuffer. The descriptor is
0x94 / 148 bytes and may be constructed on the native stack; copy it immediately.
Shared-DC XuiRenderPresent returns zero without handing off pixels, so it is
not the transport interception point.

The scoped native Vd transport connects Query/Set/Get/identifier admission;
actual Query/Set production frames are now observed. Unclaimed legacy Get
retains its BEEF fallback; that route is not native transport acceptance.
Query must reflect real ownership/backpressure, including empty publication/reset;
neither always-ready nor default-zero is acceptable.
A publication must have one authoritative execution/retirement path. A native
CP path and a title Get/Swap checkout must not execute or acknowledge the same
record twice. A submitted record is acknowledged only after its actual
matching GPU result. A verified no-submit/CPU-skip or empty-publication path
has a separate retirement contract; it must not masquerade as a GPU fence.
Preserve native identifier/writeback and graphics notification semantics.
The existing SystemCommandBuffer identifier model is explicitly host policy,
not proof of a retail identifier encoding or complete native-driver compatibility.

Retain the checked Source transaction. Native Query is pure availability: the
clear path polls before and after empty Set, and the second poll has no later
Set. A Query therefore cannot leave a persistent host Update/writer reservation.
The genuine native renderer performs record/resolve/End; its Set adapter copies
the descriptor immediately, begins the host preparation transaction and freezes
its owned primary before dependency reads. Validate the complete input/output
layout, submit once, attach the exact source/epoch GPU ticket, observe actual
completion, then retire before reusing live GPU input backing.
Pins prevent allocation mutation, not arbitrary CPU writes. Native task
concurrency and the genuine record/present/driver ownership handshake must
establish that mutable vertex/font/command data cannot change before consumption.
Alternatively, ALL validation and GPU input reads may consume the same complete
immutable snapshot. This alternative is compiled with a separate native GPU
memory bank and has actual admitted manager-frame evidence. It does not assert
universal CPU-writer exclusion. A partially captured view or live-RAM reupload
after validation does not satisfy the contract.

Native resource Lock waits use main completion status at device[2B10]+0 and
the native main serial at device+2B1C. The light recorder bypasses that normal
serial emitter, and Set does not apply its CPU resource-reference fixups.
System-command ACK at status+8 and the UI cache's render-age counter are
different domains. Neither can be relabeled as an actual GPU completion or
an asynchronous writer exclusion proof.
Holding the native publication/reuse handshake until source-copy completion
is an adapter option only after its lock/progress implications are verified;
waiting on the shared-DC XuiRenderPresent no-op would establish nothing.

Reuse packet/state/shader/read validation, isolated native bank execution,
GPU conversion, immutable image versions and actual stop proof. Factor capture,
reports and readback into optional diagnostic observers. Production frames must
not require disk output or append teardown pin-holders indefinitely.

The actual manager's records, output geometry and state chain must be validated,
rather than forced into the manual probe's 640x480 target/profile. A persistent
bank does not prove omitted register state. Supply only genuine native state
or a separately established state-chain contract; do not fill gaps with guessed
reset values or manufacture a render target to make the diagnostic pass.

Native work must progress independently of title Swap, including before a first
title frame and when a title stalls. The current backend drains only at IssueSwap
and has active/pending-title-query guards. Establish a native-only boundary that
preserves title state/query/accounting and does not fabricate title swaps,
ring acknowledgements, frame counts or interrupts. A wakeable callback queue
alone does not provide that execution boundary.

Preserve the clean title image. Present the immutable native image as a separate
plane after title processing and before host overlays. Show/hide must trigger a
repaint independently of title progress. The current diagnostic bakes UI into
the title mailbox; changing its visible flag cannot erase already baked pixels.
Keep that candidate classified as a diagnostic, not the production contract.

Presentation authority belongs to the resident domain/native visibility
revision, not a weak per-frame Request. Keep the last good image visible during
next-frame backpressure when native policy still requests it. Source completion
permits guest backing reuse; later display sampling has a separate consumer
lease and actual paint completion. A historical handoff token is not proof of
current visibility or that a hide repaint reached the screen.

Once Presenter samples native images, CP completion/stop alone cannot retire
them. Withdraw publication, block new consumer acquisitions, and drain CPU-side
acquisition/recording/submission callbacks before the device-stop proof. Retain
already-submitted GPU consumer references until their actual paint fence OR a
proven device-wide GPU stop, then release them. Waiting for an uncertain GPU
consumer fence before permitting the device-stop fallback would be circular.
Seal later UI submissions as well as CP submissions before claiming that stop.
No Vulkan handles belong in guest-memory lease containers, and no stopped-thread
callback may run PPC or native object destruction.

## Draining, failure and fallback

Close external request admission, withdraw presentation and stop native input
ingress without preventing cancellation/cleanup work that must drain. Retain
exclusive TITLE-input suppression until the native close/transition quiesces;
do not clear SystemUI-active early or restore focus/input to an old or stopping
foreground generation. Keep native tasks, graphics progress, events, timers and
object tables alive
until their existing work reaches a verified quiescent boundary. Native manager
close/destroy ordering must remain authoritative, with source completion and
module/class references protected throughout.

Stop all native producers/consumers and prevent future queue submissions before
establishing actual GPU retirement. Destroy backend children while their device
is live; release guest graph/runtime/module ownership and memory afterward.
Any uncertain submitted work retains its resources until that actual boundary.

Recovered title-task cleanup is not a complete scheduler destructor. Native
BA6B8/BA2B0 drains a registered-task list; CCE30 requests bounded hide/fade;
worker reference release does not join the eight persistent workers. A matching
full scheduler/DC/config/input-worker stop is not yet established. Retain the
Kernel-lifetime graph until a real final CPU/callback stop.

Current ExRegisterTitleTerminateNotification stores registrations but never
invokes them. Native startup registers cleanup callbacks, so registration alone
cannot supply title cleanup. Establish actual notification phase/order/context
before dispatching those callbacks. Direct Emulator::Shutdown also destroys
providers before the Kernel scheduler drain; introduce an early all-native
producer/callback stop while graphics, input and audio providers are still live.
Do not treat the existing probe-thread join as that service-graph boundary.

Fallback is chosen before request admission and requires a quiescent native
domain with no unresolved native input/publication owner. It cannot open or
complete concurrently with the same native request. Partial startup may create
workers before a request exists: a poisoned, unquiesced domain cannot silently
start a competing fallback. Preserve the failed generation rather than clearing
private fields and retrying. Runtime failure must be reported honestly; a successful
setup/readback or host fallback is not native Guide acceptance.

## Coherent implementation and acceptance

Implement the native startup/import/loader/request domain and its input/frame/
presentation lifecycle as one reviewed integration, using existing boundaries.
Complete the interface and dependency review before freezing/building; do not
start a build while a parallel review can still change its contracts.

Acceptance is end-to-end: open Guide for the initiating local user; navigate
native pages; open native keyboard, accept and cancel; return to the prior UI;
close/reopen Guide; continue the game; then relaunch. Repeat while title swaps
are stopped, while native submission/completion is delayed, during app
replacement, and during cancellation/teardown. Check no leaked input, stale
frame, duplicate result, wrong caller completion, unbounded retained state or
mutation of original user data. Features requiring later HDD/profile/avatar/
Live services receive explicit dependency outcomes without shrinking this
Guide/XUI lifecycle requirement.

The ae83 control passed 602 cases / 25,837 assertions before this integration.
The compiled e4ef9a6a component baseline now passes 625 cases / 26,027 assertions
and the limited native Home presentation checks below. Native execution,
source completion, display visibility, input, request completion and safe
teardown each require matching runtime evidence.

## Host integration and verified component scope

The first integration implements a scoped native import domain shared by static
imports, dynamic module/procedure lookup and child loads; a checked SYSTEM
kernel-callback dispatcher; and physical input transport below native XAM. The
native Task and UI managers retain scheduling, focus and completion policy.
Creating an import domain is not service readiness. Native manager startup,
notification interoperation and the full driver contract remain admission gates.

Presentation now has a resident owner independent of any one frame request,
a separate native UI plane over clean cached title output, and an independent
UI paint completion epoch. Hide/withdraw requests repaint without requiring a
title swap. Submitted images remain held through their actual consumer fences
or backend stop. Native GPU admission seals ordinary host capture for that
cohort; diagnostic native readback remains separate. Queued native work uses
owned CP safe boundaries, with a previously completed Title input image as a
prerequisite. Actual stalled-title/NoSwap and no-initial-Swap UI acceptance remain
separate from the running-Dashboard evidence.

A native cohort also requires scheduler-backed guest threads and an early
CPU/callback stop before provider destruction. Stop input ingress without
releasing TITLE suppression, discard/drain driver callbacks, drain graphics
interrupts and audio callbacks, stop timer/dispatch producers and join the guest
scheduler while providers are alive. Release input capture only after that CPU
boundary; stop GPU submission and establish retirement before destroying UI
resources, backend children or guest memory. An uncertain stop retains owners
until process exit. Native title-termination callbacks are not guessed.

The guarded two-job build succeeded with source/index identity preserved and
more than 31 GiB free. All five direct suites passed. An isolated Home run
passed native execution, raw readback and actual UI handoff; a second process
passed three frames across two full dashboard relaunches. Producer epochs
2/4/6 and independent UI epochs 1/3/5 were matched to their own completions.
The single-run and both after-cycle window captures show the genuine native
Home menu/profile strip/clock/footer over the running title. This is the manual
Home component, without the native manager's full composition, input or keyboard.
No stalled-title execution/hide or full native-service teardown is claimed.
At that component checkpoint, ordinary host capture during a native GPU cohort
and physical native desktop text input were unsupported; the bounded1c35 text
result above is separate later evidence.

Private evidence: native-runtime/guide-native-plane-e4ef9a6a.json, build/
native-system-host-integration-v3.json and tests-native-system-host-integration-v3/
results.json. The original-state audit retained 15 differences from its older
snapshot; all modification/change times predate these runs (latest 19:32:34 UTC,
first run 19:54:51 UTC). No restoration was performed. The audit history and a
fresh observed baseline for subsequent tests are preserved separately.

The next coherent service slice reserves allocation ownership before heap/CRT,
delegates those buffers to the real native rendering Task, and wires the native
startup catalogue and Vd publication/notification transport. Native resource
epochs must retire independently of title frame counters. Reuse existing query
segmentation/accumulators while isolating parked title query context; do not
remove query guards or add another sample accumulator. These changes are private,
unbuilt drafts until reviewed and integrated with the real service/producer.

The actual graphics startup condition is now closed from the existing caller
bytes: native SharedGfx and UI construction test bit 0x200 in the first DWORD
of the imported KRNL XboxHardwareInfo variable (ordinal 0x156). The current
host initializes that DWORD to 0x20, so the native branches would skip setup.
The startup catalogue must read the real import/variable. An explicit virtual
native-UI capability profile may advertise 0x200 only after the live backend
and complete Vd transport are installed; do not silently force the condition
or change the default kernel profile while its old BEEF stub remains exposed.
The unrelated main-boot mode in r31 is not this hardware flag.

Verified UI manager storage is 81AA20E0, its actual shared-DC output cell is
81AA2110, and its Task cells are 81AA20E4 / 81AA2108. The earlier 81AA0970 /
81AA0978 addresses are scheduler-stop / timer storage. Readbacks must validate
the matching image/code generation; these addresses are research evidence,
not a generic private-field inference. A not-yet-created DC after asynchronous
construction must be distinguished from a failed or admitted renderer.

Native input ownership is per-user native state. FF controls 0x80000005/6 are
also sent from another UI helper and cannot be translated into one host-wide
input-owner boolean. Preserve the real native191 high-flag/signed-counter
branches by forwarding supported original-Title input calls through native
XAM once that subsystem is ready. Driver transport still respects host modal
and focus boundaries; it does not replace native navigation or repeat policy.

## Source and private evidence

Core source boundaries: GuestRuntime and its validated plan executor;
CreateNativeXuiImportProvider; KernelState LoadGuestSystemApp/LoadUserModule;
GuestAppRegistry and AppManager; XAM Task/XMsg/input/keyboard exports;
NativeFrameOwnerSession and preparation; Vulkan native execution/presentation;
Presenter; Emulator shutdown/relaunch and TitleExecutionState.

Private research-state evidence is rooted at xenia-edge-research. Existing
native-guide/sysapp-abi and keyboard-abi records identify supplied image hashes,
verified instruction ranges and actual native export identities. The new
native-guide/system-contracts pass maps native-manager/task startup and
switch/completion callers. Interpret instruction bytes/import libraries as
primary evidence; inferred decompiler signatures and earlier hypotheses are
not contracts. Native startup/dependency, producer backpressure and cleanup
cuts remain closed until their exact interfaces are established.
