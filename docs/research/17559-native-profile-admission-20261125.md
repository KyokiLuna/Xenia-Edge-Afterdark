# Native profile admission and startup — 2026-11-25

Status: source backup with passing focused tests; native acceptance incomplete.
No replacement AppImage, checkpoint release or HDD gate closure accompanies it.
Internal build labels V123–V125 identify research builds, not releases.

## Applied implementation

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

## Validation and observed scenarios

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

Current implementation bytes are Owner `338f07b6c1eb84ca362c871885ab3373edb7bb35588cdbcf9577ce4b86e2db21`
and roster `4a97bf2cbceea0e0cff26bb1514bf43dee78e14b38ca7704900169ed5bf172b1`.
The FFmpeg submodule's working modification still exactly matches the tracked
`third_party/patches/FFmpeg/xmaframes-overlap-tail.patch`; no unpublished
submodule commit is required.

Startup readiness/progress and real shutdown remain prerequisites for the next
native profile/avatar acceptance run. Preserve required lifecycle checks until
their replacement is proven. The [console fidelity policy](17559-console-fidelity-policy.md)
also governs any new hook rundown or core change. A scheduler reclamation split
has not been shown necessary by these observations and is not implemented.
