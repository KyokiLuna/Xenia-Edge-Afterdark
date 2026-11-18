# Upstream integration: project goals determine inclusion

Afterdark is an independent fork restoring console-level Xbox 360 behavior.
Upstream changes are reviewed for useful fixes, research and compatibility
evidence. They are included only when they support the accepted project goals
and the system contracts through which those goals are implemented.

A change that conflicts with those goals is **excluded**. Its presence in
upstream, a clean merge, an isolated passing test, a performance improvement or
different host functionality does not override this requirement. Reviewing the
latest upstream work is a maintenance obligation, not an obligation to adopt it.

## Inclusion requirements

Before applying an upstream patch:

1. Identify the accepted requirement it advances and its actual consumers.
2. Check its producer/consumer, ABI, ownership, persistence, asynchronous
   completion and teardown contracts where applicable.
3. Establish that it preserves the project's native console path and shared
   interfaces. It must not conceal a missing console service behind different
   host behavior or fabricated success.
4. Review overlaps with the fork's existing implementation and preserved user
   data. Port an applicable fix deliberately; do not import unrelated behavior
   through a broad merge.
5. Validate the affected behavior and regressions with evidence appropriate to
   that requirement. A build or helper test alone does not prove native runtime
   behavior.

These requirements also apply to patches written directly for Afterdark.

## Required decision record

Record the upstream revision/change, affected requirement, decision, reason and
validation evidence. Use the following classifications consistently:

| Decision | Meaning |
| --- | --- |
| Included | Fits the project goals and contracts, and has been integrated and validated for its stated scope. |
| Equivalent | The fork already supplies the required behavior through a compatible, evidenced implementation. |
| Deferred | Potentially fits the goals, but a prerequisite, contract question or required validation remains unresolved. It has not been included. |
| Excluded | Conflicts with the goals, architecture or required behavior, or has another documented reason for rejection. It is not included. |

A known goal conflict must not be treated as temporary deferral merely because
the change exists upstream. Goal conflict is a sufficient exclusion reason;
there is no need to demonstrate that a different product feature is slower or
less useful. Explain the incompatibility without claiming unsupported technical
superiority.

Each finished gate still requires a fresh upstream review and a final recorded
snapshot under the [gate release policy](17559-gate-releases.md).

## Media: restore the console's player

The accepted target is the Xbox 360's native media browsing and playback
experience. Selected host files/folders and explicit managed imports must become
available through the console's storage, catalog, VFS and media-source services.
The native player consumes those services and installed resources/codecs, with
truthful buffers, status codes, ownership and asynchronous completion.

An upstream change that substitutes an external media player or a host playback
window for this console path is excluded. Such playback does not close the HDD
media gate, even if it opens all requested file formats successfully.

Internal audio/codec backends may implement the primitives required by the
guest interfaces. Reusing one does not by itself establish native playback.
Host decoding of a fixture is a diagnostic result, not a native-player pass.

Acceptance must exercise the native console path for viewing/playback, transport
controls, seeking, audio/video output, persistence and Guide handoff. Genuine
missing system files, codecs or update packages remain explicit prerequisites;
an external player cannot be used to bypass them or turn the result into a pass.
