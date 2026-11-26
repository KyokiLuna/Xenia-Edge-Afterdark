# 17559 checkpoint Guide regression — 2026-11-23

Status: the current V151 Linux source adds the tested
[shared storage-selector fix](17559-selector-source-update-20261126.md) and
passes all seven direct runs. The latest actual native game test remains V149.
That isolated Doritos run selects the existing QA profile and renders the native
sign-in error dialog. Ordinary publication reaches both settlement stages
7/8; the genuine `0x80151000` error is preserved. A follow-up native profile
request is refused before entry. An explicit SIGTERM received by the test
supervisor interrupts the run before the Guide check; its sender is unknown.
This is not a guest exit 143, normal shutdown or Guide acceptance. V148's
visible Guide and failed close/cancel-reopen checks remain historical results.
No replacement AppImage is accepted. See the
[current lifecycle comparison](17559-profile-lifecycle-comparison-20261125.md)
for source identities, exact runtime scope and the remaining native input and
inventory boundaries. The results below retain their historical scope.

Historical V131 Linux ELF displays the unsigned dashboard and native
Guide/profile picker. All five full direct suites and focused CPU/installed
selections passed, including twelve new lifecycle cases. The coherent native
request/outcome change passed the earlier Task producer boundary: the real
initializer returned zero and a C838 Task/read/commit record was observed.
No protocol completion, roster publication or owned CompleteIO followed;
the dashboard stayed unsigned and Guide did not reopen after selection.
An owned WM close was requested; process exit was one, fully reaped without
forced signals. This is not sign-in, Avatar Editor or AppImage acceptance.
See the [native profile admission update](17559-native-profile-admission-20261125.md)
for current evidence and historical results.
The published `hdd-milestone-20261123` AppImage remains affected. Its release page
carries a critical-regression notice. The combined HDD goal is active again;
repairing this checkpoint remains the immediate priority.

## Observed failures

The user reported dashboard/game lag and a missing Guide, then confirmed that
Guide audio plays and controller input is captured without a visible Guide.
Desktop lag may have contributed to the initial performance report; it does not
explain the reproduced missing overlay. Launching Avatar Editor from the native
dashboard profile menu also reportedly closes Afterdark; that handoff still
needs its own matching runtime trace.

Two isolated runs of the actual published AppImage established different
frontiers:

- A freshly created, signed-in research profile showed a black dashboard.
  Native initial roster preparation repeatedly returned `EBUSY` before native
  publication. Normal window close did not finish; the owned deadline guard
  terminated and reaped only that test process group. The precise busy origin
  remains unconfirmed.
- A separate unsigned research state rendered the dashboard. After Skip Sign-In,
  the physical Guide button produced audio and captured input without an overlay.
  Native GPU frames completed, but their display publication was withdrawn:
  initial profile alignment remained in `HiddenRefresh` with four empty slots.
  Normal close returned failure. No user library or original profile was changed.

The initial checkpoint acceptance covered host profile creation/persistence and
no-title startup. It did not prove dashboard/game, physical controller or Guide
behavior. Those narrower results must not be treated as evidence for this path.

## Corrections under validation

The profile manager now records an immutable fact from the actual prepared
before/after resource envelopes: both contain no profile, model, Account,
identity or storage owners; every slot is fully empty; the epoch is unchanged;
and no native Account rows were introduced. This fact becomes eligible only
after genuine native protocol, selected-read, host-map and writer commitments.

For that initial empty alignment, the existing owner can acknowledge a real
completed hidden frame from the same job, source, parent guard and native
allocation owner. Actual GPU completion, command-plan operations and restored
title state are still required. The existing prepared-query, descendant, wake,
provider, paint and final profile-manager joins continue to control reveal.
No image-read success or visible-pixel permission is fabricated. Nonempty
profiles, ordinary no-op updates and real sign-outs retain their image duties.

Image capture also compared two independently numbered generations: runtime
allocation generation versus module load generation. The observed values were
1 and 2. It now compares the full allocation owner and separately validates the
native module/root load generation.

Repeated image-observer validation has been reduced without a code-validation
cache: module/section checks use page boundaries, every original translated
instruction and complete body hash remain checked, and unused Task/render
lookups are avoided. Malformed-return availability behavior is preserved.
This is a performance ingredient; an improvement in game frame times has not
been measured.

A sampled post-pending roster refusal diagnostic records the actual failing
call-site line after existing cancellation/unlock. It preserves the real error
and will distinguish Account, storage and cache admission failures in the next
signed-in run.

## Validation and remaining work

The earlier six-product Release build passed. Direct base, network, kernel,
VFS and UI suites passed: 2,058 cases and 137,739 assertions, with source,
configuration, products, HEAD and index preserved. Two added tests exercise real profile-manager intent preparation,
publication, role separation, cancellation, nonempty no-op and sign-out refusal;
they do not claim a native GPU/controller acceptance pass.

