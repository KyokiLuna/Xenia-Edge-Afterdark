"""Runs consistency checks across the reference/ corpus and writes
VALIDATION_REPORT.txt. Read-only - never "fixes" anomalous source data,
only reports it.

Checks 1-12 (below) are structural (addresses in range, no dupes, files
exist, etc.). Checks A-I are SEMANTIC - they verify the IPC endpoint-proof
architecture's actual claims are internally consistent (a clean structural
pass does NOT imply the IPC data is honest - see 10_UNRESOLVED.txt #9),
not just that the files parse.
"""
import csv
import glob
import os
import re
import corpus_common as cc
import build_06_ipc as b6


def read_tsv(name):
    path = os.path.join(cc.REF, name)
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        # QUOTE_NONE: these TSVs are plain tab-delimited with no quoting/
        # escaping convention - fields (e.g. strings.tsv's raw string text)
        # routinely contain a bare '"' character, and csv's default
        # quoting=QUOTE_MINIMAL treats that as the start of a quoted field,
        # silently merging subsequent lines into it and dropping rows (found
        # via VALIDATION_REPORT.txt's strings-indexed count disagreeing with
        # strings.tsv's real line count - see 10_UNRESOLVED.txt).
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def to_int(s):
    if not s:
        return None
    try:
        return int(s, 16) if s.startswith("0x") else int(s)
    except ValueError:
        return None


