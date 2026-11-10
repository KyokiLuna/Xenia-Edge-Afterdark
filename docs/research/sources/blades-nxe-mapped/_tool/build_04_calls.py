"""Builds reference/04_CALL_GRAPH_INDEX.txt, reference/calls/<module>.txt,
and reference/calls.tsv.

Also exposes discover_module_functions(rel) as the CANONICAL function-
discovery routine for this corpus - build_05_strings.py and
build_06_ipc.py both import and reuse it rather than re-implementing their
own export/BFS/callback walks, so every part of the corpus that needs "all
known functions in this module" sees exactly the same set (this matters:
IPC candidate discovery and string xrefs both benefit from the same
callback-descendant coverage this module already computes for the call
graph, and prior versions of those other two scripts didn't - see
10_UNRESOLVED.txt).

(IPC candidate/endpoint analysis lives entirely in build_06_ipc.py, which
owns reference/_ipc_candidates.tsv and reference/ipc.tsv.)

Scope: corpus_common.PRIORITY_MODULES only. Two coverage modes per module:

  - Export-boundary mode (modules with >=EXPORT_BFS_THRESHOLD exports, e.g.
    xam.xex/$flash_xam.xexp): disassemble each exported function bounded by
    the NEXT export's VA - a reasonably reliable boundary since exports are
    real, ordinal-verified addresses.

  - BFS mode (modules with few/no exports, e.g. gamerprofile.xex/
    createprofile.xex/deviceselector.xex/signin.xex/dash.xex - these are
    application-style modules with almost nothing exported): additionally
    BFS-discover internal functions reachable via `bl` starting from the
    module's entry point (and any exports it does have), capped at 250
    functions. Each discovered function is bounded only by hitting a real
    blr/bctr or a hard byte cap (see disasm_support.discover_functions_bfs)
    - a cruder boundary than export-mode, so BFS-discovered functions are
    always logged as such and their "bounded" confidence stays INFERRED.

    A further opportunistic pass follows function-pointer arguments passed
    to *Register*-style imports (e.g. XamRegisterSysApp's callback arg) as
    extra BFS roots - this is how gamerprofile.xex's real logic (registered
    as a callback, never directly `bl`'d from its own entry point) gets
    discovered at all. Provenance for these is tracked explicitly and
    honestly (discovery_kind/callback_root_va/callback_registration_*
    fields) rather than collapsed into a vague "bfs-callback" label - a
    prior version of this script produced misleading placeholder text like
    "via ? call at 0x00000000 in ?" for BFS-discovered DESCENDANTS of a
    callback root (which are not themselves directly-registered callbacks)
    because it only recorded provenance for the root, not the whole
    subtree; discover_functions_bfs now tracks root_va/parent_va for every
    discovered node so this no longer happens - see 10_UNRESOLVED.txt #5.

Either way this is a connectivity map, not a decompilation.
"""
import os
import corpus_common as cc
import disasm_support as ds

EXPORT_BFS_THRESHOLD = 20  # modules with fewer exports than this also get BFS coverage
BFS_CAP = 250


def get_export_list(dst, load_address, export_table_va):
    with open(dst, "rb") as f:
        body = f.read()
    et_off = export_table_va - load_address
    if et_off + 0x2C > len(body):
        return [], body
    imagebaseaddr = cc.be32(body, et_off + 0x20)
    count = cc.be32(body, et_off + 0x24)
    base = cc.be32(body, et_off + 0x28)
    exports = []
    for i in range(count):
        o = et_off + 0x2C + i * 4
        if o + 4 > len(body):
            break
        off = cc.be32(body, o)
        if off == 0:
            continue
        exports.append((base + i, off + (imagebaseaddr << 16)))
    return exports, body


def build_thunk_map(header_bytes, body, load_address):
    """thunk_va -> 'library!name' or 'library!ordinal_0xNNN'"""
    ordinal_tables = cc.load_ordinal_tables()
    thunks = {}
    try:
        libs = cc.parse_import_libraries(header_bytes, body, load_address)
    except Exception:
        libs = []
    for lib in libs:
        by_ord = {}
        for imp in lib["imports"]:
            if imp["ordinal"] is None:
                continue
            by_ord.setdefault(imp["ordinal"], {})
            if imp["record_type"] == 1:
                by_ord[imp["ordinal"]]["thunk"] = imp["record_addr"]
        for ordinal, entry in by_ord.items():
            if "thunk" not in entry:
                continue
            name = cc.resolve_import_name(ordinal_tables, lib["name"], ordinal)
            label = f"{lib['name']}!{name}" if name else f"{lib['name']}!ordinal_0x{ordinal:04X}"
            thunks[entry["thunk"]] = label
    return thunks


