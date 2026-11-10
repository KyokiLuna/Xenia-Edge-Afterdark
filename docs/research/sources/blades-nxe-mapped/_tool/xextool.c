// Standalone XEX2 decrypt/decompress/depatch tool built from Xenia's own
// vendored crypto (third_party/crypto/rijndael) and LZX (third_party/mspack)
// code, faithfully re-implementing xenia/src/xenia/cpu/xex_module.cc's
// ReadImage*/ApplyPatch logic outside of the full emulator.
//
// Modes:
//   xextool <in.xex|xexp> <out.bin>                 decrypt+decompress a full module
//   xextool --patch <base.xex> <patch.xexp> <out.bin>  apply a DELTA patch onto a base module
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rijndael-alg-fst.h"
#include "mspack.h"
#include "lzx.h"
#include "sha1_mini.h"

static uint32_t be32(const uint8_t* p) {
  return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

static const uint8_t xex2_retail_key[16] = {
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
static const uint8_t xex2_devkit_key[16] = {0};
static const uint8_t xex1_retail_key[16] = {
    0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9,
    0x8B, 0x99, 0x92, 0x2C, 0xE9, 0x32, 0x15, 0x72};

static void aes_cbc0_decrypt(const uint8_t* session_key,
                             const uint8_t* input_buffer, size_t input_size,
                             uint8_t* output_buffer) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int Nr = rijndaelKeySetupDec(rk, session_key, 128);
  const uint8_t* ct = input_buffer;
  uint8_t* pt = output_buffer;
  for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
    rijndaelDecrypt(rk, Nr, ct, pt);
    for (int i = 0; i < 16; i++) {
      pt[i] ^= ivec[i];
      ivec[i] = ct[i];
    }
  }
}

// ---- mspack memory-file glue (copied from xenia/src/xenia/cpu/lzx.cc) ----
typedef struct mspack_memory_file_t {
  struct mspack_system sys;
  void* buffer;
  off_t buffer_size;
  off_t offset;
} mspack_memory_file;

static mspack_memory_file* mspack_memory_open(struct mspack_system* sys,
                                              void* buffer, size_t buffer_size) {
  mspack_memory_file* f = (mspack_memory_file*)calloc(1, sizeof(*f));
  f->buffer = buffer;
  f->buffer_size = (off_t)buffer_size;
  f->offset = 0;
  return f;
}
static void mspack_memory_close(mspack_memory_file* file) { free(file); }
static int mspack_memory_read(struct mspack_file* file, void* buffer, int chars) {
  mspack_memory_file* f = (mspack_memory_file*)file;
  off_t remaining = f->buffer_size - f->offset;
  off_t total = chars < remaining ? chars : remaining;
  memcpy(buffer, (uint8_t*)f->buffer + f->offset, (size_t)total);
  f->offset += total;
  return (int)total;
}
static int mspack_memory_write(struct mspack_file* file, void* buffer, int chars) {
  mspack_memory_file* f = (mspack_memory_file*)file;
  off_t remaining = f->buffer_size - f->offset;
  off_t total = chars < remaining ? chars : remaining;
  memcpy((uint8_t*)f->buffer + f->offset, buffer, (size_t)total);
  f->offset += total;
  return (int)total;
}
static void* mspack_memory_alloc(struct mspack_system* sys, size_t chars) {
  return calloc(chars, 1);
}
static void mspack_memory_free(void* ptr) { free(ptr); }
static void mspack_memory_copy(void* src, void* dest, size_t chars) {
  memcpy(dest, src, chars);
}
static struct mspack_system* mspack_memory_sys_create() {
  struct mspack_system* sys = (struct mspack_system*)calloc(1, sizeof(*sys));
  sys->read = mspack_memory_read;
  sys->write = mspack_memory_write;
  sys->alloc = mspack_memory_alloc;
  sys->free = mspack_memory_free;
  sys->copy = mspack_memory_copy;
  return sys;
}
static void mspack_memory_sys_destroy(struct mspack_system* sys) { free(sys); }

static int bit_scan_forward(uint32_t v, uint32_t* out) {
  if (!v) return 0;
  uint32_t i = 0;
  while (!(v & 1)) { v >>= 1; i++; }
  *out = i;
  return 1;
}

