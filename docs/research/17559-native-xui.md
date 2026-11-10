# Dashboard 17559 native XUI work

The current implementation direction is the
[native SYSTEM UI contract](17559-native-system-contracts.md): execute the
supplied native XAM task/UI manager through coherent host boundaries. The
diagnostics below remain evidence and reusable transport primitives; they do
not define the production UI lifecycle.

Guide rendering/XUI and shared keyboard work, begun 2026-11-03. These are
separate acceptance boundaries: native CPU objects, native GPU presentation,
and the named host keyboard fallback. None substitutes for the others.

## Guide foundation gate closed: tested local Linux AppImage

The Guide/XUI/shared-XAM foundation is **CLOSED for this cohort**, as far as
later HDD, Profile, avatar/XNA, Live and app-service prerequisites permit. The
user reports the requested copied-state AppImage checks passed: actual Rotastic
gameplay; native Guide tabs, close/reopen and game controls returning; Alt-Tab
away/back without stuck input; and closing with Guide open. This is the scope
of `manual-test/user-acceptance.json`, without invented automated frames, exact
held/release variants, IME, all-user or other-platform acceptance.

The [local release and normal launcher](/home/kyoki/.local/state/xenia-edge-research/binaries/gates/guide-17559-05f0-candidate/start-guide.sh)
use AppImage `85fbdd4b` / payload `9a2d84a2`, paired with unchanged05f0 source:
five suites **824 cases /37,686 assertions**, CPU witness and302-file archive.
The launcher retains the accepted isolated manual data, with no timed input,
auto-close or forced stdout/per-write flushing. Console assets/profiles stay
external. The payload requires **GLIBC_2.44**, tested on host glibc2.44; other
distributions, macOS and FUSE mode remain unverified.

Fresh GUI V2 (`fresh-gui-zjjlmbnj`) passes in4.079s after normally declining the
expected No Profiles Found modal, then showing the empty library. Runner and
container exit0 naturally, with no signals/faults, unchanged fresh seed and no
associated mount. Thirty artifacts are authenticated; all four observed process
paths are absent. Exact static extraction remains, not deleted/empty cleanup.
This frontend-only case creates no profile, launches no title and makes no
native Guide request. Fresh V1's modal-handling automation failure and the
earlier `ag8_02h3` invalid-command failure keep their original false gates.

Final upstream table `839e78dc` accounts for all123 compared paths; the22:57:43UTC
closeout refresh confirms unchanged074d3/head/release/asset. Included wait/qword/
corpus, equivalent, excluded and deferred cohorts have concrete reasons; the
independent fork is not wholesale synchronized. Final preservation checks10,182
user-confirmed-baseline originals with zero differences/failures. Later service
gates and explicit platform/text limitations remain open; shared foundation
regressions remain repair requirements. Earlier checkpoints below retain their
historical scope and failures.

## Historical accepted foundation: 05f0 NoSwap Host-to-native transition

`05f0788a` (`native-host-controller-modality-v1`) passes all five direct suites:
**824 cases / 37,686 assertions**, its same-ELF CPU witness and the checked
302-file source archive. The common host mapper selects controller navigation
before ImGui processes a fresh A press. The preserved baseline widget probe
fails three cases/eight assertions; the corrected mapper passes all 83 UI cases
and 861 assertions, including held-A text entry without double activation.
The earlier runtime did not log its navigation cursor state; that state is not
retrospectively asserted from its failure.

The unchanged integrated ordinary-Title fixture/actions now complete **9/9**.
Before any Swap, physical Guide opens the Host menu and B closes it. The Host
message accepts A with choice0; its lease releases before Host keyboard creation.
After Host keyboard request997, twelve genuine Swaps run; the first completed
real TitleInput (outcome7/fence1) arrives inside that same Host lease. The
existing Host request stays Host, accepts Start and returns guarded UTF16BE `keep`
plus NUL.
Its post/release precedes native route sealing and the next keyboard's997.
Native B then returns Cancel1627/extended1223 without changing its output; all
public completion results and guards pass. Owned images show Host keyboard,
Title stripes, native VK, then the same Title stripes after B.

`production-nblq8pbu` passes both capture gates in 47.378s with zero faults and
clean owned closure. Preservation matches 10,182 current-baseline originals.
Final native B selects Source273's complete immutable-input
bundle; the early Host-menu B does not select it. Source/image association is
not an owned-screenshot version match. The prior 74b blocked run remains below.
The independent nine-step audit and `verify-05f0-noswap-transition.json` retain
these results. This is bounded transition acceptance, not full Guide completion.
Manual focus recovery, broader lifecycle coverage, final upstream applicability
decisions and a tested gate AppImage remain pending. A
read-only21:24UTC upstream refresh still observes `074d3`; it does not close those
decisions or the Guide gate, which remains **OPEN**.

### Same-build Rotastic Guide and held-window close

A separate supervised `production-_ozry1rp` run at the same ELF passes bounded
native Home-to-Media navigation, B return to the Rotastic title screen, and a
second populated Home. Slot1 is actually adopted as native user1; both Guide
calls execute and return0, with native SYSUI visibility1/0/1. The final Guide
button stays down through normal owned WM_DELETE and verified owned-process
absence: the same live controller's 3,319-byte post-sink prefix is unchanged
before close and after absence, then release/close follows. Both capture and
held-close gates pass, with zero faults, no cleanup escalation and 168.561s
elapsed. This is bounded normal window exit, not proof of all C++ destructors
or every shutdown/relaunch path; screenshots are not matched to image versions.

Gameplay remains **untested at a sign-in setup prerequisite**, not a proven
profile-backend defect. Existing profile B is signed into guest0 while controller
guest1 is unassigned; the retained Slot1 prompt has no selected profile. The two
existing Account files still match historical same-byte two-slot evidence.
Normal UI can choose existing profile A for guest1 without creating a profile.
Next gameplay/Guide continuation belongs on the **candidate AppImage** using
isolated copied data and that ordinary selection, with no further native-ELF
retry. The independent held-close audit and the pinned existing-profile-readiness
contract retain this distinction. Latest preservation matches10,182 originals;
`verify-05f0-supervised-guide-close.json` verifies the build archive and run
artifacts; the separate source audit records three doc-only differences. Guide remains **OPEN**; no gate AppImage or
package/gameplay acceptance is claimed here.

## Historical observed boundary: 74b NoSwap integration remains blocked

`74b179cd` (`native-pre-swap-ui-fallback-v1`) passes all five direct suites:
**818 cases / 37,647 assertions**, its same-ELF CPU witness and the checked
301-file source archive. The preservation audit matches 10,182 originals
against the user-confirmed current settings baseline.

The integrated ordinary Title starts without a Swap. Physical Guide opens the
Host context menu and B closes it; Host lease1 releases before the Host message
request. Message lease2 releases before Host keyboard lease3 is reserved.
The first completed TitleInput publication (outcome7/fence1) occurs inside
lease3 after its keyboard request, so real GPU input arriving later does not
reselect that already-Host request.

The message does not complete after the scheduled A press; it advances after
the later B press. Start was already sent before Host keyboard creation, with
no later close press remaining. The run stops at the Host keyboard's public1FB
completion wait. No `xamgen.bin`, nine-step guard results, native keyboard phase,
SYSUI9, Source or B-selected export is produced. The capture gate passes, but
the diagnostic output gate fails; this is not a complete NoSwap transition pass.
The precise message A/focus/paint cause remains unresolved. The47.435s run has
no host/guest/startup faults and clean owned closure. Evidence: both independent
74b audits in `production-9vz070c7/` and `preservation-after-74b-noswap-blocked.json`.
The Guide gate stays **OPEN**, with no gate AppImage; earlier bounded passes below
retain their historical scope.

## Historical observed boundary: e702 held input and complete Source capture

`e7023c2f` passes all five direct suites: **808 cases / 37,521 assertions**,
the same-ELF CPU witness and frozen 296-file source verification. Two new bundle
fixture epoch failures are preserved in the v1 test record; the v2 correction
changes only those test fixtures. The recorded preservation audit passes
10,182 originals against the user-confirmed current settings baseline.

An eight-step ordinary-Title capture cancels native VK through physical B,
then reads fresh A without the still-held B. After the host observes B release,
a new B press reaches the later guarded Title read despite no intervening Title
poll. All eight results/canaries pass, including Cancel1627/1223, unchanged output,
immediate1167/neutral, later A0x1000 and new B0x2000; no faults and clean closure.

The B-selected Source273 export contains its complete primary, 25 native blocks,
231 copied shader programs and completed Title input: 708,356 bytes total.
Frozen validation checks coverage and correspondence without rereading guest RAM.
The prior direct-Swap-only eight-step fixture omitted the normal public
display/scaler notification: its first background input was NaN while the other
128 color/depth draws had finite FLOAT32 inputs. A nine-step derivative supplies
one public KRNL1B1 kind1 call after the unchanged twelve-Swap bootstrap. The
actual zero status and all80 guarded payload bytes are retained; all original
eight results/canaries still pass, including fresh A/packet5 and new B/packet8.
Source274's first64 vertex bytes are finite full-viewport XY/UV, matching the
real-title background, and its raw VK output contains the four Title stripes.
This solves that fixture prerequisite without a production C++ repair or private
native-cell writes. The31.442s capture has no faults and clean closure; actual
registered native readiness remains required, since status0 alone is insufficient.
The older ad99 clock overlap remains recorded; no general clock/composition fix
is claimed (`production-fjjuduc3/`).

A same-ELF Rotastic intro run has finite background geometry and a populated
Guide in the exact-source raw export. Owned images show intro continuation after
B; gray/partial shots occur only0.117s/0.364s after the two Guide actions and do
not establish persistent blank rendering. A populated second owned-window menu,
gameplay and fresh Start remain unobserved. Raw image association is not a
screenshot-version match. Evidence: the independent audits in
`production-1mreb7u9/` and `production-xbjzqsl1/`, plus
`verify-e702-held-B-and-bundle.json`.