def ordinal_name_map(load_address, export_table_va, fn, dst):
    """For xam-like modules, map export VA -> resolved name."""
    if fn not in ("xam.xex", "$flash_xam.xexp"):
        return {}
    tables = cc.load_ordinal_tables().get("xam", {})
    exports, _ = get_export_list(dst, load_address, export_table_va)
    return {va: tables.get(ordinal, f"ordinal_0x{ordinal:04X}") for ordinal, va in exports}


CALLBACK_REGISTRAR_HINT = "register"  # substring match against resolved import name, case-insensitive
CALLBACK_ARG_REGS = ["r3", "r4", "r5", "r6", "r7", "r8"]


def find_callback_roots(insns, load_address, image_size, thunk_map, header_size):
    """For calls to an import whose resolved name looks like a callback
    registration API (contains 'Register', e.g. XamRegisterSysApp), try
    resolving each likely argument register's value at the call site; any
    that land inside the module's own code range are treated as candidate
    callback function pointers and returned as extra BFS roots.
    Heuristic, INFERRED - see 10_UNRESOLVED.txt."""
    roots = []
    for i, insn in enumerate(insns):
        if insn.mnemonic != "bl":
            continue
        try:
            target = int(insn.op_str.strip(), 16)
        except ValueError:
            continue
        label = thunk_map.get(target)
        if not label or CALLBACK_REGISTRAR_HINT not in label.lower():
            continue
        for reg in CALLBACK_ARG_REGS:
            val = ds.resolve_register_value_before(insns, i, reg)
            if val is None:
                continue
            # skip low addresses (flags/small ints/counts), require it to
            # land past the header into code/data - i.e. plausibly a VA.
            if load_address + header_size <= val < load_address + image_size:
                roots.append((val, label, insn.address))
    return roots


def classify_and_record(rel, va, name, insns, load_address, image_size, thunk_map,
                        name_by_va, bfs_names):
    bls = ds.extract_bl_targets(insns)
    internal_callees, imported_callees, tail_calls = [], [], []
    edges = []
    for site_va, target, mnem in bls:
        if target in thunk_map:
            imported_callees.append((site_va, thunk_map[target]))
            edges.append((target, "import"))
        elif load_address <= target < load_address + image_size:
            label = name_by_va.get(target) or bfs_names.get(target)
            if mnem == "b" and site_va == insns[-1].address:
                tail_calls.append((site_va, target, label))
            else:
                internal_callees.append((site_va, target, label))
            edges.append((target, "internal"))

    return {
        "va": va, "name": name, "insn_count": len(insns),
        "bounded": insns[-1].mnemonic not in ("blr", "bctr"),
        "internal_callees": internal_callees, "imported_callees": imported_callees,
        "tail_calls": tail_calls, "edges": edges, "insns": insns,
    }


def discovery_label(disc):
    """Renders a discovery-provenance dict into a short, honest tag."""
    kind = disc["kind"]
    if kind == "export":
        return "export"
    if kind == "bfs_root":
        return "bfs (entry point)"
    if kind == "bfs_reachable":
        return f"bfs (reached from 0x{disc['root_va']:08X} via 0x{disc['parent_va']:08X})"
    if kind == "callback_root":
        return f"callback root (registered via {disc['callback_registration_import']} at 0x{disc['callback_registration_callsite']:08X} in {disc['registering_caller']})"
    if kind == "callback_descendant":
        return (f"callback descendant (reached from callback root 0x{disc['callback_root_va']:08X} "
               f"via 0x{disc['parent_va']:08X})")
    return kind