static int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest,
                          size_t dest_len, uint32_t window_size,
                          void* window_data, size_t window_data_len) {
  int result_code = 1;
  uint32_t window_bits;
  if (!bit_scan_forward(window_size, &window_bits)) return result_code;

  struct mspack_system* sys = mspack_memory_sys_create();
  mspack_memory_file* lzxsrc = mspack_memory_open(sys, (void*)lzx_data, lzx_len);
  mspack_memory_file* lzxdst = mspack_memory_open(sys, dest, dest_len);
  struct lzxd_stream* lzxd =
      lzxd_init(sys, (struct mspack_file*)lzxsrc, (struct mspack_file*)lzxdst,
                window_bits, 0, 0x8000, (off_t)dest_len, 0);
  if (lzxd) {
    if (window_data) {
      size_t padding_len = window_size - window_data_len;
      memset(&lzxd->window[0], 0, padding_len);
      memcpy(&lzxd->window[padding_len], window_data, window_data_len);
      lzxd->ref_data_size = window_size;
    }
    result_code = lzxd_decompress(lzxd, (off_t)dest_len);
    lzxd_free(lzxd);
  }
  mspack_memory_close(lzxsrc);
  mspack_memory_close(lzxdst);
  mspack_memory_sys_destroy(sys);
  return result_code;
}

// mirrors xe::cpu::lzxdelta_apply_patch (xenia/src/xenia/cpu/lzx.cc:148)
// patch entries: {old_addr(4), new_addr(4), uncompressed_len(2), compressed_len(2), data[...]}
static int lzxdelta_apply_patch(const uint8_t* patch, size_t patch_len,
                                uint32_t window_size, uint8_t* dest) {
  // xex2_delta_patch is {old_addr,new_addr,ulen,clen} = 12 logical header
  // bytes, but sizeof() the real struct rounds to 16 (trailing char
  // patch_data[1] + alignment padding); each entry occupies 12 header bytes
  // followed by `compressed_len` bytes of data (0 for fill/copy entries).
  const uint8_t* patch_end = patch + patch_len;
  const uint8_t* cur = patch;
  while (patch_end > cur) {
    uint32_t old_addr = be32(cur);
    uint32_t new_addr = be32(cur + 4);
    uint16_t uncompressed_len = be16(cur + 8);
    uint16_t compressed_len = be16(cur + 10);
    if (compressed_len == 0 && uncompressed_len == 0 && new_addr == 0 && old_addr == 0) {
      break;
    }
    if (getenv("XEXTOOL_DEBUG")) {
      fprintf(stderr, "DEBUG delta entry @+%ld: old=0x%X new=0x%X ulen=%u clen=%u\n",
              (long)(cur - patch), old_addr, new_addr, uncompressed_len, compressed_len);
    }
    if (compressed_len == 0) {
      memset(dest + new_addr, 0, uncompressed_len);
      cur += 12;
    } else if (compressed_len == 1) {
      memcpy(dest + new_addr, dest + old_addr, uncompressed_len);
      cur += 12;
    } else {
      int result = lzx_decompress(cur + 12, compressed_len, dest + new_addr,
                                  uncompressed_len, window_size,
                                  dest + old_addr, uncompressed_len);
      if (getenv("XEXTOOL_DEBUG")) {
        fprintf(stderr, "DEBUG  lzx_decompress rc=%d ulen=%u\n", result, uncompressed_len);
        FILE* df = fopen("delta_entry_out.bin", "wb");
        fwrite(dest + new_addr, 1, uncompressed_len, df);
        fclose(df);
      }
      if (result) return result;
      cur += 12 + compressed_len;
    }
  }
  return 0;
}

typedef struct {
  uint8_t* data;
  long size;
} filebuf_t;

static int read_file(const char* path, filebuf_t* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  out->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->data = (uint8_t*)malloc(out->size);
  fread(out->data, 1, out->size, f);
  fclose(f);
  return 1;
}