A later same-ELF physical tab run observes populated Home, Media and Settings
in owned images and Games & Apps in its B-selected raw export. Right/Left presses
navigate these pages; no A/menu activation is sent. The retained clock changes
7:48→7:49 without overlap. Both capture gates pass with no faults/clean closure;
no new post-B intro image exists because all12 capture slots precede B. These
are bounded tab rendering/navigation observations, not menu-action acceptance
or a raw/screenshot-version association (`production-de2844lv/`).

The Guide gate remains **OPEN**, with no gate AppImage. Focus/modal/teardown,
other users/platforms/IME, no-initial-Swap fallback, broader game continuation,
HUD-owned nested keyboard and transient composition still require acceptance.

## Historical checkpoint: 3997 native text and bounded real-title Guide

`3997176e` (`native-desktop-text-v5`) passes all five direct suites:
**796 cases / 35,548 assertions**, its same-ELF CPU boot witness and the frozen
296-file source verification. NativeQuery's text-owner bootstrap now follows
busy retirement/capacity checks; the final frame-admission and native277 owner
checks remain mandatory. Matched `keepa`/Done/fresh-A captures log 723 native image
publications versus 67 on `1c35d496`. This is a render-progress/cost observation,
not FPS, a performance benchmark or identical-command replay.

The Linux GTK/X11 bridge attaches complete-commit observers to the actual IM
context after realization. The retained `96d8acb3` attempt had no native text
availability and sent no host key. Both `1c35` and `3997` subsequently return
exact guarded UTF16BE `keepa` plus NUL through actual scene-user1/class2 adoption
and SYSTEM native277 reads. The original six-step request997, success overlap0,
immediate Title1167/neutral and later fresh physical A remain distinct results.

A separate false-passthrough seven-step run holds host key38 across native Done
and owner removal. A read-only witness captures the complete 320-byte/done7
result while the key is still down: `keepaaa` plus NUL, intact guards/overlap0,
user1 fresh A/packet4 and successful user0/packet2 with zero buttons/triggers/axes.
Native group0/user0 and group1/user1 adoptions are observed; keyboard membership
in group0 follows the checked auto-binding source, not a forced assignment.
Another false-passthrough run sends paired `a`, Backspace, `b`, Return: guarded
`keepb` plus NUL and overlap0 complete before the only controller A press. No
controller Start/Guide/B fallback is supplied for Return.

Historical `1c35` Rotastic capture `production-k1iciy3p` shows a populated native
Guide over the animated intro and intro continuation after B. Two actual
SYSUI show/hide episodes and class2 ADD/REMOVE pairs are retained, but images06
and09 contain partial blank gray panels; a populated second-menu image is not
claimed. No gameplay, fresh Start response, storage selection or save is tested.

FocusNone was rejected by the window manager's auto-refocus. A separate owned
managed-Park attempt could not switch within its budget and sent no keys.
Neither establishes native focus-loss/recovery acceptance or an emulator bug;
a manual Alt-Tab/held-release/regain check remains outstanding. These are bounded owned-window XTEST observations, not
all keys/users/devices, layouts, IME or Windows/macOS/Wayland text acceptance.
Other focus/modal/teardown handoffs and fresh mapped input after key release
remain open. Unicode default preservation is a separate earlier result.

The Guide gate stays **OPEN**; no gate AppImage is produced. Transient native
composition/clock artifacts, no-initial-Swap/fallback, broader real-game
continuation and legitimate HUD-owned nested keyboard/restoration remain open.
Evidence is retained under `desktop-text-runs/`, the independent held7/edit
and Rotastic audits, and `verify-3997-native-physical-text.json`. The 10,182-file
preservation pass uses the user-confirmed current settings baseline and keeps
all older comparison history.

## Historical checkpoint: CBB Guide close then Title keyboard

`cbbefe1e` (`native-loader-upstream-wait-qword-v1`) passes all five direct
suites: **777 cases / 34,954 assertions**. The same-plan CPU witness passes in
5.548 seconds. The checked native loader1AB/4A7 family and upstream
wait/timeout/qword changes are built; pre-refresh source verification passes.

An ordinary Title keyboard request made while Guide remains active returns5,
matching the supplied native admission guard before VK dispatch. This is an
expected refusal, not missing scheduling or a passing nested keyboard switch.
After physical B genuinely closes Guide, `production-u2klqpzs` passes the
separate ordinary-Title flow: native SYS_UI9 hide precedes Title2C1/997,
VK displays `keep`, Done returns UTF16BE `keep` plus NUL with intact unused
bytes/canaries and overlap0, then fresh Title A reaches GetState0/packet8/
buttons0x1000. Owned captures show Guide04, VK08 and exact initial Title pixels
again10; the50.874-second capture has no host/guest/startup faults and clean
owned closure. This is **Guide → close → Title keyboard → Title**, not
HUD-owned nested keyboard or HUD restoration.

The upstream author-recorded17559 Xapi corpus matches all48 semantic steps
(936 output bytes), including exact unnormalized finite421 return pointers and
INFINITE no-write. Its harness nevertheless times out and uses SIG2/15/9,
leader exit-9; semantic agreement does not establish graceful termination.
The latest upstream snapshot review is not gate closure or a release.

The same build also preserves the Unicode default string `日本🙂` through
native Done in `production-w_wplqd7`: all six steps, exact UTF16BE plus NUL,
untouched canaries, success overlap and fresh Title A (packet4/buttons0x1000).
The31.548-second capture has no faults and clean owned closure. The screenshot
shows `日本`; smiley glyph rendering is not established. This tests default
string preservation, separately from physical typing or IME. Its independent
audit and `guide-native-unicode-default-cbbefe1e.json` retain that scope.

The Guide gate stays **OPEN** under the [gate-close policy](17559-gate-releases.md);
no gate AppImage is produced. Real games, desktop committed text/class2,
no-initial-Swap/fallback admission, transient composition/clock artifacts,
held teardown and any legitimate HUD-owned nested keyboard path remain open.
Evidence: `evidence/native-runtime/guide-native-keyboard-cbbefe1e.json`, the
independent u2klqpzs audit and frozen verifier. Preservation passes10,182 files
against the user-confirmed current settings snapshot; the historical three
settings/library-metadata differences remain preserved, not reported as an
unchanged old baseline.

## Historical checkpoint: 8a0a05cb native keyboard accept/cancel

The five direct suites pass **774 cases / 33,280 assertions**; the same-plan CPU
witness `first-record-i7cf74js` passes in 5.335 seconds. Genuine native Title-heap
initialization runs before public UI admission. The installed `vk.xex` now
renders original-TITLE user1 requests; the fixture's inherited “host fallback”
description is literal text, not the execution route.

Three standalone runs pass their bounded native result contracts:

* `2cx4qgcg`: Start/Key.OK returns UTF16BE `keep` plus NUL, preserves unused
  output bytes/canaries and completes the original overlap with success0 after997.
* `ml_g4vq0`: X/A/Start returns `kee1` plus NUL with the same bounds protection.
  A fresh physical Title A after completion reaches native GetState0,
  packet8/buttons0x1000.
* `5bk0q5kv`: B returns typed Cancel result1627 / extended1223 / length0,
  leaves all guarded output untouched, then a fresh Title A reaches
  GetState0/packet4/buttons0x1000. Later captures restore the exact Title pixels.

Immediate GetState1167 with neutral output occurs before native hide; later
fresh input succeeds. Overlap completion is not an input-restoration barrier.
The unchanged legacy checker's both-status-zero condition remains false.
These fixtures perform genuine initial Title swaps and then idle; they do not
prove UI operation without any initial Swap or ordinary-game continuation.

The shared native SYSTEM `lstrcpynW`460 repair closes the actual missing import.
The preceding9f2 `qlyyd_mz` failure remains intact: success overlap with every
output byte stillCD. The preceding9f2 Guide regression `p0iia0jg` also remains
valid within its scope: all four tabs, Settings focus and Dashboard return,
with the earlier048 float-cache geometry correction preserved.

Full Guide/OSK remains incomplete. Guide-first `y_2ps7yi` faults before Main
step0 or any VK request: native UI thread01000104, PC81787760/LR81784E30;
the fault log reports READ0x100000000. No Start/B was attempted. The native
PNG/resource path is under investigation; no HUD→VK→HUD pass follows.
Transient nested rectangles during Cancel's B hold and composition/clock
artifacts remain unresolved, rather than accepted as normal animation.
No-initial-Swap, ordinary games, held teardown and fallback regression remain
separate gates, ahead of HDD/profile work.

Evidence under `evidence/native-runtime`: the same-ELF test/CPU records;
`osk-uiactive-runs/native-osk-standalone-user1-swap-2cx4qgcg/independent-8a0-native-done-audit.json`
and `osk-uiactive-runs/native-osk-standalone-user1-swap-ml_g4vq0/independent-8a0-edit-and-cancel-audit.json`;
the failed Guide-first result; and `verify-8a0a05cb-native-string-done.json`.
The three standalone captures have no host/guest/startup faults and complete
owned closure. The full original audit matches10,182 files with zero differences
or failures; artifact verification passes while preserving the failed Guide-first
capture and all earlier failure records.

## Historical checkpoint: 66af0d78

The native manager first showed bounded Guide open/B-dismiss/reopen/Dashboard
return with760 cases /32,234 assertions passing. Its own trace proves native
Source submission/restoration, producer completion, immutable image publication,
retirement and separate UI consumer completion. That checkpoint's overlapping
right tabs/footer and gray strip were real failures. No event from it supplies
a fence, command identity or pixel result for a later run. Its immutable report
is `evidence/native-runtime/guide-native-rendering-66af0d78.json`.

## Historical checkpoint: c0f5d180