The subsequent isolated current-ELF dashboard test still failed: the user saw no
Guide after Skip Sign-In or after choosing a profile. Its actual completed-frame
acknowledgement now succeeds, but profile alignment remains in HiddenRefresh.
The next investigation distinguishes provider freshness, the current Source's
descendants, and native wake completion at their original readiness checks.
Normal window close returned failure without forced signals; the owned process
group was fully reaped. This is a failed live test, not replacement acceptance.

A subsequent app/kernel build and direct kernel run also passed (1,350 cases,
84,223 assertions). Its live test still has an invisible Guide; closing Guide
does release input to the dashboard. At the original final-readiness check,
native protocol, maps, writers, native return and real frame acknowledgement
all pass. The provider scan is the first failure: an initialized type-B provider
has no completed output or image key. Descendant and wake checks are not reached.
The native constructor and initializer allocate a buffer and listener; they do
not themselves issue a tile read. The next correction must distinguish actual
provider duties from mere initialization without granting image readiness to an
unused provider or relabeling old cached textures.

The shared provider scan now recognizes a live, initialized provider with no
admitted Query, no issued output, and no active provider work as having no image
duty. Query admission removes that classification before executing the native
body. It does not set a completion source or grant image readiness. Its build
and 127 focused cases passed; the subsequent physical run failed earlier and
therefore did not validate Guide visibility or this provider path.

That physical run exposed an independent Task lifetime error. The native Close
function saves its atomic decrement result before returning. A valid nonfinal
return can arrive after another thread's final Close has deleted and freed the
Task, and after its address and metadata slot have been reused. The tracker
rejected that order and marked native UI ownership uncertain, disabling input.
The process later ended with SIGKILL before the timer requested close; the user
did not force-close it. The signal's cause is unconfirmed.

The correction consumes only the matching retained Close call for a nonzero
native result, without looking up the old Task. Every zero-reference release
check remains unchanged. Three real-ledger regression cases failed on the
original header (13 failed assertions), covering both deleting destructors and
all three allocation/reuse lanes. The corrected header passed 134 focused cases
and 13,664 assertions. A subsequent physical run kept input ownership healthy
but still had an invisible Guide. The newer captures include scalar memory
observations and an owned-process low-memory stop; they do not establish the
cause of the earlier SIGKILL.

The native provider can also genuinely finish a Query without issuing an image
read. The tracker now records that outcome from the matched native return and
unchanged read-version counter, rather than treating every Query as an image
duty. Actual read admission, unknown writes and invalid lifetime transitions
invalidate that evidence. A recorded virtual-controller run exercised the
no-read path and passed all eleven initial final-readiness predicates, including
actual Source descendants and wake completion. The Guide still remained hidden.

The next diagnostic preserved the original closing-prepared checks and exposed
their first failure: the new notification source still had nine references,
including queued notifications, while actual identity work was empty. Native
17559 broadcast/enqueue code returns without waiting for all listeners to
consume their notifications. Queued hints retain origin metadata; they do not
own profile read scopes. New-source reveal must distinguish that retained
metadata from active readers. Old-source retirement, provider completion,
actual Task descendants, wake completion and GPU/paint joins remain separate
requirements.

The correction now distinguishes passive queues from active or unknown whole
consumers, active native notification calls, and unowned successful dequeues.
Unowned consumption is recorded for the exact origin and remains blocking even
after listener destruction. A final check after profile-manager I/O waits for
healthy late work rather than publishing across it. The build and 138 focused
cases passed (13,785 assertions), but the recorded controller retest still had
an invisible Guide. It positively observed unowned consumption, so the nine
references cannot all be described as passive queued hints.

The per-origin trace identifies the actual blocked consumer: SYSTEM process
type 2, native GetNext caller `0x81A3F890`, with a genuine successful API return.
The supplied code maps establish a finite worker iteration at
`0x81A3F848`–`0x81A3F9FC`, called repeatedly by a dedicated SYSTEM thread.
Its constructor/trampoline chain creates a thread rather than an XamTask.
The tracker needs an explicit bounded thread-frame work/read lifetime, admitted
before the iteration's mutex and GetNext call and completed at its own return.
Requiring a Task for that real thread cannot establish the missing lifetime.

Public TITLE notification delivery and internal SYSTEM callback lifetime remain
distinct contracts: a game may poll notifications from an arbitrary loop.
The current blocker is the SYSTEM worker, so the public TITLE delivery proposal
is not its fix. Avatar Editor, signed-in transitions, visible Guide and normal
shutdown remain unaccepted. The diagnostic build passed 24 listener cases and
5,968 assertions; its short runtime capture closed normally without forced
signals. Those results do not establish a functioning Guide.