typedef struct {
  const uint8_t* file;
  long file_size;
  uint32_t header_size;
  uint32_t security_offset;
  uint32_t module_flags;
  uint32_t image_size;
  const uint8_t* aes_key;   // 16 bytes, points into file
  uint32_t file_format_info_off;
  uint16_t encryption_type;
  uint16_t compression_type;
  uint32_t window_size;      // valid for NORMAL/DELTA
} xex_info_t;

static int parse_xex(const uint8_t* file, long file_size, xex_info_t* info) {
  if (file_size < 0x20 || memcmp(file, "XEX2", 4) != 0) return 0;
  memset(info, 0, sizeof(*info));
  info->file = file;
  info->file_size = file_size;
  info->header_size = be32(file + 8);
  info->security_offset = be32(file + 0x10);
  uint32_t header_count = be32(file + 0x14);
  info->module_flags = be32(file + 4);

  const uint8_t* sec = file + info->security_offset;
  info->image_size = be32(sec + 4);
  info->aes_key = sec + 0x150;

  uint32_t off = 0x18;
  for (uint32_t i = 0; i < header_count; i++) {
    uint32_t key = be32(file + off);
    uint32_t value = be32(file + off + 4);
    if (key == 0x000003FF) info->file_format_info_off = value;
    off += 8;
  }
  if (!info->file_format_info_off) return 0;
  info->encryption_type = be16(file + info->file_format_info_off + 4);
  info->compression_type = be16(file + info->file_format_info_off + 6);
  if (info->compression_type == 2 || info->compression_type == 3) {
    info->window_size = be32(file + info->file_format_info_off + 8);
  }
  return 1;
}

// Try retail/devkit/xex1-retail keys (mirrors XexModule::LoadFromMemory fallback).
static void derive_session_key(const xex_info_t* info, uint8_t out_key[16],
                               const uint8_t** which_key_used) {
  static const uint8_t* candidates[3];
  candidates[0] = xex2_retail_key;
  candidates[1] = xex2_devkit_key;
  candidates[2] = xex1_retail_key;
  // We can't know which is right without attempting a full decode+hash check,
  // so callers try candidates in order and bail on hash mismatch.
  aes_cbc0_decrypt(candidates[0], info->aes_key, 16, out_key);
  *which_key_used = candidates[0];
}