`c0f5d180` (`native-title-input-and-pixel-flow-v1`) has accepted coverage of
742 cases / 30,832 assertions, including21 new cases. Four initial suites pass;
the initial net failure remains failed. Its exact failed case and one full net
rerun pass on the unchanged binary. This is separate retry evidence, not an
uninterrupted five-suite pass. The unchanged923 plan's CPU witness passes in
5.574 seconds with the genuine10,408-byte record.

Production now observes actual fenced Title framebuffer copies: sampled
`title_input_capture` outcome7/copy-fence-completed1 records carry the same
backend2/foreground1/Title module generation,1280x720 tiled input and3,768,320
raw storage bytes. These are Title GPU input captures, not Guide GPU work.
Actual binding/user1 native506/raw0 and native SYS_UI9 visible1 also repeat.

The complete packet profile, all3 shader analyses, both draw proofs, primary
comparison, input coverage and snapshot Seal pass. Request preparation then
refuses the visible-extent contract: the exact sealed resolve rectangle decodes
to856x480, while the native Set descriptor declares852x480. The failure precedes
Session request minting, final resource validation and native Guide GPU execution.
No execution request, Guide presentation or pixels are accepted. The next step is the real
logical-visible versus aligned-resolve contract, preserving immutable-byte and
ownership checks.

Production ends7.469s with `native_transport_rejected`; later input protocol
failure is secondary. Both inspected captures are black/near-black startup at
2.996/4.250s, before the6.277s Guide action; no post-notification image exists.
Both runs have no host/guest faults, complete owned closure and unchanged seeds.
The audit matches all10,182 originals with zero differences/failures. Evidence:
`evidence/native-runtime/guide-title-input-c0f5d180.json`, actual run/test records,
`verify-c0f5d180-title-input-and-pixel-flow.json` and
`preservation-title-input-c0f5d180-after.json`. The earlier c46 owned-byte witness
and d5 shader refusal remain historical evidence; Guide navigation, close/reopen,
focus, HUD/VK switching and keyboard acceptance remain outstanding.

## Historical checkpoint: d5b17738

`d5b17738` (`native-terminal-window-state-v1`) passes all five direct suites:
721 cases / 29,705 assertions. The unchanged923 plan's separate CPU witness
passes in5.757 seconds with the genuine10,408-byte record. Actual controller
binding, user1 native506/raw0 and native SYS_UI9 visible1 repeat in production.

Complete packet-profile validation now passes:62 packets,3 shader loads and
no packet error. Source FREEZE, record/output pins and output writability pass.
Reaching shader analysis also establishes, from the matching built control flow,
that BeginInputCapture, record capture and the primary read/equality comparison
passed. This is an initial captured byte view, not a complete sealed snapshot.

The first shader load fails with `shader_metadata_incomplete`:
`unsupported_control_flow` at shader DWORD3. No Seal, final request admission,
GPU submission or presentation is reached. Production rejects after7.962 seconds;
later input protocol reason1 is secondary. The next boundary is the actual
shader control-flow/metadata contract using exact owned bytes, retaining strict
validation rather than inferring readiness from packet acceptance.

Both owned images were inspected: black at3.045s and a dark startup gradient at
4.728s, before the6.815s Guide action. There is no post-notification image or
Guide/navigation/keyboard acceptance. Both runs have no host/guest faults,
complete owned closure and unchanged copied seeds. The full audit checks10,182
originals with zero differences/failures. Evidence is
`evidence/native-runtime/guide-shader-profile-d5b17738.json`, actual run artifacts,
`verify-d5b17738-terminal-window.json` and
`preservation-terminal-window-d5b17738-after.json`. Earlier34/5d failures remain
unchanged historical evidence.

## Historical checkpoint: 34dcc354

`34dcc354` (`native-frame-prepare-diagnostics-v1`) passes all five direct suites:
717 cases / 29,125 assertions. The unchanged923 plan's CPU witness passes in
5.145 seconds. Actual controller binding, user1 native506/raw0 and native SYS_UI9
visible1 are observed again in the separate physical production capture.

The first Set now identifies its exact preparation boundary:
`complete_packet_profile_refused`, unsupported register8320 (`0x2080`) at DWORD
offset2588. Source FREEZE and early record/output pin acquisition/output
writability pass. The reported `primary_matches=true` short-circuits in immutable
mode; live RAM recapture/byte comparison, input snapshot capture and Seal have
not been reached. Partial zero packet/shader/draw counters are not proof that
these commands or resources are absent. No execution request is admitted and no
GPU submission or presentation is observed.

Production ends after7.984 seconds with `native_transport_rejected`. Both owned
images precede the Guide action and show startup only; no post-notification
pixels or Guide/navigation/keyboard acceptance follow. Later input protocol
reason1 is secondary to the packet-profile failure. The next step is a verified
register/backend contract for the actual0x2080 write, retaining strict parsing.

Evidence: `evidence/native-runtime/guide-packet-profile-34dcc354.json`,
`verify-34dcc354-frame-preparation.json`, the actual CPU/production results and
`preservation-frame-prepare-34dcc354-after.json`. Both captures have no host or
guest faults, unchanged copied seeds and complete owned cleanup. All10,182
recorded originals match. Earlier5d/ad13/C3 results remain separate history.

## Historical checkpoint: 5d742233

`5d742233` (`native-input-bootstrap-import-v2`) passes all five direct suites:
717 cases / 29,125 assertions. The 14 new cases exercise real Memory input data
and the actual InputSystem/WindowedAppContext queue, including whole-callback
rundown, local strong-owner release and self/ancestor drain refusal. Native
bootstrap uses the verified original controller registrations; no device/user
or native Task state is fabricated. The intermediate `ad13cab2` CPU failure at
functions0/5 is preserved: the host validator compared `xboxkrnl.exe` against the
loader's canonical `xboxkrnl`. Correcting that lookup leaves the plan unchanged.

The same `923ec6d7` CPU witness passes in 5.136 seconds and captures the genuine
10,408-byte record. Production then executes actual user-binding callbacks:
host slots0/1 return assigned native users0/1 with raw0 and adoption observed;
the initial host-slot masks settle at3/3. A physical Guide edge on slot1 carries
input cookie3, executes native506 for actual user1 and returns raw0. The native
SYS_UI9 delivery reports visible1. These are native policy/transport observations,
not evidence of Guide pixels or input focus.

The first production Set reaches the renderer with 2,602 dwords and a852x480,
format6 descriptor, but request preparation returns null at stage23/reason24.
The capture ends after7.773 seconds with `native_transport_rejected`; no admitted
Source, GPU submission or presentation is established. Its exact payload was
not captured, so the separate CPU witness is a shape lead, not byte-for-byte
production proof. The later input-protocol reason1 occurs after the Set
rejection/cleanup and is secondary. Bounded preparation telemetry is the next
step; no internal snapshot/layout cause is yet assigned.

Only two screenshots were taken, both startup images before the Guide press.
There is no post-notification capture and no Guide pixel/navigation/keyboard
acceptance. Both runs have no host/guest faults, unchanged copied seeds and
complete owned closure. The full audit checks10,182 originals with zero
differences/failures. Current evidence is
`evidence/native-runtime/guide-input-binding-5d742233.json`,
`verify-5d742233-input-rundown.json`, the two actual run results and
`preservation-input-5d742233-after.json`. Earlier checkpoints retain their scopes.

## Historical checkpoint: c3e26f90

`c3e26f90` (`native-ui-pipeline-trace-v1`) passes all five direct executables:
703 cases / 28,870 assertions. The full `923ec6d7` plan has 31 calls and now
includes the native UIActive query. The selective CPU diagnostic passes again
in 5.348 seconds: all native phases and pre-request readiness `0x7FF`, original
SYSTEM Guide304 raw0, and the genuine 2,602-dword / 10,408-byte record. Production
and tracing are disabled in this diagnostic; no Source/GPU/ACK/pixels follow.

The same-ELF production capture reaches Running and ends cleanly after 36.166
seconds. All 100 trace envelopes match their actual log bytes and line numbers,
with no malformed lines or helper cap drops. Real host frontend samples observe
two Guide edges on source slot1, with raw and filtered buttons `0x400`. Both
reach native-input capture and reject with source-defined reason4: missing
device. Only callback kind2, SYSTEM request `816F09E8`, is recorded; kinds0/1
(user binding/device configuration), binding output and low-level read-state
records are absent. No native506, SYS_UI9, Source/GPU or presentation event is
observed. The five-argument binding callback argument map is already present; the first
concrete gap is native controller registration/device binding, not an inferred
argument-map repair. Native assigned user remains unknown.

Original TITLE public-call samples return native `0x2EC=0`, `0x191=1167` and
`0x198=4306`. The inactive UI sample and failed/empty input polls are recorded
results, not Guide acceptance. The transport's `prepared_ready` checks pass.
All 12 owned-window captures were inspected: black/startup gradient followed by
Dashboard Home/Open Tray in images02–11, with no visible Guide. Post-sink
controller emission and host frontend receipt are now observed separately from
the rejected native capture. Navigation, close/reopen and native keyboard
acceptance remain outstanding; the genuine production Source/GPU path remains
unverified.

Evidence: `evidence/native-runtime/guide-pipeline-trace-c3e26f90.json`,
`tests-native-ui-pipeline-trace-v1/results.json`,
`first-record-uiactive-runs/first-record-c3m45gci/result.json`, and
`production-uiactive-runs/production-e8fj4z17/result.json`. Both runs have no
host/guest faults, complete owned cleanup and unchanged copied seeds. The full
`preservation-pipeline-c3e26f90-after.json` audit checks all 10,182 originals with
zero differences/failures. These results retain the earlier checkpoints below.

## Historical checkpoint: 0b955c56

The earlier compiled Linux integration is `0b955c56`
(`native-shared-device-output-v1`). All five direct executables pass 702 cases /
28,714 assertions, each with a dedicated Catch output file. The earlier
`b897cf2d` single Home control passes actual GPU
execution, readback and independent-plane presentation; its window was inspected.
The `df1219b5` three-frame/two-relaunch evidence remains preserved separately.
These are manual Home component results, with a transparent background and
an overlapping footer. They do not establish the full native Guide lifecycle.

