# Native sign-in lifecycle source update — 2026-11-26

This source update addresses the shared sign-in request, profile-data handoff
and cleanup lifecycle. It does not replace the published AppImage or finish
the combined HDD gate. Full executable and native validation are recorded below
only after they run.

## Observed problem and implemented boundary

The previous Doritos Crash Course run displayed the native sign-in error
presentation. Its original request returned `0x80151000`; a later `20B` request
with flags `0x63` and no overlap was rejected before entry. Its caller was not
proven from the shared virtual address in that run. Matching module analysis
subsequently recovered the actual no-wait offline-restore contract. The
supervisor later received a termination signal. The user did not close that window, and its
actual sender remains unestablished; it is not a normal-exit acceptance result.

The implementation uses the matching installed 17559 module bodies and mapped
queue/restore contracts. It keeps the original service error and enables the
actual recovery request, rather than manufacturing Live sign-in success.

- Exact SYSTEM restore producers retain their actual module, import domain,
  caller frame and input/output provenance through their genuine own-return.
  The recovered no-wait request family remains restricted to those producers.
- Native asynchronous copy/queue admission retains request identity without
  opening a stale profile transaction. The genuinely selected serial callback
  prepares against the then-current roster and validated source generations.
- Current-user scalar queries read one actual selected immutable roster. They
  do not wait for a broad profile metadata lock or invent online account state.
- Logical profile-data handoff waits for actual callback return, following and
  writer use closure, borrowed reads and source dependencies. New reads use an
  already-issued fixed current ticket; old immutable readers retain their own
  holders. Task/free, service, presentation and storage cleanup remain separate.
- Per-acquisition mount receipts prevent delayed old cleanup from decrementing
  or detaching a replacement mount. Account pause ownership is released once,
  preserving successor exclusion through an A-to-B-to-A transition.
- Actual whole-kernel stop captures the complete reachable work ownership
  graph before retirement, restores entered scopes in reverse entry order,
  then revokes nested thread/module/resource holders outside guest locks.
  This does not fabricate a native callback return, completion or free.

The host fallback uses the shared services. Its legacy local-logon route still
reports Busy while genuine physical cleanup is pending. Copies, canceled work,
retained readers and real conflicting writers are distinguished explicitly.

## Validation

The full Linux Release build and all seven direct runs passed for the same
source/configuration/products. The 174 allowlisted installed-system inputs
were preserved. These tests do not establish native UI or game acceptance.

| Direct run | Cases | Assertions |
| --- | ---: | ---: |
| base | 334 | 28711 |
| net | 125 | 11212 |
| kernel | 1505 | 105265 |
| vfs | 136 | 9736 |
| ui | 124 | 4863 |
| cpu-focused | 5 | 130 |
| kernel-installed | 236 | 35429 |

The tested source manifest is `c8c2b3e22d9820e95c2bfa0186aff0e83966d5bd33e7145f4ed52a3a3041963a`.
The Linux ELF is `a67444bbc3ca4d4b7bfee452c673b5f80ed6f7f866755a9ec38efd87db24cd54`.
The generated version header retains the actual build base
`2418ae0aa308cb718d80bc6713a6ae2b078424d4`; a later source-backup commit
must not be described as that embedded compilation identity.

Thirteen focused cases were added for exact no-wait wire handling, current
wake/control behavior, historical callback genealogy, mount generations,
profile-data handoff, current scalar metadata and escaped/diamond/cycle stop
ownership. Existing case names/order and stronger producer fixtures remain
preserved. Runtime acceptance must cover offline restore after the real error,
Guide close/reopen and input return, cancellation, continuation and normal exit.

## Upstream accounting

Upstream was refreshed through
[`948ccbb86c`](https://github.com/has207/xenia-edge/commit/948ccbb86cfe5caa730a9f9ef458ee9565262b2c).
Four commits since the preceding `40bce17077` review are deferred together:
forced guest fibers, debugger/pause/savestate removal, host thread/APC removal
and host alertable-wait removal. Afterdark already defaults to guest fibers;
that does not establish equivalence of the entire removal.

Their scheduler, timer, I/O, backend instrumentation and teardown consumers
require a coherent CPU/XMA review after the meaningful HDD test AppImage/source
sync, before gate closure or Live work. Host alertable-wait removal must not be
mistaken for permission to discard guest APC/wait semantics. Useful diagnostic
code must remain preserved behind disabled paths under the fork's policy.
These decisions are deferrals for adaptation and validation, not a claim that
the upstream implementations perform worse.

## Remaining acceptance

Native sign-in/Guide and profile/avatar game consumption remain unaccepted
until the current executable is exercised. Complete SAVE/GPD/container flush
publication, native profile creation/repair, stock 3D avatars, two-device and
media runtime acceptance still belong to the combined HDD gate. The supplied
XNA title update and XMA/GPU/CPU fidelity review remain required before closure.
Optional Media Update/CodecPack02 remains an explicit native AAC prerequisite.
MangoHud stays disabled; macOS execution remains unverified.