The integrated correction adds an explicit SYSTEM-thread-frame owner to the
shared event-work ledger and profile read scopes. It retains the actual thread,
domain, code and entry frame before the finite worker starts. Native Task work
keeps its existing requirements; a dedicated-thread owner carries no Task or birth.
Where a finite helper runs inside an already observed native Task, it borrows
that exact allocation through the existing external-work ledger. Dedicated
thread frames retain zero Task/birth fields. The normal compiler epilogue
observes the real return after the original
save/restore helper. Pre-entry shutdown refusal and already-entered shutdown
completion use different admission rules. This integration passed 143 focused
cases and 14,002 assertions. Its first runtime test
regressed dashboard startup: the new frame path parked during `DrainingOld`,
although the existing render iteration legitimately admits current-source
reads during that phase. The next revision uses the same phase and old/writer
read-pool policy. Its actual V106 run restored dashboard startup, retained
healthy ownership and closed normally without forced signals, but Guide stayed
invisible. The focused tests passed again; the full five suites above are
historical results and have not been rerun on this revision.

V106's next matched unowned consumer is `0x8170BA20`, with GetNext returning at
`0x8170BA68`. The complete primary-image scan covers all fifteen direct GetNext
calls: the existing Task-backed Query path and nine finite SYSTEM consumer
bodies. These are being integrated as one descriptor batch through the common
frame owner, with complete body/save/restore authentication and their actual
argument contracts. A finite frame can borrow a real Task without creating a
birth or claiming callback completion. Borrowing starts only at body admission,
so a parked, unentered frame cannot block the phase it awaits. Nested consumer
dependencies survive until the real parent returns; restored native TLS and
allocation identity are checked again on return.

The V107 app/kernel build and 146 focused cases passed (14,232 assertions).
The isolated virtual-controller run rendered the dashboard and exited normally
with code 0, without forced signals. Guide was still invisible on both open and
reopen. Closing it returned input: a right press changed the dashboard selection.
There was no native ownership uncertainty. All eleven initial readiness checks
passed; the later prepared-source check still observed an unowned notification.

That caller is in the separately loaded `Xam.Community.xex`, not XAM's root
image. Its finite `0x9124F010` body has one GetNext edge and dispatches only ID
`0x11`; the identity-change ID `0xA` is discarded without reading its payload or
profile rows. The next shared contract distinguishes this proven scalar delivery
from a profile-reading SYSTEM callback. Any exemption must retain exact loaded
module/domain/code evidence and the real API/refcount return; unknown consumers
remain blocking. A cross-module consumer inventory is in progress. Guide,
signed-in transitions and Avatar Editor remain unaccepted; no replacement
AppImage has been released.

The shared scalar-delivery unit now covers the two verified non-reading SYSTEM
callers in Community and HUD. It transfers the existing queued hint reference
into the actual API cell under the native area lock. It releases only that
reference after the real API return, object-reference release and thread-cell
withdrawal match. Both the original broadcast's public ID/mask and the actual
public output ID must identify `0xA`; internal normalized IDs alone are
insufficient. Whole profile-reading consumers retain their existing ownership,
and unknown or opaque consumers remain blocking. Event-slot reuse does not let
an older API return modify or release a newer queued notification.

Module/domain/load/code leases, full original body hashes, native import
bindings and actual caller/output-stack shapes authorize those two policies.
An observed refusal exposed the loader's real metadata contract:
`SetupLibraryImports` stores the extensionless library name `xam`, rather than
the raw XEX table spelling `xam.xex`. That comparison is corrected. An already
admitted scalar API also retains its completion right if shutdown seals new
allocation admission; this grants no new entry or synthetic completion.

V109's app/kernel build and 147 focused cases passed (14,705 assertions).
The actual virtual-controller run displayed the native Guide on open and
reopen. Closing Guide returned dashboard control, verified by moving the
selection. Both recognized caller policies passed all 26 entry and return
checks, and initial alignment reached Open with matching current/published
source 3. This establishes the unsigned visual/control path only.

The native picker listed QA62. Selecting that legacy fixture reached the first
Xbox Live connection/consent prompt; choosing No returned to the unsigned Guide
without a new identity transaction. Its account type and the native consent
branch require verification before judging offline sign-in. Signed-in Guide
and Avatar Editor were not tested. Normal window close exited with code 1 and
the explicit native stop-unproven failure; no forced signals were used and the
owned processes were reaped.

The following source review identified a healthy shutdown admission refusal
being promoted into an ownership failure: a background listener poll arrived
after new sources were sealed. The common bridge now distinguishes healthy
closed admission from invalid or lost ownership under the same admission lock.
It cancels the new call without opening a cell, modifying output words, or
claiming success. Already admitted calls still owe their actual native return
and reference-release duties; invalid ancestry and ownership loss still fail.

V110's app/kernel build and 150 focused cases passed (14,906 assertions).
Its isolated 120-second run closed normally with exit code 0, reaped all owned
processes, and used no forced signals. The Guide was visible in a later capture
after an initially invisible capture; the delay's cause is not established.
This short run did not exercise close/reopen or profile creation. V109 remains
the actual unsigned close/reopen observation. Signed-in transitions, Avatar
Editor, and AppImage acceptance remain outstanding.