Actual selective native startup now executes the heap/CRT, all eight native
SYSTEM Task workers and TLS roots, shared graphics, configuration, input
auxiliary initialization, and the update predicate on a real native Task. The
native notification initializer also passes its actual readback contract, in
the original configuration → notifications → input-auxiliary order.
The full startup plan is checked by the unchanged production C++ parser against
the supplied image and real XEX section/export metadata before launch. The
native DLL-entry self-module setter runs with the actual retained root HMODULE;
its guest cell is checked afterward. No native Task/manager fields are invented.

The native CPU startup plan now passes every phase: `1,2,3,8,4,5,6,7`. Actual
readbacks validate distinct XAM/HUD ownership, all FE fields and original
constructor code, and the manager's created/active native Task handles. The
scheduled task genuinely returns `8000000A` (`E_PENDING`); the former host
`<=1` check rejected that normal state. POSIX event Query now observes real state
without consuming a signal, with two real base regressions.

The first genuine native manager command record is captured again in the
same-plan `first-record-6avw4_vg` diagnostic, which passes in 5.773 seconds.
Actual readiness reaches all `0x7FF` checks before the
single original SYSTEM Guide304 request: flags `1,1,0`, signaled manual event,
owned shared graph, and native Task statuses `1,0`. Guide304 executes and returns
raw `0`. The capture contains 2,602 dwords / 10,408 bytes from the actual renderer
Task, with command/output ownership observed. Hook counters report one accepted
nonempty Set and no rejections. Post-request `0x3FF` snapshots deliberately omit
Task status calls; bit10 is unsampled, not a readiness failure.

The captured PM4 contains a color/depth draw and copy; shader/content is unknown.
This is CPU command evidence, with no Source freeze, GPU submission, retirement
or ACK. Observation after Guide304 does not prove native production timing or
Guide causality, and the record does not identify glyphs or visible Guide content.
The descriptor is 852x480. The new layout validator keeps actual source and
destination pitches separate and requires the sealed rectangle to match the
visible extent. Five new real Memory/Session/snapshot regressions pass; this
native record has not yet exercised the production Source or GPU path.

The same-ELF production run `production-ct7n5x2g` now passes startup and reaches
foreground Running. Both diagnostic and production runs publish the actual
native LightDevice through an owned kernel-global binding: the import targets
the same kernel cell, its recorded value starts at zero, one native AddRef
returns 2, and publication serial1 leaves the cell equal to the actual device.
The late shared DC joins that same staged binding. The actual pre-resume
`source_frame_ready`/transport/receiver checks pass; this name does not prove a
frozen Source, GPU execution or pixels. The earlier `00722aee` kernel-global
guard failure remains a separate historical result.

The production capture runs for 37.151 seconds, with complete owned closure and
no guest/host-fiber faults. Five helper commands include two Guide and two B
pulses; the controller records actual post-sink button-down/up writes. Native
assigned user, device slot and 506 receipt remain unknown. All 12 owned-window
captures were checked: black and near-black startup images precede Dashboard
Home/Open Tray in images02–11, with no visible Guide at those capture instants.
A clean capture and physical emission do not establish native input delivery,
SYS_UI visibility or Guide acceptance. The next observation must locate the
first missing input → native request → visibility → recording → Source/GPU/
presentation boundary without inferring it from Dashboard images.

All 135 previously observed native HUD import targets match the supplied XAM exports, including every imported XUI,
Task, messaging, allocator and notification boundary; Dump's global HLE tags do
not describe those native bindings.

This work also fixed a host null dereference in scoped `XexUnloadImage(0)` cleanup.
A negative control now reports the native startup failure without a host or guest
crash. Error propagation retains the actual function and raw return value. The
compiled immutable-input snapshot/private GPU bank is not yet validated against
this genuine native record. Production transport, native SYS_UI visibility,
Guide opening and input changes are compiled; the production Guide remains
unshown in the recorded physical-input run. Production opt-in is disabled in
the passing first-record diagnostic and enabled in the separate production run.

Native navigation, focus, close/reopen, the installed `vk.xex` keyboard, deliberate
title stalls and service teardown remain unverified. Normal native Guide
admission stays closed. HDD and profile creation remain after Guide/XUI.

The narrow change since `7d95866f` replaces the constant-`FD` `XeCryptRandom`
stub with secure operating-system entropy. Bounded analysis of the supplied
native sampler and immutable bound proves a concrete infinite-rejection path
under that stub; the previous sampled LR alone did not prove its live operands.
The new four-case regression exercises registered zero-length/void ABI, exact
output bounds, and a real physical-memory write watch. Entropy is generated in
host scratch and copied through normal guest writes. The startup plan remains
byte-identical, separating this shared kernel repair from later UI changes.

The subsequent `ObGetWaitableObject` repair now has actual native evidence:
body `300D5018`, factory `81A821E0`, metadata `0x0C`, returned event `300D5024`,
worker `0000000A`, caller LR `816B7974`. The prior `0x81` byte was the outer
object's vtable prefix, not an event encoding. All 610,887 unsupported-type
warnings disappear with the real borrowed conversion; event decoding remains
unchanged. Six new tests cover real-memory resolution and the registered null
ABI. Native absolute/default-pointer compatibility limits remain documented in
the implementation evidence.

Evidence for the `0b955c56` checkpoint is under `evidence/native-runtime`:
`guide-shared-device-output-0b955c56.json`,
`tests-native-shared-device-output-v1/results.json`,
`first-record-runs/first-record-6avw4_vg/result.json`, and
`production-runs/production-ct7n5x2g/result.json`. The full post-run audit,
`preservation-shared-device-0b955c56-after.json`, checks all 10,182 recorded
originals without differences. The independent physical/image review is under
`native-guide/system-contracts/native-production-physical-review-20261105T040251Z`.
Earlier `00722aee` CPU success, production failure, UI-only parser recheck and
superseded PM4 interpretation remain preserved. The manual Home control remains
`guide-manager-core-b897cf2d.json`.

At this earlier checkpoint, the UIActive query and trace flag were unbuilt,
and the 31-call helper chain was unrun. They are outside the accepted
`0b955c56` runtime identity; the later C3 results are recorded above.

## Graphics ownership foundation

- [GraphicsInterruptRegistry](../../src/xenia/gpu/graphics_interrupt_registry.h)
  stores callback, guest data, registration process, module pin and monotonic
  generation together. A short mutex protects publication and lookup; no
  registry/admission lock is held while guest code executes.
- The actual `VdSetGraphicsInterruptCallback` caller supplies its process type.
  [KernelState::RegisterGraphicsInterruptCallback](../../src/xenia/kernel/kernel_state.cc)
  checks the loaded callback module and its unload claim under the kernel's
  existing global critical region before publication. Registration process is
  metadata; it does not identify the owner of an existing GPU submission.
- Replacing or clearing one process registration retires its old generation
  without deleting another process's record. The separate legacy view still
  reflects the most recent registration call, including clear. An explicit
  owner token is available only for a known process with a code lifetime pin.
  Dispatch checks the requested process, generation, current owner slot and
  admission state. An old token cannot call a replacement.
- Module shutdown first stops new graphics callback admission. Nonblocking
  self/fiber checks precede waits in other callback registries; the loader
  defers and retries when it cannot drain safely. Admitted calls finish before
  destructive module detach. Retained snapshots pin code until discarded.
  This protects callback instructions, not arbitrary guest device or
  `user_data` memory; actual device unregister/destruction ordering remains a
  separate prerequisite.
- [DPCImpersonationScope](../../src/xenia/kernel/kernel_state.h) keeps the
  selected process and CPU through the callback and deferred DPC drain, then
  restores the previous PCR process field, CPU, IRQL and DPC-active state.
  The explicit route uses its verified process. The existing legacy route
  retains its prior TITLE callback policy; registration metadata does not
  silently change that policy.
- [GraphicsSystem](../../src/xenia/gpu/graphics_system.cc) still saves only
  callback and data: the original two-DWORD layout. Restore invalidates old
  tokens and creates an unclassified legacy registration. Its code pin is
  rebound after module objects are restored. Per-owner registrations and
  generation identities are not reconstructed from this old state format.

## Legacy compatibility and validation limits

Existing vblank and PM4 producers use the legacy route. Held PM4 interrupts
retain their existing submission/completion ordering and resolve the latest
legacy registration at delivery. They do not capture a guessed owner or cancel
solely because a registration generation changed. The PM4 CPU mask remains a
delivery mask, not an ownership tag. See
[held signal handling](../../src/xenia/gpu/command_processor.cc).

With the guest scheduler enabled, each source-0 dispatch enters background
scheduling once, including when its callback is absent or retired. No source-0
fan-out or source-1 broadcast was added. The boot-animation graphics route
remains unclassified. Keeping its legacy route does not prove that it is a
title-owned renderer.

[Graphics interrupt tests](../../src/xenia/kernel/testing/graphics_interrupt_test.cc)
cover synchronized callback/data pairs, owner isolation, delayed explicit-token
retirement, legacy latest-at-delivery behavior, module pin/drain and self-unload,
state-layout compatibility, and the production DPC context helper. These are
host-side contract tests; they do not demonstrate actual guest GPU submissions,
native scene pixels, or title/boot compatibility after this change. Build and
runtime results must be recorded separately.

## Boundary required before native pixels

1. Connect the real system-command-buffer descriptor and GPU identifier
   publication/consumption contract across guest video calls and title swaps.
   The publication boundary must establish submission provenance before it can
   select an explicit owner token. See
   [video exports](../../src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc).
2. Carry that verified owner and registration generation through submission,
   GPU completion and retirement. Preserve visibility of preceding GPU writes
   before interrupts. Define safe retirement when a device or module closes;
   neither a CPU mask nor a nonzero guest device pointer supplies this policy.
