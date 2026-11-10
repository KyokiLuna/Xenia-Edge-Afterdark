import struct, sys
import capstone

def be32(b, o): return struct.unpack_from(">I", b, o)[0]

def get_export_table(raw_header_path, decoded_body_path, load_address_override=None):
    with open(raw_header_path, "rb") as f:
        raw = f.read(0x8000)
    sec_off = be32(raw, 0x10)
    load_address = be32(raw, sec_off + 0x110)
    if not load_address and load_address_override:
        load_address = load_address_override
    export_table_va = be32(raw, sec_off + 0x160)
    print(f"load_address=0x{load_address:X} export_table_va=0x{export_table_va:X}")
    if not export_table_va:
        print("no export table")
        return None, None

    with open(decoded_body_path, "rb") as f:
        body = f.read()

    et_off = export_table_va - load_address
    magic = body[et_off:et_off+12]
    imagebaseaddr = be32(body, et_off + 0x20)
    count = be32(body, et_off + 0x24)
    base = be32(body, et_off + 0x28)
    print(f"export_table magic={magic.hex()} imagebaseaddr=0x{imagebaseaddr:X} count={count} base=0x{base:X}")
    ords = {}
    for i in range(count):
        off = be32(body, et_off + 0x2C + i * 4)
        if off == 0:
            continue
        func_va = off + (imagebaseaddr << 16)
        ords[base + i] = func_va
    return ords, (load_address, body)

def main():
    header_path, body_path, ord_list_str = sys.argv[1], sys.argv[2], sys.argv[3]
    load_override = int(sys.argv[4], 0) if len(sys.argv) > 4 else None
    wanted = [int(x, 0) for x in ord_list_str.split(",")]

    ords, ctx = get_export_table(header_path, body_path, load_override)
    if not ords:
        return
    load_address, body = ctx

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_64)
    md.detail = False

    for o in wanted:
        if o not in ords:
            print(f"\n=== ordinal 0x{o:X} NOT FOUND in export table ===")
            continue
        va = ords[o]
        file_off = va - load_address
        if file_off < 0 or file_off >= len(body):
            # maybe stored as low16 needing base<<16 combine
            print(f"\n=== ordinal 0x{o:X} va=0x{va:X} looks out of range (file_off=0x{file_off:X}) ===")
            continue
        print(f"\n=== ordinal 0x{o:X}  va=0x{va:X}  file_off=0x{file_off:X} ===")
        code = body[file_off:file_off+256]
        n = 0
        for insn in md.disasm(code, va):
            print(f"  0x{insn.address:X}:\t{insn.mnemonic}\t{insn.op_str}")
            n += 1
            if n >= 40:
                break
            if insn.mnemonic == "blr":
                break

if __name__ == "__main__":
    main()
