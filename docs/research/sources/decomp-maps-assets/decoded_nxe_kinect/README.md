# NXE Kinect: extracted dashboard research corpus

Start with **XNA_HANDOFF.txt**, **VALIDATION_REPORT.txt**, and **reference/00_INDEX.txt**.

The original source is `/home/kyoki/Downloads/t/NXE Kinect`. Every source and the existing reference corpus
is checked against `dashboard_tools/preservation_baseline.json`.

* Original loose files and verified package members: `_sources/`.
* Decoded files: the same relative paths at this output root; packages remain under `packages/`.
* Original XEX headers: `<module>.source.header`. Failed delta attempts: `_attempts/`.
* UI/media resources: `assets/`; exact provenance, hashes and original keys: `reference/assets.tsv`.
* Module/import/export/call/string/IPC maps: `reference/` and its numbered indexes.
* Open Ghidra: `ghidra/project/decoded_nxe_kinect.gpr`; program aliases: `reference/ghidra_programs.tsv`.
* XNA managed C#: `xna/managed/`; API/native-bridge metadata: `reference/managed_*.tsv`.

All addresses in maps use XEX runtime virtual addresses. PE ImageBase is tracked
separately. Ghidra's image-base property may stay zero because the loader creates
sections directly at runtime addresses; every mapped section is byte-verified.
Analysis imports use an uncompressed XEX envelope to avoid the installed Java LZX
decoder's observed disagreements. Raw independent comparisons remain recorded.
Modules rejected by that loader use a verified flat-image recovery path with
section permissions and unwind-table function bounds restored from the same image.

Identical image/header pairs share one Ghidra program within this dashboard.
No dependency or binary from another dashboard is substituted. Names inferred by
Ghidra and estimated IPC arguments are labeled separately from structural facts.

BDES wrappers and opaque payloads are preserved. Extraction does not authenticate
BDES/RSA signatures. Archive parent-relative keys and duplicate members are stored
under safe names with their original keys retained in the inventory.

Reproduce using the shared runner (working directory does not matter):

```sh
python3 /home/kyoki/Downloads/t/dashboard_tools/dashboard.py kinect --stages extract,decode,resources,maps,managed,ghidra,audit,finalize
```

Stages can be selected independently. Decoding and Ghidra analysis reuse verified
hash-matched results. Decompiler: ILSpyCmd 9.1.0.7988. Native Ghidra analysis uses
two CPU cores, a 2 GiB Java heap, and a 600-second auto-analysis limit per module
(excluding import and optional pseudocode export).
XNA-relevant native programs also contain reference pseudocode in `reference/ghidra/*.c`.

Summary: {"call_references": 342433, "csharp_files": 1679, "decoded_modules": 83, "functions": 134555, "ghidra_executable_files": 77, "ghidra_unique_programs": 61, "inventoried_assets": 20909, "ipc_endpoint_candidates": 758, "managed_assemblies": 8, "package_files": 156, "resource_archives": 172, "resource_members": 20299, "source_files": 253, "string_xrefs": 37802, "unresolved_modules": 19, "xex_modules": 102}