3. Preserve title and boot ring/read-pointer state while admitting system
   work. A second registration record does not provide command-buffer
   multiplexing. Reinitializing the primary ring to start a system renderer
   would discard existing title work.
4. Initialize the actual guest renderer/device/DC path and connect its output
   to presentation with defined composition, resource lifetime and completion
   synchronization. Host Vulkan/D3D presentation objects are not guest D3D
   device objects. A CPU-only XUI object lifecycle or host keyboard fallback
   does not establish this boundary.
5. Validate a real native scene's create/update/draw/present/close cycle above
   a running title, then boot-animation/dashboard handoff and ordinary title
   presentation. Include unload/reload and delayed-completion cases to detect
   stale callbacks, lost GPU work and cross-device acknowledgements.

No routing choice should be inferred from factory flags, the host worker's
process, a guessed identifier bit, or the mere presence of a system device.

## Native engine diagnostic

The default-off `native_xui_probe` runs on the existing joined SYSTEM diagnostic
thread. It requires the separately loaded installed XAM image. It resolves and
validates actual image exports for Init, class lookup, object creation, handle
resolution, destruction and Uninit. No image-version function address is in
production code. The probe uses the HUD's default 12-byte Init parameters and
the built-in `XuiElement` class, without invoking XAM DllMain, starting a device,
or resetting the primary ring. A preexisting engine remains owned by its caller.

XUI allocation branches directly inside XAM to its native allocator, whose heap
descriptors and static locks require native startup. Resource loading and the
legacy `is_attached()` compatibility flag do not establish this state. The
ordinary HLE allocation exports do not intercept intra-image direct calls.

The isolated Linux run `20261103T231301Z-native-xui-object-okdqudl1` resolved
the six native exports, but Init did not return before the 22-second capture
closed the application. The dashboard continued presenting. A focused debugger
repeat, `20261103T231811Z-native-xui-gdb-focused-otk3mvy8`, observed the diagnostic
thread in `RtlEnterCriticalSection` waiting through `KeWaitForSingleObject` with
no timeout. The class-registry lock had no owner and a waiter, consistent with
the missing native static constructor. Static analysis identifies the class
registry and other XUI locks as CRT-constructor responsibilities, not work
performed by XuiInit. The suspected heap allocation failure was **not** observed
as a returned HRESULT; its unwind path is a static lead.

That initial blocker was resolved in a later isolated gate by executing an
explicit, image-validated native heap/CRT startup plan on the SYSTEM thread.
The owned runtime records module identity, generation, immutable call order and
ABI contracts. Failed partial initialization poisons that generation; it is not
retried against already changed native state. Its module remains resident until
kernel teardown. A plan is required: the default empty configuration refuses an
uninitialized native call. No constructor list is inferred from addresses or
from the attachment flag, and no host write fabricates private lock/heap state.

The Linux run `20261104T002641Z-native-runtime-shared-dc-rqtaxmiq`, using ELF
SHA-256 `38c53618b0c16e670e03519146f2d5df15e21a90700ebd1db0b042f1d149280c`,
completed the actual native `XuiInit`, class lookup, `XuiElement` creation,
handle-to-object query, destruction and `XuiUninit`. Init, create, query,
destroy and uninit returned HRESULT 0, with nonzero native handle/object outputs.
The title executable was preserved. This supersedes the earlier CPU-object
blocker; it does not establish a native HUD scene.

The same run separately created an actual native light D3D object, called
`XuiRenderInitShared`, created a native DC and queried its real 640-by-480
backbuffer dimensions. Each operation returned HRESULT 0 and nonzero object
outputs where required. Native DestroyDC, RenderUninit and device Release also
returned 0. The stage did not call Begin, record commands, publish work, draw a
scene or present native pixels. A shared Present return alone would not prove
composition either.

A later Linux run,
`20261104T012027Z-native-runtime-primitive-record-37788b-6jyqa2nh`, using ELF
SHA-256 `37788b7bd217fa2763632a32259bd49b8c9d6252d23fab156b02ca573e8f863b`,
passed the actual native primitive-recording gate. The dashboard was already
presenting at frame 5; the probe returned at frame 6 with the title executable,
ring and title callback preserved. It owned another real native XuiInit cycle,
created a solid brush, began rendering, selected that brush, called native
XuiFillRect and ended rendering into a fresh native recording resource. The
recording buffer was `FEAF4000`; its actual physical PM4 stream at `1EAF5000`
contained 2,528 DWORDs, 33 bounded packets and one draw packet. Recording End
and the checked native cleanup calls returned 0, including resource/brush/DC
release, XuiUninit, RenderUninit and device Release. The record was never
published or submitted, and no color render target was supplied. This accepts
native CPU draw recording, without accepting GPU drawing, a HUD scene or pixels.

The same run's strict GPU-completion probe reported Complete for epoch 1,
submission 9, with one completion notification. That marker covered preceding
title GPU work only; it did not execute or retire the native UI recording.
Additional RT/texture/resolve helpers and their stack/floating-point ABI remain
a separate, unbuilt change outside this ELF's acceptance. Actual native pixels
still require a real target, resolve, title state/EDRAM preservation, composition
and completion tied to the submitted native work.

These probes remain development-only diagnostics with an external deadline.
An unreturned guest call cannot currently be canceled and joined safely, so
normal interactive guide acceptance leaves their flags off. The application's
existing `Cheap-skate exit` uses quick_exit: exit status 0 from window close is
not proof of joined teardown. Gate acceptance above comes from the actual
returned native calls and checked outputs, rather than process exit.

The native string-table representation differs from the current HLE
representation. A native export provider must keep XUI objects, properties,
strings, string tables, allocation and rendering within the same native module
generation. In particular, LoadStringTable, LookupStringTable and FreeStringTable
cannot exchange handles with the HLE table implementation. The explicit import
provider supports preflight of all claimed consumer imports; its loader must
complete this before binding. A missing claimed native function fails admission
instead of selecting HLE for that item. Provider selection belongs to an
explicitly loaded SYSTEM consumer, without a global title-import switch.

HUD lifecycle message `80000008` borrows a live native DC, registers controls and
HUD classes, creates a `XuiCanvas` root at the DC's actual size, and builds its
resource locator for section `hud`. It loads `strings.xus`, then normally creates
`GuideMain.xur` and navigates that scene beneath the root. The data-center branch
selects `GuideMainServer.xur`. Native shared/XAM/gamercard resource locators also
depend on native XAM's own loader HMODULE, distinct from HUD's registered
resource slot and the host HLE module handle. Establish that identity through a
verified native initialization call before resource consumers. Retain the DC,
engine, provider and module through scene shutdown and any GPU completion.

Actual HUD resource/class loading, theme/font setup, update/input/draw scheduling,
HUD-specific recording, publication, GPU retirement and composition remain
separate acceptance gates. The passed CPU-object, light-device/DC and primitive
recording results do not establish native-guide or native-pixel acceptance.

## Shared host keyboard

[ImGuiTextEntryDialog](../../src/xenia/ui/imgui_text_entry_dialog.cc) provides
controller letters, digits, punctuation, shift/symbols, space/delete, and
Done/Cancel, alongside physical text editing. Every connected user slot is
considered. Held opening/closing input is consumed, directions repeat, and
cancellation takes precedence over simultaneous acceptance. Input ownership is
checked against focus and other modal dialogs. This is explicitly a host
fallback; the installed `vk.xex` native scene is not enabled by it.

[TextEntry](../../src/xenia/ui/text_entry.cc) accounts for UTF-16 code units,
keeps surrogate pairs intact, normalizes defaults, and enforces single-line
entry. The guest capacity includes NUL. Cancellation never copies text. A
zero-capacity request does not translate or touch its output; its host-only
editing budget is 256 UTF-16 units, rather than a claimed native limit.

[XamShowKeyboardUI](../../src/xenia/kernel/xam/xam_keyboard.cc) now uses owned
request/result state, UI-thread dialog construction, a shared completion fence,
and exactly-once UI reservation release. During-frame completion is deferred;
if shutdown rejects that post, only a non-UI abort sink may wake the waiter.
No caller stack is retained by an asynchronous completion.

The host input backend preserves physical keys while clearing idle/disconnected
gamepad keys. Modifier snapshots enter ImGui's event queue before their named
key, so NewFrame cannot overwrite direct legacy modifier assignments. The wx
backend binds the system Unicode clipboard on the UI thread; headless contexts
retain ImGui's defaults. Clipboard contents are never logged. Platform runtime
coverage is recorded separately below.

The supplied 17559 wrapper/VK analysis establishes these result fields, which
are checked separately from any claimed console capture:

| Path | Outer accepted return | Overlapped result / extended / length |
| --- | --- | --- |
| Accepted text | 997 | 0 / 0 / 0 |
| User cancel | 997 | 1627 / 1223 / 0 |
| External abort | 997 after admission | 1223 / 1223 / 0 |
| NULL overlap | waits internally, then 997 | no caller overlap |

Unknown style/validation flags and locale-specific native layouts remain
unimplemented; this phase covers ordinary single-line entry. Physical Unicode
text storage is distinct from font glyph coverage. See the
[controller checklist](17559-controller-checklist.md) and portable interactive
[guest corpus](../../tools/xamgen/corpus/keyboard.py); no new console golden
results are asserted by that corpus.

## Linux validation and artifacts

