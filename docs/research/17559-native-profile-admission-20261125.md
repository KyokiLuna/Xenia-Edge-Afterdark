# Native profile admission and startup — 2026-11-25

Status: source backup with passing full and focused tests; native acceptance incomplete.
No replacement AppImage, checkpoint release or HDD gate closure accompanies it.
Internal build labels V123–V131 identify research builds, not releases.

The [current full lifecycle comparison](17559-profile-lifecycle-comparison-20261125.md)
compares the matching mapped producers and consumers with the active implementation,
including request/error/cancellation ownership, storage and avatar dependencies.
It records the coherent implementation units and their actual consumer results.

The latest same-ELF diagnostic proves a pending native FC/58001 request. The
default native service pump is parked by the host's OldPaused data-consumer
gate even though its request completion precedes publication. This source-defined
cycle needs a coherent service-progress/data-source correction; whole-pump
exemption, fabricated success and HLE-overlap completion are not established
solutions. See the full comparison's current dependency section. The latest
run exited zero with clean teardown, while sign-in/publication remain unaccepted.

## Current V131 request lifecycle and runtime result

The full lifecycle comparison led to one coherent nine-file change covering
sync/async producer outputs, Task birth through native close/free/reuse,
initializer error/return ownership, and separate actual requested/canceled
publication outcomes. Requested inputs remain immutable. Unproved partial
outcomes and initializer-error source recovery remain uncertain.

All seven current runs passed: base 331/28,245 assertions, network 125/11,212,
kernel 1,448/91,190, VFS 136/9,736, UI 124/4,863, focused CPU 5/130 and focused
installed-system 179/21,427, including all twelve new cases and three explicitly
selected hidden cases. Focused selections overlap full suites.

The actual native Guide/profile list rendered. Selecting disposable local123
again requested slot 1. The genuine native initializer now returned zero with
no producer-binding refusal or execution uncertainty. A real C838 Task/read/
commit record appeared, but no protocol, roster publication or owned CompleteIO
was observed. The dashboard remained unsigned and Guide did not reopen after
selection. The next system review follows the original worker/wake/publication/
completion chain. No avatar, game/save or checkpoint acceptance is inferred.

An owned WM close was requested; process exit was one after 425.05 seconds.
The owned group was fully reaped without forced signals, and checked inputs
were preserved. This is failed native shutdown with successful host cleanup.
Current build receipt is `b980534af4c408c4d76f6535fa23ca872c3567408c67632a0c6115d557ddf09f`,
seven-run receipt `cbe73dba7fac660411547ee2a44184490d5a138df395886dcf0f85f8d133ef0c`
and runtime result `bc26d56c7110a0adb40068f0063bc9767b5dd82930da5cd046d2044d284b751d`.
See the full comparison for source identity, exact evidence and remaining
durable profile/avatar obligations. No new AppImage was produced.

## Historical V130 storage, preparation and runtime result

The integrated folder-content path retains the actual aligned native header,
including padding and opaque bytes, and validates the matching source snapshot
before publishing a replacement. Common profile preparation keeps one original
caller/deadline and retains eligible model/cache progress across contention.
V129's three base-test failures were corrected in V130: one fixture retained a
real reader while expecting resize success, and the external Header guard used
coordinates relative to the profile directory instead of the Header directory.
Real independent readers still block mutations.

All seven V130 runs passed: base 331/28,245 assertions, network 125/11,212,
kernel 1,436/90,289, VFS 136/9,736, UI 124/4,863, focused CPU 5/130, and focused
installed-system 167/20,526, including three explicitly selected hidden cases.
Focused selections overlap the full suites.

The actual current ELF displayed the dashboard and native Guide/profile list.
Selecting local123 requested slot 1 through original 224/120424. Preparation
progressed past the prior cache refusal. The first observed native denial was
Owner8226 at Schedule 0x816BA440, in its profile-producer binding. The exact
subcondition was not recorded. The executor returned 0x80004004 without the
required authenticated native own-return; protocol, CompleteIO and final roster
publication were not observed. The dashboard remained unsigned and did not
respond to subsequent Guide/RB input. This is a failed sign-in test, not a
successful native return or a usable-checkpoint acceptance.

Normal manual WM close returned zero after 411.18 seconds, fully reaped with
no forced signals and checked inputs preserved. The native empty baseline and
Guide visibility remain positive observations; profile creation, Avatar Editor,
game avatars and game/save readiness remain unaccepted. Tests used isolated QA
data with MangoHud and GameMode disabled. No new AppImage was produced.

