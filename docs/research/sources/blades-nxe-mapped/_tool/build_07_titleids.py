"""Builds reference/07_TITLE_IDS.txt and reference/title_ids.tsv.

Two sources, kept clearly separate:
  1. STRUCTURAL (CONFIRMED): each module's own xex2_opt_execution_info.title_id
     field - this is the module's OWN identity, always reliable when present.
  2. IN-CODE (INFERRED): scanning PRIORITY_MODULES' exported-function
     disassembly (same bounded pass as build_04/06) for 32-bit immediate
     loads (lis+ori / lis+addi pairs) that exactly match a title ID already
     known from source #1 or from Xenia's title_id_utils.h constants - i.e.
     comparisons against a KNOWN id, not free-form guessing of new ones.

No semantic name is assigned to a title ID unless Xenia's own
title_id_utils.h (or a module's own execution_info) already says so.
"""
import os
import re
import corpus_common as cc
import disasm_support as ds


def load_xenia_known_title_ids():
    path = os.path.join(cc.XENIA, "src/xenia/kernel/title_id_utils.h")
    known = {}
    if not os.path.exists(path):
        return known
    with open(path) as f:
        text = f.read()
    for m in re.finditer(r"inline constexpr uint32_t (\w+)\s*=\s*(0x[0-9A-Fa-f]+);", text):
        name, val = m.groups()
        known[int(val, 16)] = name
    return known


