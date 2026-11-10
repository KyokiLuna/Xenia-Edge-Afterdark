"""Builds reference/05_STRINGS_INDEX.txt, reference/strings/<module>.txt,
reference/strings.tsv.

Extracts ASCII and UTF-16BE strings (PPC is big-endian, so wide strings in
these binaries are stored big-endian, not the little-endian pattern `strings
-e l` assumes) from every successfully decoded XEX2-derived module, then
keyword-filters into the "useful" index. Full raw dumps are NOT kept per
module (would be enormous and low-signal) - only the keyword-matched subset,
each with its virtual address computed as load_address + file_offset.

xrefs (code that loads a string's address) are only computed for
PRIORITY_MODULES, and reuse build_04_calls.discover_module_functions() -
i.e. the SAME function set (exports + BFS-discovered internals + callback
descendants) the call graph and IPC scan use, not a separate export-only
walk. A string referenced only from inside a BFS/callback-discovered
function (not itself exported) is now found, where a prior version of this
script would have missed it - see 10_UNRESOLVED.txt #6.
"""
import os
import re
import corpus_common as cc
import disasm_support as ds
import build_04_calls as b4

KEYWORDS = [
    "avatar", "gamer", "profile", "storage", "content", "title", "launch",
    "xna", "xblig", "xbla", "licens", "signin", "sign-in", "sign in",
    "device", "marketplace", "xnet", "network", "error", "status",
    r"[a-z]:\\", "mount", "\\\\", "xam", "xbox", "dashboard", "system",
    "app", "package", "asset", "manifest", "session", "achievement",
    "friend", "message", "voice", "party", "quickchat", "vk\\b", "keyboard",
]
KEYWORD_RE = re.compile("|".join(KEYWORDS), re.IGNORECASE)

MIN_LEN = 5
JUNK_RE = re.compile(r"^[\W_]+$")


def extract_ascii_strings(data):
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % MIN_LEN, data):
        yield m.start(), m.group().decode("ascii")


def extract_utf16be_strings(data):
    # Big-endian UTF-16: high byte usually 0x00 for ASCII-range chars ->
    # bytes look like 00 'a' 00 'b' 00 'c' ...
    pattern = re.compile(
        rb"(?:\x00[\x20-\x7e]){%d,}" % MIN_LEN)
    for m in pattern.finditer(data):
        raw = m.group()
        start = m.start()
        try:
            text = raw.decode("utf-16-be").replace("\x00", "")
        except UnicodeDecodeError:
            continue
        if len(text) >= MIN_LEN:
            yield start, text


def is_useful(s):
    if JUNK_RE.match(s):
        return False
    return bool(KEYWORD_RE.search(s))


def compute_xrefs(mod, str_vas):
    """mod: result of build_04_calls.discover_module_functions(). str_vas:
    sorted [(va, offset), ...] for the matched strings. Returns
    {string_offset: [(xref_va, containing_function_va, containing_function_name), ...]}."""
    xrefs_by_off = {}
    str_va_set = {va: off for va, off in str_vas}
    for rec in mod["func_records"]:
        insns = rec["insns"]
        i = 0
        while i < len(insns):
            insn = insns[i]
            if insn.mnemonic == "lis" and i + 1 < len(insns) and \
               insns[i + 1].mnemonic in ("addi", "la", "ori"):
                try:
                    reg = insn.op_str.split(",")[0].strip()
                    hi = int(insn.op_str.split(",")[1].strip(), 0) & 0xFFFF
                    nxt = insns[i + 1]
                    nxt_parts = [p.strip() for p in nxt.op_str.split(",")]
                    # ori rX,rX,LO is self-referencing (dest==src==reg); addi/la
                    # rD,rA,LO uses rA (the SOURCE, operand index 1) as the base -
                    # rD (the destination) is very often a DIFFERENT register, e.g.
                    # `lis r11,HI; addi r7,r11,LO`, so it's operand[1], not
                    # operand[0], that must match the lis destination.
                    src_matches = (
                        nxt_parts[0] == reg if nxt.mnemonic == "ori"
                        else len(nxt_parts) >= 2 and nxt_parts[1] == reg
                    )
                    if src_matches:
                        lo_part = nxt_parts[-1]
                        lo = int(lo_part, 0) if lo_part.startswith("0x") or lo_part.lstrip("-").isdigit() else None
                        if lo is not None:
                            candidate = (hi << 16) + lo if nxt.mnemonic != "ori" else (hi << 16) | lo
                            if candidate in str_va_set:
                                xrefs_by_off.setdefault(str_va_set[candidate], []).append(
                                    (insn.address, rec["va"], rec["name"]))
                except (ValueError, IndexError):
                    pass
            i += 1
    return xrefs_by_off


