import struct, os, subprocess, sys

DUMP = "/home/deck/Downloads/xenia_edge/Blades + NXE Classic"
OUT = "/home/deck/Downloads/xenia_edge/decoded_dump"
REF = os.path.join(OUT, "reference")

def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be16(b, o): return struct.unpack_from(">H", b, o)[0]

# name (without $flash_ / .xexp) -> base module relpath, for the 12 known patches
BASE_MAP = {
    "bootanim": "bootanim.xex",
    "createprofile": "createprofile.xex",
    "deviceselector": "deviceselector.xex",
    "gamerprofile": "gamerprofile.xex",
    "hud": "hud.xex",
    "mfgbootlauncher": "mfgbootlauncher.xex",
    "minimediaplayer": "minimediaplayer.xex",
    "signin": "signin.xex",
    "updater": "updater.xex",
    "vk": "vk.xex",
    "xam": "xam.xex",
    "ximedic": "XIMEDIC.XEX",
}

def parse_header_meta(data):
    """data: bytes of a raw (unencrypted) XEX2 header, or a .header sidecar."""
    out = {}
    if data[:4] != b"XEX2":
        return None
    out["module_flags"] = be32(data, 4)
    out["header_size"] = be32(data, 8)
    out["security_offset"] = be32(data, 0x10)
    header_count = be32(data, 0x14)
    sec = out["security_offset"]
    out["image_size"] = be32(data, sec + 4)
    out["load_address"] = be32(data, sec + 0x110)
    out["export_table_va"] = be32(data, sec + 0x160)
    out["region"] = be32(data, sec + 0x178)
    out["page_descriptor_count"] = be32(data, sec + 0x180)
    ff_off = None
    off = 0x18
    hdrs = {}
    for i in range(header_count):
        k = be32(data, off)
        v = be32(data, off + 4)
        hdrs[k] = v
        off += 8
    out["headers"] = hdrs
    if 0x3FF in hdrs:
        ff_off = hdrs[0x3FF]
        out["encryption_type"] = be16(data, ff_off + 4)
        out["compression_type"] = be16(data, ff_off + 6)
    else:
        out["encryption_type"] = None
        out["compression_type"] = None
    # entry point: key 0x10100, low byte 0x00 -> value IS the data (stored inline)
    out["entry_point"] = hdrs.get(0x10100)
    out["is_patch"] = bool(out["module_flags"] & 0x10)
    return out

COMP_NAMES = {0: "NONE", 1: "BASIC", 2: "NORMAL", 3: "DELTA", None: "?"}
ENC_NAMES = {0: "NONE", 1: "NORMAL", None: "?"}

def file_type(path):
    try:
        r = subprocess.run(["file", "-b", path], capture_output=True, text=True, timeout=10)
        return r.stdout.strip()
    except Exception:
        return "?"

def read_export_table(body_path, load_address, export_table_va):
    if not export_table_va:
        return None
    try:
        with open(body_path, "rb") as f:
            body = f.read()
    except Exception:
        return None
    et_off = export_table_va - load_address
    if et_off < 0 or et_off + 0x2C > len(body):
        return None
    imagebaseaddr = be32(body, et_off + 0x20)
    count = be32(body, et_off + 0x24)
    base = be32(body, et_off + 0x28)
    if count == 0 or count > 100000:
        return None
    exports = []
    for i in range(count):
        o = et_off + 0x2C + i * 4
        if o + 4 > len(body):
            break
        off = be32(body, o)
        if off == 0:
            continue
        func_va = off + (imagebaseaddr << 16)
        exports.append((base + i, func_va))
    return exports

rows = []
export_dumps = []

