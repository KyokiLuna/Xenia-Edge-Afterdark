# Checkpoint V59: selector and avatar consumer failures

These are failures reported while testing the V59 AppImage, not passes inferred
from profile persistence. The combined HDD gate remains open.

Later checkpoint status: the user confirmed V60 automatic selection and V62 BO1
mode switching/exiting plus controls persistence. V61 added owned FB 2D tile
delivery; native 3D avatar rendering remains open. See the
[V62 checkpoint record](17559-hdd-profile-checkpoint-v62.md). The V59 findings
below are retained as historical evidence.

## Automatic storage selection

The user reproduced Black Ops' selector failure in V59 with native UI tracing
enabled. The real public call was user 0, content type 1, flags 0, and a full
64-bit requested size of `0x100000` bytes. Native UI admission returned
`X_ERROR_NOT_READY` (`0x15`) without executing the native selector. The wrapper
returned that error before reaching its existing automatic-device policy.

Evidence is retained locally under
`evidence/hdd-gate/root-v59-bo1-selector-actual-user-trace-v1/`: the exact log
has SHA-256
`0fcb326384d13a5e45b748b6d761b316d3b910390fd667f9b7613c06cc4dd25d`;
lines 71387 and 71390 contain the request and native admission result.

The correction must resolve automatic selection and immediate storage failures
through the common storage backend before requesting UI. Explicit prompts retain
their native/fallback route. Output publication must recheck device suitability
and caller lifetime; automatic selection must not acquire modal input focus.
There is no title-specific exception and no fabricated storage success.

The 17559 selector ABI passes the complete requested size in r6, followed by
the output and overlapped pointers in r7/r8. Splitting it into two dwords would
change those pointer registers incorrectly.

## Avatar consumption

The user reports that the saved avatar does not appear in the dashboard or
Doritos Crash Course. V59's genuine preset, Account/GPD/picture writes and restart
tests establish persistence only. They do not establish native dashboard or game
rendering.

The four recovered native FB tile read messages are not connected to the shared
tile reader in V59. This is a concrete missing consumer path. Its correction
needs owned original-call packet inputs, nested capacity/output validation,
profile identity retained through asynchronous completion, and the recovered
guest pixel layout.

Tiles do not provide the avatar geometry service. `XamAvatarGetAssetsResultSize`
still returns empty sizes, and `XamAvatarGetAssets` lacks real asset output.
The native avatar service, geometry, animation and rendering remain required
work. Enabling initialization or returning success from those stubs cannot
establish a rendered game avatar.

## Live sign-in

CastleMiner reports that Xbox Live sign-in is required. Current local profiles
report local sign-in; `license_mask` controls entitlement and does not create a
Live identity or service. The exact game's Live/XNA prerequisites remain later
work. They are separate from the observed automatic storage admission failure.

## Diagnostic launcher handoff

The first diagnostic launcher was stopped by its own process guard: GameMode's
preload mismatch caused Xenia to spawn a replacement process and exit the UI
leader. The guard then stopped the verified child. This was not evidence of an
emulator fault. Pinning GameMode off and in-process relaunch for the guarded
diagnostic allowed the user to reproduce the actual selector error and close
normally, with no cleanup signals. Backend changes that require process
replacement remain outside that guard's supported launch path.