def main():
    known_ids = load_xenia_known_title_ids()
    module_ids = {}  # title_id -> list of (module_rel)
    suspect_ids = {}  # title_id -> list of (module_rel) flagged metadata_suspect
    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        body_meta = cc.parse_xex_header(info["header_bytes"])
        tid = body_meta.get("title_id")
        if not tid:
            continue
        if info.get("metadata_suspect"):
            suspect_ids.setdefault(tid, []).append(rel)
        else:
            module_ids.setdefault(tid, []).append(rel)

    # combine into a lookup we scan code against
    all_known = dict(known_ids)
    for tid, mods in module_ids.items():
        all_known.setdefault(tid, f"(module identity of {mods[0]})")

    code_hits = []  # (module, va, title_id, matched_against)
    for rel in cc.PRIORITY_MODULES:
        info = cc.decoded_body_meta(rel)
        if "RAW" in info["status"] or not os.path.exists(info["dst"]):
            continue
        with open(info["dst"], "rb") as f:
            body = f.read()
        body_meta = cc.parse_xex_header(info["header_bytes"])
        load_address = info["load_address"]
        etva = body_meta.get("export_table_va")
        image_size = body_meta.get("xex_security_image_size")
        if not (etva and image_size and load_address and
                load_address <= etva < load_address + image_size):
            continue
        et_off = etva - load_address
        if et_off + 0x2C > len(body):
            continue
        imagebaseaddr = cc.be32(body, et_off + 0x20)
        count = cc.be32(body, et_off + 0x24)
        base = cc.be32(body, et_off + 0x28)
        exports = []
        for i in range(count):
            o = et_off + 0x2C + i * 4
            if o + 4 > len(body):
                break
            off = cc.be32(body, o)
            if off:
                exports.append(off + (imagebaseaddr << 16))
        exports.sort()

        for idx, va in enumerate(exports):
            nb = exports[idx + 1] if idx + 1 < len(exports) else None
            insns = ds.disassemble_bounded(body, load_address, va, nb)
            i = 0
            while i < len(insns) - 1:
                a, b = insns[i], insns[i + 1]
                if a.mnemonic == "lis" and b.mnemonic in ("ori", "addi"):
                    try:
                        reg = a.op_str.split(",")[0].strip()
                        hi = int(a.op_str.split(",")[1].strip(), 0)
                        if b.op_str.split(",")[0].strip() == reg:
                            lo = int(b.op_str.split(",")[-1].strip(), 0)
                            val = (hi << 16) | lo if b.mnemonic == "ori" else (hi << 16) + lo
                            if val in all_known:
                                code_hits.append((rel, a.address, val, all_known[val]))
                    except (ValueError, IndexError):
                        pass
                i += 1

    os.makedirs(cc.REF, exist_ok=True)
    with open(os.path.join(cc.REF, "07_TITLE_IDS.txt"), "w") as f:
        f.write("Title ID index\n")
        f.write("================\n")
        f.write("Regenerate with: python3 _tool/build_07_titleids.py\n\n")
        f.write("Section 1: module identity (CONFIRMED, from each module's own\n")
        f.write("xex2_opt_execution_info.title_id). This is the most reliable source -\n")
        f.write("structural, not disassembly-based.\n\n")
        for tid in sorted(k for k in module_ids if k):
            name = known_ids.get(tid, "")
            f.write(f"0x{tid:08X}{'  (' + name + ')' if name else ''}\n")
            for m in module_ids[tid]:
                f.write(f"    module={m}\n")
        f.write("\n")

        if suspect_ids:
            f.write("Section 1b: SUSPECT_METADATA module identities (title_id disagrees "
                    "with the module's own base module)\n")
            f.write("-" * 60 + "\n")
            f.write("These come from depatched $flash_*.xexp sidecars whose reconstructed\n")
            f.write("execution_info didn't match their base module's (which reports\n")
            f.write("title_id=0, as expected for a system component). At least one of\n")
            f.write("these decodes as literal ASCII text, not a real title ID - the\n")
            f.write("header-patch reconstruction likely didn't fully relocate this\n")
            f.write("particular optional header. Do NOT treat these as real title IDs.\n\n")
            for tid in sorted(suspect_ids):
                f.write(f"0x{tid:08X}  (raw bytes: {tid.to_bytes(4, 'big', signed=False)!r})\n")
                for m in suspect_ids[tid]:
                    f.write(f"    module={m}\n")
            f.write("\n")

        f.write("Section 2: known Xenia title ID constants (from "
                "xenia-edge/src/xenia/kernel/title_id_utils.h)\n")
        f.write("-" * 60 + "\n")
        for tid, name in sorted(known_ids.items()):
            in_corpus = " <- matches a module's own identity above" if tid in module_ids else ""
            f.write(f"0x{tid:08X}  {name}{in_corpus}\n")
        f.write("\n")

        f.write("Section 3: in-code comparisons against a KNOWN title ID "
                "(INFERRED - PRIORITY_MODULES only)\n")
        f.write("-" * 60 + "\n")
        f.write("Found by scanning exported-function disassembly for lis+ori/addi\n")
        f.write("pairs forming a 32-bit constant that exactly matches a title ID\n")
        f.write("already known from Section 1 or 2 - this is confirming WHERE a\n")
        f.write("known ID is compared against, not discovering new ones blind.\n\n")
        for rel, va, tid, name in sorted(code_hits):
            f.write(f"0x{tid:08X}  ({name})  module={rel}  va=0x{va:08X}\n")

    with open(os.path.join(cc.REF, "title_ids.tsv"), "w") as f:
        f.write("title_id\tsource\tmodule_or_name\tva\tconfidence\n")
        for tid, mods in sorted(module_ids.items()):
            for m in mods:
                f.write(f"0x{tid:X}\tmodule_identity\t{m}\t\tCONFIRMED\n")
        for tid, mods in sorted(suspect_ids.items()):
            for m in mods:
                f.write(f"0x{tid:X}\tmodule_identity\t{m}\t\tSUSPECT_METADATA\n")
        for tid, name in sorted(known_ids.items()):
            f.write(f"0x{tid:X}\txenia_source_constant\t{name}\t\tCONFIRMED\n")
        for rel, va, tid, name in code_hits:
            f.write(f"0x{tid:X}\tcode_reference\t{rel}\t0x{va:08X}\tINFERRED\n")

    print(f"Title IDs: {len(module_ids)} module identities, {len(known_ids)} known Xenia "
          f"constants, {len(code_hits)} in-code references (priority modules)")


if __name__ == "__main__":
    main()