| Current V130 record | SHA256 |
| --- | --- |
| Three-path correction apply | `54da86415473e61a1c75b705261b2e672230d868dce8fa4af40a5beb9f05de75` |
| Build | `8a55026bb1e7de4c4dd271a6e8acffbf42aebce410cb0700651a4c759c254682` |
| Seven test runs | `4129e65db3ed49c9f8551d226291a986167e54bcb220419493208bad6d7032e0` |
| ELF | `007862c9584abd5e137e1e7bf5b49d1ec8d0c29c38dab023249e7d6375b561e3` |
| Numeric runtime report | `2679120661c6ee6ef5ebab3a1a5da48bc998fdf50ead367b301565ae3995b933` |

## Historical V128 lifecycle and runtime result

The integrated lifecycle changes retain full hook frames and their actual
State/backing owners until return. Stop seals admission of frames that could
park; genuinely nonparking frames can remain owned through the real worker
join. Public calls still must unwind. Avatar returns now bind to the exact
admitted request, pair kind and invocation instead of searching retained
requests by a reusable physical stack frame. Shared profile preparation retries
only eligible preentry failures after actual cancellation of its previous
attempt, using the original caller and deadline. Entered native operations are
not replayed. Failure diagnostics remain disabled by default.

All five current direct suites passed: base 323/27,705 assertions, network
125/11,212, kernel 1,420/88,861, VFS 136/9,736 and UI 124/4,863. The separate CPU
selection passed 5/130; installed-system selection passed 151/19,106, including
all 18 new lifecycle cases and three explicitly selected hidden cases.
Selections overlap; they are not additional unique full-suite coverage.

The isolated current ELF established the native empty baseline, displayed the
Guide, opened its native profile picker, returned dashboard input on close and
reopened visibly. Selecting local123 still failed before native initializer
entry. The actual 224/120424 request selected slot 1, made multiple real
preparation attempts and returned E_ABORT with EBUSY. Nested diagnostics reached
GPD cache preparation and cache commit. Preparation uniquely identifies a
namespace TryAcquire refusal. Commit also has open-entry, overwrite and
capacity refusal paths; its exact cause and the lock holder are unobserved.
The next correction concerns common preparation/publication progress and
ownership; it must preserve snapshot validation and real native completion.

The Guide displayed eight profiles before selection and zero on reopening.
Mapped HUD code counts asynchronously consumed enumeration rows without
waiting for enumeration to finish. That observation alone proves neither an
empty completed inventory nor profile deletion. The Social screen's promotional
avatar artwork is not evidence of an attached profile avatar.

Normal manual WM close returned zero after 179.31 seconds, with complete owned
cleanup, no forced signals and checked inputs preserved. This is a passing
shutdown result for this unsigned/failed-preparation scenario. It does not
establish sign-in, profile creation, Avatar Editor, game-avatar or broader HDD
acceptance. The previous V126/V127 shutdown failures remain historical.
MangoHud and GameMode were disabled; testing used isolated research data and a
virtual controller. No replacement AppImage was produced.

| Current V128 record | SHA256 |
| --- | --- |
| Applied nine-path composition | `ddce15ef2ead2e75b92f7189fafb9bbdd7bc567988b532bd2d1b8eefbc40294e` |
| Build | `59d77a48a3e136d1fb490e236a6d8085b2b7a9c6b1976354fe7e70581e8262c3` |
| Current seven test runs | `9b2afa3a17e2d2ed87617483762541f6324a5b9f5453b5f41996391ab2e4cfaa` |
| ELF | `9d5e559318b79687a2a3af7fdae991ab7faa7ea0e5b166f4ccff927110a8cd93` |
| Runtime result | `a7b4fb9e2dbc967b9f1ae4e10a6c0690c4723e814f8eb5f3e3738c9fa7aff437` |
| Numeric runtime report | `f18ec5fcb4515150974e274aad45adf4727b7fcf422ed9a38d91f3b1372084c3` |
| Root visual/cleanup report | `0b72f145382393534aaae55bccc40a9f1096469f4be612a24c8b32fb270c9c57` |

V127 added failure-only GPD observations and passed 133 focused installed cases.
Its actual sign-in attempt stopped at storage-lease admission before GPD load;
normal close returned one with Avatar shutdown uncertainty. The full V126
suites were historical for V127, not rerun under that label. V128 reran all
seven required current selections and reached the later cache frontier.

## Historical V126 startup and runtime result

The actual native DB788 constructor still runs and its mandatory postcondition
is checked at its own return. Its verified initial empty identity state can now
establish the matching unchanged host roster once, after full native startup.
The fresh native provider/allocation and profile-manager transaction checks
must agree. A mismatch retains the existing mutation path.

This baseline keeps the initial read source 2, published source 0, unchanged
epoch and no identity job. It introduces a new visible pixel guard; it does not
publish a roster, run a sign-in request, complete an overlap or grant service
readiness. Subsequent real profile changes retain their native transaction.

