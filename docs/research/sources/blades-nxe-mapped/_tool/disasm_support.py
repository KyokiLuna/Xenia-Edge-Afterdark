"""Shared bounded PPC disassembly helpers for call-graph / IPC-map / string-xref
extraction. Deliberately NOT a decompiler: we disassemble each exported
function up to the next known boundary (or a hard cap) and pattern-match on
`bl` targets and immediate-load sequences. Good enough for connectivity,
not for byte-perfect semantics.
"""
import capstone

MD = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_64)
MD.detail = False

HARD_CAP_BYTES = 8192


def disassemble_bounded(body, load_address, va, next_boundary_va=None):
    """Disassemble starting at va until: a top-of-stack `blr`/`bctr` (crude
    function-end heuristic), next_boundary_va is reached, or HARD_CAP_BYTES.
    Returns list of capstone insn objects."""
    off = va - load_address
    if off < 0 or off >= len(body):
        return []
    limit = len(body)
    if next_boundary_va is not None:
        limit = min(limit, next_boundary_va - load_address)
    limit = min(limit, off + HARD_CAP_BYTES)
    code = body[off:limit]
    insns = []
    for insn in MD.disasm(code, va):
        insns.append(insn)
        if insn.mnemonic in ("blr", "bctr") :
            break
    return insns


def extract_bl_targets(insns):
    """Returns list of (call_site_va, target_va, mnemonic) for bl/b instructions
    with an immediate target operand."""
    out = []
    for insn in insns:
        if insn.mnemonic in ("bl", "b"):
            ops = insn.op_str.strip()
            if ops.startswith("0x"):
                try:
                    target = int(ops, 16)
                    out.append((insn.address, target, insn.mnemonic))
                except ValueError:
                    pass
    return out


# Every real port value found so far (0xf3, 0xfb, 0xfe) sits in the top
# quarter of the byte range - restricting to that band cuts out the flood
# of `li r3, 0`/`li r3, 1`-style false positives (boolean flags, loop
# bounds, etc.) that a bare "<=0xFF" filter picks up on non-XAM modules
# like AvatarEditor.xex, while still leaving room to discover new port
# values this corpus hasn't seen yet.
PORT_VALUE_MIN = 0x80


def extract_li_r3_sequences(insns, window=6):
    """Heuristic IPC-dispatch detector: find `li r3, IMM` (small immediate
    in [PORT_VALUE_MIN, 0xFF] - matches the observed port-number pattern)
    followed within `window` instructions by a `bl`. Returns list of
    (li_va, port_value, bl_va, bl_target)."""
    out = []
    for i, insn in enumerate(insns):
        if insn.mnemonic == "li" and insn.op_str.startswith("r3, "):
            try:
                val = int(insn.op_str.split(",")[1].strip(), 0)
            except ValueError:
                continue
            if not (PORT_VALUE_MIN <= val <= 0xFF):
                continue
            for j in range(i + 1, min(i + 1 + window, len(insns))):
                if insns[j].mnemonic == "bl":
                    try:
                        target = int(insns[j].op_str.strip(), 16)
                    except ValueError:
                        continue
                    out.append((insn.address, val, insns[j].address, target))
                    break
    return out


