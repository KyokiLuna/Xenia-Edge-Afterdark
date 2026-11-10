"""Shared helpers for the decoded_dump/reference/ corpus-building scripts.

Deliberately dependency-light (stdlib + capstone) so it can run standalone
against the decoded_dump tree without needing xenia-edge to be built.
"""
import hashlib
import os
import re
import struct
import subprocess

DUMP = "/home/deck/Documents/xbox dash/Blades + NXE Classic"
OUT = "/home/deck/Downloads/xenia_edge/decoded_dump"
REF = os.path.join(OUT, "reference")
XENIA = "/home/deck/Downloads/xenia_edge/xenia-edge"

# name (without $flash_ / .xexp) -> base module relpath, for the 12 known
# delta patches this corpus successfully depatched. This is the single
# source of truth other scripts should import rather than re-deriving.
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
    "ximedic": "XIMEDIC.XEX",  # depatch currently fails - see 10_UNRESOLVED.txt
}

# Modules explicitly prioritized by the current research goals (XAM storage /
# gamer-profile / title-content / avatar / XNA-XBLIG). Disassembly-based
# extraction (call graph, IPC map, string xrefs) is scoped to these rather
# than all 51 XEX-derived modules, per explicit instruction not to attempt
# exhaustive decompilation.
PRIORITY_MODULES = [
    "xam.xex",
    "$flash_xam.xexp",
    "dash.xex",
    "$flash_dash.xex",
    "AvatarEditor.xex",
    "AvatarMiniCreator.xex",
    "gamerprofile.xex",
    "$flash_gamerprofile.xexp",
    "createprofile.xex",
    "$flash_createprofile.xexp",
    "deviceselector.xex",
    "$flash_deviceselector.xexp",
    "signin.xex",
    "$flash_signin.xexp",
]


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def sha256_file(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_type(path):
    try:
        r = subprocess.run(["file", "-b", path], capture_output=True, text=True, timeout=10)
        return r.stdout.strip()
    except Exception:
        return "?"


def parse_xex_header(data):
    """Parse a raw (unencrypted) XEX2 header blob - works for both original
    source-file headers and reconstructed ".header" sidecars.
    Returns a dict, or None if not a XEX2 header."""
    if len(data) < 0x20 or data[:4] != b"XEX2":
        return None
    out = {}
    out["module_flags"] = be32(data, 4)
    out["header_size"] = be32(data, 8)
    out["security_offset"] = be32(data, 0x10)
    header_count = be32(data, 0x14)
    sec = out["security_offset"]
    if sec + 0x184 > len(data):
        # Some sidecars/short headers may not carry the full security_info;
        # still parse what's addressable.
        pass
    try:
        out["xex_security_image_size"] = be32(data, sec + 4)
        out["xex_load_address"] = be32(data, sec + 0x110)
        out["export_table_va"] = be32(data, sec + 0x160)
        out["region"] = be32(data, sec + 0x178)
        out["page_descriptor_count"] = be32(data, sec + 0x180)
        out["aes_key"] = data[sec + 0x150: sec + 0x160]
    except struct.error:
        out["xex_security_image_size"] = None
        out["xex_load_address"] = None
        out["export_table_va"] = None
        out["region"] = None
        out["page_descriptor_count"] = None
        out["aes_key"] = None

    hdrs = {}
    off = 0x18
    for i in range(header_count):
        if off + 8 > len(data):
            break
        k = be32(data, off)
        v = be32(data, off + 4)
        hdrs[k] = v
        off += 8
    out["headers"] = hdrs

    ff_off = hdrs.get(0x3FF)
    if ff_off:
        out["encryption_type"] = be16(data, ff_off + 4)
        out["compression_type"] = be16(data, ff_off + 6)
    else:
        out["encryption_type"] = None
        out["compression_type"] = None

    out["xex_entry_va"] = hdrs.get(0x10100)  # XEX_HEADER_ENTRY_POINT, inline value

    # XEX_HEADER_EXECUTION_INFO = 0x40006 -> xex2_opt_execution_info blob
    exec_off = hdrs.get(0x40006)
    if exec_off and exec_off + 0x18 <= len(data):
        out["media_id"] = be32(data, exec_off + 0)
        out["module_version"] = be32(data, exec_off + 4)
        out["base_version"] = be32(data, exec_off + 8)
        out["title_id"] = be32(data, exec_off + 0xC)
        out["platform"] = data[exec_off + 0x10]
        out["executable_table"] = data[exec_off + 0x11]
        out["disc_number"] = data[exec_off + 0x12]
        out["disc_count"] = data[exec_off + 0x13]
        out["savegame_id"] = be32(data, exec_off + 0x14)
    else:
        for k in ("media_id", "module_version", "base_version", "title_id",
                   "platform", "executable_table", "disc_number", "disc_count",
                   "savegame_id"):
            out[k] = None

    # XEX_HEADER_IMPORT_LIBRARIES = 0x103FF
    out["import_libraries_off"] = hdrs.get(0x103FF)

    out["is_patch"] = bool(out["module_flags"] & 0x10)
    return out


def version_str(v):
    if v is None:
        return None
    major = (v >> 28) & 0xF
    minor = (v >> 24) & 0xF
    build = (v >> 8) & 0xFFFF
    qfe = v & 0xFF
    return f"{major}.{minor}.{build}.{qfe}"


def parse_import_libraries(header_data, body_data, load_address):
    """Returns list of dicts: {name, id, version, version_min, count, imports:[...]}
    Each import: {ordinal, record_type, record_addr, resolved_va (if variable)}.
    body_data/load_address needed to dereference each import_table[] VA into
    the decoded PE body to read record_type/ordinal."""
    hdrs_off = None
    if header_data[:4] != b"XEX2":
        return []
    hc = be32(header_data, 0x14)
    off = 0x18
    for i in range(hc):
        k = be32(header_data, off)
        v = be32(header_data, off + 4)
        if k == 0x103FF:
            hdrs_off = v
        off += 8
    if hdrs_off is None:
        return []

    base = hdrs_off
    total_size = be32(header_data, base)
    st_size = be32(header_data, base + 4)
    st_count = be32(header_data, base + 8)
    st_data_off = base + 0xC

    # string table: st_count strings, each nul-terminated, 4-byte aligned
    strings = []
    i = 0
    for _ in range(st_count):
        start = st_data_off + i
        if start >= len(header_data):
            break
        end = header_data.find(b"\x00", start)
        if end == -1:
            break
        strings.append(header_data[start:end].decode("ascii", "replace"))
        i += (end - start) + 1
        if i % 4:
            i += 4 - (i % 4)

    libraries = []
    lib_off = base + st_size + 12
    while lib_off < base + total_size:
        if lib_off + 0x28 > len(header_data):
            break
        lib_size = be32(header_data, lib_off)
        if not lib_size:
            break
        lib_id = be32(header_data, lib_off + 0x18)
        version_value = be32(header_data, lib_off + 0x1C)
        version_min_value = be32(header_data, lib_off + 0x20)
        name_index = be16(header_data, lib_off + 0x24) & 0xFF
        count = be16(header_data, lib_off + 0x26)
        name = strings[name_index] if name_index < len(strings) else f"?idx{name_index}"

        imports = []
        for i in range(count):
            entry_off = lib_off + 0x28 + i * 4
            if entry_off + 4 > len(header_data):
                break
            record_addr = be32(header_data, entry_off)
            file_off = record_addr - load_address
            record_value = None
            if body_data is not None and 0 <= file_off and file_off + 4 <= len(body_data):
                record_value = be32(body_data, file_off)
            record_type = (record_value >> 24) & 0xFF if record_value is not None else None
            ordinal = record_value & 0xFFFF if record_value is not None else None
            imports.append({
                "record_addr": record_addr,
                "record_type": record_type,
                "ordinal": ordinal,
            })

        libraries.append({
            "name": name,
            "id": lib_id,
            "version": version_str(version_value),
            "version_min": version_str(version_min_value),
            "count": count,
            "imports": imports,
        })
        lib_off += lib_size

    return libraries


def load_ordinal_tables():
    """Parses xam_table.inc / xboxkrnl_table.inc / xbdm_table.inc into
    {module_key: {ordinal: name}}. module_key matches the first XE_EXPORT
    argument (e.g. 'xam', 'xboxkrnl', 'xbdm')."""
    tables = {}
    paths = [
        os.path.join(XENIA, "src/xenia/kernel/xam/xam_table.inc"),
        os.path.join(XENIA, "src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc"),
        os.path.join(XENIA, "src/xenia/kernel/xbdm/xbdm_table.inc"),
    ]
    pat = re.compile(
        r"XE_EXPORT\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,")
    for p in paths:
        if not os.path.exists(p):
            continue
        with open(p) as f:
            for line in f:
                m = pat.search(line)
                if not m:
                    continue
                mod, ordhex, name = m.groups()
                tables.setdefault(mod, {})[int(ordhex, 16)] = name
    return tables


IMPORT_LIB_ALIASES = {
    "xboxkrnl.exe": "xboxkrnl",
    "xam.xex": "xam",
    "xbdm.xex": "xbdm",
}


def resolve_import_name(ordinal_tables, lib_name, ordinal):
    key = IMPORT_LIB_ALIASES.get(lib_name)
    if key and key in ordinal_tables:
        return ordinal_tables[key].get(ordinal)
    return None


def list_xex2_source_files():
    """Yields (rel, abs_path) for every XEX2-magic file under DUMP."""
    for root, dirs, files in os.walk(DUMP):
        dirs.sort()
        for fn in sorted(files):
            src = os.path.join(root, fn)
            with open(src, "rb") as f:
                magic = f.read(4)
            if magic == b"XEX2":
                yield os.path.relpath(src, DUMP), src


def decoded_body_meta(rel):
    """For a given source-relative XEX2 file, figure out:
      - dst: decoded output path in OUT
      - decode_status: decoded | depatched | RAW - no base module in dump | RAW - depatch failed
      - base_rel: base module relpath if this is a depatched flash module
      - header_bytes: the header bytes that describe the DECODED body's layout
        (sidecar for depatched files, source header otherwise)
      - load_address: correct load address for the decoded body
    """
    src = os.path.join(DUMP, rel)
    dst = os.path.join(OUT, rel)
    with open(src, "rb") as f:
        src_header = f.read(0x8000)
    meta = parse_xex_header(src_header)
    if meta is None:
        return None

    fn = os.path.basename(rel)
    key = None
    if fn.startswith("$flash_") and fn.endswith(".xexp"):
        key = fn[len("$flash_"):-len(".xexp")]

    sidecar = dst + ".header"
    base_rel = None
    load_address = meta["xex_load_address"]
    header_bytes = src_header

    if meta["is_patch"]:
        if key and key in BASE_MAP:
            base_rel = BASE_MAP[key]
            if os.path.exists(sidecar):
                with open(sidecar, "rb") as f:
                    header_bytes = f.read()
                body_meta = parse_xex_header(header_bytes)
                base_full = os.path.join(DUMP, base_rel)
                with open(base_full, "rb") as f:
                    base_header = f.read(0x8000)
                base_meta = parse_xex_header(base_header)
                load_address = body_meta["xex_load_address"] or base_meta["xex_load_address"]
                status = "depatched (base=%s)" % base_rel

                # Sanity-check execution_info: patches essentially never
                # change a system component's title_id, and every base
                # module we've checked reports title_id=0. If the
                # reconstructed sidecar's title_id disagrees with the base's,
                # that's a sign the header-patch reconstruction didn't fully
                # relocate this particular optional header (confirmed on at
                # least one module, where the "title_id" decoded as the
                # ASCII bytes "XBOX" - clearly not a real title ID) - flag it
                # rather than silently trusting either value.
                metadata_suspect = (
                    body_meta.get("title_id") != base_meta.get("title_id") or
                    body_meta.get("module_version") != base_meta.get("module_version")
                )
            else:
                status = "RAW - depatch failed"
                header_bytes = src_header
                metadata_suspect = False
        else:
            status = "RAW - no base module in dump"
            header_bytes = src_header
            metadata_suspect = False
    else:
        status = "decoded"
        metadata_suspect = False

    return {
        "rel": rel,
        "src": src,
        "dst": dst,
        "status": status,
        "base_rel": base_rel,
        "header_bytes": header_bytes,
        "load_address": load_address,
        "src_header": src_header,
        "src_meta": meta,
        "sidecar_exists": os.path.exists(sidecar),
        "metadata_suspect": metadata_suspect,
    }