The current seven-product Release build passed. Direct full suites passed:
base 323, network 125, kernel 1,401, VFS 136 and UI 124 cases (2,109 cases and
141,542 assertions). The separate CPU selection passed five cases/130
assertions; installed-system selection passed 93 cases/14,103 assertions,
including all nine new baseline cases and three explicitly selected hidden
cases. Focused selections overlap the full suites and are not additional
unique coverage. The CPU instruction corpus was not run in full.

The isolated native dashboard run then established the intended aligned/Open
source-2 baseline without a synthetic startup job or original logon attempt.
A virtual controller opened the visible native Guide, used its native sign-in
picker, closed it, navigated the dashboard and reopened the visible Guide.
This is current ELF evidence, not acceptance of the published AppImage or a
new physical-controller run.

Selecting disposable profile local123 requested slot 1 through actual
224/120424. Caller admission passed, but PrepareRoster returned EBUSY at its
GPD model/intents/load check (existing refusal line 1104), before native
initializer entry. Source analysis narrows this to LoadGpdModel: model creation
clears the error and can report EINVAL, while a missing owner with no error
would report cancellation. Neither yields the observed EBUSY. The nested
loader stage remains unobserved. Native sign-in, creation and
Avatar Editor are not accepted. Do not substitute the host toolbar's slot-0
label for this request's slot-1 state.

Normal WM close after 210.06 seconds returned code 1 with complete owned
cleanup and no forced signals. This run's first shutdown failure was nonzero
identity hooks; their exact owners remain unobserved. It is distinct from
V125's JoinBootstrap failure. Source, configuration, products and read-only
inputs were preserved; installed user data was not used. MangoHud and GameMode
were disabled.

| Current V126 record | SHA256 |
| --- | --- |
| Applied nine-path source freeze | `c41e4dc3ee71507070a38d9bb8631657df2f7d387aa46a68ca97e1c75af8c949` |
| Build | `d4230ec4e60b522c7b52df13c896b85228c955869d3feda5045cb5032269a189` |
| Direct tests | `bbeb1463a47feac629051b38ff5f9205cc70204c8f6bdfa606e3fccb599c8cab` |
| ELF | `cc077ea6eb594cbf0518ee22a6e835623804d0bce28e8e5b19ad8606b29b06a6` |
| Runtime result | `06ba7802673484840c1fd5381565fa3e4b959a3bd22cc909d8a9b472afd4a815` |
| Numeric baseline/profile/stop report | `f46a52230481e12b2ddbedf00fd71608eed68c161c40e9feaada044844b5df5e` |

Next work addresses the shared GPD loading/publication pipeline and genuine
hook rundown. No busy check, reader ownership or native completion is waived
by this startup correction.

## Earlier V123–V125 implementation

The original native 20B/224 initializers copy input into native request storage.
For the verified asynchronous request families, they do not replace the old
profile allocations before queuing the data worker. The reentrant render path
therefore retains the entire old frame/read while gating the real E148/C838
worker until the existing source, read, listener, Wake and paint boundaries.
Compatible retained readers and Task bookkeeping are distinguished from real
identity writers and unbounded execution; existing source reservation checks
remain in place.

Native SignIn also calls 224 from an ordinary Task callback. The mapped SignIn
module then polls for actual overlapped completion. Callback return is not a
valid prerequisite for that completion. The common caller path now permits a
verified readless Task issuer to use normal preentry, after proving it has no
same-caller read, publication, suspended restoration, old-work, Expected, Wake
or bootstrap-progress dependency. It creates no ProfileRead. The full existing
old-data/paint cut precedes native initializer entry on that path. Native return
values, input ownership and actual completion remain authoritative.

These are production checks with bounded diagnostics, not evidence that the
new paths passed their native scenarios. No request-flag expansion, Gold/Live
provisioning, fabricated completion or scheduler change is included.

Primary evidence uses the supplied 17559 XAM and Metro SignIn mappings. The
supplied [XNA title update](17559-xna-update-reference.md) is separately pinned
to its actual 4.0.21021.0 target; it is not replaced by an older mapped launcher.

## Earlier validation and observed scenarios

Each V123, V124 and V125 app/kernel build completed with preserved source,
configuration and fixture records. Each directly ran the same 81 focused
kernel cases, including three explicitly selected hidden cases: 13,166
assertions passed. These cover profile persistence, wire ABI, Task/reference
lifecycle, avatar contracts, roster selection and overlapped behavior.
They do not establish native sign-in or rendering success. Older full base,
network, kernel, VFS, UI and CPU results remain historical; they were not
relabelled as current executions.