def discover_functions_bfs(body, load_address, image_size, roots, cap=250):
    """Bounded breadth-first discovery of function entry points reached via
    `bl` from a set of root addresses (typically: the module's entry point
    plus its exports, if any). Each discovered function is disassembled with
    disassemble_bounded() using a fixed byte cap (no "next export" boundary
    available for internal functions, so this relies purely on hitting a
    real blr/bctr or the hard byte cap).

    This is NOT a disassembler-grade function-boundary analysis - two
    functions can still get merged if one falls through into the next
    without a blr, and a function's true end may be missed if it exceeds
    HARD_CAP_BYTES. Good enough for connectivity on small, simple modules
    (which is what this is used for - modules with few/no exports, where
    exported-function-only coverage would find almost nothing).

    Returns dict: va -> {"insns": [...], "bounded": bool, "root_va": va,
    "parent_va": va-or-None}. `root_va` is which entry in `roots` this
    function was ultimately discovered from (so descendants reached via
    several hops of `bl`-following can still be traced back to the actual
    registered root, rather than losing that provenance - see
    build_04_calls.py's callback-descendant handling). `parent_va` is the
    immediate caller within this BFS tree that led to this function, or
    None if this function IS one of the roots itself.
    """
    result = {}
    queue = [(r, r, None) for r in dict.fromkeys(roots)]  # (va, root_va, parent_va), de-dup roots
    while queue and len(result) < cap:
        va, root_va, parent_va = queue.pop(0)
        if va in result:
            continue
        if not (load_address <= va < load_address + image_size):
            continue
        insns = disassemble_bounded(body, load_address, va)
        if not insns:
            continue
        bounded = insns[-1].mnemonic not in ("blr", "bctr")
        result[va] = {"insns": insns, "bounded": bounded, "root_va": root_va, "parent_va": parent_va}
        for site_va, target, mnem in extract_bl_targets(insns):
            if mnem == "bl" and load_address <= target < load_address + image_size:
                if target not in result and len(result) + len(queue) < cap:
                    queue.append((target, root_va, va))
    return result


def resolve_register_value_before(insns, index, reg, _depth=0):
    """Scan backward from insns[index] (exclusive) looking for the most
    recent definition of `reg`, resolving through `li`, `lis` (returned as
    hi<<16 on its own), `ori`/`addi` combining with a (possibly
    different-register) `lis` source - i.e. both the self-referencing
    `lis rX; ori rX,rX,LO` idiom AND the very common cross-register
    `lis r11,HI; addi r5,r11,LO` address-formation idiom - and single-hop
    `mr` register copies. Returns an int or None if the definition isn't a
    form we understand (e.g. loaded from memory, computed arithmetically,
    or redefined by something else first). Deliberately conservative -
    returns None rather than guessing."""
    if _depth > 6:
        return None
    for j in range(index - 1, -1, -1):
        insn = insns[j]
        parts = [p.strip() for p in insn.op_str.split(",")]
        if not parts or parts[0] != reg:
            continue
        if insn.mnemonic == "li":
            try:
                return int(parts[1], 0)
            except (ValueError, IndexError):
                return None
        if insn.mnemonic == "lis":
            try:
                # capstone prints this operand as a signed 16-bit value
                # (e.g. -0x6ff0); mask to the unsigned bit pattern before
                # shifting, since it's really the top half of a VA.
                return (int(parts[1], 0) & 0xFFFF) << 16
            except (ValueError, IndexError):
                return None
        if insn.mnemonic in ("ori", "addi") and len(parts) >= 3:
            src_reg = parts[1]
            try:
                lo = int(parts[-1], 0)
            except ValueError:
                return None
            base = resolve_register_value_before(insns, j, src_reg, _depth + 1)
            if base is None:
                return None
            return (base | lo) if insn.mnemonic == "ori" else (base + lo)
        if insn.mnemonic == "mr" and len(parts) >= 2:
            return resolve_register_value_before(insns, j, parts[1], _depth + 1)
        # Some other definition (load, arithmetic, etc.) - unknown, stop.
        return None
    return None


def trace_incoming_register(insns, index, reg, _depth=0):
    """Like resolve_register_value_before, but for register-COPY provenance
    rather than constant-VALUE resolution: walks backward through `mr`
    chains ONLY. Returns:
      - the register name (str) this value ultimately traces back to, if it
        reaches the top of the function without ever being defined by
        anything other than `mr` (i.e. it's a pure passthrough of one of
        the function's own incoming parameter registers - the caller can
        then resolve THAT register at the call site that invoked this
        function to recover the real value);
      - None if the register is defined by something other than `mr`
        (a local constant, a computed value, a memory load, etc.) before
        reaching the top - i.e. NOT a pure passthrough.
    Used to generalize the avatar-style "helper forwards its caller's r3"
    pattern to wrapper chains that don't necessarily use the exact same
    register numbering at every hop.
    """
    if _depth > 8:
        return None
    for j in range(index - 1, -1, -1):
        insn = insns[j]
        parts = [p.strip() for p in insn.op_str.split(",")]
        if not parts or parts[0] != reg:
            continue
        if insn.mnemonic == "mr" and len(parts) >= 2:
            return trace_incoming_register(insns, j, parts[1], _depth + 1)
        return None  # defined by something other than a pure register copy
    return reg  # never redefined - this IS the incoming parameter register


