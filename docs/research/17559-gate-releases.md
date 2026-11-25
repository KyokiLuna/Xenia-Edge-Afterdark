# Gate closure and independent fork releases

Requested 2026-11-05. Every completed gate must include an upstream review and a
tested Linux AppImage for the user's normal downtime testing. The project will
remain an independent fork; upstream merge requests are outside its delivery
plan. Public GitHub releases will provide an AppImage and matching `source.zip`.

The required gates are Guide/XUI/shared XAM, HDD/storage, profiles, Live, system
apps and networking apps. Preserve the roadmap's avatar/XNA dependency order;
any separately closed avatar/XNA gate follows this same release contract.
Shared XAM fixes belong to the gate that needs them and remain regression
requirements for later gates.

**Guide/XUI/shared XAM foundation is closed for the local05f0 Linux cohort**,
as far as later HDD/Profile/avatar/XNA/Live/app-service prerequisites permit.
The tested AppImage85fbdd4b has user-reported gameplay/Guide/focus/close acceptance,
automated fresh-GUI startup, final upstream decisions and preserved originals.
See the [native XUI record](17559-native-xui.md) and
[normal local launcher](/home/kyoki/.local/state/xenia-edge-research/binaries/gates/guide-17559-05f0-candidate/start-guide.sh).
Explicit platform/text/service limits and historical failures remain recorded;
a shared-foundation regression reopens repair work. Later gates remain open and
follow the release contract below.

## Current HDD pre-closure review

The user's 2026-11-25 instruction adds a core review before closing the combined
HDD gate. First produce a meaningful tested HDD AppImage and sync its source
to GitHub. Then review XNA, XMA, GPU and CPU behavior under the
[console fidelity and overhead policy](17559-console-fidelity-policy.md),
before moving to Live. The supplied XNA launcher title update is a primary
version-specific reference, alongside the mapped dashboard modules.

Runtime work absent from the console needs a concrete host-emulation or
correctness reason. Disable unnecessary logging, diagnostic capture and helper
work while retaining their source. Do not remove real completion/ownership
requirements or fabricate success to reduce work. After resulting changes,
repeat affected acceptance and package/test/sync the matching final artifacts.
The earlier test AppImage does not itself close the gate or qualify a stable
finished-gate release.

## Upstream review at each gate

The [upstream integration policy](17559-upstream-integration-policy.md) governs
every inclusion decision. Review does not imply adoption: an upstream change
must support Afterdark's accepted goals and system contracts. A conflicting
change is excluded, rather than added as an alternative implementation of a
different goal.

1. Review the latest [has207/xenia-edge](https://github.com/has207/xenia-edge)
   `edge` head, relevant releases, dependency/submodule revisions and build or
   packaging changes. Record retrieval UTC, full commit identities and source
   links. Compare against both the research baseline and the upstream snapshot
   reviewed for the preceding gate.
2. Account for the delta in a short decision table: commit/change, affected area,
   included/equivalent/deferred/excluded, reason, and validation evidence.
   Summarize unrelated changes together without silently omitting them.
3. Integrate only fixes that pass the project-goal and system-contract review,
   through common interfaces while preserving the
   existing dirty source, index and user data. Resolve overlaps deliberately;
   do not blindly merge or reset the working checkout. Rerun checks affected by
   integration, including earlier gate regressions where relevant.
4. Give a concrete reason for each retained difference: conflict with an accepted
   project goal, incompatible ownership contracts, reproduced regression,
   platform scope, missing prerequisite, or
   an equivalent implementation already present. Calling our approach better
   requires observable behavior, correctness or measurements; speculation is
   recorded as uncertainty.
   A goal conflict is sufficient grounds for exclusion; there is no requirement
   to adopt or benchmark functionality that serves a different project goal.
5. Check for a newer upstream head before closing the release. If it moved,
   review the additional delta and record the final snapshot. A release is
   current as of that recorded review, not a promise about future commits.

## AppImage and acceptance

These requirements apply to the final finished-gate packet. A requested earlier
development AppImage must identify its tested scope and unfinished behavior;
it does not substitute for final gate acceptance.

- Close the gate's agreed behavior and regression checks. Record every remaining
  prerequisite or later-system dependency explicitly; unresolved in-scope
  crashes and failures do not become passes because an AppImage can be built.
- Build the accepted source using upstream Release/build tooling and the
  [Linux packaging recipe](../../.github/workflows/build.yml), recording any
  justified deviations. Pin compiler, dependencies, submodules, configuration,
  patches, source archive and packaging-tool identities.
- Budget packaging scratch space and the finished artifact before starting.
  Preserve the absolute **20 GiB free-space floor**. Retain the diagnostic
  executable; package/strip its staged copy without silently replacing it.
- Launch the finished AppImage itself with isolated fresh and copied state.
  Verify the delivered payload's identity, library resolution, startup,
  configuration and the gate's relevant behavior. Native-ELF test results alone
  do not validate the AppImage wrapper or its bundled dependencies.
- Keep MangoHud disabled. Normal releases must not run timed research fixtures
  or force verbose diagnostic capture. Document data/configuration paths and
  required launch settings; preserve existing profiles, saves and system files.
  User-supplied dashboard/game/profile binaries remain outside the package.
- Report the actual host/platform and compatibility limits. Linux packaging
  does not establish macOS execution or another distribution's glibc floor.

## Delivered gate packet

Each gate gets a versioned local release directory containing:

- The executable `.AppImage`, an absolute download link and SHA-256 checksum.
- A matching `source.zip` and checksum, with the exact accepted source and
  dependency/submodule provenance needed to reproduce that AppImage.
- Build/source/configuration provenance and packaged-payload identity.
- The upstream decision table and short release notes explaining what works,
  known limitations, retained differences and any migration behavior.
- A reproducible controller/user checklist, expected results, log location and
  a small guide to reporting downtime-test regressions.

The gate record links that exact packet and its acceptance evidence before work
moves to the next gate. User feedback feeds back into the same gate; a reproduced
foundation regression is repaired rather than hidden by later work. Delivery
does not require the user to acknowledge the release before work can continue,
unless they request a testing hold.

Local gate testing precedes public release. Prepare public GitHub artifacts and
release notes from the same accepted source revision, retaining the fork's
licenses, build instructions and explicit platform limits. Establish the public
repository/release target before publishing. Upstream comparison remains a
maintenance requirement; it does not imply a plan to submit a merge request.