| Check | Recorded result |
| --- | --- |
| Earlier host keyboard/fallback release: direct base/network/kernel/VFS/UI executables | 344 cases / 16,427 assertions passed |
| Foreground-state/bridge build 61c73361: five direct test executables | 439 cases / 18,894 assertions passed; all five executables passed |
| SYSTEM graphics ownership/target build 31178365: five direct test executables | 452 cases / 19,200 assertions passed; all five executables passed |
| Callback-queue/frame/bank build 35b06f81: five direct test executables | 463 cases / 20,439 assertions passed, including concurrent callback producers, FIFO/reentry/teardown and diagnostic shader-scan cases |
| Audio service CPU fix fcf92fc5: five direct test executables | Same 463 cases / 20,439 assertions passed on the corrected binary |
| Host tree and used-read inventory build b1a1c09f: five direct test executables | 473 cases / 20,619 assertions passed, including ten synthetic shader/draw-inventory cases |
| Native view/skin build 812a4194: five direct test executables | Same 473 cases / 20,619 assertions passed; build/index and disk guards passed |
| Vertex bounds/private bank build f674ce99: five direct test executables | 495 cases / 21,517 assertions passed; all 32 native draws have bounded used-input inventories |
| Frame owner/pin/constant build b25fdc09: five direct test executables | 534 cases / 23,366 assertions passed: base 84/2,287, net 124/11,169, kernel 265/9,208, VFS 14/218 and UI 47/484 |
| Packet profile/prepared request build f9555d93: five direct test executables | 567 cases / 24,606 assertions passed: base 84/2,287, net 124/11,169, kernel 298/10,448, VFS 14/218 and UI 47/484 |
| Native execution/reuse build 18d37f2b: five direct test executables | 587 cases / 24,787 assertions passed: base 84/2,287, net 124/11,169, kernel 318/10,629, VFS 14/218 and UI 47/484 |
| Six generated guest/controller keyboard cases | Passed on intermediate ELF ee93976c; acceptance/cancel repeated on final ELF |
| Real physical Ctrl+A with an idle controller | Reproduced `originalxz` failure on earlier builds; final ELF returns exactly `z` |
| System clipboard Unicode paste | Passed via genuine Ctrl+A/Ctrl+V in a dedicated nested Xwayland display; final harness exits 0 and the user's clipboard contents are neither read nor changed |
| Final AppImage keyboard export case | Async accept passed, including actual output, completion fields and input handoff |
| AppImage guide/cancel/game continuation/return home | Passed with inspected screenshots and completed in-process shutdown/relaunch |
| Native HUD lifecycle during that relaunch | Two successful initialize/unload/retire cycles before and after, title preserved |
| Native XUI Init/object lifecycle | Passed on later diagnostic ELF 38c53618, with actual native outputs and cleanup |
| Native light device/shared renderer/DC | Passed on that later ELF; no command recording, HUD scene or native pixels accepted |
| Native primitive recording | Passed on ELF 37788b7b: 2,528 DWORDs, 33 bounded packets, one draw; never published or submitted |
| Native primitive capture on e95549f1 and b776fdbf | Same recording SHA-256 as the earlier capture; external pixel shader snapshotted while live. Strict parser rejects incomplete local fetch state; capture is not executable |
| Real native UI audio/background/controller | Passed on e95549f1 and b776fdbf, including native audio-engine/driver validation, actual hudbkgnd scene load, and published controller ownership |
| Full relaunch with that resident native graph | Passed on both builds: completed in-process shutdown, then fresh initialization and strict title-work fence in GPU epoch 2; dashboard screenshots inspected |
| HUD scene after those prerequisites | Failed on e95549f1 at native PC 8169CE78: uninitialized native title-loader manager, directly queried during background layout. HUD08 did not return; no scene/render/input acceptance |
| Foreground-title query on 61c73361 | Passed through the real bound guest entry: returns FFFE07D1 while the native manager remains zero. Original 8169CE78 fault cleared |
| Relaunch with foreground state and real host dispatch | Passed on 61c73361: generations 1 and 2, seven runtime calls per generation, native controller ready twice, complete shutdown and dashboard screenshot inspected |
| Next HUD scene gate on 61c73361 | Failed at 81835E38 in resource invalidation: SYSTEM resource locking selects the unset VdGlobalXamDevice. No scene/render/input acceptance |
| Genuine SYSTEM device owner and actor on 31178365 | Passed the former resource-lock fault: actual factory device published with module/graph ownership, requested actor confirmed through paired native functions, HUD08 returns zero with valid root and child |
| Real target/viewport/clear/resolve CPU recording | Passed on 31178365: 640×480 RGBA8, finite projection, color writes, 2,748 DWORDs / 93 packets / three draws. Recorded resolve destination matches the actual physical allocation; no GPU execution |
| Resident HUD scene callback on that target | Native render/post-render/End return zero; complete scene/cache/module graph retained through recording. 2,761 DWORDs / 97 packets / three draws: the zero-time initial tick adds no HUD geometry, so visible native guide rendering is not established |
| Full relaunch with SYSTEM owner and resident scene | Passed on 31178365: old publication retires, full shutdown completes, fresh device publishes in kernel/backend epoch 2, both scene gates return zero, dashboard screenshot inspected |
| Native timers/layout/animation updates on 35b06f81 | 31 native updates with a bounded simulated 500 ms animation interval pass; timer clock remains native. HUD adds an actual four-vertex gradient draw: 2,995 DWORDs / 121 packets / four draws. Full visible guide content and input are not established |
| Independent Vulkan bank allocation | Passed on 35b06f81: separate RT/pipeline resources, 10 MiB EDRAM at 1×1, one descriptor set; repeated allocation reuses that bank and leaves title identities/state unchanged. Allocation only; no native context activation or GPU execution |
| Full relaunch with updates and allocated bank | Failed on 35b06f81: both CPU/scene/bank callbacks return zero and old device retires, but a new-generation asynchronous guest thread faults at 819A4220 (NULL+14). The inspected screenshot contains the crash dialog; successful probe markers do not establish title continuation |
| Audio crash diagnosis and two-relaunch regression | Before fix, live Audio Worker PCR CPU5 selected NULL mixer buffers; native SYSTEM initialized only CPU4's pair. On fcf92fc5, cold start and two full relaunches in one process all use CPU4, complete three native probes with zero faults, and show healthy dashboard screenshots after both restarts |
| Actual native AppHost attachment and composite canvas | Parent and native Show readbacks pass. Painting the actual owner's canvas still records only the existing gradient; attachment alone does not establish visible guide content |
| Complete native tree and measured timer comparison on b1a1c09f | Both runs inspect 135 elements without unsupported queries or truncation. Home controls exist with shown, nonzero-opacity ancestors. Thirty-one updates take 22 ms with simulated deltas and 509 ms with measured pacing; animation values change, but both primary records have the same SHA-256 and four draws |
| Actual shader/vertex read inventory on that build | Seven shader loads analyzed and four used vertex spans captured (84, 48, 32 and 24 bytes). All six dependency files match recorded hashes. Strict analysis still rejects DW237; index bounds, live allocation ownership, transform coverage and GPU execution remain unproven |
| Host tree and measured pacing across two full relaunches | Three native probes pass in one process with completed shutdown/reinitialization, zero guest faults and inspected dashboard images after both restarts. This validates the diagnostic lifecycle, not native guide presentation |
| Authentic owner view and missing-visual baseline on 812a4194 | Native matrix construction and render setter succeed; the 640×480 test target receives the original -106 X translation. Native control queries report 68 missing visuals and no attached visuals; the record still has four draws |
| Real skin/visual/base-path/locale initialization on that same build | The real resource module is loaded and leased before background/HUD creation. Native load/base-path/readback/font calls succeed, and SYSTEM opens XenonJKLatin.xtt. The bounded tree reports 62 valid attached visuals, no missing visuals among queried controls and no unsupported queries; 256-node capacity causes 26 truncations. The record grows to 6,233 DWORDs / 669 packets / 32 draws. Full-tree, string-table and native pixel acceptance remain open |
| Skinned native graph across two full relaunches | All three native initialization/scene cycles pass with 62 attached visuals per bounded walk, two completed in-process shutdown/restarts, zero guest faults and inspected dashboard images. Commands remain unpublished and unsubmitted |
| Actual completed-frame owner and backing pins on b25fdc09 | All 32 draws, 35 shader loads and 375 used float-vector values pass their CPU checks. Seventy-one requested ranges are covered by 11 real parent allocations totaling 1,675,264 bytes. A pinned record release is refused without changing its identity or contents. Explicit unsubmitted abort closes the old update |
| Owner retirement across two full relaunches on b25fdc09 | Three distinct owner/frame reports pass in the same process. Both earlier owners retire during completed shutdowns, zero guest faults occur, and both after-relaunch dashboard images are healthy. The verified, server-synchronized window-close repeat exits 0 |
| Earlier b25fdc09 final-close capture | Its two relaunches, three frame reports and two retirements pass, but final window close times out with runner 124. Overall result remains failed; the cause is unproven and no emulator-source fix is claimed by the passing repeat |
| Original-data audit after owner tests | All 10,182 recorded originals match the fresh before snapshot by SHA-256, size and mtime. Four exact prior deltas and a separate pre-phase timestamp-only observation retain their original classifications |
| Actual packet profile and typed request on f9555d93 | Version-2 report passes all 669 packets, 32 draws and 35 shader loads. The CPU request is prepared and admitted immediately before explicit unsubmitted Abort, then canceled. No prepared handoff or GPU work is published |
| Prepared request across two full relaunches on f9555d93 | Three version-2 frame reports and both owner retirements pass in one process, with zero guest faults and runner exit 0. Both after-relaunch dashboard images were inspected |
| Checked Vulkan helpers on f9555d93 | All six source/header files compile. Ordinary dashboard rendering and relaunch pass through legacy paths; native draw/resolve/texture/bank-selection helpers are uncalled and have no runtime acceptance |
| Original-data audit after prepared-frame tests | Full SHA-256, size and mtime comparison of all 10,182 originals finds zero new changes, hashing 8,634,292,865 bytes in 12.84 seconds |
| Actual native GPU execution on 18d37f2b | Submitted 31 draws and one copy; exact native fence Complete, title state restored, source attached and retired. First and repeated post-use bank diagnostics pass. Output pixels remain unverified |
| Actual native execution across two full relaunches | Three owner/source/backend associations complete and retire, with two prior-owner retirements, zero guest faults and runner exit 0. Both healthy dashboard after-images were inspected |
| Retained initial execution run on 032f7f14 | Native GPU execution and retirement pass, but the later post-use bank diagnostic fails the old constructor-reset RF hash check. Overall XUI probe remains failed; the corrected reuse diagnostic passes on 18d37f2b |
| Native keyboard ownership/assets research | Bounded Ghidra instructions identify a borrowed DC/common XUI and separately owned root/classes. All 143 identified staged and seed assets match Metro central-directory size/CRC per root; native keyboard execution remains unverified |
| Original-data audit after native execution | All 10,182 originals match full SHA-256, size and mtime; 8,634,292,865 bytes hashed in 12.917 seconds with zero new changes |
| Strict GPU completion probe on ELF 37788b7b | Complete for preceding title work only; no native UI GPU-draw or pixel acceptance |
| Window close with pending keyboard | Application exits naturally; existing quick_exit prevents a joined-teardown claim |
| macOS, native vk.xex layouts and broader title matrix | Unverified in this phase |

