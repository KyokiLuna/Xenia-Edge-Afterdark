import struct, sys

def be32(b, off): return struct.unpack_from(">I", b, off)[0]
def be16(b, off): return struct.unpack_from(">H", b, off)[0]

def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = data[0:4]
    if magic != b"XEX2":
        print(path, "-> not XEX2, magic=", magic)
        return
    module_flags = be32(data, 4)
    header_size = be32(data, 8)
    security_offset = be32(data, 0x10)
    header_count = be32(data, 0x14)

    print(f"=== {path} ===")
    print(f"  file size        : {len(data)}")
    print(f"  header_size       : 0x{header_size:X}")
    print(f"  security_offset   : 0x{security_offset:X}")
    print(f"  header_count      : {header_count}")

    # security info
    sec = security_offset
    sec_header_size = be32(data, sec)
    image_size = be32(data, sec + 4)
    image_flags = be32(data, sec + 0x10C)
    load_address = be32(data, sec + 0x110)
    import_table_count = be32(data, sec + 0x128)
    aes_key = data[sec+0x150:sec+0x160]
    export_table = be32(data, sec + 0x160)
    region = be32(data, sec + 0x178)
    allowed_media_types = be32(data, sec + 0x17C)
    page_descriptor_count = be32(data, sec + 0x180)
    print(f"  image_size        : 0x{image_size:X}")
    print(f"  load_address      : 0x{load_address:X}")
    print(f"  image_flags       : 0x{image_flags:X}")
    print(f"  aes_key           : {aes_key.hex()}")
    print(f"  region            : 0x{region:X}")
    print(f"  page_descriptor_c : {page_descriptor_count}")

    # optional headers array starts at 0x18
    off = 0x18
    file_format_info_off = None
    for i in range(header_count):
        key = be32(data, off)
        value = be32(data, off + 4)
        header_id = key & 0xFFFFFF00 if False else key  # full key incl low byte = format flag
        # low byte encodes storage type; key top bits are the id
        if (key & 0xFF) == 0x00 or (key & 0xFF) == 0x01:
            pass
        if key == 0x000003FF:
            file_format_info_off = value
        off += 8

    print(f"  file_format_info@ : {hex(file_format_info_off) if file_format_info_off else None}")
    if file_format_info_off:
        info_size = be32(data, file_format_info_off)
        encryption_type = be16(data, file_format_info_off + 4)
        compression_type = be16(data, file_format_info_off + 6)
        enc_names = {0: "NONE", 1: "NORMAL"}
        comp_names = {0: "NONE", 1: "BASIC", 2: "NORMAL", 3: "DELTA"}
        print(f"  encryption_type   : {encryption_type} ({enc_names.get(encryption_type,'?')})")
        print(f"  compression_type  : {compression_type} ({comp_names.get(compression_type,'?')})")
    print()

for p in sys.argv[1:]:
    parse(p)