// Decodes NONE/BASIC/NORMAL compressed body given an already-derived session_key.
// Returns 0 on success. On block-hash mismatch (wrong key) returns 2 so caller can retry.
static int decode_body(const xex_info_t* info, const uint8_t session_key[16],
                       uint8_t** out, size_t* out_size) {
  int rc = 0;
  if (info->compression_type == 0) {
    uint32_t exe_length = (uint32_t)(info->file_size - info->header_size);
    *out_size = exe_length;
    *out = (uint8_t*)malloc(*out_size);
    const uint8_t* p = info->file + info->header_size;
    if (info->encryption_type == 0) {
      memcpy(*out, p, exe_length);
    } else {
      aes_cbc0_decrypt(session_key, p, exe_length, *out);
    }
  } else if (info->compression_type == 1) {
    uint32_t info_size = be32(info->file + info->file_format_info_off);
    uint32_t block_count = (info_size - 8) / 8;
    const uint8_t* blk = info->file + info->file_format_info_off + 8;
    uint32_t uncompressed_size = 0;
    for (uint32_t n = 0; n < block_count; n++) {
      uncompressed_size += be32(blk + n * 8) + be32(blk + n * 8 + 4);
    }
    *out_size = uncompressed_size;
    *out = (uint8_t*)calloc(1, *out_size);
    const uint8_t* p = info->file + info->header_size;
    uint8_t* d = *out;
    uint32_t rk[4 * (MAXNR + 1)];
    uint8_t ivec[16] = {0};
    int Nr = rijndaelKeySetupDec(rk, session_key, 128);
    for (uint32_t n = 0; n < block_count; n++) {
      uint32_t data_size = be32(blk + n * 8);
      uint32_t zero_size = be32(blk + n * 8 + 4);
      if (info->encryption_type == 0) {
        memcpy(d, p, data_size);
      } else {
        const uint8_t* ct = p;
        uint8_t* pt = d;
        for (uint32_t m = 0; m < data_size; m += 16, ct += 16, pt += 16) {
          rijndaelDecrypt(rk, Nr, ct, pt);
          for (int i = 0; i < 16; i++) { pt[i] ^= ivec[i]; ivec[i] = ct[i]; }
        }
      }
      p += data_size;
      d += data_size + zero_size;
    }
  } else if (info->compression_type == 2) {
    uint32_t exe_length = (uint32_t)(info->file_size - info->header_size);
    const uint8_t* exe_buffer = info->file + info->header_size;
    uint8_t* input_buffer = NULL;
    int free_input = 0;
    if (info->encryption_type == 0) {
      input_buffer = (uint8_t*)exe_buffer;
    } else {
      input_buffer = (uint8_t*)calloc(1, exe_length);
      aes_cbc0_decrypt(session_key, exe_buffer, exe_length, input_buffer);
      free_input = 1;
    }
    const uint8_t* cur_block = info->file + info->file_format_info_off + 12;
    uint8_t* compress_buffer = (uint8_t*)calloc(1, exe_length);
    const uint8_t* p = input_buffer;
    uint8_t* d = compress_buffer;
    while (1) {
      uint32_t block_size = be32(cur_block);
      if (!block_size) break;
      const uint8_t* pnext = p + block_size;
      const uint8_t* next_block = p;
      uint8_t digest[20];
      sha1_mini(p, block_size, digest);
      if (memcmp(digest, cur_block + 4, 20) != 0) { rc = 2; break; }
      p += 4; p += 20;
      while (1) {
        uint32_t chunk_size = (p[0] << 8) | p[1];
        p += 2;
        if (!chunk_size) break;
        memcpy(d, p, chunk_size);
        p += chunk_size;
        d += chunk_size;
      }
      p = pnext;
      cur_block = next_block;
    }
    if (!rc) {
      *out_size = info->image_size;
      *out = (uint8_t*)calloc(1, *out_size);
      rc = lzx_decompress(compress_buffer, d - compress_buffer, *out, *out_size,
                          info->window_size, NULL, 0);
    }
    free(compress_buffer);
    if (free_input) free(input_buffer);
  } else {
    return 5;
  }
  return rc;
}

// Full decode with key fallback (retail -> devkit -> xex1-retail).
static int decode_full(const xex_info_t* info, uint8_t session_key_out[16],
                       uint8_t** out, size_t* out_size) {
  static const uint8_t* keys[3];
  keys[0] = xex2_retail_key;
  keys[1] = xex2_devkit_key;
  keys[2] = xex1_retail_key;
  static const char* names[3] = {"retail", "devkit", "xex1retail"};
  for (int k = 0; k < 3; k++) {
    uint8_t session_key[16];
    aes_cbc0_decrypt(keys[k], info->aes_key, 16, session_key);
    uint8_t* out_buf = NULL; size_t out_sz = 0;
    int rc = decode_body(info, session_key, &out_buf, &out_sz);
    if (getenv("XEXTOOL_DEBUG")) {
      fprintf(stderr, "DEBUG decode_full try %s -> rc=%d\n", names[k], rc);
    }
    if (rc == 0) {
      memcpy(session_key_out, session_key, 16);
      *out = out_buf; *out_size = out_sz;
      return 0;
    }
    free(out_buf);
    if (rc != 2) return rc;  // hard failure, not a key issue
  }
  return 2;
}

static int mode_single(const char* in_path, const char* out_path) {
  filebuf_t fb;
  if (!read_file(in_path, &fb)) { perror("fopen"); return 1; }
  if (memcmp(fb.data, "XEX2", 4) != 0) { fprintf(stderr, "SKIP not-xex2\n"); return 2; }
  xex_info_t info;
  if (!parse_xex(fb.data, fb.size, &info)) { fprintf(stderr, "SKIP no-file-format-info\n"); return 3; }
  if (info.compression_type == 3) { fprintf(stderr, "SKIP delta-compressed (use --patch)\n"); return 4; }
  if (info.compression_type > 2) { fprintf(stderr, "SKIP unknown-compression\n"); return 5; }
  if (info.module_flags & 0x10) { fprintf(stderr, "SKIP is-patch-module (use --patch)\n"); return 6; }

  uint8_t session_key[16];
  uint8_t* out = NULL; size_t out_size = 0;
  int rc = decode_full(&info, session_key, &out, &out_size);
  if (rc) { fprintf(stderr, "FAIL rc=%d\n", rc); return 10 + rc; }

  FILE* of = fopen(out_path, "wb");
  fwrite(out, 1, out_size, of);
  fclose(of);
  fprintf(stderr, "OK %zu bytes -> %s\n", out_size, out_path);
  return 0;
}

