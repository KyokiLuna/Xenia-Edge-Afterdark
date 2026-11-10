"""Builds reference/00_INDEX.txt (v2, richer fields) + reference/modules.tsv.

Structural only (no disassembly): headers, execution_info, sha256, sizes.
"""
import os
import struct
import corpus_common as cc

TSV_COLS = [
    "path", "module_name", "source_type", "decode_status",
    "xex_load_address", "xex_entry_va", "pe_image_base", "pe_entry_rva",
    "decoded_file_size", "pe_size_of_image", "xex_security_image_size",
    "title_id", "module_version", "base_version", "module_flags",
    "export_table_va", "export_count", "original_sha256", "decoded_sha256",
    "base_module", "patch_module", "header_sidecar", "disassembly_path",
    "notes", "confidence",
]


def pe_image_base_and_entry(body_path):
    """Read the standalone PE32 header inside the decoded body (this is the
    Xbox-toolchain-emitted PE ImageBase/AddressOfEntryPoint, which is a
    SEPARATE address space concept from the XEX runtime load address - the
    two happen to often match for un-relocated modules but must not be
    conflated)."""
    try:
        with open(body_path, "rb") as f:
            data = f.read(0x400)
    except Exception:
        return None, None, None
    if data[:2] != b"MZ" or len(data) < 0x40:
        return None, None, None
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 0x58 > len(data):
        return None, None, None
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        return None, None, None
    # IMAGE_FILE_HEADER is 20 bytes after the PE signature; optional header
    # follows. AddressOfEntryPoint @ +0x10, ImageBase @ +0x1C,
    # SizeOfImage @ +0x38 (standard PE32 optional header layout).
    opt = e_lfanew + 4 + 20
    if opt + 0x3C > len(data):
        return None, None, None
    entry_rva = struct.unpack_from("<I", data, opt + 0x10)[0]
    image_base = struct.unpack_from("<I", data, opt + 0x1C)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 0x38)[0]
    return image_base, entry_rva, size_of_image


def export_count_for(dst, load_address, export_table_va, image_size):
    if not (export_table_va and load_address and image_size):
        return None
    if not (load_address <= export_table_va < load_address + image_size):
        return None
    try:
        with open(dst, "rb") as f:
            body = f.read()
    except Exception:
        return None
    et_off = export_table_va - load_address
    if et_off + 0x28 > len(body):
        return None
    table_count = cc.be32(body, et_off + 0x24)
    if not (0 < table_count < 100000):
        return None
    # table_count is the export table's declared capacity; count only the
    # actually-populated (non-zero ordOffset) slots to match what
    # reference/exports/*.txt actually lists.
    populated = 0
    for i in range(table_count):
        o = et_off + 0x2C + i * 4
        if o + 4 > len(body):
            break
        if cc.be32(body, o) != 0:
            populated += 1
    return populated