The supplied native sign-in bodies also establish that declining the first
connection consent returns `E_ABORT` before logon. Dashboard context can request
this consent independently of account type, so the prompt does not establish
that QA62 is a Live account or a broken local profile. The next profile test uses
a fresh native local creation flow rather than changing that legacy fixture.
No replacement checkpoint or gate release is accepted by these results.

The longer V110b test used the identical qualified ELF. Guide open, close,
dashboard input release, and reopen all passed visually. The dashboard's
Create Profile action opened ImGui; its shared service saved a fresh local
profile but failed to load it. The native picker continued to list only the
legacy QA62 fixture. This is a discovery/mount/cache-publication blocker before
sign-in, not evidence of an Avatar Editor handoff failure. Existing diagnostics
do not identify the actual Load/Mount refusal.

Normal close after this longer scenario exited with code 1 and the native
stop-unproven failure, with complete owned cleanup and no forced signals.
Identity ownership had no recorded loss; its F848 pre-entry shutdown refusal
is expected. The precise remaining stop subjoin is not established. The earlier
short V110 exit-0 result remains valid only for that short unsigned scenario;
normal shutdown is not accepted across the longer scenario yet.

V111 added diagnostic-only load/mount and whole-stop observations; its build
and the same 150 focused cases passed. On restart the native profile list
discovered the local profile saved in V110b. The saved payload is therefore
recoverable; this does not establish why the original immediate load failed.
The only recorded Account admission refusal in V111 was an earlier startup
mount's nonblocking namespace-lock refusal, not a fresh creation failure.

Create Profile from Guide takes the native SYSTEM path and displayed its HDD
selector. Accepting HDD returned to the unsigned dashboard before name entry.
That differs from the foreground dashboard action's ImGui route. The native
creator then made an observed undefined call to `XGetOverlappedResult` (ordinal
`0x43B`). Its mapped body and `0x1FB` require the real completed overlap result
to be reread after waiting; the current `0x1FB` implementation instead retains
the wait status. A shared completion-query correction is being prepared.

The same run positively recorded its first identity-owner loss at the generic
CompleteIO frame check, called from the native device selector. The later
CompleteIO-specific branch already intends to leave unrelated overlaps alone,
but it runs after that frame refusal. The required correction is common routing
between identity-owned and unrelated I/O, including nested return matching;
allowing a selector caller address alone would not establish that contract.
Afterward UI queries entered a rejection hot loop. Memory grew to approximately
8 GiB and the owned guard stopped the run before its scheduled normal close.
The allocation source and its causal relationship to that loop remain unproven.
This run supplies no normal-shutdown acceptance result. Its original cleanup
record says leader not reaped; a separate later check found the exact owned PID
absent, without rewriting that original result. The dedicated virtual device
was explicitly removed. No replacement AppImage was produced.

The V112 shared correction routes CompleteIO by the actual pending identity
request before applying identity-only frame checks. Ordinary SYSTEM calls retain
their own neutral entry/return marker and cannot consume an older identity
return. Claimed identity calls retain exact job/control-block, serial, source,
overlap and native frame obligations. A malformed neutral return remains sticky.
The shared query unit implements `0x43B` and corrects `0x1FB`, with distinct
Win32/NT wait-error behavior, lazy DWORD access, native length-before-result
ordering, actual post-wait operation results, and native zero-extension.

V112 built successfully; 160 focused cases passed with 15,654 assertions.
The five-minute native test did not reproduce the earlier owner loss or
rejection loop. Its peak owned RSS was 822,464,512 bytes, with 284 timestamped
samples. This establishes the memory result for this sequence, not the allocation
cause in V111 or game performance. Guide opened and its native Create Profile
action displayed the HDD selector. Accepting HDD now left a persistent blank
native panel instead of dropping immediately to dashboard. Name entry, OSK,
profile creation and avatar setup remain unaccepted. B had no visible effect;
Guide closed that panel, and a subsequent right press changed the dashboard
selection. Reopening Guide was attempted near the timed close but not captured.

Normal close still exited with code 1, without forced signals and with all owned
processes reaped. The whole-stop observation identifies `JoinBootstrap`'s actual
host-thread join as the failed boundary; it does not identify the thread's wait
or prove scene/query completion. The next investigation targets the native
creator scene and that wait on the same qualified ELF. No replacement checkpoint
has been packaged or accepted.

A bounded V112 debugger observation then captured a real lock-order deadlock
on a clean Guide Create request, before the HDD selector appeared. The task
entry path held the identity State mutex while `Word`/`QueryRangeAccess` waited
for the global critical region; the task return path held that global lock
while waiting for State. Bootstrap, UI, input, audio and rendering were queued
behind them. This establishes the shared lock inversion for this run. It does
not establish that every earlier blank panel or failed shutdown has that cause.
No completion-query frame was present in the snapshot.