def discover_module_functions(rel):
    """CANONICAL function-discovery routine for a PRIORITY_MODULES module.
    Returns None if the module is unavailable/undecoded, else a dict:
      body, load_address, image_size, name_by_va, thunk_map,
      func_records: [{"va","name","insn_count","bounded","internal_callees",
                       "imported_callees","tail_calls","edges","insns"}, ...]
      discovery: {va: {"kind": ..., ...provenance fields...}}
    Every OTHER script that needs "all known functions in this module"
    (string xrefs, IPC candidate scanning) should call this instead of
    re-implementing export/BFS/callback discovery - see this module's
    docstring for why.
    """
    if not os.path.exists(os.path.join(cc.DUMP, rel)):
        return None
    info = cc.decoded_body_meta(rel)
    if "RAW" in info["status"] or not os.path.exists(info["dst"]):
        return None
    body_meta = cc.parse_xex_header(info["header_bytes"])
    load_address = info["load_address"]
    etva = body_meta.get("export_table_va")
    image_size = body_meta.get("xex_security_image_size")
    entry_va = body_meta.get("xex_entry_va")
    if not image_size:
        return None
    etva_valid = (etva and load_address and image_size and
                  load_address <= etva < load_address + image_size)

    with open(info["dst"], "rb") as f:
        body = f.read()

    exports = []
    if etva_valid:
        exports, _ = get_export_list(info["dst"], load_address, etva)

    fn = os.path.basename(rel)
    name_by_va = ordinal_name_map(load_address, etva, fn, info["dst"]) if etva_valid else {}
    thunk_map = build_thunk_map(info["header_bytes"], body, load_address)

    func_records = []
    bfs_names = {}
    discovery = {}

    # --- export-boundary mode ---
    if exports:
        sorted_vas = sorted(va for _, va in exports)
        for ordinal, va in sorted(exports, key=lambda x: x[1]):
            idx = sorted_vas.index(va)
            nb = sorted_vas[idx + 1] if idx + 1 < len(sorted_vas) else None
            insns = ds.disassemble_bounded(body, load_address, va, nb)
            if not insns:
                continue
            name = name_by_va.get(va, f"ordinal_0x{ordinal:04X}")
            rec = classify_and_record(rel, va, name, insns, load_address, image_size,
                                      thunk_map, name_by_va, bfs_names)
            discovery[va] = {"kind": "export"}
            func_records.append(rec)

    # --- BFS mode (small/no export table modules) ---
    if len(exports) < EXPORT_BFS_THRESHOLD and entry_va:
        roots = [entry_va] + [va for _, va in exports]
        discovered = ds.discover_functions_bfs(body, load_address, image_size, roots, cap=BFS_CAP)
        already_covered = {r["va"] for r in func_records}
        for va, d in discovered.items():
            if va in already_covered:
                continue
            label = "entry_point" if va == entry_va else f"sub_0x{va:08X}"
            bfs_names[va] = label
        for va, d in sorted(discovered.items()):
            if va in already_covered:
                continue
            name = bfs_names[va]
            rec = classify_and_record(rel, va, name, d["insns"], load_address, image_size,
                                      thunk_map, name_by_va, bfs_names)
            rec["bounded"] = True
            if d["parent_va"] is None:
                discovery[va] = {"kind": "bfs_root"}
            else:
                discovery[va] = {"kind": "bfs_reachable", "root_va": d["root_va"], "parent_va": d["parent_va"]}
            func_records.append(rec)

    # --- callback-root expansion (opportunistic, BFS-mode modules only) ---
    if len(exports) < EXPORT_BFS_THRESHOLD:
        header_size = body_meta.get("header_size") or 0x1000
        callback_roots = []
        callback_evidence = {}
        for rec in func_records:
            for val, label, call_va in find_callback_roots(
                    rec["insns"], load_address, image_size, thunk_map, header_size):
                callback_roots.append(val)
                callback_evidence[val] = (rec["name"], call_va, label)
        already_covered = {r["va"] for r in func_records}
        new_roots = [r for r in callback_roots if r not in already_covered]
        if new_roots:
            more = ds.discover_functions_bfs(body, load_address, image_size, new_roots,
                                             cap=BFS_CAP - len(func_records))
            for va, d in sorted(more.items()):
                if va in already_covered:
                    continue
                already_covered.add(va)
                bfs_names[va] = f"sub_0x{va:08X}"
                rec = classify_and_record(rel, va, bfs_names[va], d["insns"], load_address,
                                          image_size, thunk_map, name_by_va, bfs_names)
                rec["bounded"] = True
                if d["parent_va"] is None:
                    registering_caller, callsite_va, import_label = callback_evidence.get(
                        va, ("UNKNOWN", None, "UNKNOWN"))
                    discovery[va] = {
                        "kind": "callback_root",
                        "callback_registration_import": import_label,
                        "callback_registration_callsite": callsite_va,
                        "registering_caller": registering_caller,
                    }
                else:
                    discovery[va] = {
                        "kind": "callback_descendant",
                        "callback_root_va": d["root_va"],
                        "parent_va": d["parent_va"],
                    }
                func_records.append(rec)

    if not func_records:
        return {
            "body": body, "load_address": load_address, "image_size": image_size,
            "name_by_va": name_by_va, "thunk_map": thunk_map,
            "func_records": [], "discovery": {},
        }

    # re-resolve internal_callees labels now that bfs_names is complete
    for rec in func_records:
        for lst in ("internal_callees", "tail_calls"):
            rec[lst] = [
                (site, target, name_by_va.get(target) or bfs_names.get(target))
                for site, target, _ in rec[lst]
            ]

    return {
        "body": body, "load_address": load_address, "image_size": image_size,
        "name_by_va": name_by_va, "thunk_map": thunk_map,
        "func_records": func_records, "discovery": discovery,
    }


