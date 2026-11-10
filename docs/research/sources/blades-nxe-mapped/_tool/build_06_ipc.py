"""Builds reference/06_IPC_SERVICE_MAP.txt, reference/ipc.tsv (PROVEN IPC
only), and reference/_ipc_candidates.tsv (every candidate, proven or not -
this is now the single source of truth for both files; build_04_calls.py no
longer writes its own separate candidates dump).

=====================================================================
ENDPOINT-PROOF ARCHITECTURE (rewritten - see 10_UNRESOLVED.txt #9 for the
prior, more permissive version's known problems and why this replaced it)
=====================================================================

CANDIDATE DISCOVERY (unchanged bootstrap): scan every known function
(exports + BFS-discovered internals, same set build_04_calls.py uses) for
the `li r3, PORT-shaped-value(0x80..0xFF); ...; bl TARGET` shape via
disasm_support.extract_li_r3_sequences. This is a CANDIDATE signal only -
a small in-range r3 value can just be an ordinary parameter to an ordinary
function (real examples seen: XamUserCreateStatsEnumerator's candidate
target turned out to be XamCreateEnumeratorHandle; several others resolve
to RtlSetLastError, XuiElementGetFocusUser, XamContentCreateInternal,
XamContentCreateDeviceEnumerator - none of those are IPC). EVERY occurrence
is kept as its own candidate (module, caller_va, callsite_va) - a function
can dispatch different commands on different branches, and a single
candidate list entry is per CALLSITE, not per function.

ENDPOINT REGISTRY (per module): the set of VAs that are PROVEN IPC
endpoints for that module, built from actual local data, never
hardcoded:
  - any of the module's OWN exports whose real name (resolved from
    Xenia's xam ordinal table, when available) starts with "XMsg";
  - any import thunk whose resolved name (from reference/imports/*.txt,
    i.e. Xenia's kernel/xam/xbdm ordinal databases) starts with "XMsg".
This is NOT hardcoded to the 4 XMsg functions this corpus happened to
find first (XMsgInProcessCall/XMsgSystemProcessCall/XMsgStartIORequest/
XMsgStartIORequestEx) - whatever XMsg* names actually appear in a given
module's own tables is what gets used for that module.

ENDPOINT-CHAIN PROOF: for each candidate, disasm_support.find_endpoint_chain
does a bounded (depth<=4, node_cap<=40) breadth-first search over `bl`
targets starting from the candidate's target, looking for ANY path to a
registered endpoint - not just the exact li-r3-pattern shape at every hop.
  - No path found within bounds -> candidate stays CANDIDATE, is NOT
    written to ipc.tsv/06_IPC_SERVICE_MAP.txt (only to
    _ipc_candidates.tsv).
  - Path found -> promoted to DERIVED confidence, with the full
    caller->...->endpoint chain recorded (not flattened into a single
    "dispatcher" field - see evidence_chain).

ARGUMENT RESOLUTION ALONG THE CHAIN: resolve_ipc_args() walks the proven
chain from the endpoint backward toward the original caller, resolving
port(r3)/command(r4) at each hop via disasm_support.resolve_register_value_before,
and where a hop's own value is unresolved but is a pure register-copy of
one of ITS OWN incoming parameters (disasm_support.trace_incoming_register),
recurses to the PREVIOUS hop to resolve that parameter's real value there.
This generalizes the old hardcoded "leaf ⁄ helper ⁄ fixed" 0/1-hop-only
logic to arbitrary chain depth, and is what CONFIRMED_AVATAR is validated
against below (the old logic was a special case of this).

command_source / port_source vocabulary:
  CALLER_CONSTANT    - resolved to a literal constant AT the original
                       candidate callsite itself (0-hop, or forwarded
                       transparently through 1+ pure-passthrough wrappers).
  FIXED_WRAPPER       - resolved to a literal constant baked into an
                       intermediate wrapper (NOT the original caller) -
                       e.g. dash.xex's 0x921ea088, which always sends
                       command 0x58004 regardless of its caller.
  FORWARDED_ARGUMENT  - traced all the way back to being exactly one of
                       the ORIGINAL CALLER's own incoming parameters, but
                       that parameter's actual value isn't known (we'd
                       need to look at code outside this scan's scope -
                       whoever calls the original caller).
  COMPUTED_WRAPPER    - an intermediate wrapper defines the value via
                       something other than a constant or a pure register
                       copy (a memory load, arithmetic, etc.) - genuinely
                       not statically resolvable at this granularity.
  UNKNOWN             - none of the above could be established.
"""
import os
import corpus_common as cc
import disasm_support as ds
import build_04_calls as b4