| Scenario | Result | Evidence and first actionable limit |
| --- | --- | --- |
| V123 fresh native creation | Creation observed; sign-in failed | Native journal stopped before initializer entry on the other-owner check. Runtime was externally interrupted before normal close; no shutdown result is inferred. |
| V124 fresh creation | Failed before name entry | Guide and native HDD picker were visible; the next pane stayed blank and did not visibly respond to Back. No original sign-in API was called. The cause is unproven. |
| V124 existing profile, startup | Blocked by readiness | Native 224/25 returned E_ABORT while alignment was incomplete. |
| V124 existing profile, settled Guide | Failed before initializer | Actual 224/120424 reached the admission layer inside B8B08. The required render-caller check refused it. The later other-owner classifier was not evaluated. |
| V125 existing-profile acceptance | Untested | No profile selection was submitted because startup readiness was not established. The new normal caller path was not exercised. |
| V125 Guide visibility | Failed in this run | Two controller edges for native user 1 were accepted/dispatched; settled screenshots still showed the dashboard. Visible open/close completion and cause remain unverified. |
| Native shutdown | Failed | V124 reached nonzero-hook checks. V125 instead first refused JoinBootstrap; its identity-conjunct captures were unavailable. These are distinct failures. |

The V125 coordinator snapshot at GUI exit is fresh evidence: startup job 3 had
completed the old-data, paint and source/Wake arm stages, entered its owned
continuation, and had no recorded native return. The captured continuation
stage was entered but unfinished. Earlier periodic samples cannot identify a
later blocking primitive. The snapshot does not prove a particular wait,
timeout, missing thread or deadlock.

The startup producer schedules a real SYSTEM continuation through 96558; it
does not call native 224 directly on the waiting bootstrap thread. The entered
continuation/initializer path is the next startup frontier to investigate.
The native callback and worker ordering must be recovered before changing it.

All three completed V124/V125 runs closed through an ordinary WM request,
exited with code 1, and had complete owned cleanup without forced signals.
That host cleanup is not successful native shutdown. The latest run lasted
427.78 seconds within the unchanged 600/660-second controls. MangoHud and
GameMode were disabled. Disposable research data was used; installed user data
and the accepted older artifacts were preserved.

## Reproduction and evidence

Use the recorded current ELF with isolated storage/content/cache/log paths,
the pinned 17559 installed resources and the same source configuration. Cancel
startup sign-in, establish actual alignment Open/job 0, then open native Guide,
choose the disposable existing profile and follow its native consent flow.
Do not infer readiness from elapsed time or a stale periodic sample. Dismiss
modals and close normally after the meaningful result; preserve the real exit.

Private evidence bundle: `~/.local/state/xenia-edge-research/evidence/hdd-gate/`.
Raw logs, screenshots, profiles and signing payloads are not published here.

| Record | SHA256 |
| --- | --- |
| V125 source freeze | `4d5c654adb8acfa28aa887c481655a09c42eb0869ff7ff2ca0531374c8da2014` |
| V125 production review | `762bd34298984b6d1ea233a5b94b1e193439a7a7f79df8c47a05ed8c551f97e5` |
| V125 source application | `948ae49d1052057a80e8451654001c897c693ed0910b3c9e038e33535d80b6d3` |
| V125 build | `63ba780bf90f61ed2852c186f00814432244baba888b579b8a40f151aca39f90` |
| V125 focused tests | `482516f8b210602996a27348320fb7a971d09e91e0aedb0d0a05e60a08011b74` |
| V125 ELF | `0c064b3867aad9755e502e736f1887f8e3842b0397f1f0ba75238e07ab9e4db5` |
| V124 existing-profile numeric report | `bfc28e27e94b869b7ac692e333b75b95f387a7c7fadecc729ecfbf5950d32092` |
| V125 numeric startup/stop report | `32f4d1f154274f8ff1903fc34be34fd7b9112800aa659e4e2c2378f6d101bb91` |

V125 implementation bytes were Owner `338f07b6c1eb84ca362c871885ab3373edb7bb35588cdbcf9577ce4b86e2db21`
and roster `4a97bf2cbceea0e0cff26bb1514bf43dee78e14b38ca7704900169ed5bf172b1`.
The FFmpeg submodule's working modification still exactly matches the tracked
`third_party/patches/FFmpeg/xmaframes-overlap-tail.patch`; no unpublished
submodule commit is required.

Real profile loading/sign-in and shutdown remain prerequisites for the next
native profile/avatar acceptance run. Preserve required lifecycle checks until
their replacement is proven. The [console fidelity policy](17559-console-fidelity-policy.md)
also governs any new hook rundown or core change. A scheduler reclamation split
has not been shown necessary by these observations and is not implemented.