ENDPOINT_SEARCH_MAX_DEPTH = 4
ENDPOINT_SEARCH_NODE_CAP = 40


def find_endpoint_chain(body, load_address, image_size, start_va, endpoint_registry,
                        max_depth=ENDPOINT_SEARCH_MAX_DEPTH, node_cap=ENDPOINT_SEARCH_NODE_CAP):
    """Bounded search for a path of `bl` calls from start_va to a proven
    endpoint (any va present in endpoint_registry, e.g. an XMsg* import
    thunk or local export - see build_06_ipc.py). This is general
    reachability, NOT limited to the li-r3-port pattern - a candidate whose
    target is an ordinary named wrapper function is still followed through
    its own callees looking for a proven endpoint several hops down.

    Returns a list of (va, kind, label) hops from start_va (inclusive) to
    the endpoint (inclusive), where kind is "start"/"wrapper"/"endpoint",
    or None if no path to a registered endpoint was found within
    max_depth hops / node_cap total functions explored.

    Deliberately NOT memoized across calls - each candidate gets an
    independent, correctly-depth-bounded search rather than a cache that
    would silently reuse a shallower/deeper partial result. The corpus is
    small enough (a few hundred candidates, each capped at node_cap
    disassembly calls) that this stays fast in practice.
    """
    if start_va in endpoint_registry:
        kind, label = endpoint_registry[start_va]
        return [(start_va, "endpoint", label)]

    visited = set()
    # BFS by depth so the FIRST path found is the shortest (most legible) one.
    frontier = [(start_va, [(start_va, "wrapper", None)])]
    visited.add(start_va)
    nodes_explored = 0
    for depth in range(max_depth):
        next_frontier = []
        for va, path in frontier:
            if nodes_explored >= node_cap:
                return None
            if not (load_address <= va < load_address + image_size):
                continue
            insns = disassemble_bounded(body, load_address, va, va + 0x600)
            nodes_explored += 1
            if not insns:
                continue
            for site_va, target, mnem in extract_bl_targets(insns):
                if mnem != "bl":
                    continue
                if target in endpoint_registry:
                    kind, label = endpoint_registry[target]
                    return path + [(target, "endpoint", label)]
                if target in visited:
                    continue
                if not (load_address <= target < load_address + image_size):
                    continue
                visited.add(target)
                next_frontier.append((target, path + [(target, "wrapper", None)]))
        frontier = next_frontier
        if not frontier:
            break
    return None


def extract_command_word_stores(insns, window=8):
    """Heuristic: `lis rX,HI` + `ori rX,rX,LO` (or a bare `li rX,LO`) forming a
    32-bit constant, followed within `window` instrs by `stw rX, OFF(r1)`
    (stack buffer store) - the pattern used to build 0x6000XX-style command
    words before handing them to a dispatch helper. Returns list of
    (const_build_va, value, store_va, store_off)."""
    out = []
    i = 0
    while i < len(insns):
        insn = insns[i]
        value = None
        reg = None
        start_va = insn.address
        if insn.mnemonic == "lis" and i + 1 < len(insns) and insns[i + 1].mnemonic == "ori":
            try:
                reg = insn.op_str.split(",")[0].strip()
                hi = int(insn.op_str.split(",")[1].strip(), 0)
                lo = int(insns[i + 1].op_str.split(",")[-1].strip(), 0)
                if insns[i + 1].op_str.split(",")[0].strip() == reg:
                    value = (hi << 16) | lo
            except (ValueError, IndexError):
                pass
        if value is not None:
            for j in range(i + 1, min(i + 1 + window, len(insns))):
                if insns[j].mnemonic == "stw" and insns[j].op_str.startswith(reg + ", "):
                    try:
                        off_part = insns[j].op_str.split(",")[1].strip()
                        out.append((start_va, value, insns[j].address, off_part))
                    except IndexError:
                        pass
                    break
        i += 1
    return out