// XEX_HEADER_DELTA_PATCH_DESCRIPTOR = 0x5FF, resolved like file_format_info (offset from header base).
static uint32_t find_opt_header(const uint8_t* file, uint32_t key) {
  uint32_t header_count = be32(file + 0x14);
  uint32_t off = 0x18;
  for (uint32_t i = 0; i < header_count; i++) {
    uint32_t k = be32(file + off);
    uint32_t v = be32(file + off + 4);
    if (k == key) return v;
    off += 8;
  }
  return 0;
}

static int mode_patch(const char* base_path, const char* patch_path, const char* out_path) {
  filebuf_t base_fb, patch_fb;
  if (!read_file(base_path, &base_fb)) { perror("fopen base"); return 1; }
  if (!read_file(patch_path, &patch_fb)) { perror("fopen patch"); return 1; }

  xex_info_t base_info;
  if (!parse_xex(base_fb.data, base_fb.size, &base_info)) {
    fprintf(stderr, "FAIL base parse\n");
    return 2;
  }
  if (!(base_info.module_flags == 0 || 1)) {}  // base module flags not further checked

  // 1) Fully decode the base module's body (this is what ApplyPatch calls
  //    "module", already loaded/decompressed).
  uint8_t base_session_key[16];
  uint8_t* base_image = NULL; size_t base_image_size = 0;
  int rc = decode_full(&base_info, base_session_key, &base_image, &base_image_size);
  if (rc) { fprintf(stderr, "FAIL base decode rc=%d\n", rc); return 3; }

  // 2) Set up a mutable header buffer seeded from the BASE module's raw
  //    (never encrypted/compressed) header bytes, with slack for growth.
  size_t header_cap = base_info.header_size > 0x40000 ? base_info.header_size + 0x10000 : 0x40000;
  uint8_t* header_ptr = (uint8_t*)calloc(1, header_cap);
  memcpy(header_ptr, base_fb.data, base_info.header_size);

  // 3) Locate the patch's own delta-patch descriptor + file-format-info headers.
  uint32_t patch_desc_off = find_opt_header(patch_fb.data, 0x5FF);
  if (!patch_desc_off) { fprintf(stderr, "FAIL patch has no delta descriptor\n"); return 4; }
  xex_info_t patch_info;
  if (!parse_xex(patch_fb.data, patch_fb.size, &patch_info)) {
    fprintf(stderr, "FAIL patch parse\n");
    return 5;
  }

  const uint8_t* pd = patch_fb.data + patch_desc_off;
  uint32_t pd_size = be32(pd + 0x0);
  uint8_t image_key_source[16]; memcpy(image_key_source, pd + 0x20, 16);
  uint32_t size_of_target_headers = be32(pd + 0x30);
  uint32_t delta_headers_source_offset = be32(pd + 0x34);
  uint32_t delta_headers_source_size = be32(pd + 0x38);
  uint32_t delta_headers_target_offset = be32(pd + 0x3C);
  uint32_t delta_image_source_offset = be32(pd + 0x40);
  uint32_t delta_image_source_size = be32(pd + 0x44);
  uint32_t delta_image_target_offset = be32(pd + 0x48);
  const uint8_t* pd_info = pd + 0x4C;  // xex2_delta_patch array start

  if (getenv("XEXTOOL_DEBUG")) {
    fprintf(stderr,
            "DEBUG pd_size=%u size_of_target_headers=0x%X "
            "delta_headers(src_off=0x%X src_size=0x%X tgt_off=0x%X) "
            "delta_image(src_off=0x%X src_size=0x%X tgt_off=0x%X)\n",
            pd_size, size_of_target_headers, delta_headers_source_offset,
            delta_headers_source_size, delta_headers_target_offset,
            delta_image_source_offset, delta_image_source_size, delta_image_target_offset);
  }
  uint32_t header_target_size = size_of_target_headers;
  if (!header_target_size) {
    header_target_size = delta_headers_target_offset + delta_headers_source_size;
  }
  if (header_target_size > header_cap) {
    fprintf(stderr, "FAIL header_target_size %u exceeds cap %zu\n", header_target_size, header_cap);
    return 6;
  }

  if (delta_headers_source_offset) {
    memcpy(header_ptr + delta_headers_target_offset,
           header_ptr + delta_headers_source_offset, delta_headers_source_size);
  }

  // Apply the header delta patch (uses the PATCH's own window_size, which for
  // DELTA compression lives at file_format_info+8 same as NORMAL).
  rc = lzxdelta_apply_patch(pd_info, pd_size, patch_info.window_size, header_ptr);
  if (rc) { fprintf(stderr, "FAIL header patch rc=%d\n", rc); return 7; }

  // 4) Re-read the (now patched) base security info to get the new image size / aes_key.
  uint32_t new_security_offset = be32(header_ptr + 0x10);
  uint32_t new_image_size = be32(header_ptr + new_security_offset + 4);
  const uint8_t* new_base_aes_key = header_ptr + new_security_offset + 0x150;

  // 5) Recompute base session key from the (possibly patched) aes_key, then
  //    derive the patch's own session key keyed off of it (NOT the retail key).
  //    The "outer" key (retail/devkit/xex1-retail) that actually applies to
  //    this base module isn't knowable in advance for modules whose body
  //    encryption_type is NONE (e.g. xam.xex), since decode_full() never
  //    exercised the key there (nothing was AES-decrypted, so a wrong key
  //    silently "succeeds"). Disambiguate here using patch_header's
  //    image_key_source, which must decrypt (under the correct outer key,
  //    chained through the *new* base session key) back to the *original*
  //    base session key.
  static const uint8_t* outer_keys[3] = {xex2_retail_key, xex2_devkit_key, xex1_retail_key};
  static const char* outer_key_names[3] = {"xex2-retail", "xex2-devkit", "xex1-retail"};
  uint8_t base_session_key2[16];
  uint8_t patch_session_key[16];
  int found_key = -1;
  for (int k = 0; k < 3; k++) {
    uint8_t candidate_new[16], candidate_orig[16], test_delta_key[16];
    aes_cbc0_decrypt(outer_keys[k], new_base_aes_key, 16, candidate_new);
    aes_cbc0_decrypt(outer_keys[k], base_info.aes_key, 16, candidate_orig);
    aes_cbc0_decrypt(candidate_new, image_key_source, 16, test_delta_key);
    if (getenv("XEXTOOL_DEBUG")) {
      fprintf(stderr, "DEBUG try %s: candidate_new=", outer_key_names[k]);
      for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", candidate_new[i]);
      fprintf(stderr, " candidate_orig=");
      for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", candidate_orig[i]);
      fprintf(stderr, " test_delta_key=");
      for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", test_delta_key[i]);
      fprintf(stderr, "\n");
    }
    if (memcmp(test_delta_key, candidate_orig, 16) == 0) {
      memcpy(base_session_key2, candidate_new, 16);
      found_key = k;
      break;
    }
  }
  if (getenv("XEXTOOL_DEBUG")) {
    FILE* hf = fopen("header_ptr_dump.bin", "wb");
    fwrite(header_ptr, 1, header_cap, hf);
    fclose(hf);
    fprintf(stderr, "DEBUG new_base_aes_key=");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", new_base_aes_key[i]);
    fprintf(stderr, " new_security_offset=0x%X new_image_size=0x%X header_target_size=0x%X\n",
            new_security_offset, new_image_size, header_target_size);
  }
  if (found_key < 0) {
    fprintf(stderr, "FAIL couldn't determine outer key (image_key_source check failed for all candidates)\n");
    return 9;
  }
  if (getenv("XEXTOOL_DEBUG")) {
    fprintf(stderr, "DEBUG outer_key=%s new_security_offset=0x%X new_image_size=0x%X\n",
            outer_key_names[found_key], new_security_offset, new_image_size);
  }
  aes_cbc0_decrypt(base_session_key2, patch_info.aes_key, 16, patch_session_key);

  // 6) Grow the image buffer if needed.
  size_t image_cap = base_image_size;
  if (new_image_size > image_cap) image_cap = new_image_size;
  image_cap += 0x400000;  // slack
  uint8_t* base_exe = (uint8_t*)calloc(1, image_cap);
  memcpy(base_exe, base_image, base_image_size);

  if (delta_image_source_offset) {
    memcpy(base_exe + delta_image_target_offset, base_exe + delta_image_source_offset,
           delta_image_source_size);
  }
  uint32_t image_target_size = delta_image_target_offset + delta_image_source_size;
  if (image_target_size < base_image_size) {
    memset(base_exe + image_target_size, 0, base_image_size - image_target_size);
  }

  // 7) Decrypt the patch's own compressed body using the derived patch session key.
  uint32_t patch_body_len = (uint32_t)(patch_fb.size - patch_info.header_size);
  const uint8_t* patch_body = patch_fb.data + patch_info.header_size;
  uint8_t* input_buffer;
  int free_input = 0;
  if (patch_info.encryption_type == 0) {
    input_buffer = (uint8_t*)patch_body;
  } else {
    input_buffer = (uint8_t*)calloc(1, patch_body_len);
    aes_cbc0_decrypt(patch_session_key, patch_body, patch_body_len, input_buffer);
    free_input = 1;
  }

  // 8) Walk the patch's compressed-block chain, applying each block's delta
  //    patch entries onto base_exe. Mirrors xex_module.cc's ApplyPatch /
  //    ReadImageCompressed block-chaining exactly: cur_block (size+hash)
  //    starts out pointing at the header's cached copy of the first block,
  //    and each iteration captures "next_block" as the block's own position
  //    (p) *before* advancing past it, then hands that off as next
  //    iteration's cur_block once p has been walked forward to it.
  const uint8_t* cur_block = patch_fb.data + patch_info.file_format_info_off + 12;
  const uint8_t* p = input_buffer;
  int blocks_done = 0;
  while (cur_block && be32(cur_block)) {
    uint32_t block_size = be32(cur_block);
    const uint8_t* pnext = p + block_size;
    const uint8_t* next_block = p;

    uint8_t digest[20];
    sha1_mini(p, block_size, digest);
    if (memcmp(digest, cur_block + 4, 20) != 0) {
      fprintf(stderr, "WARN patch block hash mismatch at block %d\n", blocks_done);
      rc = 8;
      break;
    }
    p += 20; p += 4;
    uint32_t block_data_size = block_size - 20 - 4;
    rc = lzxdelta_apply_patch(p, block_data_size, patch_info.window_size, base_exe);
    if (rc) { fprintf(stderr, "FAIL delta apply rc=%d\n", rc); break; }
    blocks_done++;
    p = pnext;
    cur_block = next_block;
  }

  if (rc) { fprintf(stderr, "FAIL rc=%d after %d block(s)\n", rc, blocks_done); return 8; }

  FILE* of = fopen(out_path, "wb");
  fwrite(base_exe, 1, new_image_size, of);
  fclose(of);

  // Also emit the patched (target) raw header, so downstream tools can read
  // the correct security_info/export_table/load_address for this patched
  // image instead of the base module's pre-patch header.
  char header_out_path[4096];
  snprintf(header_out_path, sizeof(header_out_path), "%s.header", out_path);
  FILE* hof = fopen(header_out_path, "wb");
  fwrite(header_ptr, 1, header_target_size, hof);
  fclose(hof);

  fprintf(stderr, "OK %u bytes (%d block(s)) -> %s (+ .header)\n", new_image_size, blocks_done, out_path);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3) {
    return mode_single(argv[1], argv[2]);
  } else if (argc == 5 && strcmp(argv[1], "--patch") == 0) {
    return mode_patch(argv[2], argv[3], argv[4]);
  }
  fprintf(stderr,
          "usage: %s <in.xex> <out.bin>\n"
          "       %s --patch <base.xex> <patch.xexp> <out.bin>\n",
          argv[0], argv[0]);
  return 1;
}
