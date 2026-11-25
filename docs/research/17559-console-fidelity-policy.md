# Console fidelity and emulator overhead

The Xbox 360's actual behavior is the reference for Afterdark. This is a
standing development rule, including code inherited from upstream and code
already added in this fork.

## Implementation decisions

Establish the guest producer/consumer contract from the matching executable,
update, mapped references and observed behavior. Restore the console's own
service or UI where it exists. A host fallback consumes the same truthful
services; it does not become a substitute for native functionality.

Host-only work requires a specific reason. CPU/JIT translation, GPU translation,
audio decoding, ABI bridging, host devices/I/O and resource lifetime can require
different machinery, but their existence does not justify every extra check or
helper. Record the concrete requirement, relevant boundary and evidence for a
non-obvious difference in its implementation or subsystem contract. Keep that
explanation proportionate to the change.

Review repeated hashing, module lookups, defensive guards, polling, threads,
processes, tracing and capture for necessity and cost. A retained reader is not
automatically a writer conflict. A native pending operation is not a completed
operation, and host bookkeeping does not grant guest data access. Preserve
real ownership and completion while simplifying artificial restrictions.

Disable unneeded logging and diagnostic work with clear comments or a disabled
diagnostic guard. Retain the code for later debugging instead of deleting it.
Avoid paying for collection, hashing, formatting or helper execution merely
to suppress the final log line. Required error reporting remains truthful.
Changes to functional checks require an established replacement contract;
commenting out a necessary check or returning fabricated success is not a fix.

Use matched runtime evidence for performance claims. Separate shader warm-up
from steady state, record host/build/configuration identity, and keep MangoHud
disabled for this project's comparisons. Counter totals and static code cost
are leads, not measured FPS or input-latency improvements.

## Current gate sequence

1. Make the combined HDD work sufficiently functional and tested to produce a
   meaningful Linux AppImage for testing. Sync the corresponding source and
   notes to GitHub. This does not itself close the unfinished gate.
2. Review and improve XNA, then XMA, GPU and CPU behavior against this rule.
   Classify console behavior, necessary host machinery and unnecessary work;
   correct shared causes rather than adding title-specific workarounds.
3. Re-run affected regressions and the combined gate's mandatory acceptance.
   Package and test the resulting AppImage itself, sync matching source and
   provenance, and perform the final upstream goal-alignment accounting.
4. Close the gate only with the complete required evidence. Live/P2P work
   follows this foundation and core review. Genuine missing prerequisites
   remain recorded; they are not fabricated into passes.

The full HDD scope, preserved originals, private console-key/data exclusions,
20 GiB filesystem floor, native UI default/fallback launcher, and final icon
coverage requirements remain in force. A development AppImage is not a stable
finished-gate release.

## Primary XNA update reference

The user-supplied update at
`/home/kyoki/Documents/emulations/360/XNA Title update/584E07D2/000B0000/`
is the primary input for the upcoming XNA work. Its verified identity and
version distinctions are recorded in the [XNA update reference](17559-xna-update-reference.md).
Pin the actual package hash,
content identity, media/version/base-version and launcher/runtime identities.
Compare installed and supplied bytes before diagnosing an absent update.

Recover installation and title-update selection, launch data, GameInfo/product
identity, managed runtime/assembly resolution, native bridges, storage/profile
consumers, audio/XMA and graphics usage from this actual update. Use targeted
Ghidra and managed-code analysis as appropriate. The matching binaries are
primary evidence; decompilation and inferred contracts retain their limits.

Keep older dashboard/runtime versions separately labeled. Do not reuse their
addresses or claim an old launcher map describes this update without matching
image provenance. Reuse existing extraction/maps where identities match,
preserve original packages, and preflight selective extraction space. Source
references and analysis notes may be published; private data and supplied
proprietary package binaries do not belong in the public source repository.
