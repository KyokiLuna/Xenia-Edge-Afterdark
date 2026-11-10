"""Regenerates reference/01_EXPORT_TABLES_INDEX.txt, reference/exports/*.txt,
and reference/exports.tsv. Structural only - reads export tables, resolves
names against Xenia's own ordinal databases where the module is a known
kernel-side component (xam/xboxkrnl/xbdm); otherwise leaves name blank.
"""
import os
import corpus_common as cc


def read_export_table(dst, load_address, export_table_va):
    with open(dst, "rb") as f:
        body = f.read()
    et_off = export_table_va - load_address
    if et_off + 0x2C > len(body):
        return None
    imagebaseaddr = cc.be32(body, et_off + 0x20)
    count = cc.be32(body, et_off + 0x24)
    base = cc.be32(body, et_off + 0x28)
    if not (0 < count < 100000):
        return None
    exports = []
    for i in range(count):
        o = et_off + 0x2C + i * 4
        if o + 4 > len(body):
            break
        off = cc.be32(body, o)
        if off == 0:
            continue
        func_va = off + (imagebaseaddr << 16)
        exports.append((base + i, func_va))
    return exports


# Modules whose own export table ordinals should be resolved against xam's
# ordinal database (this is XAM itself, or a depatched copy of it).
XAM_LIKE = {"xam.xex", "$flash_xam.xexp"}


def main():
    ordinal_tables = cc.load_ordinal_tables()
    export_dumps = []
    tsv_rows = []

    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        if "RAW" in info["status"] or not os.path.exists(info["dst"]):
            continue
        body_meta = cc.parse_xex_header(info["header_bytes"])
        load_address = info["load_address"]
        etva = body_meta.get("export_table_va")
        image_size = body_meta.get("xex_security_image_size")
        if not (etva and load_address and image_size and
                load_address <= etva < load_address + image_size):
            continue
        exports = read_export_table(info["dst"], load_address, etva)
        if not exports:
            continue

        fn = os.path.basename(rel)
        name_table = ordinal_tables.get("xam") if fn in XAM_LIKE else None

        out_name = rel.replace("/", "__").replace("$", "") + ".exports.txt"
        export_dumps.append((rel, out_name, load_address, etva, exports, name_table))
        for ordinal, va in exports:
            resolved = name_table.get(ordinal) if name_table else None
            tsv_rows.append((rel, f"0x{ordinal:04X}", f"0x{va:08X}", resolved or ""))

    os.makedirs(os.path.join(cc.REF, "exports"), exist_ok=True)
    export_index_lines = []
    for rel, out_name, load_address, etva, exports, name_table in export_dumps:
        path = os.path.join(cc.REF, "exports", out_name)
        with open(path, "w") as f:
            f.write(f"Export table for {rel}\n")
            f.write(f"xex_load_address=0x{load_address:08X} export_table_va=0x{etva:08X} count={len(exports)}\n")
            if name_table:
                f.write("Ordinal names resolved against xenia-edge's xam_table.inc (CONFIRMED - Xenia's own maintained ordinal database)\n")
            f.write("ordinal(hex) -> function_va(hex)  [resolved_name]\n")
            f.write("-" * 60 + "\n")
            for ordinal, va in sorted(exports):
                resolved = name_table.get(ordinal) if name_table else None
                suffix = f"  {resolved}" if resolved else ""
                f.write(f"0x{ordinal:04X} -> 0x{va:08X}{suffix}\n")
        export_index_lines.append(
            f"{rel}  ->  reference/exports/{out_name}  ({len(exports)} exports)"
        )

    with open(os.path.join(cc.REF, "01_EXPORT_TABLES_INDEX.txt"), "w") as f:
        f.write("Which decoded modules have full ordinal->address export dumps\n")
        f.write("===============================================================\n")
        f.write("Regenerate with: python3 _tool/build_02_exports.py\n\n")
        f.write("Each line below is a module that had a readable XEX export table;\n")
        f.write("see reference/exports/<name>.exports.txt for the full ordinal list.\n")
        f.write("For xam.xex / $flash_xam.xexp, ordinal names are resolved against\n")
        f.write("xenia-edge/src/xenia/kernel/xam/xam_table.inc directly (CONFIRMED).\n")
        f.write("Other modules' exports are unnamed (no comparable ordinal database\n")
        f.write("exists for them in this corpus) - ordinal + VA only.\n\n")
        for line in sorted(export_index_lines):
            f.write(line + "\n")

    with open(os.path.join(cc.REF, "exports.tsv"), "w") as f:
        f.write("module\tordinal\tfunction_va\tresolved_name\n")
        for row in tsv_rows:
            f.write("\t".join(row) + "\n")

    print(f"Wrote {len(export_dumps)} export table dumps ({len(tsv_rows)} export rows total)")


if __name__ == "__main__":
    main()