Host-keyboard/guide-fallback release ELF SHA-256:
`74cfe1240752df7ab489516400aa72ea904636c725923df1c94227d0275f014f`.
The packaged artifact is
`xenia-edge-guide-osk-b65dd25-74cfe124-x86_64.AppImage`, SHA-256
`b7e0a4749713cb02bb27746e6dcb4aebb75e620929a7049ffc9ecc8ee01889f1`.
Packaging used the existing upstream linuxdeploy workflow and retained the
unstripped native binary unchanged. All runs disable MangoHud.

Private `evidence/native-xui/` contains build/test logs, exact source and binary
identities, keyboard runtime summaries, debugger records and packaging metadata.
The final packaged guide run is
`20261103T232611Z-guide-packaged-navigation-fixed-return-dashboard-mac3bv2v`.
Its return path logs completed Emulator::Shutdown and relaunch, rather than
relying on the final application's quick_exit status.

The original inventory audit checks 361 core-file hashes and 9,821 shared file
size/mtime records. Four history/config/dashboard-cache files have differences
timestamped 22:18 UTC, before this phase's first runtime at 23:07 UTC. They are
retained as found; their timestamps do not identify who changed them. Recorded
system/profile/save files match. See private `preservation-audit.json` for the
exact distinction from the older inventory; no restoration of user history or
cache is performed. Each diagnostic run separately verifies its immutable seed.

The owner-test audit additionally fingerprints all 10,182 recorded originals
with SHA-256, size and mtime before and after runtime. The after audit finds zero
changes against that fresh snapshot. It preserves the four exact prior
fingerprints separately from an older timestamp-only difference in
`xenia_edge.conf`, whose size and contents still match the original inventory.
That fifth observation is not relabeled as one of the known four. Private
`evidence/native-runtime/preservation-owner-lease-after.json` records the full
scope and snapshot identity.

The later `preservation-prepared-f9555d93-after.json` repeats that full audit
after the prepared-frame and two-relaunch gates. All 10,182 originals remain
unchanged against the same fresh before snapshot. The four prior differences
and the separate earlier timestamp-only observation keep their classifications.

The coherent native XUI/resource provider has now bound HUD's native imports.
Real background/controller initialization resolves the earlier missing HUDScene
class prerequisite. A common foreground execution snapshot now tracks preparing,
running, stopping and unloaded generations. The bound main guest thread
publishes Running after DLL notifications/APCs and before guest entry. Late
notifications cannot reactivate a stopped or replaced generation.

The private plan verifies the native active-title getter's full code signature
and binds it to this real host state. Both native manager checks stay zero;
full native loader initialization is not needed for that query. This covers
the getter contract, not all native loader state-machine APIs.

The former export bridge only set symbol metadata. Actual PPC host dispatch
now uses an immutable translation view containing the import-call/return pair;
guest bytes and their image hash stay unchanged. Ten synthetic-XEX cases
execute real entry/direct/tail callers and check byte preservation. Previously
compiled targets are rejected because callers may embed literal host addresses.
Explicit native-runtime preparation therefore computes metadata first and
installs bindings before compiling native entries.

Resource locking selects VdGlobalXamDevice independently of the device passed
to shared XUI. The normal native initializer publishes after installing engines,
interrupts and a ring, which would replace the title's ring. An explicit
diagnostic now owns and publishes the genuine factory-created light device
through a common graphics-device lifetime model. It checks the bounded CPU
recording profile, holds module/scene/cache ownership, closes normal TITLE
admission into the native HUD, and compare-clears only its own publication after
consumers stop. Pending or uncertain GPU ownership cannot establish retirement.
The adapter does not submit native GPU work. The real actor setter/getter pair
and HUD08 now pass without the earlier resource-lock fault.

The target producer calls real native surface/header creation, viewport,
Clear and Resolve APIs. The CPU resource header retains its allocation alias;
native Resolve converts that alias to the actual physical destination when
recording commands. No host rewrite of that descriptor is needed. One native
child-enumeration call also required its real HRESULT/output-pointer ABI.

Native frame updates now call timers, layout and typed Float32 animation deltas,
then repeat layout during painting. The bounded simulated animation interval
yields a genuine untextured HUD gradient; native timer deadlines still use
actual uptime. This is not a real-time input pump or a complete visible guide.
Actual app-host attachment, its parent/Show readbacks and composite-canvas
painting now pass. A bounded walk finds the real Home controls with positive
visibility values along their ancestor path. Measured pacing changes animation
values but leaves the primary record byte-identical. The authentic owner view
transform now passes independently and still produces only four draws.

The optional [native skin bootstrap](../../src/xenia/kernel/native_skin_probe.h)
runs in the original order, after borrowed-DC owner initialization and before
background/HUD creation. It leases the real data-only resource module, builds
its native locator, loads the visual table, sets and reads back the base path,
then runs the separately audited locale/font stage. Every partial failure
retains its owners and stops native UI; private globals and control visibility
are not manufactured. On the same build, real Home buttons gain valid visual
children and the record grows from four to 32 draws. SYSTEM also opens the
native typeface file successfully. This fixes an observed producer prerequisite;
the internal string-table load, complete expanded tree and native pixels remain
unverified. Two full relaunches pass with the skinned graph.

A bounded diagnostic shader scan captures later external shaders despite the
older strict parser's rejection. It does not recurse into indirect buffers or
turn its diagnostic inventory into an executable stream. On `f674ce99`, all
35 shader loads and 32 used vertex/texture inventories pass. The sixteen
textured QuadList batches now have bounded vertex and normalized 2D texture
reads. All 68 dependency snapshots match their recorded sizes and hashes:
20 shader-code copies, 32 used vertex spans and 16 texture-storage copies.
Four unique linear `k_8` atlases contain the captured glyph data. The vertex
access proof reparses each selected owned VS and remains conditional on the
backend's checked initial auto-index contract. The direct copy uses 24 bytes.
The half-size clear uses a 4x-MSAA EDRAM alias of the 1x color target; raw
coordinates alone do not establish incomplete clearing.

The default-off `native_frame_lease_probe` on `b25fdc09` tags real native
physical allocations from before heap/CRT initialization. The persistent
SYSTEM-thread owner retains the genuine module/device/scene graph. After native
recording End, it pins the full record allocation before copying commands under
the allocator lock, freezes the producer update, and pins the output plus every
used shader, vertex and texture range. Pins protect complete backing arenas
against allocator mutation through the parent and all physical aliases. They
do not prevent arbitrary CPU writes; producer quiescence is a separate owner
requirement. Allocation IDs and owner epochs reject stale or wrong-owner use.

The actual 32-draw gate also verifies explicit used constant values without
inventing unknown unused state. All 35 shader loads pass, as do 375 used
float-vector values across the draw snapshots. Seventy-one ranges resolve to
11 parent allocations totaling 1,675,264 bytes. A real release attempt returns
pinned, and rechecking confirms the same record allocation and bytes. The
checked unsubmitted abort returns the owner to Bound with no active update;
further production requires a new update. Failed or uncertain paths retain
their pins and close admission. This CPU gate never attaches a native GPU
completion token or submits its recording.

The accepted owner relaunch run creates three distinct owner reports and retires
both earlier owners during two full in-process shutdown/restarts. It records
zero guest faults and a normal final exit after a process/protocol-checked,
server-synchronized close request. Both after-relaunch dashboard images were
inspected. A prior run passes the owner/relaunch checks but times out during
final close and remains an overall failure. The repeat improves close-request
evidence; it does not prove the earlier cause or an emulator-source fix. Final
window exit still uses quick_exit and does not prove final joined teardown.

Private `evidence/native-runtime/guide-frame-owner-b25fdc09.json` joins the exact
build, direct suites, single-frame report, accepted and failed relaunch captures,
and original-data audit. That owner gate's ELF SHA-256 is
`b25fdc09b8c735d6e97bc278d848ef0bb677e5120885d0699e1a824bb6596787`.

On `f9555d93`, a separate bounded typed command plan accepts the entire actual
6,233-DWORD, 669-packet recording, including its native clear and final copy.
The older default strict parser remains unchanged. The version-2 lease report
also verifies the same 32 draw/input/constant snapshots, 35 shader loads, 375
used float-vector values and 71 ranges backed by 11 parent allocations. It
prepares an owned typed request and checks admission immediately before Abort;
the explicit unsubmitted Abort then cancels that request. `prepared_handoff`,
`execution_ready`, `gpu_submitted` and `shader_semantics_verified` stay false.
This is CPU preparation and revocation evidence, without backend execution
permission or a native GPU completion token.

The single run `20261104T141113Z-native-runtime-prepared-frame-f9555d93-z0u02992`
passes with runner exit 0 and no guest fault. The two-relaunch run
`20261104T141505Z-native-runtime-prepared-two-relaunches-f9555d93-tpha3fdt`
passes three distinct version-2 reports, both full in-process shutdown/restarts
and both prior-owner retirements, with zero guest faults and runner exit 0.
The ordinary dashboard image from the single run and both after-relaunch images
were inspected. Private `evidence/native-runtime/guide-prepared-frame-f9555d93.json`
joins the exact build, source bundle, direct tests, reports, captures and audit.
Its accepted ELF SHA-256 is
`f9555d93d4bf5acfbec3abc71bca3aa10ec984dce08a57bc05ca38324732bb86`.

