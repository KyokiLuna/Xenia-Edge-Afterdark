# Content mount ownership and volume identity

Current-source contract, 2026-11-08. Implementation and acceptance remain open.
This work follows the V59 local-profile checkpoint; it does not change or
invalidate that frozen AppImage or its source archive.

## Evidence and scope

V59's actual AppImage created two local profiles with genuine stock avatar
presets and pictures, cancelled another creation, and retained Account, GPD,
header, picture and native appearance bytes across restart. Fifteen direct test
runs passed. The separate current-ELF ordinary-title fixture passed its nine
public results and the traced host-to-native input ownership oracle. Full
native profile creation, Avatar Editor rendering and the HDD gate remain open.

Local evidence, relative to the research `evidence` directory:

- `hdd-gate/root-v59-corrected-checkpoint-acceptance-v1/ACCEPTANCE.json`
- `hdd-gate/root-v59-native-nine-step-oracle-independent-review-v1/`
- `hdd-gate/upstream-storage-mount-cohort-start-v1/`

The following are static findings in the V59 source, not newly observed runtime
crashes:

- `ContentManager::OpenContentFile` holds GCRC while constructing a package.
  Its constructor reaches `RegisterDeviceExclusive`, which explicitly refuses
  an inherited GCRC. The disc-entry branch of `OpenContent` has the same shape.
- `NativeSystemContent::Observe(kLookup)` holds another GCRC across its common
  package mount and TOC read. Fixing only the inner opener leaves that outer
  caller invalid.
- `CloseContent` and descendant closure delete packages and close native files
  under GCRC. Native admission and final native-owner rundown belong outside
  that lock.
- Package constructors currently publish their device and alias before the
  manager inserts the package. Moving construction outside GCRC alone would
  expose a child that a concurrent parent close cannot yet find.
- XAM app message `0x20021` hardcodes HDD for Content mounts and uses title
  deployment for Package mounts. It also accepts an asserted length, an
  unbounded conversion of a 64-byte name, and an unchecked output address.

## Producer and consumer contract

| Boundary | Required ownership and behavior |
| --- | --- |
| Source selection | Retain the exact registry source, generation and read policy. A host path or caller-filled device number cannot replace that selection. |
| Path capture | Resolve aliases and retain the actual mounted parent under short namespace protection. A mutable borrowed Entry must not escape that protection without its own valid lifetime. |
| Read preparation | Open the actual read backing, validate its extent and prepare its device outside GCRC. Retain parent/source ownership through success, failure and cancellation. Do not reopen a different file by a remembered pathname. |
| Namespace publication | Preclaim the root name. Validate the selected source and retained parent, then publish the prepared device, owned alias and manager entry in one short namespace transaction. No visible but untracked child is permitted. |
| Rollback and close | Withdraw only the matching device/alias generation, including descendants. Release native files, backing handles and last source owners after leaving GCRC. Reused names cannot authorize removal of a replacement. |
| Native installed-resource probe | Capture native SYSTEM/module/source identity briefly, perform common mount and TOC I/O outside GCRC, then revalidate before recording success. A successful common TOC read does not admit the native avatar task/provider or prove rendering. |

Preparation is allowed to fail. It must leave the root-name claim, namespace,
manager map and owned backing consistent, with truthful errors and recoverable
original bytes. Unsupported or retired sources never become successful empty
packages. The existing storage admission and 20 GiB filesystem floor remain
authoritative.

## Shared volume descriptor

A device receives its immutable console storage descriptor before admission
and publication. It contains presentation metadata: console device class,
registered device/source identity and generation where known, and an explicit
origin. It is not filesystem access permission, format authority or proof of
current physical attachment. Known source withdrawal may retire its cached
metadata without acquiring native owners or performing observing I/O.

Managed and installed-content mounts take their class from the exact captured
source. A package opened from another guest device inherits that actual
parent's descriptor, including optical media and a secondary USB device.
Unregistered host library packages use an explicit logical-HDD policy; their
physical host disk type is not guessed. A disc title installed on logical HDD
therefore reports HDD regardless of its title deployment value.

The XAM query resolves a bounded alias and longest whole-component mount while
retaining the actual device. It reads only the captured volume metadata. It
does not enumerate sources, query capacity, observe the host filesystem or
borrow a raw Entry during dispatch. Arbitrary unannotated devices, cycles,
stale registrations and malformed requests preserve the caller's output.

Keep the existing 32-bit device-class ABI values: HDD 1, MU 2, System 3, ODD 4,
TransferCable 8, Sapphire 15 and USBMASS 16. Validate the complete request, its
name terminator and writable output before committing a result.

Upstream commit
[30d2dc9](https://github.com/has207/xenia-edge/commit/30d2dc99df0c7a805be88dca53825e5698300d8e)
corrects the deployment/backing confusion by reporting HDD for its known
package mounts. This fork must preserve that installed-disc behavior through
the shared descriptor while also representing its registered secondary devices
and real disc-backed children. The upstream change remains deferred until this
cohort is implemented and tested; it is not classified as inferior.

## Acceptance for this cohort

Use real isolated Emulator/ContentManager/VFS and AppManager paths. Required
cases include primary HDD with optical title deployment, a registered USB
secondary, an actual entry-backed package, nested package closure, alias reuse
and removal, stale source generations, malformed requests and guarded outputs.
Exercise common package opening with the supplied installed AvatarAssetPack
TOC without copying the installed source into user content. Test retained file
ownership, cancellation/failure cleanup and namespace rollback, including the
parent-close race. A helper-only getter test does not prove these paths.

Native asset buffers, geometry, materials, animation, profile wizard/editor
lifecycle and actual game avatar rendering require subsequent acceptance.
Neither this document nor a metadata descriptor enables them.

## Recovered 17559 namespace selection

Authenticated native dispatch `81687C00` → `816830B0` → `8167FDA0` →
`8167B940` establishes that packet DWORD `+0x40` selects the alias namespace.
For a bare root, zero uses `\System??\%s:` and nonzero uses `\??\%s:`.
An explicit `\??\` or `\System??\` prefix is preserved using `%s:`
regardless of the flag. No exactly-zero-or-one restriction is observed.
Capture this canonical namespace on the original call; a deferred worker must
not choose another namespace. Same-named System and title aliases remain
distinct even when they ultimately share one registered source.

The native query obtains device information from the retained mounted package
file rather than title deployment. This static evidence is retained under
`evidence/hdd-gate/root-volume-query-native-namespace-contract-v1/`; common
backend and native runtime acceptance remain open.
