# Primary XNA launcher update reference

The supplied `tu32000100_00000000` in
`/home/kyoki/Documents/emulations/360/XNA Title update/584E07D2/000B0000/`
is the version-specific primary reference for Afterdark's XNA work.

## Pinned identity

| Field | Verified value |
| --- | --- |
| Package bytes | 28,303,360 |
| SHA-256 | `2e5a4f44cfd9135a940954474f67bd6100050844c0093ec73a70dfc7d239c249` |
| Container | `LIVE` / STFS |
| Title / media | `584E07D2` / `00000000` |
| Content type | `000B0000` (`XContentType::kInstaller`) |
| Container header / base version | **3.2.1.0** |
| Launcher delta | **3.2.1.0 → 4.0.21021.0** |
| Ten full native modules | **4.0.21021.0** |

The header version does not describe a successfully patched launcher. The
launcher becomes 4.0.21021.0 only after its delta is applied to the compatible
verified base. Presence of the TU file alone does not establish installation,
index selection, application or successful execution.

The [member manifest](sources/xna-tu-4.0.21021/package-manifest.json) records all
73 filenames, sizes, hashes and format/version metadata. All 6,857 allocated
data blocks, 41 level-zero hash tables and the root hash were checked with the
existing STFS reader, including member chains, bounds, paths and duplicates.
This establishes internal content integrity, not publisher RSA verification or
runtime compatibility. The initial inventory was read-only and memory-only;
the later bounded launcher derivation is recorded below.

## Verified launcher derivation

The preserved Metro and NXE Kinect source launchers supply the exact patch
base. Following `KernelState::IsPatchSignatureProper`, SHA-1 of their public
256-byte XEX RSA-signature field equals descriptor digest
`0586e913a1cbf032f1811c082217fec24789c68c`. This comparison is not a hash of the
whole XEX or decoded PE image, and does not verify the publisher signature.

Only `xna_titlelauncher.xexp` was extracted into a new private research folder.
The existing offline patch tool applied it to the verified base and checked
its ten encrypted delta blocks. The target header identifies native PowerPC
title `584E07D2`, version **4.0.21021.0**, base `0x82000000`, entry `0x8204BE48`.
The older mapped launcher's base is `0x92000000`; its addresses must not be
reused. All 2,762 target function extents fit the native text section; 183
unique kernel/XAM import ordinals resolve against the current export tables.

| Derived research output | Bytes | SHA-256 |
| --- | --- | --- |
| Native resident image | 1,507,328 | `a6483b0bcc8003533f350b91fc6a62fac4e67c8ac16bf740b39b603e835bcf60` |
| Target header | 8,192 | `f4e717ce3c60a8086817c00663b742d3cc69d9ef0c05da3b486e33277d02a143` |

These private outputs are decoded research inputs, not a signed/repacked
launcher or an installed runtime. The PE relocation tail exceeds the supplied
native resident span. Plaintext interpretation of page-descriptor hashes is
unresolved for both the known base and target; no page-authentication pass is
claimed. Originals and tools were preserved; new private outputs total about
2.06 MiB. Version-specific Ghidra mapping and runtime validation remain ahead.

## Runtime corpus and analysis order

The TU contains ten full native XEX modules, one launcher delta, 32 managed CLR
PE assemblies and 30 NLP assets, across `Runtime/v3.0`, `v3.1` and `v4.0`.
`HostLoader.dll` and NetCF DLL-suffixed files are native XEX wrappers. Managed
`.dlx` PE machine value `014C` does not make their IL Xbox native x86 code.

The existing mapped dashboard launcher is 3.2.1.0. Its addresses and behavior
cannot be substituted for the target launcher. Only six of the 32 managed TU
binaries exactly match the checked older NXE corpus; the other 26 require
updated analysis. Retain version/folder aliases even when file hashes match.
The separately supplied Game Studio Connect/offline bundle is a distinct
`584E07D1` route, not this `584E07D2` retail launcher update.

Use the exact pin now for HDD catalog, installed-resource and update-lookup
contracts. After the meaningful HDD test AppImage/source sync, the core pass
must use the derived target launcher, decode the updated native
modules, build matching Ghidra maps, and export managed C#/dependency/native-
bridge records with the established workflow. Preserve originals and preflight
space before writing any derived corpus.

Trace installation and TU selection through `KernelState::FindTitleUpdate`
and `LoadTitleUpdate`, the UPDATE mount and module-relative patch resolution.
Trace real dashboard/XBLIG launch data, GameInfo and managed dependency/runtime
resolution. CastleMiner and CastleMiner Z keep their distinct internal product
identities despite sharing launcher host Title ID `584E07D2`.

Then recover profile/storage/avatar services, XMA/audio and graphics consumers
from those binaries, and compare emulator work against the
[standing console-fidelity rule](17559-console-fidelity-policy.md). Preserve real
native failures and later Live prerequisites; do not replace the official
runtime with a host workaround or a title-specific success patch.

## Current limits

The supplied TU and its members are pinned and structurally verified. Current
installed/indexed identity remains unverified after bounded public-root checks.
The bounded offline delta application above passed its stated checks. No new
Ghidra/ILSpy run, guest execution, game playability or performance pass is
claimed by this reference inventory. The 20 GiB floor and
private profile/console-key exclusions remain in force.