The debugger detached, was reaped and left the owned process resumed with
`TracerPid` zero. Normal close at 300 seconds did not end the deadlocked run;
the deadline guard then sent TERM/KILL to the owned process. The original
cleanup record reports leader not reaped; a separate later check found its
exact PID absent. That original result remains intact. The virtual controller
was explicitly removed. The proposed correction acquires the global critical
region before the entire final State scope, matching the existing return path.
It retains the actual native publication read and existing ownership checks.

A separate V112 Sign-In cancel followed by Create Profile crashed before the
HDD selector. Its PC and LR match the previously unloaded Sign-In module,
while CreateProfile had replaced the same guest address range with different
bytes. This is evidence of stale-generation execution. The retaining cache or
callback edge is not established; a cache invalidation change alone is not yet
accepted as a complete lifetime fix. No replacement checkpoint is accepted.

V113 applies the reviewed whole-scope lock-order correction. The app and kernel
built, and the same 160 focused cases passed with 15,654 assertions. The native
run showed Guide, Create Profile and the real HDD selector. After a temporary
blank selector panel, it reached the genuine Enter Profile Name OSK. Desktop
typing replaced the selected default name correctly. Submitting Done produced
the native Cannot Sign In error; the underlying failed operation is still being
traced. Native profile persistence and avatar handoff remain unaccepted.

The post-HDD snapshot did not show the earlier ABBA cycle. It showed task code
authentication holding GCRC while rendering waited for that lock, with Bootstrap
in its ordinary delay. The subsequent OSK progress rules out permanent deadlock
for that observed interval. One snapshot does not establish the source of any
perceived lag. Debugger cleanup again completed without signals. Normal WM close
at 300.072 seconds returned 1 with all owned processes reaped and no forced
signals. Exit 1 remains a failed shutdown, not a pass. The new stop observation
points to a different nested Bootstrap failure, still under investigation.
No replacement AppImage was produced or accepted.

A real CPU regression now reproduces one common unload defect: normal dispatch
still contains a compiled target after its owning module is removed and a
smaller same-base replacement is loaded. The test runs actual compiled near/far
calls first, then stops at a hard dispatch-state assertion before any possible
call into retired code. It passed 28 assertions and failed exactly that guard;
its process exited 1 normally without signals. All six existing app/UI/kernel/
base/network/VFS products remained unchanged through the CPU-test-only build.
This proves the dispatch-retirement gap. It does not yet identify the precise
retaining edge in the Sign-In cancellation crash, or establish a fix for direct
calls, concurrent compilation or active guest callbacks. No production cache
correction had been applied at that point.

V114 applies the shared normal-dispatch retirement correction, including a
regression that preserves a different live module at an overlapping address.
The original failing test remains unchanged and now passes. Base, network,
kernel, VFS and UI executables all passed directly (2,087 cases and 140,152
assertions); the additional CPU and installed profile/avatar runs passed
46 cases and 10,161 assertions. These are direct executable results, not
AppImage or native-flow acceptance.

The isolated V114 dashboard visibly opened Guide, closed it, returned input to
the dashboard and visibly reopened Guide. A profile created during V113 also
appeared after restart, establishing persistence and later enumeration despite
the earlier sign-in error. Fresh V114 creation remained at a blank panel after
the native HDD selector. That run received an external termination signal before
its scheduled close; its sender was not observed, so it provides no normal
shutdown result.

A separate V114 run selected that existing profile but did not establish a
signed-in state; Guide still offered Sign In. An explicit normal window close
returned 1, with all owned processes reaped and no forced signals. The earlier
shutdown admission probe was unavailable because avatar state had already
become uncertain at request-task closure. This is a different observed frontier
from V113's shutdown-wrapper refusal. The task ownership contract remains under
review. No replacement checkpoint is accepted.

Primary 17559 code also establishes an inverted passcode predicate in the HLE
profile producer: the native callback sets the protected flag when any of the
four passcode bytes is nonzero, while the V114 HLE producer sets it when all
four are zero. This supersedes the earlier inverse interpretation of the native
helper. The mismatch is proven statically; its connection to the observed sign-in
failure remains unverified. V115 corrects that predicate and adds a registered
230/234 ABI test covering all four byte positions, preserved existing flags and
unmodified caller data. The app/kernel build and all 49 selected profile,
avatar-contract and task-close cases passed, including the three installed
resource fixtures. The isolated native run reached Guide, the HDD selector and
the genuine name-entry keyboard, then submitted a fresh synthetic profile.
Recorded 230/234 and transaction results establish successful publication and
discovery. The subsequent native screen still reported Cannot Sign In, so the
passcode correction does not close that failure. The later native stage remains
under investigation; the creation operation itself is no longer an unknown.
Existing profiles are not rewritten on that basis.

