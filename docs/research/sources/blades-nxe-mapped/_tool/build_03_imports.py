"""Builds reference/03_IMPORT_TABLES_INDEX.txt, reference/imports/<module>.txt,
and reference/imports.tsv.

Structural parse of xex2_opt_import_libraries (thunk/variable record pairs),
cross-resolved against Xenia's xam/xboxkrnl/xbdm ordinal tables where the
imported library is one of those three kernel-side modules. Call-site
discovery (which function calls which thunk) is left to build_04_calls.py,
which fills in the "callsites" column for PRIORITY_MODULES only.
"""
import os
import corpus_common as cc


def main():
    ordinal_tables = cc.load_ordinal_tables()
    os.makedirs(os.path.join(cc.REF, "imports"), exist_ok=True)

    index_lines = []
    tsv_rows = []
    total_imports = 0
    total_modules_with_imports = 0

    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        if "RAW" in info["status"] or not os.path.exists(info["dst"]):
            continue
        with open(info["dst"], "rb") as f:
            body = f.read()
        try:
            libs = cc.parse_import_libraries(info["header_bytes"], body, info["load_address"])
        except Exception as e:
            print(f"WARN: import parse failed for {rel}: {e}")
            continue
        if not libs:
            continue

        total_modules_with_imports += 1
        out_name = rel.replace("/", "__").replace("$", "") + ".imports.txt"
        out_path = os.path.join(cc.REF, "imports", out_name)
        with open(out_path, "w") as f:
            f.write(f"Import libraries for {rel}\n")
            f.write(f"xex_load_address=0x{info['load_address']:08X}\n\n")
            for lib in libs:
                f.write(f"{lib['name']}  (id=0x{lib['id']:X}  version={lib['version']}  "
                        f"version_min={lib['version_min']}  count={lib['count']})\n")
                # Group by ordinal: variable record + thunk record share an
                # ordinal (see xex_module.cc:SetupLibraryImports).
                by_ordinal = {}
                for imp in lib["imports"]:
                    if imp["ordinal"] is None:
                        continue
                    by_ordinal.setdefault(imp["ordinal"], {})
                    if imp["record_type"] == 0:
                        by_ordinal[imp["ordinal"]]["variable_addr"] = imp["record_addr"]
                    elif imp["record_type"] == 1:
                        by_ordinal[imp["ordinal"]]["thunk_addr"] = imp["record_addr"]
                for ordinal in sorted(by_ordinal):
                    entry = by_ordinal[ordinal]
                    name = cc.resolve_import_name(ordinal_tables, lib["name"], ordinal)
                    conf = "CONFIRMED (Xenia ordinal db)" if name else "UNKNOWN (no ordinal db match)"
                    f.write(f"  ordinal 0x{ordinal:04X}  name={name or '?'}  [{conf}]\n")
                    if "variable_addr" in entry:
                        f.write(f"    variable_slot_va=0x{entry['variable_addr']:08X}\n")
                    if "thunk_addr" in entry:
                        f.write(f"    thunk_va=0x{entry['thunk_addr']:08X}  (callsites: see calls/ for priority modules)\n")
                    total_imports += 1
                    tsv_rows.append((
                        rel, lib["name"], f"0x{ordinal:04X}", name or "",
                        f"0x{entry.get('thunk_addr', 0):08X}" if "thunk_addr" in entry else "",
                        f"0x{entry.get('variable_addr', 0):08X}" if "variable_addr" in entry else "",
                        "CONFIRMED" if name else "UNKNOWN",
                    ))
                f.write("\n")

        index_lines.append(f"{rel}  ->  reference/imports/{out_name}  "
                           f"({sum(l['count'] for l in libs)} import records, "
                           f"{len(libs)} libraries: {', '.join(l['name'] for l in libs)})")

    with open(os.path.join(cc.REF, "03_IMPORT_TABLES_INDEX.txt"), "w") as f:
        f.write("Which decoded modules have parsed import-library tables\n")
        f.write("===========================================================\n")
        f.write("Regenerate with: python3 _tool/build_03_imports.py\n\n")
        f.write("Structural parse of each module's XEX_HEADER_IMPORT_LIBRARIES\n")
        f.write("optional header (xex2_import_library records). Each import is a\n")
        f.write("(variable_slot, thunk) pair sharing an ordinal - see\n")
        f.write("xenia-edge/src/xenia/cpu/xex_module.cc:SetupLibraryImports for the\n")
        f.write("original semantics this was reverse-engineered against (that's\n")
        f.write("Xenia's own loader code, so CONFIRMED, not guessed).\n\n")
        f.write("Names are resolved ONLY when the imported library is xam.xex,\n")
        f.write("xboxkrnl.exe, or xbdm.xex (Xenia ships ordinal->name databases\n")
        f.write("for those three). Imports of anything else (e.g. one dashboard\n")
        f.write("module importing from another) are ordinal-only - do not invent\n")
        f.write("names for them.\n\n")
        for line in sorted(index_lines):
            f.write(line + "\n")

    with open(os.path.join(cc.REF, "imports.tsv"), "w") as f:
        f.write("module\tlibrary\tordinal\tresolved_name\tthunk_va\tvariable_slot_va\tconfidence\n")
        for row in tsv_rows:
            f.write("\t".join(row) + "\n")

    print(f"Indexed imports for {total_modules_with_imports} modules, {total_imports} import records")


if __name__ == "__main__":
    main()