def main():
    os.makedirs(os.path.join(cc.REF, "strings"), exist_ok=True)
    index_lines = []
    tsv_rows = []
    total_matched = 0
    total_xrefd_functions = set()

    all_modules = []
    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        if "RAW" in info["status"] or not os.path.exists(info["dst"]):
            continue
        all_modules.append((rel, info))

    for rel, info in all_modules:
        with open(info["dst"], "rb") as f:
            body = f.read()
        load_address = info["load_address"]

        found = {}  # offset -> (text, kind)
        for off, s in extract_ascii_strings(body):
            if is_useful(s):
                found[off] = (s, "ascii")
        for off, s in extract_utf16be_strings(body):
            if is_useful(s):
                found.setdefault(off, (s, "utf16be"))

        if not found:
            continue

        xrefs_by_off = {}
        if rel in cc.PRIORITY_MODULES and load_address:
            mod = b4.discover_module_functions(rel)
            if mod and mod["func_records"]:
                str_vas = [(load_address + off, off) for off in found]
                xrefs_by_off = compute_xrefs(mod, str_vas)

        out_name = rel.replace("/", "__").replace("$", "") + ".strings.txt"
        out_path = os.path.join(cc.REF, "strings", out_name)
        with open(out_path, "w") as f:
            f.write(f"Keyword-filtered strings for {rel}\n")
            f.write(f"xex_load_address=0x{load_address:08X}\n" if load_address else "xex_load_address=UNKNOWN\n")
            f.write(f"({len(found)} matches; full unfiltered dump not kept - re-run strings(1) "
                    f"on the decoded file directly if you need everything)\n")
            if rel in cc.PRIORITY_MODULES:
                f.write("xrefs computed (priority module) - lis+addi/la/ori pattern match, "
                        "INFERRED confidence (heuristic, not full data-flow), against the SAME\n")
                f.write("function set (exports + BFS-discovered internals + callback\n")
                f.write("descendants) build_04_calls.py's call graph uses - see\n")
                f.write("build_04_calls.discover_module_functions().\n")
            else:
                f.write("xrefs NOT computed (not a priority module) - address/offset only.\n")
            f.write("-" * 60 + "\n\n")
            for off in sorted(found):
                text, kind = found[off]
                va = load_address + off if load_address else None
                va_s = f"0x{va:08X}" if va else "?"
                f.write(f"[{va_s}] file_off=0x{off:X} ({kind}): {text!r}\n")
                xrefs = xrefs_by_off.get(off)
                if xrefs:
                    for xva, func_va, func_name in sorted(set(xrefs)):
                        f.write(f"    xref: 0x{xva:08X}  (in {func_name} @ 0x{func_va:08X})\n")
                        total_xrefd_functions.add((rel, func_va))
                tsv_rows.append((
                    rel, va_s, f"0x{off:X}", kind, text.replace("\t", " "),
                    ";".join(f"0x{xva:08X}:{func_name}@0x{func_va:08X}"
                            for xva, func_va, func_name in sorted(set(xrefs))) if xrefs else "",
                ))
                total_matched += 1

        index_lines.append(f"{rel}  ->  reference/strings/{out_name}  ({len(found)} matches)")

    with open(os.path.join(cc.REF, "05_STRINGS_INDEX.txt"), "w") as f:
        f.write("Keyword-filtered string index\n")
        f.write("================================\n")
        f.write("Regenerate with: python3 _tool/build_05_strings.py\n\n")
        f.write("Only strings matching a curated keyword list (avatar, gamer/profile,\n")
        f.write("storage, content, title, XNA, XBLIG/XBLA, licensing, sign-in, device,\n")
        f.write("marketplace, XNet/network, errors/status, paths, mount names, system\n")
        f.write("apps, etc.) are indexed here to keep signal high - see build_05_strings.py\n")
        f.write("KEYWORDS for the exact list. xrefs (which code loads a string's address)\n")
        f.write("are only computed for PRIORITY_MODULES and are INFERRED, not CONFIRMED -\n")
        f.write("the lis+addi/la/ori pattern match is a heuristic, not full data-flow - but\n")
        f.write("they DO now cover every function build_04_calls.py's call graph knows about\n")
        f.write("(exports, BFS-discovered internals, and callback descendants), not just\n")
        f.write("exported functions.\n\n")
        for line in sorted(index_lines):
            f.write(line + "\n")

    with open(os.path.join(cc.REF, "strings.tsv"), "w") as f:
        f.write("module\tva\tfile_offset\tkind\ttext\txrefs\n")
        for row in tsv_rows:
            f.write("\t".join(row) + "\n")

    print(f"Indexed {total_matched} keyword-matched strings across {len(index_lines)} modules; "
          f"{len(total_xrefd_functions)} distinct functions have >=1 string xref")


if __name__ == "__main__":
    main()