def main():
    module_index_lines = []
    tsv_rows = []
    total_edges = 0
    total_funcs = 0
    total_bfs_funcs = 0

    os.makedirs(os.path.join(cc.REF, "calls"), exist_ok=True)

    for rel in cc.PRIORITY_MODULES:
        mod = discover_module_functions(rel)
        if mod is None:
            info = cc.decoded_body_meta(rel) if os.path.exists(os.path.join(cc.DUMP, rel)) else None
            status = info["status"] if info else "MISSING"
            module_index_lines.append(f"{rel}  ->  SKIPPED ({status})")
            continue
        func_records, discovery = mod["func_records"], mod["discovery"]
        if not func_records:
            module_index_lines.append(f"{rel}  ->  SKIPPED (no exports and no entry point reachable)")
            continue
        load_address = mod["load_address"]

        edges_all = []
        for rec in func_records:
            for target, kind in rec["edges"]:
                edges_all.append((rec["va"], target, kind))
        n_export = sum(1 for r in func_records if discovery[r["va"]]["kind"] == "export")
        n_bfs = len(func_records) - n_export
        total_funcs += n_export
        total_bfs_funcs += n_bfs

        callers_of = {}
        for caller_va, callee_va, kind in edges_all:
            if kind == "internal":
                callers_of.setdefault(callee_va, []).append(caller_va)

        out_name = rel.replace("/", "__").replace("$", "") + ".calls.txt"
        out_path = os.path.join(cc.REF, "calls", out_name)
        with open(out_path, "w") as f:
            f.write(f"Call graph for {rel}\n")
            f.write(f"xex_load_address=0x{load_address:08X}\n")
            f.write(f"{n_export} exported functions (boundary = next export's VA), "
                    f"{n_bfs} BFS-discovered internal functions (boundary = hard cap/return, "
                    f"capped at {BFS_CAP}) - see each function's discovery= line for exactly "
                    f"how it was found (entry-point reachability vs. callback-registration "
                    f"provenance are tracked separately and honestly, not collapsed).\n")
            f.write("Neither boundary method is verified prologue/epilogue analysis - see "
                    "per-function confidence tags below.\n")
            f.write("=" * 70 + "\n\n")
            for rec in sorted(func_records, key=lambda r: r["va"]):
                disc = discovery[rec["va"]]
                f.write(f"{rec['name']}  [{disc['kind']}]  va=0x{rec['va']:08X}\n")
                f.write(f"    discovery: {discovery_label(disc)}\n")
                confidence = "INFERRED (boundary-truncated/BFS)" if rec["bounded"] else "CONFIRMED (reached real return)"
                f.write(f"    disassembled {rec['insn_count']} insns, {confidence}\n")
                callers = sorted(set(callers_of.get(rec["va"], [])))
                if callers:
                    f.write(f"    known_callers: {', '.join('0x%08X' % c for c in callers)}\n")
                if rec["internal_callees"]:
                    f.write("    internal_callees:\n")
                    for site, target, label in rec["internal_callees"]:
                        lbl = f"  ({label})" if label else ""
                        f.write(f"      0x{site:08X} -> 0x{target:08X}{lbl}\n")
                if rec["imported_callees"]:
                    f.write("    imported_callees:\n")
                    for site, label in rec["imported_callees"]:
                        f.write(f"      0x{site:08X} -> {label}\n")
                if rec["tail_calls"]:
                    f.write("    tail_calls:\n")
                    for site, target, label in rec["tail_calls"]:
                        lbl = f"  ({label})" if label else ""
                        f.write(f"      0x{site:08X} -> 0x{target:08X}{lbl}\n")
                f.write("\n")
                for site, target, label in rec["internal_callees"]:
                    tsv_rows.append((rel, rec["name"], f"0x{rec['va']:08X}", "internal",
                                     f"0x{site:08X}", f"0x{target:08X}", label or ""))
                for site, label in rec["imported_callees"]:
                    tsv_rows.append((rel, rec["name"], f"0x{rec['va']:08X}", "import",
                                     f"0x{site:08X}", "", label))
                total_edges += len(rec["internal_callees"]) + len(rec["imported_callees"])

        module_index_lines.append(
            f"{rel}  ->  reference/calls/{out_name}  "
            f"({n_export} exported + {n_bfs} BFS-discovered functions disassembled)"
        )

    with open(os.path.join(cc.REF, "04_CALL_GRAPH_INDEX.txt"), "w") as f:
        f.write("Call-graph coverage index (priority modules only)\n")
        f.write("=====================================================\n")
        f.write("Regenerate with: python3 _tool/build_04_calls.py\n\n")
        f.write("Scope is intentionally limited to corpus_common.PRIORITY_MODULES\n")
        f.write("(XAM/avatar/gamer-profile/content/title-relevant modules).\n\n")
        f.write(f"Modules with >= {EXPORT_BFS_THRESHOLD} exports (xam.xex/$flash_xam.xexp) use\n")
        f.write("export-boundary-only coverage (reliable boundaries, large export tables\n")
        f.write("already give good coverage).\n\n")
        f.write(f"Modules with fewer than {EXPORT_BFS_THRESHOLD} exports additionally get\n")
        f.write(f"BFS-discovered internal-function coverage (capped at {BFS_CAP} functions,\n")
        f.write("reached via `bl` starting from the entry point, PLUS an opportunistic\n")
        f.write("callback-root expansion for *Register*-style imports) - these are\n")
        f.write("application-style modules (gamerprofile/createprofile/deviceselector/\n")
        f.write("signin/dash) whose real logic lives almost entirely in unexported\n")
        f.write("functions, so export-only coverage would find almost nothing. Each\n")
        f.write("function's calls/*.txt entry states EXACTLY how it was discovered\n")
        f.write("(export / bfs entry-reachable / callback root / callback descendant) -\n")
        f.write("only the function whose address was literally passed to a *Register*\n")
        f.write("import is labeled as a callback root; anything reached from it via\n")
        f.write("further `bl`s is honestly labeled a descendant, not another root.\n\n")
        f.write("build_05_strings.py (string xrefs) and build_06_ipc.py (IPC candidate\n")
        f.write("scanning) both reuse discover_module_functions() from this script, so\n")
        f.write("they see exactly this same function set, including callback\n")
        f.write("descendants - not a narrower export-only view.\n\n")
        for line in sorted(module_index_lines):
            f.write(line + "\n")

    with open(os.path.join(cc.REF, "calls.tsv"), "w") as f:
        f.write("module\tcaller_name\tcaller_va\tcallee_kind\tcallsite_va\tcallee_va\tcallee_label\n")
        for row in tsv_rows:
            f.write("\t".join(row) + "\n")

    n_modules = len([l for l in module_index_lines if "SKIPPED" not in l])
    print(f"Disassembled {total_funcs} exported + {total_bfs_funcs} BFS-discovered functions "
          f"across {n_modules} priority modules, {total_edges} call edges")


if __name__ == "__main__":
    main()