V115 closed through an explicit normal WM request at approximately 244 seconds
and returned 1 with full reaping, no forced signals and unchanged qualified
inputs. In this run Avatar shutdown admission succeeded without prior avatar
uncertainty; the later stop failed the common identity-drain postcondition.
This differs from V114's failed avatar task closure and must not be reported as
the same failure recurring. Profile mount attempts also recorded ordinary GCRC
contention refusals at both reuse and namespace publication. Their locking
contract is under review; the popup's specific cause remains unproven. The native
error popup was still open when WM close was requested, so this is not an idle
dashboard shutdown comparison.

V116 changes both short profile-mount namespace scopes to acquire GCRC before
trying PM metadata. It waits for ordinary namespace contention without holding
PM metadata; inherited caller GCRC still refuses. Host filesystem preparation,
source validation and native resource destruction stay outside those scopes.
A registered ProfileOpen regression exercises both cold and reused aliases
under a real competing GCRC owner. The build passed, followed by all 1,381
default kernel cases (86,952 assertions) and all 50 installed-focus cases
(10,913 assertions). Native post-creation sign-in remains unaccepted.

The 17559 creator map now identifies the specific error path: after successful
234 consumption, it starts an overlapped four-XUID LogonTask through 20B with
flags 0x24. A negative LogonTask result opens the observed Can't Sign In dialog;
this differs from the generic profile-update error. The current bridge prepares
the roster and mounts its profile before native execution, so the common mount
contract is on that admission path. The actual failing logon phase has not yet
been observed; this static path does not establish which mount refusal caused
the runtime popup.

V117 integrates the reviewed native context-reference lifecycle correction.
Each actual BA3C0 episode obtains its own native CurrentTask reference; pending
episodes close it before rescheduling, so a fresh worker supplies a fresh
baseline. A separately held avatar reference can close during either active
callback without borrowing an old dispatch frame or promoting a historical
address into a Task allocation generation. Six lifecycle regressions cover
resumption, nesting, body admission, revocation and nonfinal release. Build,
tests and actual native-path verification remain pending. The first build
stopped at a CPU frontend assertion: its six-slot binding table did not match
the ten observation sites. All seven previously qualified products remained
unchanged. The frontend table, extent and consumers require the same integration
correction before another build; this failed attempt is not a qualified image.

V118 adds the two missing frontend consumers. Registration derives its loop
from the names array, with compile-time checks against both the actual builtin
storage and the shared observation descriptors. HIR already uses that descriptor
count dynamically. The generated dependency graph also links the CPU archive
into base and UI tests, so all seven products will be rebuilt/relinked and the
five direct suites, CPU regression and installed focus rerun. These checks and
native runtime verification are pending.

The first V118 full build completed 245 of 246 steps without compiler errors,
then its 1,800-second guard stopped the final application link. Cleanup fully
reaped the owned process; the six completed products and the missing application
were recorded explicitly. A separate resume operation verified unchanged source,
configuration and identity, admitted exactly one remaining executable link and
finished normally in 5.87 seconds. The failed operation was preserved without
backfilling a successful build receipt.

The completed V118 resume then passed all seven direct runs: base 323/27,705,
network 125/11,212, kernel 1,387/87,186, VFS 136/9,736, UI 124/4,863, CPU focus
5/130 and installed focus 56/11,147 (cases/assertions). All six context-reference
cases and all three installed-resource fixtures were selected and passed.
Qualified source, configuration, index, seven products and the 174 allowlisted
system files remained preserved. Native sign-in, asynchronous avatar cleanup,
editor handoff and shutdown still require runtime verification before packaging.

The completed V118 native run rendered stock avatar models in the actual
profile picker, including visible pose changes. Guide opened and reopened
visibly; closing it released input and a right press moved the dashboard
selection. Selecting the existing synthetic profile did not establish accepted
sign-in evidence. Initial native-query captures returned `0x80070525` for all
four slots; these are first-record cells and cannot be attributed to the later
profile selection. A later
Xbox Live connection/privacy prompt was answered No. Mapped native dashboard
policy permits that prompt even for a local Account; No returns `E_ABORT`
before logon and does not choose an offline fallback. That cancellation must be
distinguished from the earlier startup selection's unobserved logon result.
Guide's displayed
available-profile count changed from four to one and then two. The mapped HUD
formats its in-progress enumeration row count without waiting for completion;
that native behavior can explain changing counts without physical deletion.
The particular restart event was not captured. No previous Mount or
roster-preparation refusal was captured.

Avatar Editor was not reached because native local sign-in remained a
prerequisite. The new typed context-reference close lane was not captured, so
the passing lifecycle tests do not establish its runtime coverage. Root
dismissed all modals and closed Guide before requesting normal WM close. The
process returned 1 after 479.44 seconds and was fully reaped without forced
signals; qualified inputs remained unchanged. This supplies a modal-dismissed
shutdown comparison, but successful shutdown is still unaccepted. This was a
direct-ELF diagnostic, not an AppImage acceptance run. The final stop observation
again failed the common identity drain/retention check; Avatar shutdown entry
was admitted without an uncertainty capture. The particular identity predicate
was not captured, so the now-dismissed modal cannot be assigned as its cause.

