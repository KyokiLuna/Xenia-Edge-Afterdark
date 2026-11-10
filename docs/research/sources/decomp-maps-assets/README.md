# Mapped Metro and NXE Kinect research sources

Registered 2026-11-07 from the user-supplied ready corpus at
`/home/kyoki/Documents/xbox dash/Decomp+maps+assets/`.
The complete reference folders and accompanying top-level text files are kept
in this repository: `decoded_metro/`, `decoded_nxe_kinect/`, and the existing
`../blades-nxe-mapped/` snapshot. `reference-snapshot-pins.json` records all 909
source/destination matches. The copy added 771 files and reused 138 identical
blades/NXE files, preserving the original trees.

Read binary assets and Ghidra projects in place. `source-pins.json` records
selected metadata and module identities; it does not contain retail payloads.

The decoded Metro XAM and dashboard match the previously frozen 17559 images
byte for byte. Use `decoded_metro/` as the primary build domain. The decoded
NXE Kinect XAM/dashboard are 2.0.12581.0 and form a separate comparison domain.
Maps use XEX runtime virtual addresses. PE ImageBase and the Ghidra image-base
property are distinct; several modules use verified flat-image recovery after
an independent loader mismatch. Do not transfer old-build addresses or inferred
structure layouts without checking the actual module.

| Work | Entry points beneath the registered root |
| --- | --- |
| Provenance and limits | Each decoded tree: README.md, VALIDATION_REPORT.txt, module_metadata.json, ghidra_manifest.json, reference/10_UNRESOLVED.txt |
| Native code and callers | reference/modules.tsv, functions.tsv, imports.tsv, exports.tsv, calls.tsv; per-program reference/ghidra/ files; ghidra/project/ |
| Native notification/XUI/SYSTEM | Metro packages/su20076000_00000000/$flash_xam.xex and $flash_hud.xex; XAM program 342c8fdca6d4__flash_xam.xex |
| Storage and account selection | Metro $flash_dash.xex, $flash_deviceselector.xex, $flash_signin.xex, $flash_createprofile.xex, $flash_gamerprofile.xex under that package; reference/06_IPC_SERVICE_MAP.txt and ipc.tsv |
| Avatars | Metro AvatarEditor.xex, Guide.AvatarMiniCreator.xex; reference/02_AVATAR_COMMAND_TABLE.txt, ipc.tsv, imports.tsv and assets.tsv |
| Installed resources | assets/, packages/; reference/assets.tsv preserves original keys, offsets, hashes and provenance |
| Media | Metro dash.ExtraAVCodecs.xex, dash.ExtraAVCodecs2.xex, $flash_minimediaplayer.xex; matching import/caller/resource maps |
| Later XNA work | Each tree's XNA_HANDOFF.txt; Metro Xna_TitleLauncher.xex and Xam.Community.xex pseudocode in reference/ghidra/; NXE runtime/v3.0, xna/managed and managed_*.tsv |

IPC command/argument candidates and Ghidra signatures are inferred navigation
material, not verified ABI declarations. Reachability and source byte identity
are stronger, separately recorded evidence. Opaque avatar/media/font assets are
preserved originals; extraction does not imply a working decoder or renderer.
Metro has no supplied managed XNA/runtime assemblies; the separate NXE corpus
must not conceal a missing 17559 prerequisite.

The existing blades/NXE reference snapshot remains under
`docs/research/sources/blades-nxe-mapped/`. No extraction/decompilation runner was
rerun to register this corpus. Historical paths inside generated files point at
`/home/kyoki/Downloads/t`; resolve by relative module path against the new root
and verify the recorded hash.