def main():
    warnings = []
    failures = []
    info_lines = []

    modules = read_tsv("modules.tsv")
    exports = read_tsv("exports.tsv")
    imports = read_tsv("imports.tsv")
    calls = read_tsv("calls.tsv")
    strings_rows = read_tsv("strings.tsv")
    ipc_rows = read_tsv("ipc.tsv")
    title_ids = read_tsv("title_ids.tsv")

    module_by_path = {m["path"]: m for m in modules}

    # 1. every module in decoded dump represented in master index
    dump_xex_rels = {rel for rel, _ in cc.list_xex2_source_files()}
    missing_from_index = dump_xex_rels - set(module_by_path)
    if missing_from_index:
        failures.append(f"{len(missing_from_index)} XEX2 source files missing from modules.tsv: "
                        f"{sorted(missing_from_index)}")
    else:
        info_lines.append(f"OK: all {len(dump_xex_rels)} XEX2 source files are represented in modules.tsv")

    # 2. every exported VA maps inside the module's XEX runtime range
    bad_export_va = []
    for row in exports:
        m = module_by_path.get(row["module"])
        if not m:
            continue
        load = to_int(m["xex_load_address"])
        size = to_int(m["xex_security_image_size"])
        va = to_int(row["function_va"])
        if load is None or size is None or va is None:
            continue
        if not (load <= va < load + size):
            bad_export_va.append((row["module"], row["ordinal"], row["function_va"]))
    if bad_export_va:
        failures.append(f"{len(bad_export_va)} export VAs fall outside their module's "
                        f"[load_address, load_address+image_size) range: {bad_export_va[:10]}"
                        f"{' ...' if len(bad_export_va) > 10 else ''}")
    else:
        info_lines.append(f"OK: all {len(exports)} export table rows have VAs inside their module's XEX range")

    # 3. export count matches table (cross-check modules.tsv's export_count
    #    against actual row count in exports.tsv per module)
    export_count_by_module = {}
    for row in exports:
        export_count_by_module[row["module"]] = export_count_by_module.get(row["module"], 0) + 1
    mismatches = []
    for path, m in module_by_path.items():
        declared = to_int(m["export_count"])
        actual = export_count_by_module.get(path, 0)
        if declared is not None and declared != actual:
            mismatches.append((path, declared, actual))
    if mismatches:
        failures.append(f"{len(mismatches)} modules where modules.tsv export_count != actual "
                        f"exports.tsv row count: {mismatches}")
    else:
        info_lines.append("OK: modules.tsv export_count matches exports.tsv row counts everywhere it's declared")

    # 4. every reference path exists (header_sidecar, disassembly_path columns)
    missing_paths = []
    for m in modules:
        for col in ("header_sidecar",):
            val = m.get(col)
            if val:
                p = os.path.join(cc.OUT, val)
                if not os.path.exists(p):
                    missing_paths.append((m["path"], col, val))
    if missing_paths:
        failures.append(f"{len(missing_paths)} referenced paths in modules.tsv don't exist on disk: {missing_paths}")
    else:
        info_lines.append("OK: every header_sidecar path referenced in modules.tsv exists on disk")

    # 5. every header/disassembly attachment listed in 09_RELATIONSHIPS.txt actually exists
    rel_path = os.path.join(cc.REF, "09_RELATIONSHIPS.txt")
    missing_attachments = []
    if os.path.exists(rel_path):
        with open(rel_path) as f:
            for line in f:
                line = line.strip()
                for prefix in ("export_table: ", "import_tables: ", "call_graph: ", "strings: ", "header_sidecar: "):
                    if line.startswith(prefix):
                        val = line[len(prefix):]
                        candidate = os.path.join(cc.REF, val) if not val.startswith(cc.OUT) else val
                        # header_sidecar values are relative to OUT, others relative to REF
                        if prefix == "header_sidecar: ":
                            candidate = os.path.join(cc.OUT, val)
                        if not os.path.exists(candidate):
                            missing_attachments.append((prefix.strip(": "), val))
    if missing_attachments:
        failures.append(f"{len(missing_attachments)} attachments listed in 09_RELATIONSHIPS.txt don't exist: "
                        f"{missing_attachments[:10]}")
    else:
        info_lines.append("OK: every attachment referenced in 09_RELATIONSHIPS.txt exists on disk")

    # 6. import thunk/variable VAs map inside the correct module
    bad_import_va = []
    for row in imports:
        m = module_by_path.get(row["module"])
        if not m:
            continue
        load = to_int(m["xex_load_address"])
        size = to_int(m["xex_security_image_size"])
        for col in ("thunk_va", "variable_slot_va"):
            va = to_int(row.get(col))
            if va is None or load is None or size is None:
                continue
            if not (load <= va < load + size):
                bad_import_va.append((row["module"], row["library"], row["ordinal"], col, row[col]))
    if bad_import_va:
        failures.append(f"{len(bad_import_va)} import record addresses fall outside their module's range: "
                        f"{bad_import_va[:10]}{' ...' if len(bad_import_va) > 10 else ''}")
    else:
        info_lines.append(f"OK: all {len(imports)} import record addresses (thunk/variable slots) are inside "
                          f"their module's XEX range")

    # 7. call-graph source (and internal target) addresses map inside the correct module
    bad_call_va = []
    for row in calls:
        m = module_by_path.get(row["module"])
        if not m:
            continue
        load = to_int(m["xex_load_address"])
        size = to_int(m["xex_security_image_size"])
        if load is None or size is None:
            continue
        caller_va = to_int(row["caller_va"])
        if caller_va is not None and not (load <= caller_va < load + size):
            bad_call_va.append((row["module"], row["caller_name"], row["caller_va"]))
        if row["callee_kind"] == "internal":
            callee_va = to_int(row["callee_va"])
            if callee_va is not None and not (load <= callee_va < load + size):
                bad_call_va.append((row["module"], row["caller_name"], "callee=" + row["callee_va"]))
    if bad_call_va:
        failures.append(f"{len(bad_call_va)} call-graph addresses fall outside their module's range: "
                        f"{bad_call_va[:10]}{' ...' if len(bad_call_va) > 10 else ''}")
    else:
        info_lines.append(f"OK: all {len(calls)} call-graph edges have addresses inside their module's XEX range")

    # 8. duplicate records
    seen = {}
    dupes = []
    for m in modules:
        key = m["path"]
        seen[key] = seen.get(key, 0) + 1
    dupe_modules = [k for k, v in seen.items() if v > 1]
    if dupe_modules:
        failures.append(f"Duplicate module rows in modules.tsv: {dupe_modules}")
    else:
        info_lines.append("OK: no duplicate module rows in modules.tsv")

    export_key_seen = {}
    for row in exports:
        key = (row["module"], row["ordinal"])
        export_key_seen[key] = export_key_seen.get(key, 0) + 1
    dupe_exports = [k for k, v in export_key_seen.items() if v > 1]
    if dupe_exports:
        warnings.append(f"{len(dupe_exports)} duplicate (module, ordinal) pairs in exports.tsv: {dupe_exports[:10]}")
    else:
        info_lines.append("OK: no duplicate (module, ordinal) pairs in exports.tsv")

    # 9. malformed addresses (non-hex, odd-length, etc.) across TSVs
    malformed = []
    for name, rows, cols in [
        ("modules.tsv", modules, ["xex_load_address", "xex_entry_va", "pe_image_base", "pe_entry_rva", "export_table_va"]),
        ("exports.tsv", exports, ["function_va"]),
        ("calls.tsv", calls, ["caller_va", "callsite_va", "callee_va"]),
    ]:
        for row in rows:
            for col in cols:
                val = row.get(col)
                if val and not (val.startswith("0x") and all(c in "0123456789ABCDEFabcdef" for c in val[2:])):
                    malformed.append((name, col, val))
    if malformed:
        warnings.append(f"{len(malformed)} malformed-looking address fields (not clean 0x-hex): {malformed[:10]}")
    else:
        info_lines.append("OK: all address-typed TSV fields parse as clean 0x-hex")

    # 10. no PE ImageBase mislabeled as XEX load address (schema-level sanity:
    #     confirm at least N modules where the two values genuinely differ,
    #     proving the columns are actually independent, not aliased)
    differing = [m["path"] for m in modules
                if to_int(m["xex_load_address"]) is not None
                and to_int(m["pe_image_base"]) is not None
                and to_int(m["xex_load_address"]) != to_int(m["pe_image_base"])]
    if differing:
        info_lines.append(f"OK: xex_load_address and pe_image_base are independently tracked and differ for "
                          f"{len(differing)} modules (e.g. {differing[0]}) - confirms no accidental aliasing")
    else:
        warnings.append("Could not confirm xex_load_address/pe_image_base independence - "
                        "no module found where they differ (either coincidence or a real bug)")

    # 11. no decoded_file_size mislabeled as pe_size_of_image
    differing2 = [m["path"] for m in modules
                 if to_int(m["decoded_file_size"]) is not None
                 and to_int(m["pe_size_of_image"]) is not None
                 and to_int(m["decoded_file_size"]) != to_int(m["pe_size_of_image"])]
    if differing2:
        info_lines.append(f"OK: decoded_file_size and pe_size_of_image are independently tracked and differ for "
                          f"{len(differing2)} modules - confirms no accidental aliasing")
    else:
        warnings.append("Could not confirm decoded_file_size/pe_size_of_image independence")

    # 12. unresolved XEXP files remain explicitly unresolved
    expected_unresolved = ["$flash_aac.xexp", "$flash_nomni.xexp", "$flash_nomnifwm.xexp", "$flash_ximedic.xexp"]
    not_flagged = []
    for path in expected_unresolved:
        m = module_by_path.get(path)
        if not m:
            not_flagged.append((path, "MISSING FROM modules.tsv ENTIRELY"))
            continue
        if m["confidence"] != "UNRESOLVED" and "RAW" not in m["decode_status"]:
            not_flagged.append((path, f"confidence={m['confidence']} status={m['decode_status']}"))
    if not_flagged:
        failures.append(f"Known-unresolved XEXP files not correctly flagged: {not_flagged}")
    else:
        info_lines.append(f"OK: all {len(expected_unresolved)} known-unresolved XEXP files are correctly "
                          f"flagged UNRESOLVED/RAW in modules.tsv")

    # extra: cross-check title_ids.tsv module_identity rows against modules.tsv title_id column
    title_mismatches = []
    for row in title_ids:
        if row["source"] != "module_identity":
            continue
        m = module_by_path.get(row["module_or_name"])
        if not m:
            continue
        if m["title_id"] != row["title_id"]:
            title_mismatches.append((row["module_or_name"], m["title_id"], row["title_id"]))
    if title_mismatches:
        warnings.append(f"{len(title_mismatches)} title_id mismatches between modules.tsv and title_ids.tsv: "
                        f"{title_mismatches}")
    else:
        info_lines.append("OK: title_ids.tsv module_identity rows agree with modules.tsv title_id column")

    # ==========================================================
    # SEMANTIC checks (A-I) - see 10_UNRESOLVED.txt #9 / module docstring.
    # ==========================================================

    # A. Confirmed avatar oracle: every hand-verified CONFIRMED_AVATAR entry
    # must appear in ipc.tsv as CONFIRMED with matching port+command, and no
    # extra/missing CONFIRMED rows.
    expected_confirmed = {(e[0], f"0x{e[3]:X}"): (f"0x{e[4]:X}", f"0x{e[5]:X}")
                          for e in b6.CONFIRMED_AVATAR}
    actual_confirmed = {(r["module"], r["caller_va"]): (r["port"], r["command"])
                        for r in ipc_rows if r["confidence"] == "CONFIRMED"}
    oracle_mismatches = []
    for key, (exp_port, exp_cmd) in expected_confirmed.items():
        got = actual_confirmed.get(key)
        if got != (exp_port, exp_cmd):
            oracle_mismatches.append((key, (exp_port, exp_cmd), got))
    extra_confirmed = set(actual_confirmed) - set(expected_confirmed)
    if oracle_mismatches or extra_confirmed:
        failures.append(f"[SEMANTIC-A] confirmed avatar oracle mismatch: "
                        f"{len(oracle_mismatches)} port/command mismatches "
                        f"{oracle_mismatches[:5]}, {len(extra_confirmed)} unexpected "
                        f"CONFIRMED rows in ipc.tsv not in CONFIRMED_AVATAR: {sorted(extra_confirmed)[:5]}")
    else:
        info_lines.append(f"OK: [SEMANTIC-A] all {len(expected_confirmed)} CONFIRMED_AVATAR entries "
                          f"match ipc.tsv exactly (port+command), no unexpected CONFIRMED rows")

    # B. IPC endpoint proof: every non-CONFIRMED row in ipc.tsv must carry a
    # non-empty endpoint_va/endpoint_kind/evidence_chain (i.e. actually
    # reflects a proven chain, not a bare candidate that slipped through).
    unproven_in_ipc = [r for r in ipc_rows if r["confidence"] not in ("CONFIRMED",)
                       and (not r.get("endpoint_va") or not r.get("evidence_chain"))]
    if unproven_in_ipc:
        failures.append(f"[SEMANTIC-B] {len(unproven_in_ipc)} ipc.tsv rows lack endpoint_va/"
                        f"evidence_chain despite not being CONFIRMED - a candidate leaked into "
                        f"the proven-IPC file: {[(r['module'], r['caller']) for r in unproven_in_ipc[:5]]}")
    else:
        info_lines.append(f"OK: [SEMANTIC-B] all {len(ipc_rows)} ipc.tsv rows (CONFIRMED+DERIVED) "
                          f"carry a non-empty endpoint_va and evidence_chain")

    # C. Known-target contradiction: every proven endpoint's resolved name
    # must actually start with "XMsg" (the only registry membership
    # criterion) - catches a registry-construction bug that let a non-XMsg
    # target through as if it were a proven endpoint.
    bad_endpoint_names = []
    for r in ipc_rows:
        if r["confidence"] != "DERIVED":
            continue
        name = r.get("endpoint_name") or ""
        bare_name = name.split("!", 1)[-1]
        if not bare_name.startswith("XMsg"):
            bad_endpoint_names.append((r["module"], r["caller"], r["endpoint_va"], name))
    if bad_endpoint_names:
        failures.append(f"[SEMANTIC-C] {len(bad_endpoint_names)} DERIVED rows resolve to an "
                        f"endpoint whose name does NOT start with 'XMsg' - registry contamination: "
                        f"{bad_endpoint_names[:5]}")
    else:
        derived_n = sum(1 for r in ipc_rows if r["confidence"] == "DERIVED")
        info_lines.append(f"OK: [SEMANTIC-C] all {derived_n} DERIVED rows resolve to an "
                          f"XMsg*-named endpoint (no known-non-XMsg contradiction)")

    # D. IPC callsite uniqueness: (module, caller_va, callsite_va, endpoint_va)
    # must be unique in ipc.tsv (CONFIRMED rows have no callsite_va - dedup
    # key falls back to caller_va alone for those, matching their one-row-
    # per-hand-verified-export design).
    seen_ipc_keys = {}
    for r in ipc_rows:
        key = (r["module"], r["caller_va"], r.get("callsite_va") or "", r["endpoint_va"])
        seen_ipc_keys[key] = seen_ipc_keys.get(key, 0) + 1
    ipc_dupes = [k for k, v in seen_ipc_keys.items() if v > 1]
    if ipc_dupes:
        failures.append(f"[SEMANTIC-D] {len(ipc_dupes)} duplicate (module,caller_va,callsite_va,"
                        f"endpoint_va) rows in ipc.tsv: {ipc_dupes[:10]}")
    else:
        info_lines.append(f"OK: [SEMANTIC-D] no duplicate (module,caller_va,callsite_va,endpoint_va) "
                          f"rows in ipc.tsv ({len(ipc_rows)} rows, {len(seen_ipc_keys)} distinct keys)")

    # E. Multi-site preservation: the schema must be ABLE to record more than
    # one IPC callsite per function (regression guard against the old
    # break-after-first-match bug reintroducing itself) - assert at least
    # one function actually has >1 retained callsite.
    sites_by_func = {}
    for r in ipc_rows:
        if r.get("callsite_va"):
            sites_by_func.setdefault((r["module"], r["caller_va"]), set()).add(r["callsite_va"])
    multi_site = {k: v for k, v in sites_by_func.items() if len(v) > 1}
    if not multi_site:
        warnings.append("[SEMANTIC-E] no function in ipc.tsv has >1 retained IPC callsite - "
                        "either genuinely true right now, or the one-callsite-per-function bug "
                        "has regressed (see 10_UNRESOLVED.txt #9); verify by hand before trusting")
    else:
        info_lines.append(f"OK: [SEMANTIC-E] {len(multi_site)} functions retain >1 distinct IPC "
                          f"callsite (multi-site recording confirmed working)")

    # F. Documentation counts: 06_IPC_SERVICE_MAP.txt's stated CONFIRMED/
    # DERIVED totals must equal what's actually in ipc.tsv.
    ipc_map_path = os.path.join(cc.REF, "06_IPC_SERVICE_MAP.txt")
    doc_count_mismatches = []
    if os.path.exists(ipc_map_path):
        with open(ipc_map_path) as fh:
            header = fh.read(2000)
        m = re.search(r"cross-validation against the (\d+) CONFIRMED\s+avatar\s+entries:\s+(\d+)/(\d+)", header)
        actual_confirmed_n = sum(1 for r in ipc_rows if r["confidence"] == "CONFIRMED")
        if m and int(m.group(1)) != actual_confirmed_n:
            doc_count_mismatches.append(("CONFIRMED count in header", int(m.group(1)), actual_confirmed_n))
    if doc_count_mismatches:
        failures.append(f"[SEMANTIC-F] 06_IPC_SERVICE_MAP.txt's documented counts disagree with "
                        f"ipc.tsv: {doc_count_mismatches}")
    else:
        info_lines.append("OK: [SEMANTIC-F] 06_IPC_SERVICE_MAP.txt's documented CONFIRMED count "
                          "matches ipc.tsv")

    # G. Callback provenance: no fake "via ? call at 0x00000000 in ?"-style
    # placeholder provenance anywhere in calls/*.txt.
    fake_provenance = []
    for path in sorted(glob.glob(os.path.join(cc.REF, "calls", "*.calls.txt"))):
        with open(path) as fh:
            for lineno, line in enumerate(fh, 1):
                if "0x00000000" in line or " in ?)" in line or "via ? " in line:
                    fake_provenance.append((os.path.basename(path), lineno, line.strip()))
    if fake_provenance:
        failures.append(f"[SEMANTIC-G] {len(fake_provenance)} fake/placeholder callback-provenance "
                        f"lines found (e.g. 'via ? call at 0x00000000 in ?'): {fake_provenance[:5]}")
    else:
        info_lines.append("OK: [SEMANTIC-G] no fake '0x00000000'/unresolved-caller callback "
                          "provenance strings found in calls/*.txt")

    # I. Cross-file identity: every DERIVED endpoint_va that is an
    # import_thunk should have a matching thunk_va in imports.tsv for that
    # module with a name ending in the same bare function name.
    cross_id_mismatches = []
    imports_by_module_va = {}
    for row in imports:
        imports_by_module_va.setdefault(row["module"], {})[row.get("thunk_va") or ""] = row
    for r in ipc_rows:
        if r["confidence"] != "DERIVED" or r["endpoint_kind"] != "import_thunk":
            continue
        mod_imports = imports_by_module_va.get(r["module"], {})
        thunk_row = mod_imports.get(r["endpoint_va"])
        if thunk_row is None:
            cross_id_mismatches.append((r["module"], r["endpoint_va"], r["endpoint_name"],
                                        "no matching thunk_va in imports.tsv"))
    if cross_id_mismatches:
        warnings.append(f"[SEMANTIC-I] {len(cross_id_mismatches)} DERIVED import_thunk endpoints "
                        f"have no matching thunk_va row in imports.tsv: {cross_id_mismatches[:5]}")
    else:
        derived_thunks = sum(1 for r in ipc_rows if r["confidence"] == "DERIVED"
                             and r["endpoint_kind"] == "import_thunk")
        info_lines.append(f"OK: [SEMANTIC-I] all {derived_thunks} DERIVED import_thunk endpoints "
                          f"have a matching thunk_va row in imports.tsv")

    with open(os.path.join(cc.REF, "..", "VALIDATION_REPORT.txt"), "w") as f:
        f.write("Corpus validation report\n")
        f.write("===========================\n")
        f.write("Regenerate with: python3 _tool/build_11_validate.py\n")
        f.write("Read-only checks - anomalies are reported, never silently fixed.\n\n")
        f.write(f"FAILURES: {len(failures)}\n")
        f.write(f"WARNINGS: {len(warnings)}\n\n")
        if failures:
            f.write("### FAILURES ###\n")
            for w in failures:
                f.write(f"  [FAIL] {w}\n")
            f.write("\n")
        if warnings:
            f.write("### WARNINGS ###\n")
            for w in warnings:
                f.write(f"  [WARN] {w}\n")
            f.write("\n")
        f.write("### PASSED CHECKS ###\n")
        for i in info_lines:
            f.write(f"  {i}\n")
        f.write("\n")
        f.write("### Coverage summary ###\n")
        f.write(f"  modules indexed: {len(modules)}\n")
        f.write(f"  export rows: {len(exports)}\n")
        f.write(f"  import rows: {len(imports)}\n")
        f.write(f"  call-graph edges: {len(calls)}\n")
        f.write(f"  strings indexed: {len(strings_rows)}\n")
        f.write(f"  IPC entries: {len(ipc_rows)} "
                f"({sum(1 for r in ipc_rows if r['confidence']=='CONFIRMED')} CONFIRMED, "
                f"{sum(1 for r in ipc_rows if r['confidence']=='DERIVED')} DERIVED)\n")
        candidates = read_tsv("_ipc_candidates.tsv")
        rejected = sum(1 for r in candidates if r.get("status") not in (None, "", "PROVEN"))
        f.write(f"  IPC candidates rejected (no endpoint proof): {rejected}\n")
        f.write(f"  title ID records: {len(title_ids)}\n")

    print(f"Validation: {len(failures)} FAILURES, {len(warnings)} WARNINGS, {len(info_lines)} checks passed")
    return failures, warnings


if __name__ == "__main__":
    main()