The shared original-logon parser also rejected valid native producers before
entry. It accepted only flags `0x0`–`0xF`, while the creator uses `0x24` and
the ordinary HUD SignIn screen forwards `0x120420`/`0x120424` through its actual
context vtable into 224. Whole native bodies establish that `0x20` suppresses
UI popups, separately from `0x200000` identity-publication suppression. The
common SignIn flags retain native preflight, recovery and error policy.
V120 admits those exact local requests and preserves the original input,
native errors, asynchronous completion and publication checks. Optional
machine-service requests containing `0x80` remain refused for later Live work.

The minimal V119 source/tool packet was preserved without a build; V120
composes both local-flow corrections into one build. Its first preflight
mistook LLVM's `--thin` archive option for an object and stopped before
compilation, preserving all seven existing binaries. A separate corrected
preflight retained the dependency checks and completed the five actual build
steps in 39.61 seconds. Full kernel passed 1,389 cases/87,416 assertions;
installed/profile/native-UI focus passed 62/11,417, including all six wire cases
and three hidden resource fixtures. The other five products remain byte-for-byte
V118 products with their prior passing results. Native local creation, sign-in,
editor and successful shutdown were still pending before its native trial.

That V120 trial displayed the native Guide after a settled toggle, the native
HDD selector and the genuine keyboard with the requested synthetic name. The
creation producer, terminal 234 consumer and transaction each returned zero
with published/discovered flags set. The UI returned to name entry; actual
post-creation logon admission/completion remains unobserved, so this is not a
successful sign-in or editor test. Initial Guide captures were premature;
recorded button edges reached the native callback and a later screenshot
positively showed Guide. No input-rejection cause is assigned.

Before Root completed dialog cancellation or normal WM close, the supervisor
received an unassigned external signal at 445.62 seconds, before its 600-second
automatic close. Its guard terminated only the owned guest. Cleanup recorded
no remaining live/unverified processes, but `leader_reaped` remained false;
the PID was subsequently absent. The record is preserved exactly. There was
no normal-close attempt or native stop diagnostic, so this run supplies no
normal-shutdown result. The virtual controller was removed, qualified inputs
remained unchanged, and no replacement AppImage was accepted or published.

V121 adds one bounded observation package for the remaining lifecycle gaps.
Actual 20B/224 export attempts retain their native flags, input/desired slot
presence masks, first original admission refusal and returned DWORD. Native
initializer return, C838 protocol, publication, settlement and existing
CompleteIO cuts remain distinct observations. A separate scalar sink records
the original shutdown predicates without reevaluating them after failure.
Neither package changes admission, short-circuit checks, guest return values,
completion behavior or caller/read authority. Both exact patches passed
independent review and produced identical results in either composition order.

The seven-path integration built normally in 617.26 seconds. All current runs
passed: base 323/27,705; network 125/11,212; kernel 1,392/87,544; VFS 136/9,736;
UI 124/4,863; CPU 5/130; installed focus 65/11,545. The three added journal tests
cover independent lifetime, bounded concurrent records and actual registered
export ABI results. Source, configuration, identity, all seven products and
the 174 allowlisted system fixtures remained preserved. The instrumented native
run remains required; unit diagnostics do not establish sign-in or shutdown.

The completed V121 native run now establishes the original refusal. Creation
and discovery succeeded, then an actual 20B request with flags `0x24` and an
overlap returned `E_ABORT`. Its first cut is the retained same-thread ordinary
Work with an owned, same-source profile read. This is before buffer parsing or
native initialization: input/desired masks and all native/protocol/completion
stages remain unavailable. The observation does not identify the Work function
or prove its current guest ancestry. No neutral-work exemption is justified.

Root cancelled name entry, the device picker and Guide, positively captured the
dashboard without a modal, and sent normal WM close. The process returned 1 in
244.67 seconds, fully reaped with no forced signals. Both original identity
stop predicates first rejected nonzero `hooks` (evaluated mask 7, truth mask 3).
Later Task/context/modal predicates were not evaluated. This establishes an
in-flight-hook stop cut, not its duration, count or underlying cause. The
virtual controller was removed; qualified inputs stayed unchanged. Avatar
Editor remains blocked by the sign-in path, and no replacement image is
accepted.

V122 identifies that reader without changing any admission or return behavior.
The two-file metadata extension built in 45.80 seconds; the current installed
focus passed 65 cases and 11,545 assertions, including all three hidden fixtures.
The other five products are unchanged V121 products; V121's broader test runs
remain historical, rather than being reported as current V122 executions.