for root, dirs, files in os.walk(DUMP):
    dirs.sort()
    for fn in sorted(files):
        src = os.path.join(root, fn)
        rel = os.path.relpath(src, DUMP)
        dst = os.path.join(OUT, rel)
        with open(src, "rb") as f:
            magic = f.read(4)

        if magic != b"XEX2":
            ft = file_type(dst) if os.path.exists(dst) else "MISSING"
            rows.append({
                "rel": rel, "kind": "non-xex2", "status": "copied",
                "filetype": ft, "size": os.path.getsize(dst) if os.path.exists(dst) else 0,
            })
            continue

        with open(src, "rb") as f:
            raw_header = f.read(0x8000)
        meta = parse_header_meta(raw_header)

        # Figure out which header describes the DECODED body: for successful
        # patches, it's the ".header" sidecar (post-patch layout differs from
        # the raw source file's own tiny patch header).
        sidecar = dst + ".header"
        base_rel = None
        load_address = meta["load_address"] if meta else None
        body_meta = meta

        key = None
        if fn.startswith("$flash_") and fn.endswith(".xexp"):
            key = fn[len("$flash_"):-len(".xexp")]

        status = "decoded"
        if meta and meta["is_patch"]:
            if key and key in BASE_MAP:
                base_rel = BASE_MAP[key]
                if os.path.exists(sidecar):
                    with open(sidecar, "rb") as f:
                        sidecar_data = f.read()
                    body_meta = parse_header_meta(sidecar_data)
                    # patched security_info.load_address reads 0 in practice;
                    # these patches don't relocate, so fall back to the base's.
                    base_full = os.path.join(DUMP, base_rel)
                    with open(base_full, "rb") as f:
                        base_header = f.read(0x8000)
                    base_meta = parse_header_meta(base_header)
                    load_address = body_meta["load_address"] or base_meta["load_address"]
                    status = "depatched (base=%s)" % base_rel
                else:
                    status = "RAW - depatch failed"
                    body_meta = meta
                    load_address = meta["load_address"]
            else:
                status = "RAW - no base module in dump"
                body_meta = meta
                load_address = meta["load_address"]
        else:
            status = "decoded"

        ft = file_type(dst) if os.path.exists(dst) else "MISSING"
        content_kind = None
        if os.path.exists(dst) and "RAW" not in status:
            with open(dst, "rb") as f:
                head = f.read(16)
            if head[:4] == b"XUIZ":
                content_kind = "XUI compiled UI skin/resource (NOT executable code)"
            elif head[:4] == b"D100":
                content_kind = "IME dictionary resource blob (NOT executable code)"
            elif head[:2] == b"MZ":
                content_kind = "PowerPC executable code"

        # Sanity-check export_table_va: some depatched modules' security_info
        # carries a small/garbage value here (likely modules with genuinely
        # no exports) rather than a real VA - only trust it if it falls
        # inside this module's own image.
        etva = body_meta.get("export_table_va") if body_meta else None
        img_size = body_meta.get("image_size") if body_meta else None
        etva_valid = (
            etva and load_address and img_size and
            load_address <= etva < load_address + img_size
        )
        if not etva_valid:
            etva = None

        export_count = None
        if etva_valid and os.path.exists(dst) and "RAW" not in status:
            exports = read_export_table(dst, load_address, etva)
            if exports:
                export_count = len(exports)
                out_name = rel.replace("/", "__").replace("$", "") + ".exports.txt"
                export_dumps.append((rel, out_name, load_address, etva, exports))

        rows.append({
            "rel": rel, "kind": "xex2", "status": status,
            "filetype": ft, "size": os.path.getsize(dst) if os.path.exists(dst) else 0,
            "load_address": load_address,
            "entry_point": body_meta.get("entry_point") if body_meta else None,
            "security_offset": body_meta.get("security_offset") if body_meta else None,
            "image_size": img_size,
            "export_table_va": etva,
            "export_count": export_count,
            "encryption_type": ENC_NAMES.get(meta.get("encryption_type")) if meta else "?",
            "compression_type": COMP_NAMES.get(meta.get("compression_type")) if meta else "?",
            "base_module": base_rel,
            "header_sidecar": (rel + ".header") if os.path.exists(sidecar) else None,
            "content_kind": content_kind,
        })