The later `18d37f2b` gate executes that checked record at an actual title swap.
It selects the independent native RF/cache/descriptor/EDRAM bank, submits 31
draw commands and one copy, and returns a source-tagged native fence. The exact
Complete token is attached to that source before retirement; title state is
restored. Execution requires an explicit opt-in at final request consumption.
Uncertain work halts guest decoding and retains its owners until an actual
device-wide stop boundary. Healthy shutdown/relaunch is observed; device-loss
and allocation-failure paths have host-model/static coverage, not injected GPU
failure acceptance.

The single run `20261104T152704Z-native-execution-reuse-18d37f2b-63gr7ada` passes.
The run `20261104T153005Z-native-execution-two-relaunches-18d37f2b-2xdq8jq4`
passes three exact native execution/fence/source retirements in one process,
two full shutdown/restarts and both prior-owner retirements, with zero guest
faults and runner exit 0. Root inspected the single and both after-relaunch
images: healthy ordinary Dashboard Home/Open Tray, text, icons, tiles and title
toolbar. Both 1280×797 after-images have identical decoded RGB SHA-256
`dece9956087ac6eda9969065fbb378d15fa1cfc687fcda738e5bfdaf59ad9936`.
These images establish title continuation, without showing the offscreen native
output. The original PNGs are preserved.

The initial run `20261104T151947Z-native-execution-first-032f7f14-tz9ohutt`
retains its overall failure. Its native execution branch passes, but the later
bank-reuse diagnostic compares post-frame private RF state with constructor
reset state and fails status 5/stage 6. Some false aggregate flags also include
an unexecuted repeat's default result; they do not establish bank recreation.
The fix compares a reused bank with its actual entry hash across the reversible
canary; a fresh bank additionally requires its constructor hash. It resets no
registers and copies no title RF state. The accepted single and relaunch runs
pass the first/repeated post-use diagnostics.

Private `evidence/native-runtime/guide-native-executed-18d37f2b.json` joins the
accepted build, direct tests, exact capture/helper/report hashes, retained
failure and full original-data audit. Accepted ELF SHA-256:
`18d37f2bef8fd7cda88aa81c7fd3d0e4131c56fccba5def5fa5db3911bde80ea`.
`preservation-native-executed-18d37f2b-after.json` reports zero new changes to all
10,182 originals, retaining the prior four differences and separate older
timestamp-only observation. CPU handoff reports/capture manifests precede
execution and keep their false GPU flags; separate `.execute.json` reports
establish the later native submission and retirement.

The later `25382ef9` gate adds operation-owned readback after the exact native
fence and before source retirement. The producer carries its real six-word XG
texture header. The checked immutable plan, pinned final-copy vertices and
actual physical output range establish a full 640×480 rectangle, pitch 640,
tiled format 6, endian 0 and BGRA bytes. The decoder maps all 307,200 pixels,
converts BGRA to RGBA once, and preserves alpha and orientation. Runtime and
decoder reports keep their original false pixel/composition/input flags; the
separate visual-inspection report records the bounded Home-scene acceptance.

The single run `20261104T161119Z-native-readback-overlay-25382ef9-m6o5fu2x`
passes 30 native draw commands, one copy, exact fence/source attachment,
1,228,800-byte readback and retirement. The run
`20261104T162916Z-native-readback-pinned-two-relaunches-25382ef9-5wcwclxm`
passes three such frames, both in-process shutdown/restarts, both prior-owner
retirements and both after-cycle captures, with zero guest faults and runner
exit 0. Its final close targets the pinned main XID and verifies the live
process, WM protocol and server synchronization. Final quick_exit still does
not establish joined final teardown.

The first raw capture on `69c00daf` retains its green diagnostic primitive and
opaque clear as a transport milestone. The accepted scene-producing guide path
omits that test primitive and uses a transparent clear; the standalone primitive
diagnostic retains its purpose. Root inspected the accepted single image and
all three relaunch owners: native Home labels/menu, profile, clock and footer
are visible, with transparent pixels outside the guide. Headers and footers
match exactly across owners. Profile alpha matches, with RGB variation at most
one. Both ordinary dashboard after-images are healthy title continuation.
This covers the current Home scene; full-scene, format, gamma, scaling and
console-parity coverage remain unverified.

`native-alpha-convention-25382ef9.json` records transparent clear plus the
captured RGB SrcAlpha/OneMinusSrcAlpha and alpha One/OneMinusSrcAlpha blend.
The offscreen result stores premultiplied color. Its later composition must
use the stored RGB once, without multiplying by alpha again. This source/frame
analysis does not establish an actual GPU composite.

The earlier relaunch run
`20261104T161643Z-native-readback-two-relaunches-25382ef9-h2teft_l` stays an
overall failure with runner exit 124. All native execution/readback subgates
pass, but two matching Xenia windows make capture ambiguous and prevent a final
close request. The private helper fix pins the first verified main XID to the
owned PID, process start time and executable, requires after-cycle captures,
and records bounded inventories on capture failure. The accepted pinned repeat
is separate evidence; the failed run has not been relabelled.

Private `evidence/native-runtime/guide-native-readback-25382ef9.json` joins the
build, 596 passing cases/25,141 assertions across all five direct suites,
current source hashes, capture/dependency/raw/metadata/decoded-image hashes,
helper version archives, visual inspection and preservation audit. Accepted
ELF SHA-256:
`25382ef9bf3c8c966297c30659ec87779f05ce2900fcd44c7a6e97a4b408600c`.
`preservation-native-readback-25382ef9-after.json` confirms all 10,182 originals
unchanged by content, size and mtime, retaining their earlier classifications.

The next rendering gate is a CP-owned versioned presentation image, converted
on the GPU before the source's final fence, then blended after title gamma and
before mailbox release. Its lifetime must follow the actual CP submission that
samples it, with checked submission and presenter handoff. A CPU upload is not
the integration substitute. Normal native guide admission stays closed;
composition, input/focus/dismissal and native keyboard integration remain
unverified. Guide work stays ahead of HDD/profile work.

Bounded Ghidra work in private
`evidence/native-guide/keyboard-abi/vk-ownership-ghidra-20261104T151216Z/findings.json`
establishes that normal VK borrows an initialized native DC/common XUI while
owning its root and class registrations. Its registration wrapper discards
helper failures, so outer setup HRESULT 0 cannot prove all four classes exist.
Independent class checks, matching resources, borrowed-DC lifetime and the FF
app/input owner transitions remain prerequisites. Instruction/import/literal
cross-checks support these findings; inferred decompiler prototypes are not ABI
declarations. Metro has all 143 identified guide/VK assets already present in
both staged and seed roots with matching ZIP central-directory size/CRC. No
archive-member SHA-256 equality or native OSK runtime acceptance is claimed.

Review of the CP callback path found a pre-existing cross-thread std::queue
race. The common mailbox now serializes enqueue/dequeue, retains an atomic idle
hint, executes and destroys callbacks outside its lock, and preserves nested
owner-thread/FIFO behavior. A null worker cannot establish owner-thread identity.
Joined teardown discards pending callbacks safely, including reentrant capture
destruction. The new concurrency cases and actual CP allocation/completion
callbacks pass on Linux.

The later audio fault was a separate service-affinity defect. Native SYSTEM
XAudio initializes its per-processor mixing buffers for reserved logical CPU4.
The host Audio Worker inherited the default rotating guest assignment, reaching
CPU5 after relaunch and selecting NULL interfaces. Live PCR and buffer reads,
plus the named Audio Worker exception stack, establish this mismatch. Setup now
selects guest CPU4 after ordinary creation and before render-client admission.
This preserves the default CPU-assignment sequence for other threads and does
not pin the host service to a Linux CPU. The crash-aware regression helper
requires completed teardown/relaunch cycles, stable process identity and zero
guest faults independently of successful probe markers. Audio quality and
broader game coverage are not established by this dashboard regression.

The existing `is_attached()` XAM special case is
not proof that native CRT constructors ran.
The supplied image has no standard PE TLS callback or separate CRT section
that provides a portable initialization shortcut. Its ordered internal CRT
tables run after native heap setup. The existing compile-only static-initializer
heuristic sorts/deduplicates targets and must not become an execution plan.
Initializing lock bytes or fake heaps by image address would conceal this
prerequisite. Guide work remains active before HDD/storage.

## Research disk budget

The completed-run compaction removed 37,127 files across 85 disposable runs
only after checking their contents against preserved seeds with SHA-256.
It recovered about 18.1 GiB. Logs, screenshots, unique run state, seeds, and
original assets were retained. Each compacted run has a
`compacted-seed-files.json` reconstruction ledger; missing duplicates can be
copied from its seed without replacing existing changed files. Private inventory
contains the full plan, result, and a tested restoration helper. A fresh audit
found no new original-data differences against the recorded baseline.

A second pass compacted 6,100 exact duplicates across 13 additional completed
runs, about 2.94 GiB of allocated files. All 110 changed/unique data files were
excluded. The pinned private applier revalidates hashes, process absence and
non-aliased paths, saves a durable reconstruction ledger before unlinking, and
defaults to dry run. Eight tiny safety tests pass, including interrupted removal
and restoration without overwriting changed files. Both passes preserve all
logs, screenshots, original assets and immutable seeds.

The runner requires more than 20 GiB plus 1 GiB headroom and estimated copy
allocation before copying, then checks again before launch. A once-per-second
watchdog stops the owned capture process group at 20 GiB plus 512 MiB. All 18
runner tests pass, including refusal before copy/launch and child-process
cleanup. Incremental builds also used a disk watchdog. These checks cannot
control unrelated writers on the same filesystem.