The native trial used spaced single inputs, settled transition captures and no
automatic screenshots. Guide, the HDD selector and OSK were visible. Creation
again returned zero with publication and discovery recorded, followed by a
visible native "Can't Sign In" dialog. Actual 20B/`0x24` returned `E_ABORT`
before native entry. Its retained reader is source-table role 42,
`0x816CD6E8`: the managed native frame iteration admitted at Query/Poll's
`0x816CD70C` return and ended at the authenticated `0x816CD920` boundary.
It owns a same-source read on the actual caller. The current SP is at or below
the recorded entry, within the same host-recorded stack bounds; this category
alone does not prove live ancestry. The next contract is the frame's nested UI
callback versus data/paint/read phases, preserving its complete frame, source,
listener and code lifetime. A generic `Latest` exemption remains unjustified.

The recovered native producer contract now supports a bounded two-phase
implementation. Initializer 224 deep-copies request/authentication buffers;
its prequeue writes change presence and saved online-XUID scalars rather than
freeing old scene, Account, tile or rendering resources. With a non-null
overlap, the proved flag families `0`, `4`, `0xC`, `0x20`, `0x24`, `0x2C`,
`0x120420` and `0x120424` queue native work and return the genuine native DWORD
without waiting. Either E148 or direct C838 can race before that return; both
need an actual prebody gate. E148 can itself queue C838 as a distinct Task, so
same-Task ancestry must never be assumed. Keep both old-source descendant
checks and exclude conflicting non-render owners before initializer entry.
The old render frame remains readable and retains its source through CD920;
the existing paint/source barriers govern later worker admission. This is a
source-backed contract, not an implemented or runtime-passed fix. Null-overlap,
remove-only `8`/`0x28`, low-bit `1`/`2`, optional `0x80` and other unproved
reentrant variants remain outside this new lane; existing non-reentrant
behavior is preserved.

After all dialogs and Guide were closed, normal WM close returned 1 in
284.73 seconds. Cleanup fully reaped the owned processes without signals.
Both original stop predicates again stopped at nonzero `hooks`; later
predicates were not reached. The separate hook audit finds RAII releases at
all twelve counted paths but no shared terminal admission seal. A momentary
nonzero count is therefore not proof of a leak; a zero sample is not proof of
sealed admission either. Proper rundown must allow required real returns.

The reported Guide slowdown remains open. A static audit confirms repeated
functional code authentication and polling, but existing counters do not
measure their CPU time, FPS or input latency. No performance cause or fix is
claimed. Runtime checks allow transitions to settle, keep capture overhead
low, and distinguish delayed display from input rejection or invisible output.
MangoHud and GameMode remain disabled. No replacement AppImage was produced.

V123 applied the reviewed one-file two-phase candidate and built normally in
42.93 seconds. The extended 81-case selection passed 13,166 assertions,
including three installed-resource fixtures and sixteen additional real
PM/read/overlap cases. Other test products stayed unchanged; this is supporting
coverage, not a native scheduling test.

The actual native trial created and discovered its fresh profile, but still
showed "Can't Sign In." Its selected render/read/caller/root/Task/stack proof
passed; the eligibility mask was `0xFFF` with only `0x400` failing. This is the
new blanket `other_owner` condition, which rejects another retained ordinary,
read, Commit, borrowed or suspended owner. No initializer, worker, protocol or
overlap-completion stage ran. The other Work's identity was not captured and
must not be inferred from older reports. The next change requires a shared
initializer-access classification: compatible retained readers versus genuine
cache/resource mutations or opaque execution, while preserving all later
frame/read/source/paint retirement obligations.

Before a normal WM close, the V123 supervisor received an external termination
signal at 237.91 seconds. The sender is unestablished. Its recorded trigger was
`explicit_signal`; memory remained above the floor. Owned cleanup left no live
or unverified processes, while `leader_reaped` stayed false and is preserved
exactly. No native shutdown diagnostics were recorded. This is an interrupted
run with no normal-close result; V122's separate hook-stop failure is not a
V123 outcome. The virtual controller was removed, and no release was created.

The controller regression sequence uses isolated state and no MangoHud:

1. Select Skip Sign-In, open Guide, wait for its animation, and capture it.
2. Close Guide and move the dashboard selection; reopen and capture Guide again.
3. If the unsigned path passes, create a fresh local profile and repeat
   capture/release after successful native sign-in.
4. Open Avatar Editor from the dashboard's profile/avatar menu and record the
   actual launch, refusal or exit before making a handoff change.
5. Close normally and verify the owned process exits without forced cleanup.

The trace must distinguish admitted/returned worker frames from queued hints,
orphaned consumption, old-source retirement and visible GPU publication. A
successful build or an empty orphan diagnostic alone cannot pass this sequence.

Required next checks are real dashboard and Guide visibility, controller capture
and release, signed-in profile preparation, normal shutdown, a running-game
Guide regression, and the Avatar Editor handoff. A replacement AppImage and
GitHub source update follow verified behavior. This does not close the HDD gate.

Existing source, index, releases, original profiles/saves/system content and the
20 GiB project-filesystem reserve remain preserved. Raw runtime logs, screenshots
and generated console signing material remain outside the public repository.