# Hand-verified avatar entries (raw disassembly read, not just automated
# decoding) - CONFIRMED confidence. This script's automated chain-proof
# engine is cross-validated against every one of these on every run before
# its output is trusted for the rest of the corpus.
CONFIRMED_AVATAR = [
    # (module, export_name, ordinal, caller_va, port, command, dispatcher_va, note)
    ("$flash_xam.xexp", "XamAvatarInitialize", 0x5DC, 0x818D2EA8, 0xF3, 0x600001, 0x818FD2D8, ""),
    ("$flash_xam.xexp", "XamAvatarShutdown", 0x5DD, 0x818D24D0, 0xF3, 0x600002, 0x818FD2D8, "matches pre-existing xam_avatar.cc comment"),
    ("$flash_xam.xexp", "XamAvatarGetManifestLocalUser", 0x5DE, 0x818D2520, 0xF3, 0x600003, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGetManifestsByXuid", 0x5DF, 0x818D2578, 0xF3, 0x600004, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGetAssetsResultSize", 0x5E0, 0x818D2618, 0xF3, 0x600005, 0x818FD548, ""),
    ("$flash_xam.xexp", "XamAvatarGetAssets", 0x5E1, 0x818D3038, 0xF3, 0x600006, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGenerateMipMaps", 0x5E2, 0x818D27D0, 0xF3, 0x600007, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGetAssetBinary", 0x5F5, 0x818D2680, 0xF3, 0x600008, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarSetManifest", 0x5E4, 0x818D2FD0, 0xF3, 0x600009, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarSetCustomAsset", 0x5E3, 0x818D2A90, 0xF3, 0x60000A, 0x818FD548, ""),
    ("$flash_xam.xexp", "XamAvatarGetAssetIcon", 0x5F3, 0x818D2728, 0xF3, 0x60000B, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarBeginEnumAssets", 0x5E8, 0x818D2B08, 0xF3, 0x60000C, 0x818D2470, "matches pre-existing xam_avatar.cc comment"),
    ("$flash_xam.xexp", "XamAvatarEnumAssets", 0x5EA, 0x818D2B88, 0xF3, 0x60000D, 0x818D2470, "matches pre-existing xam_avatar.cc comment"),
    ("$flash_xam.xexp", "XamAvatarEndEnumAssets", 0x5E9, 0x818D2BF8, 0xF3, 0x60000E, 0x818D2470, "existing comment said port 0xf2 (older build 12611); this retail (9199) build uses 0xf3"),
    ("$flash_xam.xexp", "XamAvatarLoadAnimation", 0x5E7, 0x818D2850, 0xF3, 0x60000F, 0x818D2470, "matches pre-existing xam_avatar.cc comment"),
    ("$flash_xam.xexp", "XamAvatarGetMetadataRandom", 0x5EB, 0x818D2C48, 0xF3, 0x600010, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGetMetadataSignedOutProfileCount", 0x5EE, 0x818D2CD0, 0xF3, 0x600013, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamAvatarGetMetadataSignedOutProfile", 0x5EF, 0x818D2D10, 0xF3, 0x600014, 0x818D2470, ""),
    ("$flash_xam.xexp", "XamShowAvatarMiniCreatorUI", 0x6A4, 0x81916F90, 0xFE, 0x21018, 0x818FD5B8, ""),
    ("$flash_xam.xexp", "XamUserGetUsersMissingAvatars", 0x22C, 0x81928570, 0xFE, 0x2300A, 0x818FD540, ""),
    ("$flash_xam.xexp", "XamShowAvatarAwardsUI", 0x3C6, 0x81917298, 0xFE, 0x21008, 0x818FD5B8, ""),
    ("$flash_xam.xexp", "XamShowAvatarAwardGamesUI", 0x3C7, 0x819171F0, 0xFE, 0x21008, 0x818FD5B8, ""),
    ("$flash_xam.xexp", "XamAvatarWearNow", 0x5F4, 0x818D28E8, 0xF3, 0x600018, 0x818D2470,
     "else-branch (non-kAvatarEditorID title) hand-verified; matches pre-existing xam_avatar.cc "
     "comment exactly. kAvatarEditorID branch does NOT use this IPC path at all - it does a "
     "lock-free ring-buffer write instead (XamSendMessageToLoadedApps/XNotifyBroadcast per the "
     "existing comment), consistent with that comment's description."),
]


def get_all_functions(rel):
    """Thin adapter over build_04_calls.discover_module_functions() (the
    CANONICAL function-discovery routine - see that module's docstring) so
    the rest of this file can keep using its existing {va: (name, insns)}
    shape. This is what gives the IPC scan the SAME function set (including
    callback descendants, e.g. gamerprofile.xex's real profile-writing
    logic) that the call graph and string-xref passes see - a candidate
    li-r3-port pattern found only inside a callback-discovered function
    would have been invisible to IPC scanning before this reuse."""
    mod = b4.discover_module_functions(rel)
    if mod is None or not mod["func_records"]:
        return None
    funcs = {rec["va"]: (rec["name"], rec["insns"]) for rec in mod["func_records"]}
    return {
        "body": mod["body"], "load_address": mod["load_address"], "image_size": mod["image_size"],
        "funcs": funcs, "name_by_va": mod["name_by_va"],
    }


def build_endpoint_registry(rel, name_by_va, thunk_map):
    """va -> (kind, label) for every PROVEN XMsg* endpoint local to this
    module - derived entirely from this module's own export/import tables,
    never hardcoded."""
    registry = {}
    fn = os.path.basename(rel)
    for va, name in name_by_va.items():
        if name.startswith("XMsg"):
            registry[va] = ("local_export", f"{fn}!{name}")
    for va, label in thunk_map.items():
        name = label.split("!", 1)[-1]
        if name.startswith("XMsg"):
            registry[va] = ("import_thunk", label)
    return registry


def resolve_ipc_args(chain, caller_insns, caller_bl_index, body, load_address, image_size):
    """chain: [(va, 'wrapper'/'endpoint', label), ...] from find_endpoint_chain
    (chain[0] is the candidate's direct target; chain[-1] is the endpoint).
    Returns dict with port/command values+sources and the per-hop VA list."""
    hops = [(caller_insns, caller_bl_index)]
    for i in range(len(chain) - 1):
        va, next_va = chain[i][0], chain[i + 1][0]
        insns = ds.disassemble_bounded(body, load_address, va, va + 0x600)
        bl_index = None
        for k, ins in enumerate(insns):
            if ins.mnemonic != "bl":
                continue
            try:
                t = int(ins.op_str.strip(), 16)
            except ValueError:
                continue
            if t == next_va:
                bl_index = k
                break
        if bl_index is None:
            return None  # chain claimed a path that we can't re-locate - bail honestly
        hops.append((insns, bl_index))

    def resolve_at_hop(hop_idx, reg):
        insns, bl_index = hops[hop_idx]
        val = ds.resolve_register_value_before(insns, bl_index, reg)
        if val is not None:
            return val, ("CALLER_CONSTANT" if hop_idx == 0 else "FIXED_WRAPPER")
        aliased = ds.trace_incoming_register(insns, bl_index, reg)
        if aliased is not None:
            if hop_idx == 0:
                return None, "FORWARDED_ARGUMENT"
            return resolve_at_hop(hop_idx - 1, aliased)
        return None, ("UNKNOWN" if hop_idx == 0 else "COMPUTED_WRAPPER")

    final_hop = len(hops) - 1
    port, port_source = resolve_at_hop(final_hop, "r3")
    command, command_source = resolve_at_hop(final_hop, "r4")
    # trailing args, resolved at the FINAL hop only (i.e. the values actually
    # handed to the endpoint) - registers r5..r8, best-effort.
    ipc_args = []
    for reg in ("r5", "r6", "r7", "r8"):
        v, _ = resolve_at_hop(final_hop, reg)
        ipc_args.append(v)
    # caller's OWN registers at the original candidate callsite, for
    # reference even when they turned out not to be the real IPC args
    # (e.g. a FIXED_WRAPPER case) - see 08_STRUCTURES.txt.
    caller_args = [ds.resolve_register_value_before(caller_insns, caller_bl_index, r)
                   for r in ("r3", "r4", "r5", "r6", "r7", "r8")]
    return {
        "port": port, "port_source": port_source,
        "command": command, "command_source": command_source,
        "ipc_args": ipc_args, "caller_args": caller_args,
        "hop_count": len(chain) - 1,
    }


def dispatcher_kind_for(hop_count, endpoint_kind, command_source):
    if hop_count == 0:
        return "DIRECT_XMSG_THUNK" if endpoint_kind == "import_thunk" else "DIRECT_XMSG_EXPORT"
    if hop_count == 1:
        if command_source == "FIXED_WRAPPER":
            return "FIXED_COMMAND_HELPER"
        if command_source in ("CALLER_CONSTANT", "FORWARDED_ARGUMENT"):
            return "FORWARDING_HELPER"
        return "COMPUTED_COMMAND_HELPER"
    return f"WRAPPER_CHAIN_{hop_count}HOP"


def main():
    confirmed_rows = []
    for e in CONFIRMED_AVATAR:
        module, name, ordinal, va, port, command, dispatcher, note = e
        confirmed_rows.append({
            "module": module, "caller": name, "caller_va": va, "callsite_va": None,
            "port": port, "command": command, "command_source": "CONFIRMED",
            "endpoint_va": dispatcher, "endpoint_name": None, "endpoint_kind": "hand-verified",
            "dispatcher_kind": "CONFIRMED", "evidence_chain": "hand-verified (raw disassembly read)",
            "confidence": "CONFIRMED", "note": note,
            "ipc_args": None, "caller_args": None,
        })
    confirmed_keys = {(r["module"], r["caller_va"]) for r in confirmed_rows}

    module_data = {}
    thunk_maps = {}
    endpoint_registries = {}
    for rel in cc.PRIORITY_MODULES:
        data = get_all_functions(rel)
        if not data:
            continue
        module_data[rel] = data
        info = cc.decoded_body_meta(rel)
        thunk_maps[rel] = b4.build_thunk_map(info["header_bytes"], data["body"], data["load_address"])
        endpoint_registries[rel] = build_endpoint_registry(rel, data["name_by_va"], thunk_maps[rel])

    # --- validate the chain-proof engine against every CONFIRMED entry ---
    # Also uses this SAME validated engine to fill in ipc_args/caller_args
    # for the CONFIRMED rows (the hand-verification only established
    # port+command by eye; trailing-argument values are mechanically
    # recomputed here, not hand-copied - see 08_STRUCTURES.txt).
    confirmed_by_va = {r["caller_va"]: r for r in confirmed_rows}
    validation_matches = 0
    validation_mismatches = []
    xam_rel = "$flash_xam.xexp"
    xam_data = module_data.get(xam_rel)
    if xam_data:
        body, load_address, image_size = xam_data["body"], xam_data["load_address"], xam_data["image_size"]
        registry = endpoint_registries[xam_rel]
        for e in CONFIRMED_AVATAR:
            _, name, ordinal, va, exp_port, exp_command, exp_dispatcher, _ = e
            if va not in xam_data["funcs"]:
                validation_mismatches.append((name, exp_port, exp_command, "NOT FOUND (not in scanned function set)"))
                continue
            _, insns = xam_data["funcs"][va]
            found = None
            for i, insn in enumerate(insns):
                if insn.mnemonic != "bl":
                    continue
                try:
                    target = int(insn.op_str.strip(), 16)
                except ValueError:
                    continue
                chain = ds.find_endpoint_chain(body, load_address, image_size, target, registry)
                if not chain:
                    continue
                result = resolve_ipc_args(chain, insns, i, body, load_address, image_size)
                if result and result["port"] == exp_port:
                    found = result
                    break
            if found and found["command"] == exp_command:
                validation_matches += 1
                row = confirmed_by_va.get(va)
                if row is not None:
                    row["ipc_args"] = found["ipc_args"]
                    row["caller_args"] = found["caller_args"]
            else:
                validation_mismatches.append((name, exp_port, exp_command,
                                              (found["port"], found["command"]) if found else None))

    # --- corpus-wide candidate discovery + chain proof (EVERY callsite kept) ---
    proven_rows = []
    candidate_rows = []
    chain_cache = {}  # (rel, target_va) -> chain-or-None, safe: find_endpoint_chain is depth-invariant per target

    for rel, data in module_data.items():
        body, load_address, image_size, funcs = (
            data["body"], data["load_address"], data["image_size"], data["funcs"])
        registry = endpoint_registries[rel]
        for va, (name, insns) in sorted(funcs.items()):
            for li_va, port_guess, bl_va, target in ds.extract_li_r3_sequences(insns):
                if (rel, va) in confirmed_keys:
                    continue  # avoid duplicating the hand-verified rows
                bl_index = next((k for k, ins in enumerate(insns) if ins.address == bl_va), None)
                if bl_index is None:
                    continue
                cache_key = (rel, target)
                if cache_key not in chain_cache:
                    chain_cache[cache_key] = ds.find_endpoint_chain(
                        body, load_address, image_size, target, registry)
                chain = chain_cache[cache_key]

                if not chain:
                    candidate_rows.append({
                        "module": rel, "caller": name, "caller_va": va, "callsite_va": bl_va,
                        "target_va": target, "port_guess": port_guess,
                        "status": "UNRESOLVED_NO_ENDPOINT",
                        "note": "candidate target does not statically reach a registered XMsg* "
                               "endpoint within the search bound (depth<=4, node_cap<=40) - "
                               "target may be an ordinary function, or the real path exceeds "
                               "this scan's bound",
                    })
                    continue

                result = resolve_ipc_args(chain, insns, bl_index, body, load_address, image_size)
                if result is None or result["port"] is None or not (ds.PORT_VALUE_MIN <= result["port"] <= 0xFF):
                    candidate_rows.append({
                        "module": rel, "caller": name, "caller_va": va, "callsite_va": bl_va,
                        "target_va": target, "port_guess": port_guess,
                        "status": "ENDPOINT_FOUND_BUT_ARGS_UNRESOLVED",
                        "note": f"chain reaches {chain[-1][2]} but port could not be resolved "
                               f"to a plausible value",
                    })
                    continue

                endpoint_va, _, endpoint_label = chain[-1]
                hop_count = result["hop_count"]
                dispatcher_kind = dispatcher_kind_for(hop_count, endpoint_registries[rel][endpoint_va][0],
                                                      result["command_source"])
                chain_str = " -> ".join(
                    f"0x{va:08X}" + (f"({label})" if label else "") for va, kind, label in chain)
                evidence_chain = f"0x{va:08X}[{name}]@0x{bl_va:08X} -> " + chain_str

                proven_rows.append({
                    "module": rel, "caller": name, "caller_va": va, "callsite_va": bl_va,
                    "port": result["port"], "command": result["command"],
                    "command_source": result["command_source"],
                    "endpoint_va": endpoint_va, "endpoint_name": endpoint_label,
                    "endpoint_kind": endpoint_registries[rel][endpoint_va][0],
                    "dispatcher_kind": dispatcher_kind,
                    "evidence_chain": evidence_chain,
                    "confidence": "DERIVED",
                    "note": f"{hop_count}-hop chain; port_source={result['port_source']}",
                    "ipc_args": result["ipc_args"], "caller_args": result["caller_args"],
                })

    # de-dup safety net: (module, caller_va, callsite_va, endpoint_va) must be unique
    seen_keys = set()
    deduped_proven = []
    dup_count = 0
    for r in proven_rows:
        key = (r["module"], r["caller_va"], r["callsite_va"], r["endpoint_va"])
        if key in seen_keys:
            dup_count += 1
            continue
        seen_keys.add(key)
        deduped_proven.append(r)
    proven_rows = deduped_proven

    all_rows = confirmed_rows + proven_rows

    os.makedirs(cc.REF, exist_ok=True)
    with open(os.path.join(cc.REF, "06_IPC_SERVICE_MAP.txt"), "w") as f:
        f.write("System-message / IPC service map (PROVEN IPC only)\n")
        f.write("=======================================================\n")
        f.write("Regenerate with: python3 _tool/build_06_ipc.py\n\n")
        f.write("Every row here has a statically-proven evidence chain terminating at a\n")
        f.write("registered XMsg* endpoint (a local export or import thunk whose real,\n")
        f.write("ordinal-database-resolved name starts with 'XMsg' - see this script's\n")
        f.write("module docstring). Candidates whose target does NOT reach such an\n")
        f.write("endpoint within the search bound are NOT here - see\n")
        f.write("_ipc_candidates.tsv for the full unfiltered candidate list, including\n")
        f.write("ones demoted for lacking endpoint proof.\n\n")
        f.write("CONFIRMED rows are hand-verified against raw disassembly (see\n")
        f.write("CONFIRMED_AVATAR in build_06_ipc.py) for port+command only. DERIVED rows\n")
        f.write("are automated chain-proof results - the endpoint-reaching mechanism is\n")
        f.write("validated against every CONFIRMED entry below before being trusted\n")
        f.write("corpus-wide. Once validated, that SAME engine is also used to fill in\n")
        f.write("ipc_args/caller_args for the CONFIRMED rows (trailing arguments were not\n")
        f.write("part of the original hand-verification, only port+command were) - so\n")
        f.write("those two fields are DERIVED-mechanism-sourced even on CONFIRMED rows;\n")
        f.write("see 08_STRUCTURES.txt.\n\n")
        f.write(f"Chain-proof engine cross-validation against the {len(CONFIRMED_AVATAR)} CONFIRMED\n")
        f.write(f"avatar entries: {validation_matches}/{len(CONFIRMED_AVATAR)} port+command pairs matched exactly.\n")
        if validation_mismatches:
            f.write(f"{len(validation_mismatches)} MISMATCHES:\n")
            for name, exp_port, exp_command, found in validation_mismatches:
                f.write(f"    {name}: expected port=0x{exp_port:X} command=0x{exp_command:X}, "
                        f"decoder found {found}\n")
        else:
            f.write("No mismatches.\n")
        if dup_count:
            f.write(f"\n{dup_count} duplicate (module,caller_va,callsite_va,endpoint_va) rows "
                    f"were found and collapsed to one.\n")
        f.write("\n" + "-" * 70 + "\n\n")

        # group by (module, caller) for readability, but each callsite is its own entry
        by_func = {}
        for r in all_rows:
            by_func.setdefault((r["module"], r["caller"], r["caller_va"]), []).append(r)

        for (mod, caller, caller_va) in sorted(by_func, key=lambda k: (k[0], k[2])):
            sites = sorted(by_func[(mod, caller, caller_va)], key=lambda r: (r["callsite_va"] or 0))
            f.write(f"{mod}  {caller}  (va=0x{caller_va:08X})\n")
            if len(sites) > 1:
                f.write(f"    {len(sites)} distinct IPC callsites in this function:\n")
            for r in sites:
                site_s = f"0x{r['callsite_va']:08X}" if r["callsite_va"] else "(hand-verified, exact site not tracked separately)"
                f.write(f"    callsite={site_s}  port=0x{r['port']:X}  "
                        f"command={'0x%X' % r['command'] if r['command'] else 'UNKNOWN'}"
                        f"  command_source={r['command_source']}\n")
                f.write(f"      endpoint=0x{r['endpoint_va']:08X}"
                        f"{'  (' + str(r['endpoint_name']) + ')' if r['endpoint_name'] else ''}"
                        f"  dispatcher_kind={r['dispatcher_kind']}\n")
                f.write(f"      evidence_chain: {r['evidence_chain']}\n")
                if r.get("ipc_args"):
                    args_s = ", ".join(f"0x{a:X}" if a is not None else "?" for a in r["ipc_args"])
                    f.write(f"      ipc_args=[{args_s}]  (resolved at the final hop into the "
                            f"endpoint - see 08_STRUCTURES.txt before assigning meaning)\n")
                if r.get("caller_args") and r["caller_args"] != r.get("ipc_args"):
                    cargs_s = ", ".join(f"0x{a:X}" if a is not None else "?" for a in r["caller_args"])
                    f.write(f"      caller_args=[{cargs_s}]  (raw registers at the ORIGINAL "
                            f"callsite - informational; NOT necessarily what reaches the "
                            f"endpoint when a wrapper transforms them, e.g. FIXED_COMMAND_HELPER)\n")
                f.write(f"      confidence={r['confidence']}\n")
                if r["note"]:
                    f.write(f"      note: {r['note']}\n")
            f.write("\n")

    with open(os.path.join(cc.REF, "ipc.tsv"), "w") as f:
        cols = ["module", "caller", "caller_va", "callsite_va", "port", "command", "command_source",
                "endpoint_va", "endpoint_name", "endpoint_kind", "dispatcher_kind", "evidence_chain",
                "ipc_args", "caller_args", "confidence", "note"]
        f.write("\t".join(cols) + "\n")
        for r in all_rows:
            def hx(v):
                return f"0x{v:X}" if isinstance(v, int) else ("" if v is None else str(v))
            def arglist(v):
                if not v:
                    return ""
                return ",".join(f"0x{a:X}" if a is not None else "?" for a in v)
            row = [
                r["module"], r["caller"], hx(r["caller_va"]), hx(r["callsite_va"]),
                hx(r["port"]), hx(r["command"]), r["command_source"],
                hx(r["endpoint_va"]), str(r["endpoint_name"] or ""), str(r["endpoint_kind"] or ""),
                r["dispatcher_kind"], r["evidence_chain"] or "",
                arglist(r.get("ipc_args")), arglist(r.get("caller_args")),
                r["confidence"], r["note"],
            ]
            f.write("\t".join(row) + "\n")

    with open(os.path.join(cc.REF, "_ipc_candidates.tsv"), "w") as f:
        f.write("module\tcaller\tcaller_va\tcallsite_va\ttarget_va\tport_guess\tstatus\tnote\n")
        for c in candidate_rows:
            f.write("\t".join([
                c["module"], c["caller"], f"0x{c['caller_va']:08X}", f"0x{c['callsite_va']:08X}",
                f"0x{c['target_va']:08X}", f"0x{c['port_guess']:X}", c["status"], c["note"],
            ]) + "\n")
        # also record proven rows here for completeness (every candidate,
        # proven or not, per this script's module docstring) - marked PROVEN.
        for r in proven_rows:
            f.write("\t".join([
                r["module"], r["caller"], f"0x{r['caller_va']:08X}", f"0x{r['callsite_va']:08X}",
                f"0x{r['endpoint_va']:08X}", f"0x{r['port']:X}" if r["port"] else "",
                "PROVEN", r["dispatcher_kind"],
            ]) + "\n")

    confirmed_n = len(confirmed_rows)
    derived_n = len(proven_rows)
    multi_site_funcs = sum(1 for k, v in by_func.items() if len(v) > 1)
    print(f"IPC map: {confirmed_n} CONFIRMED + {derived_n} DERIVED (endpoint-proven) = "
          f"{len(all_rows)} entries. {len(candidate_rows)} candidates demoted "
          f"(no endpoint proof). {multi_site_funcs} functions have >1 retained IPC callsite. "
          f"Validation: {validation_matches}/{len(CONFIRMED_AVATAR)} "
          f"matched, {len(validation_mismatches)} mismatches, {dup_count} duplicates removed.")


if __name__ == "__main__":
    main()
