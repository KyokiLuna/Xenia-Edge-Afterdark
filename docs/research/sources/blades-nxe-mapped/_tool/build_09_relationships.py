"""Builds reference/09_RELATIONSHIPS.txt.

Purely structural - makes explicit the attachment graph between a module and
every artifact this corpus has about it (header sidecar, export/import/call/
string dumps, base<->patch pairs, and the special-case XNA managed-DLL chain).
No new extraction here, just cross-linking what the other scripts produced.
"""
import os
import corpus_common as cc

# titlelauncher -> hostloader -> runtimehost chain (XNA/managed-code path).
# This is documented Xbox 360 XNA Community Games boot behavior (titlelauncher
# is the native bootstrap, hostloader.dll is the CLR shim, runtimehost.xex
# hosts the actual CLR and loads the managed DLX assemblies) - recorded here
# as INFERRED since it's based on module naming/role, not a traced call graph
# (these are largely non-PPC/managed pieces outside this pass's disasm scope).
XNA_CHAIN_NOTE = (
    "titlelauncher.xex -> hostloader.dll -> runtime/v3.{0,1}/runtimehost.xex "
    "-> runtime/v3.{0,1}/*.dlx (managed .NET assemblies, decompilable with "
    "ilspycmd - see the earlier avatar-XNA cross-reference work). "
    "Relationship INFERRED from module naming/role, not a traced call graph."
)


def artifact_path(rel, subdir, suffix):
    name = rel.replace("/", "__").replace("$", "") + suffix
    p = os.path.join(cc.REF, subdir, name)
    return (os.path.join(subdir, name) if os.path.exists(p) else None)


def main():
    lines = []

    for rel, src in cc.list_xex2_source_files():
        info = cc.decoded_body_meta(rel)
        block = [f"{rel}"]
        block.append(f"    decoded_body: {rel}  (in decoded_dump/, status={info['status']})")
        if info["sidecar_exists"]:
            block.append(f"    header_sidecar: {rel}.header")
        if info["base_rel"]:
            block.append(f"    base_module: {info['base_rel']}  (this file is a DELTA patch reconstructed against it)")
        # reverse: which patches target THIS module as their base
        patches = [k for k, v in cc.BASE_MAP.items() if v == rel]
        for p in patches:
            block.append(f"    patched_by: $flash_{p}.xexp")

        exp = artifact_path(rel, "exports", ".exports.txt")
        if exp:
            block.append(f"    export_table: {exp}")
        imp = artifact_path(rel, "imports", ".imports.txt")
        if imp:
            block.append(f"    import_tables: {imp}")
        calls = artifact_path(rel, "calls", ".calls.txt")
        if calls:
            block.append(f"    call_graph: {calls}")
        strs = artifact_path(rel, "strings", ".strings.txt")
        if strs:
            block.append(f"    strings: {strs}")

        if rel == "$flash_xam.xexp":
            block.append("    ipc_map: 06_IPC_SERVICE_MAP.txt (all avatar-family + heuristic entries)")
            block.append("    avatar_command_table: 02_AVATAR_COMMAND_TABLE.txt")
            block.append("    raw_avatar_disassembly: _avatar_disasm/avatar_disasm_full.txt")
            block.append("    source_edits: xenia-edge/src/xenia/kernel/xam/xam_avatar.cc "
                         "(comments cross-referenced against this module)")

        if len(block) > 1:
            lines.append("\n".join(block))

    xna_block = (
        "XNA / managed-code chain\n"
        f"    {XNA_CHAIN_NOTE}\n"
        "    Fully decompiled (not just disassembled) via ilspycmd - see\n"
        "    runtime/v3.1/Microsoft.Xna.Framework.dlx for the managed\n"
        "    GamerServices.Avatar* classes cross-referenced against xam_avatar.cc.\n"
    )

    with open(os.path.join(cc.REF, "09_RELATIONSHIPS.txt"), "w") as f:
        f.write("Module relationship / attachment map\n")
        f.write("=======================================\n")
        f.write("Regenerate with: python3 _tool/build_09_relationships.py\n\n")
        f.write("For any module, this lists every OTHER artifact in this corpus that\n")
        f.write("relates to it: header sidecar, base<->patch pairing, export/import/\n")
        f.write("call-graph/string dumps, and (for the avatar-relevant XAM chain) the\n")
        f.write("upstream source edits and raw disassembly this corpus already produced.\n\n")
        f.write("-" * 70 + "\n\n")
        f.write(xna_block + "\n")
        f.write("-" * 70 + "\n\n")
        for block in sorted(lines):
            f.write(block + "\n\n")

    print(f"Wrote relationship records for {len(lines)} modules")


if __name__ == "__main__":
    main()