# ---- write master index ----
os.makedirs(REF, exist_ok=True)
with open(os.path.join(REF, "00_INDEX.txt"), "w") as f:
    f.write("decoded_dump master file index\n")
    f.write("================================\n")
    f.write("One row per file in the dump (relative to decoded_dump/, mirrors\n")
    f.write("the original 'Blades + NXE Classic/' layout). Generated by\n")
    f.write("_tool/build_index.py from the original (encrypted) dump headers\n")
    f.write("plus the decoded output, so it stays authoritative even if you\n")
    f.write("re-run the decoder.\n\n")
    f.write("Columns: relpath | status | load_addr | entry_point | security_off | image_size | export_table_va | export_count | src_enc/comp | base_module | file(1)\n")
    f.write("src_enc/comp describes how the file was wrapped in the ORIGINAL\n")
    f.write("dump (encryption/compression of the .xex or .xexp source), not the\n")
    f.write("decoded body sitting next to this index - that body is always a\n")
    f.write("plain, already-decrypted-and-decompressed PE image (or resource\n")
    f.write("blob) when status is 'decoded' or 'depatched'.\n")
    f.write("-" * 100 + "\n\n")

    xex_rows = [r for r in rows if r["kind"] == "xex2"]
    other_rows = [r for r in rows if r["kind"] != "xex2"]

    f.write("### XEX2-derived files (%d) ###\n\n" % len(xex_rows))
    for r in sorted(xex_rows, key=lambda r: r["rel"].lower()):
        la = f"0x{r['load_address']:08X}" if r["load_address"] else "-"
        ep = f"0x{r['entry_point']:08X}" if r["entry_point"] else "-"
        so = f"0x{r['security_offset']:X}" if r["security_offset"] else "-"
        isz = f"0x{r['image_size']:X}" if r["image_size"] else "-"
        etva = f"0x{r['export_table_va']:08X}" if r["export_table_va"] else "-"
        ec = str(r["export_count"]) if r["export_count"] else "-"
        encomp = f"{r['encryption_type']}/{r['compression_type']}"
        base = r["base_module"] or "-"
        f.write(f"{r['rel']}\n")
        f.write(f"    status={r['status']}  load={la}  entry={ep}  sec_off={so}  "
                f"image_size={isz}\n")
        f.write(f"    export_table_va={etva}  export_count={ec}  enc/comp={encomp}  "
                f"base={base}\n")
        if r["header_sidecar"]:
            f.write(f"    header_sidecar={r['header_sidecar']}\n")
        if r["content_kind"]:
            f.write(f"    content={r['content_kind']}\n")
        f.write(f"    file(1)={r['filetype']}\n\n")

    f.write("\n### Non-XEX2 files (%d) - copied through unchanged ###\n\n" % len(other_rows))
    for r in sorted(other_rows, key=lambda r: r["rel"].lower()):
        f.write(f"{r['rel']}  ({r['size']} bytes)  {r['filetype']}\n")

# ---- write export dumps ----
export_index_lines = []
for rel, out_name, load_address, etva, exports in export_dumps:
    path = os.path.join(REF, "exports", out_name)
    with open(path, "w") as f:
        f.write(f"Export table for {rel}\n")
        f.write(f"load_address=0x{load_address:08X} export_table_va=0x{etva:08X} count={len(exports)}\n")
        f.write("ordinal(hex) -> function_va(hex)\n")
        f.write("-" * 40 + "\n")
        for ordinal, va in sorted(exports):
            f.write(f"0x{ordinal:04X} -> 0x{va:08X}\n")
    export_index_lines.append(f"{rel}  ->  reference/exports/{out_name}  ({len(exports)} exports)")

with open(os.path.join(REF, "01_EXPORT_TABLES_INDEX.txt"), "w") as f:
    f.write("Which decoded modules have full ordinal->address export dumps\n")
    f.write("===============================================================\n")
    f.write("Each line below is a module that had a readable XEX export table;\n")
    f.write("see reference/exports/<name>.exports.txt for the full ordinal list.\n")
    f.write("Cross-reference ordinal numbers against xenia-edge/src/xenia/kernel/xam/xam_table.inc\n")
    f.write("(or the xboxkrnl/xbdm _table.inc files) to get human-readable names.\n\n")
    for line in sorted(export_index_lines):
        f.write(line + "\n")

print(f"Wrote {os.path.join(REF, '00_INDEX.txt')}")
print(f"Wrote {os.path.join(REF, '01_EXPORT_TABLES_INDEX.txt')}")
print(f"Wrote {len(export_dumps)} export table dumps to reference/exports/")
print(f"Total files indexed: {len(rows)} ({len(xex_rows)} xex2, {len(other_rows)} other)")

# DEPRECATED: superseded by build_01_modules.py (richer schema: PE ImageBase
# vs XEX load address kept separate, sha256, execution_info, confidence
# labels, etc.) + build_02_exports.py. Kept only for history; do not run
# this - it will regenerate 00_INDEX.txt in the OLD, less detailed format
# and will not touch modules.tsv/exports.tsv at all.
