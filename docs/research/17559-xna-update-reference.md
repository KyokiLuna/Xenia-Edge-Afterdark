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
runtime compatibility. Analysis was read-only and memory-only; no duplicate
member binaries or corpus were extracted.

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
must reconstruct the compatible launcher delta, decode the updated native
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
No delta application, new Ghidra/ILSpy run, guest execution, game playability or
performance pass is claimed by this reference inventory. The 20 GiB floor and
private profile/console-key exclusions remain in force.
