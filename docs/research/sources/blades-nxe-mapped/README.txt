decoded_dump/ — Xbox 360 NXE/Blades ("9199") reverse-engineering corpus
=========================================================================

What this is
-------------
A decrypted/decompressed mirror of a real Xbox 360 dashboard flash dump
(NXE/"Blades", dashboard build 2.0.9199.0 — see AvatarEditor.xex's
module_version in reference/00_INDEX.txt, and version_min=2.0.9199.0 on
several imports in reference/imports/*.txt), PLUS a structured reference/
corpus built on top of it: normalized module metadata, export/import
tables, a bounded call graph, keyword-filtered strings with xrefs, an
IPC/system-message service map, a title-ID index, a structures notebook,
and a relationship map — everything cross-linked so you can start from any
known item (a function name, ordinal, address, IPC port/command, title ID,
module name, string, or structure offset) and quickly find its module,
address, callers/callees, dependencies, and related artifacts.

The original encrypted dump lives at
"../Documents/xbox dash/Blades + NXE Classic/" (NOTE: this moved from an
earlier "Downloads/xenia_edge/Blades + NXE Classic/" path during this
session's environment reset — decoded_dump/_tool/corpus_common.py's DUMP
constant points at the current location; update it there if it moves
again). Neither that original dump nor decoded_dump/ itself is modified by
the reference/ tooling — reference/ is purely additive, safe to delete and
regenerate at any time.

decoded_dump/ relationship to the original dump
--------------------------------------------------
Every file in decoded_dump/ mirrors the corresponding file in the original
dump at the same relative path, except XEX2 modules have been
AES-decrypted and LZX-decompressed (using Xenia's own vendored crypto/
compression code and fixed retail keys — see "How the decoder works"
below), so they can be opened directly in a disassembler or parsed by the
reference/ tooling without needing Xenia itself or any key material at
hand.

XEX runtime VA vs PE ImageBase — READ THIS FIRST
----------------------------------------------------
Every address in this corpus (exports, imports, call graph, IPC map,
strings, structures) is in **XEX runtime virtual address space**
(`xex_load_address` / `xex_entry_va` in reference/modules.tsv — sourced
from xex2_security_info.load_address and the XEX_HEADER_ENTRY_POINT
optional header). This is the address space the Xbox 360 kernel actually
maps the module at, and the one Xenia itself uses.

This is a **different concept** from the standalone PE32 header's own
`ImageBase`/`AddressOfEntryPoint` fields (`pe_image_base` / `pe_entry_rva`
in modules.tsv), which the Xbox toolchain also embeds inside the decoded
body. These often coincide for un-relocated system modules but are NOT
guaranteed to, and reference/modules.tsv deliberately tracks them as
separate columns — VALIDATION_REPORT.txt has an explicit check confirming
they're never silently aliased (they differ for 41 of the 55 indexed
modules).

If you're navigating by address, use `xex_load_address`/exports/imports/
calls — those are all in the same coordinate system. If you open a module
in a raw PE-only tool that reports `pe_image_base` instead, translate:
`xex_va = pe_relative_offset + xex_load_address` (NOT `+ pe_image_base`).

reference/ file index
------------------------
  00_INDEX.txt / modules.tsv        - one record per module: status, XEX
                                       load address vs PE ImageBase (kept
                                       separate — see above), entry point,
                                       decoded file size vs PE SizeOfImage
                                       vs XEX security image_size (three
                                       DIFFERENT fields, kept separate),
                                       title_id/version, export table,
                                       base/patch relationship, sha256 of
                                       both original and decoded bytes, and
                                       a confidence label per row.
  01_EXPORT_TABLES_INDEX.txt /
  exports/<module>.exports.txt /
  exports.tsv                       - full ordinal -> function VA export
                                       tables for every module that has
                                       one. xam.xex / $flash_xam.xexp
                                       ordinals are resolved to real names
                                       against Xenia's own xam_table.inc
                                       (CONFIRMED); other modules are
                                       ordinal+VA only (no comparable name
                                       database exists for them here).
  02_AVATAR_COMMAND_TABLE.txt       - the original hand-verified avatar
                                       IPC command table (pre-dates the
                                       generalized 06_IPC_SERVICE_MAP.txt;
                                       kept as-is since every entry is
                                       CONFIRMED, not regenerated).
  03_IMPORT_TABLES_INDEX.txt /
  imports/<module>.imports.txt /
  imports.tsv                       - structural parse of every module's
                                       XEX import-library tables (variable
                                       slot + thunk VA per ordinal), names
                                       resolved for xam.xex/xboxkrnl.exe/
                                       xbdm.xex imports (Xenia ships
                                       ordinal databases for those three
                                       only — everything else is
                                       ordinal-only, not guessed).
  04_CALL_GRAPH_INDEX.txt /
  calls/<module>.calls.txt /
  calls.tsv                         - bounded call graph, in
                                       corpus_common.PRIORITY_MODULES only
                                       — internal/imported callees, known
                                       callers, tail calls. Two coverage
                                       modes: modules with >=20 exports
                                       (xam.xex/$flash_xam.xexp) use
                                       export-boundary-only disassembly
                                       (boundary = next export's VA);
                                       modules with fewer exports (the
                                       application-style modules —
                                       gamerprofile/createprofile/
                                       deviceselector/signin/dash/
                                       AvatarEditor/AvatarMiniCreator) also
                                       get a capped (250-function) BFS walk
                                       of internal `bl` targets from the
                                       entry point, PLUS one-hop expansion
                                       through function-pointer arguments
                                       passed to *Register*-style imports
                                       (e.g. XamRegisterSysApp's callback).
                                       Neither boundary method is verified
                                       prologue/epilogue analysis — see the
                                       per-function `discovery:` line in
                                       each calls/*.txt file for exact
                                       provenance (export / bfs_root /
                                       bfs_reachable / callback_root /
                                       callback_descendant, the last two
                                       carrying the real registering import
                                       + callsite, never a fake "0x00000000"
                                       placeholder).
  05_STRINGS_INDEX.txt /
  strings/<module>.strings.txt /
  strings.tsv                       - keyword-filtered strings (avatar,
                                       gamer/profile, storage, content,
                                       title, XNA/XBLIG/XBLA, licensing,
                                       sign-in, device, marketplace,
                                       XNet/network, errors/status, paths/
                                       mount names, system apps — see
                                       build_05_strings.py's KEYWORDS) with
                                       VA/file-offset. xrefs (code that
                                       loads a string's address) only
                                       computed for PRIORITY_MODULES,
                                       INFERRED confidence (pattern-match
                                       heuristic, not full data-flow).
  06_IPC_SERVICE_MAP.txt / ipc.tsv  - system-message/IPC dispatch map,
                                       covering every PRIORITY_MODULES
                                       module, not just xam.xex. ONLY rows
                                       with a statically-proven evidence
                                       chain to a registered XMsg* endpoint
                                       (a local export or import thunk whose
                                       real, ordinal-database-resolved name
                                       starts with "XMsg" — never a
                                       hardcoded function list) appear here
                                       — see build_06_ipc.py's module
                                       docstring for the full
                                       endpoint-proof architecture. CONFIRMED
                                       rows are the 23 hand-verified avatar
                                       entries (port+command hand-read from
                                       raw disassembly; their ipc_args/
                                       caller_args trailing-register fields
                                       are mechanically recomputed by the
                                       same validated engine — see
                                       08_STRUCTURES.txt). DERIVED rows are
                                       proven by disasm_support.py's bounded
                                       find_endpoint_chain (depth<=4,
                                       node_cap<=40) plus hop-by-hop
                                       argument resolution — 279 total
                                       entries (23 CONFIRMED + 256 DERIVED).
                                       A candidate whose `li r3,port; ...;
                                       bl target` shape does NOT statically
                                       reach a registered endpoint is
                                       REJECTED — not written here at all,
                                       only to _ipc_candidates.tsv with
                                       confidence=CANDIDATE (91 rejected in
                                       the current corpus, including every
                                       named false-positive this
                                       architecture specifically catches —
                                       see 10_UNRESOLVED.txt #9). Every
                                       distinct `li r3,port;...;bl` callsite
                                       is its own row (no one-per-function
                                       cap) — 6 functions currently retain
                                       >1 IPC callsite. The chain-proof
                                       engine self-validates against every
                                       CONFIRMED entry on every run
                                       (currently 23/23 exact matches — see
                                       the top of 06_IPC_SERVICE_MAP.txt).
                                       Ports found: 0xF3/0xFE (avatar
                                       family), plus others discovered on
                                       dash.xex/$flash_dash.xex, signin.xex/
                                       $flash_signin.xexp, and
                                       deviceselector.xex — see
                                       06_IPC_SERVICE_MAP.txt for the
                                       current full breakdown. gamerprofile.
                                       xex/createprofile.xex show no
                                       port/command IPC path found in
                                       current static coverage (not the same
                                       as "these modules never do IPC" —
                                       see 10_UNRESOLVED.txt #7's "DO NOT
                                       OVER-PROMOTE NEGATIVE RESULTS" note):
                                       they register a callback via
                                       XamRegisterSysApp instead (see
                                       reference/calls/
                                       gamerprofile.xex.calls.txt).
  07_TITLE_IDS.txt / title_ids.tsv  - every module's own title_id
                                       (structural, CONFIRMED), cross-
                                       referenced against Xenia's
                                       title_id_utils.h constants (e.g.
                                       AvatarEditor.xex's title_id reads
                                       back as EXACTLY 0x584D07D1 ==
                                       kAvatarEditorID — a real structural
                                       confirmation of that constant, not
                                       an assumption), plus in-code
                                       comparisons against known IDs
                                       (INFERRED, PRIORITY_MODULES only).
                                       A SUSPECT_METADATA section flags
                                       reconstructed-sidecar title_ids that
                                       don't cross-validate — see
                                       10_UNRESOLVED.txt #5.
  08_STRUCTURES.txt                 - hand-maintained structure/offset
                                       notebook (NOT auto-regenerated).
                                       Only records patterns actually
                                       observed with evidence — no
                                       fabricated structs.
  09_RELATIONSHIPS.txt              - for any module, every other artifact
                                       in this corpus related to it
                                       (sidecar, base<->patch pairing,
                                       export/import/call/string dumps).
  10_UNRESOLVED.txt                 - hand-maintained anomaly register
                                       (NOT auto-regenerated). The 4
                                       still-encrypted XEXP files, the
                                       SUSPECT_METADATA sidecar issue, and
                                       known gaps in the IPC/call-graph
                                       coverage, each with what was tried,
                                       why it stopped, and what would
                                       resolve it.
  exports.tsv / imports.tsv /
  calls.tsv / strings.tsv /
  ipc.tsv / title_ids.tsv /
  modules.tsv                       - TSV mirrors of the above, stable
                                       column names, grep/awk/pandas-
                                       friendly. Human-readable .txt is the
                                       primary source; TSVs are generated
                                       from the same underlying scan.
  _ipc_candidates.tsv               - written by build_06_ipc.py (NOT
                                       build_04_calls.py — build_04 no
                                       longer writes any IPC-candidate
                                       output). The single source of truth
                                       for every `li r3,port;...;bl target`
                                       candidate found, proven or not:
                                       rejected candidates carry a status
                                       (UNRESOLVED_NO_ENDPOINT /
                                       ENDPOINT_FOUND_BUT_ARGS_UNRESOLVED)
                                       and a reason; candidates that WERE
                                       proven are also listed here (status
                                       PROVEN) for completeness, alongside
                                       their full row in ipc.tsv.

VALIDATION_REPORT.txt (in decoded_dump/, not reference/) — automated
consistency checks: every module represented in the index, every export/
import/call address falls inside its module's XEX range, export counts
match their tables, every referenced path exists, no duplicate records, no
malformed addresses, PE-ImageBase/XEX-load-address and decoded-size/
PE-SizeOfImage are confirmed genuinely independent (not accidentally
aliased), and the 4 known-unresolved XEXP files are confirmed still
flagged unresolved. Regenerate with `_tool/build_11_validate.py`; anomalies
are reported, never silently "fixed".

How to trace an export from ordinal to calls/imports/IPC
--------------------------------------------------------------
1. Look up the ordinal's name in reference/exports/<module>.exports.txt
   (or reference/exports.tsv, filter by module+ordinal).
2. Note its function_va.
3. Search reference/calls/<module>.calls.txt for that VA (or its name) to
   see internal/imported callees and known callers.
4. Cross-check imported callees against reference/imports/<module>.imports.txt
   for the resolved kernel/XAM API name.
5. If it's an avatar-family or xam.xex/$flash_xam.xexp export, check
   reference/06_IPC_SERVICE_MAP.txt (and 02_AVATAR_COMMAND_TABLE.txt for
   the hand-verified subset) for the system-message port/command it sends.
6. Check reference/strings/<module>.strings.txt for any string this
   function's disassembly window references (xref-matched for priority
   modules).
7. Check reference/09_RELATIONSHIPS.txt for the module's full artifact set
   (raw disassembly, source-code cross-references already made, etc.)

How the decoder works (xextool.c)
--------------------------------------
A standalone C tool built from Xenia's own vendored, already-open-source
crypto/decompression code:
  - AES: xenia-edge/third_party/crypto/rijndael-alg-fst.c
  - LZX: xenia-edge/third_party/mspack/lzxd.c
  - Retail/devkit/xex1 AES keys + the ReadImage*/ApplyPatch algorithm:
    xenia-edge/src/xenia/cpu/xex_module.cc (ported line-for-line)
These are the same fixed keys/algorithms Xenia itself already uses
internally to load any retail Xbox 360 XEX2 module — nothing here is
specific to this dump or bypasses anything beyond what Xenia already does
at runtime. Build with `_tool/build.sh` (needs a xenia-edge checkout next
to this dump).

Reconstructed XEXP (DELTA patch) modules
---------------------------------------------
$flash_*.xexp files are DELTA patches, not full modules — the Xbox stores
"flash-resident" dashboard modules as a small binary diff against the
plain on-disk module of the same name. 11 of 15 were successfully
depatched against their matching base module and are full plain PE images:
  $flash_bootanim.xexp, $flash_createprofile.xexp, $flash_deviceselector.xexp,
  $flash_gamerprofile.xexp, $flash_hud.xexp, $flash_mfgbootlauncher.xexp,
  $flash_minimediaplayer.xexp, $flash_signin.xexp, $flash_updater.xexp,
  $flash_vk.xexp, $flash_xam.xexp

Each successfully depatched module has a "<name>.header" sidecar: the
reconstructed (post-patch) raw XEX header for that image (different
security_offset/image_size/export_table_va/aes_key than its base module's
own header — tools parsing the depatched body's optional headers MUST read
the sidecar, not the base module's header). Load address for these is
taken from the base module (these patches don't relocate) — see
corpus_common.py:decoded_body_meta() for the exact fallback logic, and
10_UNRESOLVED.txt #5 for a known gap in sidecar execution_info reliability.

Known unresolved files (see 10_UNRESOLVED.txt for full detail)
--------------------------------------------------------------------
  $flash_aac.xexp        - no base module anywhere in the dump (confirmed
                           by load-address scan, not just filename).
  $flash_nomni.xexp      - same, no base at load address 0x91090000.
  $flash_nomnifwm.xexp   - same, no base at load address 0x91070000.
  $flash_ximedic.xexp    - base (XIMEDIC.XEX) present, header-patch
                           decompression confirmed correct, but
                           image_size/page_descriptor_count read back as
                           0 post-patch for an unidentified reason.

A handful of XEX2-wrapped files decode to non-PE content ON PURPOSE, not
by bug: huduiskin.xex / $flash_huduiskin.xex (XUIZ-magic compiled UI skin
resources) and XIMEDIC.XEX / XIMEDICCH.XEX / XIMEDICEX.XEX (D100-magic IME
dictionary blobs). Verified via LZX block-SHA1 match against the
header-stored hash, not just eyeballing the output — `file` correctly
reports these as "data". reference/00_INDEX.txt tags these with
`content=... (NOT executable code)`.

Reference confidence labels (used throughout reference/)
--------------------------------------------------------------
  CONFIRMED         - structurally parsed from headers, or hand-verified
                      against real disassembly/decompilation, or matches
                      an independent source (e.g. Xenia's own ordinal
                      tables or title_id_utils.h constants).
  INFERRED          - derived via a documented heuristic (pattern-match
                      disassembly scan, fallback value) — plausible, not
                      independently verified.
  DERIVED           - IPC-map-specific (ipc.tsv/06_IPC_SERVICE_MAP.txt
                      only): stronger than INFERRED — a STATICALLY-PROVEN
                      evidence chain reaches a registered XMsg* endpoint
                      (see build_06_ipc.py's docstring and
                      10_UNRESOLVED.txt #9), not just a plausible-looking
                      pattern match. Individual command words within
                      DERIVED rows still haven't been independently
                      hand-verified/semantically named one-by-one, which is
                      why this is a distinct tier from CONFIRMED. A
                      candidate that does NOT reach a proven endpoint is
                      never labeled DERIVED — it's rejected to
                      _ipc_candidates.tsv as CANDIDATE instead.
  UNKNOWN           - field legitimately absent/not applicable for this
                      module (e.g. no export table).
  UNRESOLVED        - file still encrypted/compressed; nothing to analyze.
  SUSPECT_METADATA  - a specific, named data-quality concern was found and
                      is documented (see 10_UNRESOLVED.txt) — do not treat
                      the flagged field as fact without further work.

Regeneration procedure
-------------------------
Every reference/ file except 02_AVATAR_COMMAND_TABLE.txt, 08_STRUCTURES.txt,
and 10_UNRESOLVED.txt (hand-maintained notebooks) is regenerated from
scratch by re-running its build_NN_*.py script in decoded_dump/_tool/, in
this order (each depends only on decoded_dump/ + the original dump's
headers, not on any other script's output):

  python3 _tool/build_01_modules.py       # 00_INDEX.txt, modules.tsv
  python3 _tool/build_02_exports.py       # 01_EXPORT_TABLES_INDEX.txt, exports/, exports.tsv
  python3 _tool/build_03_imports.py       # 03_IMPORT_TABLES_INDEX.txt, imports/, imports.tsv
  python3 _tool/build_04_calls.py         # 04_CALL_GRAPH_INDEX.txt, calls/, calls.tsv
  python3 _tool/build_05_strings.py       # 05_STRINGS_INDEX.txt, strings/, strings.tsv (reuses build_04's function set)
  python3 _tool/build_06_ipc.py           # 06_IPC_SERVICE_MAP.txt, ipc.tsv, _ipc_candidates.tsv (reuses build_04's function set)
  python3 _tool/build_07_titleids.py      # 07_TITLE_IDS.txt, title_ids.tsv
  python3 _tool/build_09_relationships.py # 09_RELATIONSHIPS.txt
  python3 _tool/build_11_validate.py      # VALIDATION_REPORT.txt

All of these are pure-Python (stdlib + capstone) and read directly from
decoded_dump/ and the original dump — none of them need Xenia to be built,
only the xenia-edge source tree to exist (for ordinal tables / title ID
constants / xex2_info.h structure references baked into corpus_common.py).
_tool/corpus_common.py's DUMP/XENIA constants are the only paths that might
need updating if either tree moves.

_avatar_disasm/ — raw disassembly output + the Capstone script
(disasm_avatar.py) from the original hand-verification pass this whole
corpus builds on. Kept as raw evidence, not regenerated.