def main():
    rows = []
    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        src_meta = info["src_meta"]
        body_meta = cc.parse_xex_header(info["header_bytes"]) or src_meta

        fn = os.path.basename(rel)
        source_type = "xexp_reconstructed" if src_meta["is_patch"] else "xex"

        dst = info["dst"]
        exists = os.path.exists(dst)
        pe_image_base = pe_entry_rva = pe_size_of_image = None
        original_sha256 = cc.sha256_file(src)
        decoded_sha256 = cc.sha256_file(dst) if exists else None
        content_kind = None
        if exists and "RAW" not in info["status"]:
            with open(dst, "rb") as f:
                head = f.read(4)
            if head == b"XUIZ":
                content_kind = "xui_resource"
            elif head == b"D100":
                content_kind = "ime_dictionary"
            elif head[:2] == b"MZ":
                content_kind = "pe_executable"
                pe_image_base, pe_entry_rva, pe_size_of_image = pe_image_base_and_entry(dst)

        load_address = info["load_address"]
        image_size = body_meta.get("xex_security_image_size")
        export_table_va = body_meta.get("export_table_va")
        exp_count = None
        etva_valid = (
            export_table_va and load_address and image_size and
            load_address <= export_table_va < load_address + image_size
        )
        if etva_valid and exists and "RAW" not in info["status"]:
            exp_count = export_count_for(dst, load_address, export_table_va, image_size)
        if not etva_valid:
            export_table_va = None

        sidecar_rel = (rel + ".header") if info["sidecar_exists"] else None

        confidence = "CONFIRMED"
        notes = []
        if "RAW" in info["status"]:
            confidence = "UNRESOLVED"
            notes.append("still encrypted/compressed - see 10_UNRESOLVED.txt")
        elif content_kind in ("xui_resource", "ime_dictionary"):
            notes.append(f"decoded body is a {content_kind}, not executable code (verified via LZX block-hash match)")
        if src_meta["is_patch"] and not info["sidecar_exists"] and "RAW" not in info["status"]:
            confidence = "SUSPECT_METADATA"
        if info.get("metadata_suspect"):
            confidence = "SUSPECT_METADATA"
            notes.append("title_id/module_version disagree with base module - reconstructed "
                         "sidecar's execution_info header may not be fully relocated; "
                         "treat title_id/module_version/base_version for this row with caution")

        if load_address is None:
            confidence = "UNKNOWN"

        # module_version/base_version/title_id come from execution_info in
        # the ORIGINAL (pre-patch) source header for un-patched modules, or
        # from the reconstructed sidecar for depatched ones - either way
        # body_meta is already the right one per decoded_body_meta().
        title_id = body_meta.get("title_id")
        module_version = cc.version_str(body_meta.get("module_version"))
        base_version = cc.version_str(body_meta.get("base_version"))

        row = {
            "path": rel,
            "module_name": fn,
            "source_type": source_type,
            "decode_status": info["status"],
            "xex_load_address": load_address,
            "xex_entry_va": body_meta.get("xex_entry_va"),
            "pe_image_base": pe_image_base,
            "pe_entry_rva": pe_entry_rva,
            "decoded_file_size": os.path.getsize(dst) if exists else None,
            "pe_size_of_image": pe_size_of_image,
            "xex_security_image_size": image_size,
            "title_id": title_id,
            "module_version": module_version,
            "base_version": base_version,
            "module_flags": src_meta.get("module_flags"),
            "export_table_va": export_table_va,
            "export_count": exp_count,
            "original_sha256": original_sha256,
            "decoded_sha256": decoded_sha256,
            "base_module": info["base_rel"],
            "patch_module": (fn if src_meta["is_patch"] else None),
            "header_sidecar": sidecar_rel,
            "disassembly_path": None,  # filled in by build_03_calls.py if produced
            "notes": "; ".join(notes) if notes else "",
            "confidence": confidence,
            "content_kind": content_kind,
            "filetype": cc.file_type(dst) if exists else "MISSING",
        }
        rows.append(row)

    # non-xex2 files, minimal rows
    other_rows = []
    for root, dirs, files in os.walk(cc.DUMP):
        dirs.sort()
        for fn in sorted(files):
            src = os.path.join(root, fn)
            rel = os.path.relpath(src, cc.DUMP)
            with open(src, "rb") as f:
                magic = f.read(4)
            if magic == b"XEX2":
                continue
            dst = os.path.join(cc.OUT, rel)
            other_rows.append({
                "rel": rel,
                "size": os.path.getsize(dst) if os.path.exists(dst) else None,
                "filetype": cc.file_type(dst) if os.path.exists(dst) else "MISSING",
            })

    os.makedirs(cc.REF, exist_ok=True)

    # ---- 00_INDEX.txt (human-readable) ----
    with open(os.path.join(cc.REF, "00_INDEX.txt"), "w") as f:
        f.write("decoded_dump master module index (v2)\n")
        f.write("=========================================\n")
        f.write("One record per file in the dump (relative to decoded_dump/,\n")
        f.write("mirrors the original 'Blades + NXE Classic/' layout).\n")
        f.write("Regenerate with: python3 _tool/build_01_modules.py\n\n")
        f.write("IMPORTANT - address space note:\n")
        f.write("  xex_load_address / xex_entry_va = XEX2 runtime virtual\n")
        f.write("    address space (xex2_security_info.load_address, and the\n")
        f.write("    XEX_HEADER_ENTRY_POINT optional header). THIS is the\n")
        f.write("    address space used everywhere else in this corpus\n")
        f.write("    (exports, imports, call graph, IPC map).\n")
        f.write("  pe_image_base / pe_entry_rva = the standalone PE32 header's\n")
        f.write("    own ImageBase/AddressOfEntryPoint, embedded inside the\n")
        f.write("    decoded body by the Xbox toolchain. These often equal the\n")
        f.write("    XEX values for un-relocated modules but are NOT guaranteed\n")
        f.write("    to and must never be assumed interchangeable.\n")
        f.write("  decoded_file_size = size of the file sitting in decoded_dump/.\n")
        f.write("  pe_size_of_image = the PE header's own SizeOfImage field.\n")
        f.write("  xex_security_image_size = xex2_security_info.image_size.\n")
        f.write("    These three sizes are DIFFERENT fields, recorded separately\n")
        f.write("    on purpose - do not assume they're equal.\n\n")
        f.write("Confidence: CONFIRMED (structurally parsed from headers) |\n")
        f.write("  INFERRED (derived/fallback value) | UNKNOWN (field absent) |\n")
        f.write("  UNRESOLVED (file still encrypted) | SUSPECT_METADATA (patch\n")
        f.write("  header reconstruction may be incomplete)\n")
        f.write("-" * 100 + "\n\n")

        f.write("### XEX2-derived files (%d) ###\n\n" % len(rows))
        for r in sorted(rows, key=lambda r: r["path"].lower()):
            def hx(v):
                return f"0x{v:X}" if isinstance(v, int) else "-"
            f.write(f"{r['path']}\n")
            f.write(f"    confidence={r['confidence']}  status={r['decode_status']}\n")
            f.write(f"    xex_load_address={hx(r['xex_load_address'])}  xex_entry_va={hx(r['xex_entry_va'])}\n")
            f.write(f"    pe_image_base={hx(r['pe_image_base'])}  pe_entry_rva={hx(r['pe_entry_rva'])}\n")
            f.write(f"    decoded_file_size={r['decoded_file_size']}  pe_size_of_image={hx(r['pe_size_of_image'])}  "
                    f"xex_security_image_size={hx(r['xex_security_image_size'])}\n")
            f.write(f"    title_id={hx(r['title_id'])}  module_version={r['module_version']}  base_version={r['base_version']}\n")
            f.write(f"    export_table_va={hx(r['export_table_va'])}  export_count={r['export_count'] or '-'}\n")
            f.write(f"    base_module={r['base_module'] or '-'}  patch_module={r['patch_module'] or '-'}  "
                    f"header_sidecar={r['header_sidecar'] or '-'}\n")
            f.write(f"    original_sha256={r['original_sha256']}\n")
            f.write(f"    decoded_sha256={r['decoded_sha256']}\n")
            if r["notes"]:
                f.write(f"    notes: {r['notes']}\n")
            f.write(f"    file(1)={r['filetype']}\n\n")

        f.write("\n### Non-XEX2 files (%d) - copied through unchanged ###\n\n" % len(other_rows))
        for r in sorted(other_rows, key=lambda r: r["rel"].lower()):
            f.write(f"{r['rel']}  ({r['size']} bytes)  {r['filetype']}\n")

    # ---- modules.tsv ----
    with open(os.path.join(cc.REF, "modules.tsv"), "w") as f:
        f.write("\t".join(TSV_COLS) + "\n")
        for r in sorted(rows, key=lambda r: r["path"].lower()):
            def s(v):
                if v is None:
                    return ""
                if isinstance(v, int):
                    return f"0x{v:X}"
                return str(v)
            f.write("\t".join(s(r[c]) for c in TSV_COLS) + "\n")

    print(f"Indexed {len(rows)} xex2 modules + {len(other_rows)} other files")
    return rows


if __name__ == "__main__":
    main()
