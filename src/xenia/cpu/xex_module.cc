/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xex_module.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/pe_image.h"
#include "xenia/base/xex_image.h"

#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/lzx.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/xex2_util.h"
#include "xenia/kernel/xmodule.h"

#include "third_party/mbedtls/include/mbedtls/aes.h"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_instr.h"
DEFINE_bool(disable_instruction_infocache, false,
            "Disables caching records of called instructions/mmio accesses.",
            "CPU");

DEFINE_bool(writable_code_segments, false,
            "Enables a program to write to its own code segments in memory.",
            "CPU");

DEFINE_bool(
    enable_early_precompilation, true,
    "Compile guest functions found by code analysis at launch instead of on "
    "first call, avoiding stutter when they first run.",
    "CPU");
UPDATE_from_bool(enable_early_precompilation, 2026, 9, 14, 13, false);

DECLARE_bool(allow_plugins);

DECLARE_bool(disable_context_promotion);

static const uint8_t* xe_xex1_retail_key = xe::XexImageMasterKey(true, false);
static const uint8_t* xe_xex1_devkit_key = xe::XexImageMasterKey(true, true);
static const uint8_t* xe_xex2_retail_key = xe::XexImageMasterKey(false, false);
static const uint8_t* xe_xex2_devkit_key = xe::XexImageMasterKey(false, true);

void aes_decrypt_buffer(const uint8_t* session_key, const uint8_t* input_buffer,
                        const size_t input_size, uint8_t* output_buffer,
                        const size_t output_size) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  mbedtls_aes_setkey_dec(&context, session_key, 128);
  uint8_t ivec[16] = {0};
  mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_DECRYPT,
                        std::min(input_size, output_size) & ~size_t(15), ivec,
                        input_buffer, output_buffer);
  mbedtls_aes_free(&context);
}

namespace xe {
namespace cpu {

using xe::kernel::KernelState;

void DecryptXex2ImageKey(const uint8_t* encrypted_key, uint8_t* key) {
  aes_decrypt_buffer(xe_xex2_retail_key, encrypted_key, 16, key, 16);
}

XexModule::XexModule(Processor* processor, KernelState* kernel_state)
    : Module(processor), kernel_state_(kernel_state) {}

XexModule::~XexModule() {}

bool XexModule::GetOptHeader(const xex2_header* header, xex2_header_keys key,
                             void** out_ptr) {
  return xe::GetXexOptHeader(header, key, out_ptr);
}

bool XexModule::GetOptHeader(xex2_header_keys key, void** out_ptr) const {
  return XexModule::GetOptHeader(xex_header(), key, out_ptr);
}

const void* XexModule::GetSecurityInfo(const xex2_header* header) {
  return reinterpret_cast<const void*>(uintptr_t(header) +
                                       header->security_offset);
}

const PESection* XexModule::GetPESection(const char* name) {
  for (std::vector<PESection>::iterator it = pe_sections_.begin();
       it != pe_sections_.end(); ++it) {
    if (!strcmp(it->name, name)) {
      return &(*it);
    }
  }
  return nullptr;
}

uint32_t XexModule::GetProcAddress(uint16_t ordinal) const {
  // First: Check the xex2 export table.
  if (xex_security_info()->export_table) {
    auto export_table = memory()->TranslateVirtual<const xex2_export_table*>(
        xex_security_info()->export_table);

    ordinal -= export_table->base;
    if (ordinal >= export_table->count) {
      XELOGE("GetProcAddress({:03X}): ordinal out of bounds", ordinal);
      return 0;
    }

    uint32_t num = ordinal;
    uint32_t ordinal_offset = export_table->ordOffset[num];
    ordinal_offset += export_table->imagebaseaddr << 16;
    return ordinal_offset;
  }

  // Second: Check the PE exports.
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
        base_address_ + pe_export_directory->offset);
    assert_not_null(e);

    uint32_t* function_table =
        reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

    if (ordinal < e->NumberOfFunctions) {
      return base_address_ + function_table[ordinal];
    }
  }

  return 0;
}

uint32_t XexModule::GetProcAddress(const std::string_view name) const {
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (!GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    // No exports by name.
    return 0;
  }

  auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
      base_address_ + pe_export_directory->offset);
  assert_not_null(e);

  // e->AddressOfX RVAs are relative to the IMAGE_EXPORT_DIRECTORY!
  uint32_t* function_table =
      reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

  // Names relative to directory
  uint32_t* name_table =
      reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfNames);

  // Table of ordinals (by name)
  uint16_t* ordinal_table =
      reinterpret_cast<uint16_t*>(uintptr_t(e) + e->AddressOfNameOrdinals);

  for (uint32_t i = 0; i < e->NumberOfNames; i++) {
    auto fn_name = reinterpret_cast<const char*>(uintptr_t(e) + name_table[i]);
    uint16_t ordinal = ordinal_table[i];
    uint32_t addr = base_address_ + function_table[ordinal];
    if (name == std::string_view(fn_name)) {
      // We have a match!
      return addr;
    }
  }

  // No match
  return 0;
}

int XexModule::ApplyPatch(XexModule* module) {
  if (!is_patch()) {
    // This isn't a XEX2 patch.
    return 1;
  }

  const uint32_t original_base_address = module->base_address();

  // Grab the delta descriptor and get to work.
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
               reinterpret_cast<void**>(&patch_header));
  if (!patch_header) {
    // is_patch() only checks the module flags, so a malformed patch file can
    // still lack the descriptor.
    XELOGE("XEX patch has no delta patch descriptor");
    return 1;
  }

  uint32_t size = module->xex_header()->header_size;
  if (patch_header->delta_headers_source_offset > size) {
    XELOGE("XEX header patch source is outside base XEX header area");
    return 2;
  }

  uint32_t header_size_available =
      size - patch_header->delta_headers_source_offset;
  if (patch_header->delta_headers_source_size > header_size_available) {
    XELOGE("XEX header patch source is too large");
    return 3;
  }

  if (patch_header->delta_headers_target_offset >
      patch_header->size_of_target_headers) {
    XELOGE("XEX header patch target is outside base XEX header area");
    return 4;
  }

  uint32_t delta_target_size = patch_header->size_of_target_headers -
                               patch_header->delta_headers_target_offset;
  if (patch_header->delta_headers_source_size > delta_target_size) {
    return 5;  // ? unsure what the point of this test is, kernel checks for it
               // though
  }

  // Patch base XEX header
  uint32_t original_image_size = module->image_size();
  uint32_t header_target_size = patch_header->size_of_target_headers;

  if (!header_target_size) {
    header_target_size = patch_header->delta_headers_target_offset +
                         patch_header->delta_headers_source_size;
  }

  size_t mem_size = module->xex_header_mem_.size();

  // Increase xex header buffer length if needed
  if (header_target_size > module->xex_header_mem_.size()) {
    module->xex_header_mem_.resize(header_target_size);
  }

  auto header_ptr = (uint8_t*)module->xex_header();

  // If headers_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_headers_source_offset) {
    // Same buffer at both ends, so the ranges can overlap - see the note in
    // lzxdelta_apply_patch.
    memmove(header_ptr + patch_header->delta_headers_target_offset,
            header_ptr + patch_header->delta_headers_source_offset,
            patch_header->delta_headers_source_size);
  }

  // If new size is smaller than original, null out the difference
  if (header_target_size < module->xex_header_mem_.size()) {
    memset(header_ptr + header_target_size, 0,
           module->xex_header_mem_.size() - header_target_size);
  }

  auto file_format_header = opt_file_format_info();
  assert_not_null(file_format_header);

  // Apply header patch...
  uint32_t headerpatch_size = patch_header->size;

  int result_code = lzxdelta_apply_patch(
      &patch_header->info, headerpatch_size,
      file_format_header->compression_info.normal.window_size, header_ptr);
  if (result_code) {
    XELOGE("XEX header patch application failed, error code {}", result_code);
    return result_code;
  }

  // Decrease xex header buffer length if needed (but only after patching)
  if (module->xex_header_mem_.size() > header_target_size) {
    module->xex_header_mem_.resize(header_target_size);
  }

  // Update security info context with latest security info data
  module->ReadSecurityInfo();

  // image_size() uses the heap at base_address_, so move the base first.
  xe::be<uint32_t>* base_addr_opt = nullptr;
  if (module->GetOptHeader(XEX_HEADER_IMAGE_BASE_ADDRESS, &base_addr_opt)) {
    module->base_address_ = *base_addr_opt;
  }
  const uint32_t new_base_address = module->base_address_;
  const bool base_moved = new_base_address != original_base_address;

  uint32_t new_image_size = module->image_size();

  // Check if we need to alloc new memory for the patched xex
  if (base_moved || new_image_size > original_image_size) {
    uint32_t addr_new_mem;
    uint32_t size_delta;
    if (base_moved) {
      XELOGW(
          "Patch for module: {} changed base_address from {:08X} to {:08X}, "
          "need to reallocate xex "
          "data!",
          module->name(), original_base_address, new_base_address);
      addr_new_mem = new_base_address;
      // Room for the old image, trimmed below if the patched one is smaller.
      size_delta = std::max(original_image_size, new_image_size);
    } else {
      addr_new_mem = new_base_address + original_image_size;
      size_delta = new_image_size - original_image_size;
    }

    bool alloc_result =
        memory()
            ->LookupHeap(addr_new_mem)
            ->AllocFixed(
                addr_new_mem, size_delta, 4096,
                xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite);

    if (!alloc_result) {
      XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", addr_new_mem,
             size_delta);
      assert_always();
      return 6;
    }

    // Move the image to its new base and free the old allocation.
    if (base_moved) {
      kernel_state_->memory()->Copy(new_base_address, original_base_address,
                                    original_image_size);
      memory()
          ->LookupHeap(original_base_address)
          ->Release(original_base_address);
    }
  }

  uint8_t orig_session_key[0x10];
  memcpy(orig_session_key, module->session_key_, 0x10);

  // Header patch updated the base XEX key, need to redecrypt it
  aes_decrypt_buffer(
      module->is_dev_kit_ ? xe_xex2_devkit_key : xe_xex2_retail_key,
      reinterpret_cast<const uint8_t*>(module->xex_security_info()->aes_key),
      16, module->session_key_, 16);

  // Decrypt the patch XEX's key using base XEX key
  aes_decrypt_buffer(
      module->session_key_,
      reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
      session_key_, 16);

  // Test delta key against our decrypted keys
  // (kernel doesn't seem to check this, but it's the one use for the
  // image_key_source field I can think of...)
  uint8_t test_delta_key[0x10];
  aes_decrypt_buffer(module->session_key_, patch_header->image_key_source, 0x10,
                     test_delta_key, 0x10);

  if (memcmp(test_delta_key, orig_session_key, 0x10) != 0) {
    XELOGE("XEX patch image key doesn't match original XEX!");
    return 7;
  }

  // Decrypt (if needed).
  bool free_input = false;
  const uint8_t* patch_buffer = xexp_data_mem_.data();
  const size_t patch_length = xexp_data_mem_.size();

  const uint8_t* input_buffer = patch_buffer;

  switch (file_format_header->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      // No-op.
      break;
    case XEX_ENCRYPTION_NORMAL:
      // TODO: a way to do without a copy/alloc?
      free_input = true;
      input_buffer = (const uint8_t*)calloc(1, patch_length);
      aes_decrypt_buffer(session_key_, patch_buffer, patch_length,
                         (uint8_t*)input_buffer, patch_length);
      break;
    default:
      assert_always();
      return 8;
  }

  const xex2_compressed_block_info* cur_block =
      &file_format_header->compression_info.normal.first_block;

  const uint8_t* p = input_buffer;
  uint8_t* base_exe = memory()->TranslateVirtual(module->base_address_);

  // If image_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_image_source_offset) {
    // Same buffer at both ends, so the ranges can overlap - see the note in
    // lzxdelta_apply_patch.
    memmove(base_exe + patch_header->delta_image_target_offset,
            base_exe + patch_header->delta_image_source_offset,
            patch_header->delta_image_source_size);
  }

  // TODO: should we use new_image_size here instead?
  uint32_t image_target_size = patch_header->delta_image_target_offset +
                               patch_header->delta_image_source_size;

  // If new size is smaller than original, null out the difference
  if (image_target_size < original_image_size) {
    memset(base_exe + image_target_size, 0,
           original_image_size - image_target_size);
  }

  uint8_t digest[0x14];
  // Now loop through each block and apply the delta patches inside
  while (cur_block->block_size) {
    const auto* next_block = (const xex2_compressed_block_info*)p;

    // Compare block hash, if no match we probably used wrong decrypt key
    mbedtls_sha1(p, cur_block->block_size, digest);

    if (memcmp(digest, cur_block->block_hash, 0x14) != 0) {
      result_code = 9;
      XELOGE("XEX patch block hash doesn't match hash inside block info!");
      break;
    }

    // skip block info
    p += 20;
    p += 4;

    uint32_t block_data_size = cur_block->block_size - 20 - 4;

    // Apply delta patch
    result_code = lzxdelta_apply_patch(
        (xex2_delta_patch*)p, block_data_size,
        file_format_header->compression_info.normal.window_size, base_exe);
    if (result_code) {
      break;
    }

    p += block_data_size;
    cur_block = next_block;
  }

  if (!result_code) {
    // Decommit unused pages if new image size is smaller than original
    if (original_image_size > new_image_size) {
      uint32_t size_delta = original_image_size - new_image_size;
      uint32_t addr_free_mem = module->base_address_ + new_image_size;

      bool free_result = memory()
                             ->LookupHeap(addr_free_mem)
                             ->Decommit(addr_free_mem, size_delta);

      if (!free_result) {
        XELOGE("Unable to decommit XEX memory at {:08X}-{:08X}.", addr_free_mem,
               size_delta);
        assert_always();
      }
    }

    xex2_version source_ver, target_ver;
    source_ver = patch_header->source_version();
    target_ver = patch_header->target_version();
    XELOGI(
        "XEX patch applied successfully: base version: {}.{}.{}.{}, new "
        "version: {}.{}.{}.{}",
        source_ver.major, source_ver.minor, source_ver.build, source_ver.qfe,
        target_ver.major, target_ver.minor, target_ver.build, target_ver.qfe);
  } else {
    XELOGE("XEX patch application failed, error code {}", result_code);
  }

  if (free_input) {
    free((void*)input_buffer);
  }
  return result_code;
}

// Also tells ReadImage whether its key was the right one. CLR native images
// (.ni.dll) don't have the usual DOS stub after MZ. The PE header that e_lfanew
// points at is checked instead.
bool XexModule::is_valid_executable() const {
  assert_not_zero(base_address_);
  if (!base_address_) {
    return false;
  }
  const uint8_t* image = memory()->TranslateVirtual(base_address_);
  auto dos_header = reinterpret_cast<const XIMAGE_DOS_HEADER*>(image);
  if (dos_header->e_magic != XIMAGE_DOS_SIGNATURE) {
    return false;
  }
  const uint32_t nt_offset = uint32_t(dos_header->e_lfanew);
  const uint32_t size = image_size();
  if (size < sizeof(XIMAGE_NT_HEADERS32) ||
      nt_offset > size - sizeof(XIMAGE_NT_HEADERS32)) {
    return false;
  }
  auto nt_headers =
      reinterpret_cast<const XIMAGE_NT_HEADERS32*>(image + nt_offset);
  return nt_headers->Signature == XIMAGE_NT_SIGNATURE;
}

bool XexModule::has_code() const {
  for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; i++) {
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(xex_security_info()->page_descriptors[i].value);
    if (desc.info == XEX_SECTION_CODE) {
      return true;
    }
  }
  return false;
}

int XexModule::ReadImage(const void* xex_addr, size_t xex_length,
                         const uint8_t* key) {
  if (!opt_file_format_info()) {
    return 1;
  }

  is_dev_kit_ = key[0] == 0x00;

  if (is_patch()) {
    // Make a copy of patch data for other XEX's to use with ApplyPatch()
    const uint32_t data_len =
        static_cast<uint32_t>(xex_length - xex_header()->header_size);
    xexp_data_mem_.resize(data_len);
    std::memcpy(xexp_data_mem_.data(),
                (uint8_t*)xex_addr + xex_header()->header_size, data_len);
    return 0;
  }

  // Drop the image left by an attempt with a different key. Only release our
  // own region: other modules may already live in this heap.
  ReleaseImage();

  aes_decrypt_buffer(
      key, reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
      session_key_, 16);

  int result_code = 0;
  switch (opt_file_format_info()->compression_type) {
    case XEX_COMPRESSION_NONE:
      result_code = ReadImageUncompressed(xex_addr, xex_length);
      break;
    case XEX_COMPRESSION_BASIC:
      result_code = ReadImageBasicCompressed(xex_addr, xex_length);
      break;
    case XEX_COMPRESSION_NORMAL:
      result_code = ReadImageCompressed(xex_addr, xex_length);
      break;
    default:
      assert_always();
      return 2;
  }

  if (result_code) {
    return result_code;
  }

  // Without code there's no PE header to tell the key by.
  if (is_patch() || is_valid_executable() || !has_code()) {
    return 0;
  }

  // Not a patch and image doesn't have proper PE header, return 3
  return 3;
}

bool XexModule::AllocImage(uint32_t size) {
  image_allocated_ =
      memory()
          ->LookupHeap(base_address_)
          ->AllocFixed(
              base_address_, size, 4096,
              xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
              xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
  return image_allocated_;
}

void XexModule::ReleaseImage() {
  if (image_allocated_) {
    memory()->LookupHeap(base_address_)->Release(base_address_);
    image_allocated_ = false;
  }
}

// Convert the already decoded loader headers to the shared bounded codec.
// The caller owns both block storage and image allocation.
static std::optional<XexImageOptions> ImageOptions(
    const xex2_opt_file_format_info* info, const uint8_t* session_key,
    std::vector<XexImageBasicBlock>& blocks, const xex2_header* header,
    size_t header_size) {
  const uintptr_t offset = uintptr_t(info) - uintptr_t(header);
  if (!info || uintptr_t(info) < uintptr_t(header) || offset > header_size ||
      header_size - offset < 8 || info->info_size < 8 ||
      info->info_size > header_size - offset) {
    return std::nullopt;
  }
  XexImageOptions options;
  options.compression = static_cast<XexImageCompression>(
      static_cast<uint16_t>(info->compression_type));
  options.encryption = static_cast<XexImageEncryption>(
      static_cast<uint16_t>(info->encryption_type));
  std::copy_n(session_key, 16, options.session_key.begin());
  if (options.compression == XexImageCompression::kBasic) {
    if ((info->info_size - 8) % 8) {
      return std::nullopt;
    }
    for (uint32_t i = 0; i < (info->info_size - 8) / 8; ++i) {
      const auto& block = info->compression_info.basic.blocks[i];
      blocks.push_back({block.data_size, block.zero_size});
    }
    options.basic_blocks = blocks;
  } else if (options.compression == XexImageCompression::kNormal) {
    if (info->info_size < 36) {
      return std::nullopt;
    }
    options.normal_window_size = info->compression_info.normal.window_size;
    const auto& first = info->compression_info.normal.first_block;
    options.normal_first_block_size = first.block_size;
    std::copy_n(first.block_hash, 20, options.normal_first_block_hash.begin());
  }
  return options;
}

int XexModule::ReadImageUncompressed(const void* xex_addr, size_t xex_length) {
  if (xex_header()->header_size > xex_length) {
    return 1;
  }
  const uint32_t size = uint32_t(xex_length - xex_header()->header_size);
  if (!AllocImage(size)) {
    XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
           size);
    return 2;
  }
  std::vector<XexImageBasicBlock> blocks;
  const auto options =
      ImageOptions(opt_file_format_info(), session_key_, blocks, xex_header(),
                   xex_header_mem_.size());
  if (!options) {
    return 1;
  }
  return DecodeXexImage(
             {static_cast<const uint8_t*>(xex_addr) + xex_header()->header_size,
              size},
             {memory()->TranslateVirtual(base_address_), size},
             *options) == XexImageResult::kSuccess
             ? 0
             : 1;
}

int XexModule::ReadImageBasicCompressed(const void* xex_addr,
                                        size_t xex_length) {
  if (xex_header()->header_size > xex_length) {
    return 1;
  }
  std::vector<XexImageBasicBlock> blocks;
  const auto options =
      ImageOptions(opt_file_format_info(), session_key_, blocks, xex_header(),
                   xex_header_mem_.size());
  if (!options) {
    return 1;
  }
  auto heap = memory()->LookupHeap(base_address_);
  uint64_t total_size = 0;
  for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; ++i) {
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(xex_security_info()->page_descriptors[i].value);
    total_size += uint64_t(desc.page_count) * heap->page_size();
  }
  if (total_size > UINT32_MAX || !AllocImage(uint32_t(total_size))) {
    return 1;
  }
  return DecodeXexImage(
             {static_cast<const uint8_t*>(xex_addr) + xex_header()->header_size,
              xex_length - xex_header()->header_size},
             {memory()->TranslateVirtual(base_address_), size_t(total_size)},
             *options) == XexImageResult::kSuccess
             ? 0
             : 1;
}

int XexModule::ReadImageCompressed(const void* xex_addr, size_t xex_length) {
  if (xex_header()->header_size > xex_length) {
    return 1;
  }
  std::vector<XexImageBasicBlock> blocks;
  const auto options =
      ImageOptions(opt_file_format_info(), session_key_, blocks, xex_header(),
                   xex_header_mem_.size());
  if (!options) {
    return 1;
  }
  const uint32_t size = image_size();
  if (!AllocImage(size)) {
    return 3;
  }
  const auto result = DecodeXexImage(
      {static_cast<const uint8_t*>(xex_addr) + xex_header()->header_size,
       xex_length - xex_header()->header_size},
      {memory()->TranslateVirtual(base_address_), size}, *options);
  if (result == XexImageResult::kBlockHashMismatch) {
    return 2;
  }
  return result == XexImageResult::kSuccess ? 0 : 1;
}

int XexModule::ReadPEHeaders() {
  const uint8_t* p = memory()->TranslateVirtual(base_address_);

  // Verify DOS signature (MZ).
  auto doshdr = reinterpret_cast<const XIMAGE_DOS_HEADER*>(p);
  if (doshdr->e_magic != XIMAGE_DOS_SIGNATURE) {
    XELOGE("PE signature mismatch; likely bad decryption/decompression");
    return 1;
  }

  // Move to the NT header offset from the DOS header.
  p += doshdr->e_lfanew;

  // Verify NT signature (PE\0\0).
  auto nthdr = reinterpret_cast<const XIMAGE_NT_HEADERS32*>(p);
  if (nthdr->Signature != XIMAGE_NT_SIGNATURE) {
    return 1;
  }

  // Verify matches an Xbox PE.
  const XIMAGE_FILE_HEADER* filehdr = &nthdr->FileHeader;
  if ((filehdr->Machine != XIMAGE_FILE_MACHINE_POWERPCBE) ||
      !(filehdr->Characteristics & XIMAGE_FILE_32BIT_MACHINE)) {
    return 1;
  }
  // Verify the expected size.
  if (filehdr->SizeOfOptionalHeader != XIMAGE_SIZEOF_NT_OPTIONAL_HEADER) {
    return 1;
  }

  // Verify optional header is 32bit.
  const XIMAGE_OPTIONAL_HEADER32* opthdr = &nthdr->OptionalHeader;
  if (opthdr->Magic != XIMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    return 1;
  }
  // Verify subsystem.
  if (opthdr->Subsystem != XIMAGE_SUBSYSTEM_XBOX) {
    return 1;
  }

  // Quick scan to determine bounds of sections.
  size_t upper_address = 0;
  const XIMAGE_SECTION_HEADER* sechdr = XIMAGE_FIRST_SECTION(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    const size_t physical_address = opthdr->ImageBase + sechdr->VirtualAddress;
    upper_address =
        std::max(upper_address, physical_address + sechdr->Misc.VirtualSize);
  }

  // Setup/load sections.
  sechdr = XIMAGE_FIRST_SECTION(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    PESection section;
    memcpy(section.name, sechdr->Name, sizeof(sechdr->Name));
    section.name[8] = 0;
    section.raw_address = sechdr->PointerToRawData;
    section.raw_size = sechdr->SizeOfRawData;
    section.address = base_address_ + sechdr->VirtualAddress;
    section.size = sechdr->Misc.VirtualSize;
    section.flags = sechdr->Characteristics;
    pe_sections_.push_back(section);
  }

  // DumpTLSDirectory(pImageBase, pNTHeader, (PIMAGE_TLS_DIRECTORY32)0);
  // DumpExportsSection(pImageBase, pNTHeader);
  return 0;
}

void XexModule::ReadSecurityInfo() {
  switch (xex_format_) {
    case kFormatXex25: {
      const xex25_security_info* xex25_sec_info =
          reinterpret_cast<const xex25_security_info*>(
              GetSecurityInfo(xex_header()));

      security_info_.rsa_signature = xex25_sec_info->rsa_signature;
      security_info_.aes_key = xex25_sec_info->aes_key;
      security_info_.image_size = xex25_sec_info->image_size;
      security_info_.image_flags = xex25_sec_info->image_flags;
      security_info_.export_table = xex25_sec_info->export_table;
      security_info_.load_address = xex25_sec_info->load_address;
      security_info_.page_descriptor_count =
          xex25_sec_info->page_descriptor_count;
      security_info_.page_descriptors = xex25_sec_info->page_descriptors;
    } break;
    case kFormatXex1: {
      const xex1_security_info* xex1_sec_info =
          reinterpret_cast<const xex1_security_info*>(
              GetSecurityInfo(xex_header()));

      security_info_.rsa_signature = xex1_sec_info->rsa_signature;
      security_info_.aes_key = xex1_sec_info->aes_key;
      security_info_.image_size = xex1_sec_info->image_size;
      security_info_.image_flags = xex1_sec_info->image_flags;
      security_info_.export_table = xex1_sec_info->export_table;
      security_info_.load_address = xex1_sec_info->load_address;
      security_info_.page_descriptor_count =
          xex1_sec_info->page_descriptor_count;
      security_info_.page_descriptors = xex1_sec_info->page_descriptors;
    } break;
    case kFormatXex2: {
      const xex2_security_info* xex2_sec_info =
          reinterpret_cast<const xex2_security_info*>(
              GetSecurityInfo(xex_header()));

      security_info_.rsa_signature = xex2_sec_info->rsa_signature;
      security_info_.aes_key = xex2_sec_info->aes_key;
      security_info_.image_size = xex2_sec_info->image_size;
      security_info_.image_flags = xex2_sec_info->image_flags;
      security_info_.export_table = xex2_sec_info->export_table;
      security_info_.load_address = xex2_sec_info->load_address;
      security_info_.page_descriptor_count =
          xex2_sec_info->page_descriptor_count;
      security_info_.page_descriptors = xex2_sec_info->page_descriptors;
    } break;
  }
}

bool XexModule::Load(const std::string_view name, const std::string_view path,
                     const void* xex_addr, size_t xex_length) {
  auto src_header = reinterpret_cast<const xex2_header*>(xex_addr);

  switch (src_header->magic) {
    case kXEX0Signature:
      XELOGE("XEX0 format not supported");
      return false;
      break;
    case kXEXQSignature:
      XELOGE("XEX? format not supported");
      return false;
      break;
    case kXEXHSignature:
      XELOGE("XEX- format not supported");
      return false;
      break;
    case kXEX25Signature:
      xex_format_ = kFormatXex25;
      XELOGI("Loading XEX%");
      break;
    case kXEX1Signature:
      xex_format_ = kFormatXex1;
      XELOGI("Loading XEX1");
      break;
    case kXEX2Signature:
      xex_format_ = kFormatXex2;
      break;
    default:
      XELOGE("XEX format not supported");
      return false;
  }

  assert_false(loaded_);
  loaded_ = true;

  // Read in XEX headers
  xex_header_mem_.resize(src_header->header_size);
  std::memcpy(xex_header_mem_.data(), src_header, src_header->header_size);

  // Read/convert XEX1/XEX2 security info to a common format
  ReadSecurityInfo();

  auto sec_header = xex_security_info();

  // Try setting our base_address based on XEX_HEADER_IMAGE_BASE_ADDRESS, fall
  // back to xex_security_info otherwise
  base_address_ = xex_security_info()->load_address;
  xe::be<uint32_t>* base_addr_opt = nullptr;
  if (GetOptHeader(XEX_HEADER_IMAGE_BASE_ADDRESS, &base_addr_opt)) {
    base_address_ = *base_addr_opt;
  }

  // Setup debug info.
  name_ = name;
  path_ = path;

  // Load in the XEX basefile
  // We'll try using both XEX2 keys to see if any give a valid PE
  int result_code = ReadImage(xex_addr, xex_length, xe_xex2_retail_key);
  if (result_code) {
    XELOGW("XEX load failed with code {}, trying with devkit encryption key...",
           result_code);

    result_code = ReadImage(xex_addr, xex_length, xe_xex2_devkit_key);
    if (result_code) {
      XELOGE(
          "XEX load failed with code {}, trying with xex1 retail encryption "
          "key...",
          result_code);

      result_code = ReadImage(xex_addr, xex_length, xe_xex1_retail_key);
      if (result_code) {
        XELOGE(
            "XEX load failed with code {}, trying with xex1 devkit encryption "
            "key...",
            result_code);

        result_code = ReadImage(xex_addr, xex_length, xe_xex1_devkit_key);
        if (result_code) {
          XELOGE("XEX load failed with code {}", result_code);
          ReleaseImage();
          return false;
        }
      }
    }
  }

  // Note: caller will have to call LoadContinue once it's determined whether a
  // patch file exists or not!
  return true;
}

// The names to load an import library by, in order.
static std::vector<std::string> ImportLibraryNames(
    const std::string_view module_name, uint32_t title_id,
    const std::string_view library_name) {
  std::vector<std::string> names;
  // xefu2019.xex and the xefu2021 builds all import xefutitle.xex. A folder
  // holding several names each after its build, as xefutitle2019.xex, or
  // without the build letter, as xefutitle2021.xex for xefu2021c.xex.
  if (title_id == kXeFuTitleId &&
      xe::utf8::equal_case(library_name, "xefutitle.xex") &&
      module_name.size() > 4 &&
      xe::utf8::starts_with_case(module_name, "xefu")) {
    const std::string suffix(module_name.substr(4));
    names.push_back("xefutitle" + suffix + ".xex");
    std::string build = suffix;
    while (!build.empty() && ((build.back() >= 'a' && build.back() <= 'z') ||
                              (build.back() >= 'A' && build.back() <= 'Z'))) {
      build.pop_back();
    }
    if (!build.empty() && build != suffix) {
      names.push_back("xefutitle" + build + ".xex");
    }
  }
  names.emplace_back(library_name);
  return names;
}

bool XexModule::LoadContinue() {
  // Second part of image load
  // Split from Load() so that we can patch the XEX before loading this data
  assert_false(finished_load_);
  if (finished_load_) {
    return true;
  }

  finished_load_ = true;

  if (has_code() && ReadPEHeaders()) {
    XELOGE("Failed to load XEX PE headers!");
    return false;
  }

  // Parse any "unsafe" headers into safer variants
  xex2_opt_generic_u32* alternate_titleids;
  if (GetOptHeader(xex2_header_keys::XEX_HEADER_ALTERNATE_TITLE_IDS,
                   &alternate_titleids)) {
    auto count = alternate_titleids->count();
    for (uint32_t i = 0; i < count; i++) {
      opt_alternate_title_ids_.push_back(alternate_titleids->values[i]);
    }
  }

  // Scan and find the low/high addresses.
  // All code sections are continuous, so this should be easy.
  // could use a source for the above information
  auto heap = memory()->LookupHeap(base_address_);
  auto page_size = heap->page_size();

  low_address_ = UINT_MAX;
  high_address_ = 0;

  auto sec_header = xex_security_info();
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    const auto start_address = base_address_ + (page * page_size);
    const auto end_address = start_address + (desc.page_count * page_size);
    if (desc.info == XEX_SECTION_CODE) {
      low_address_ = std::min(low_address_, start_address);
      high_address_ = std::max(high_address_, end_address);
    }

    page += desc.page_count;
  }

  // Notify backend that we have an executable range.
  if (has_code()) {
    processor_->backend()->CommitExecutableRange(low_address_, high_address_);
  } else {
    low_address_ = 0;
  }

  // Add all imports (variables/functions).
  xex2_opt_import_libraries* opt_import_libraries = nullptr;
  GetOptHeader(XEX_HEADER_IMPORT_LIBRARIES, &opt_import_libraries);

  if (opt_import_libraries) {
    // FIXME: Don't know if 32 is the actual limit, but haven't seen more than
    // 2.
    const char* string_table[32];
    std::memset(string_table, 0, sizeof(string_table));

    // Parse the string table
    for (size_t i = 0, o = 0; i < opt_import_libraries->string_table.size &&
                              o < opt_import_libraries->string_table.count;
         ++o) {
      assert_true(o < xe::countof(string_table));
      const char* str = &opt_import_libraries->string_table.data[i];

      string_table[o] = str;
      i += std::strlen(str) + 1;

      // Padding
      if ((i % 4) != 0) {
        i += 4 - (i % 4);
      }
    }

    const auto* execution_info = opt_execution_info();
    const uint32_t title_id =
        execution_info ? static_cast<uint32_t>(execution_info->title_id) : 0;

    auto library_data = reinterpret_cast<uint8_t*>(opt_import_libraries);
    uint32_t library_offset = opt_import_libraries->string_table.size + 12;
    if (guest_import_provider_) {
      // Validate the entire consumer family before any slot/thunk changes.
      // Once a native object family is selected, a late missing export cannot
      // leave the first half native and the second half HLE.
      std::vector<GuestImportProvider::Request> requests;
      for (uint32_t offset = library_offset;
           offset < opt_import_libraries->size;) {
        if (uint64_t(offset) + offsetof(xex2_import_library, import_table) >
            opt_import_libraries->size) {
          return false;
        }
        const auto* library =
            reinterpret_cast<const xex2_import_library*>(library_data + offset);
        const size_t name_index = library->name_index & 0xFF;
        if (library->size < offsetof(xex2_import_library, import_table) ||
            uint64_t(offset) + library->size > opt_import_libraries->size ||
            uint64_t(offsetof(xex2_import_library, import_table)) +
                    uint64_t(library->count) * 4 >
                library->size ||
            name_index >= xe::countof(string_table) ||
            name_index >= opt_import_libraries->string_table.count ||
            !string_table[name_index]) {
          return false;
        }
        const std::string library_name(string_table[name_index]);
        std::vector<uint32_t> records;
        std::unordered_set<uint16_t> function_ordinals;
        for (uint32_t i = 0; i < library->count; ++i) {
          const uint32_t address = library->import_table[i];
          if (address < base_address_ ||
              uint64_t(address) + 4 > uint64_t(base_address_) + image_size() ||
              memory()->QueryRangeAccess(address, 4) ==
                  xe::memory::PageAccess::kNoAccess) {
            return false;
          }
          const uint32_t value =
              xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(address));
          if ((value >> 24) > 1) {
            return false;
          }
          records.push_back(value);
          if ((value >> 24) == 1) {
            function_ordinals.insert(uint16_t(value));
          }
        }
        for (const uint32_t value : records) {
          const uint16_t ordinal = uint16_t(value);
          const auto* known = processor_->export_resolver()->GetExportByOrdinal(
              library_name, ordinal);
          const auto kind = known
                                ? (known->get_type() == Export::Type::kFunction
                                       ? GuestImportProvider::Kind::kFunction
                                       : GuestImportProvider::Kind::kData)
                                : (function_ordinals.contains(ordinal)
                                       ? GuestImportProvider::Kind::kFunction
                                       : GuestImportProvider::Kind::kData);
          requests.push_back({library_name, ordinal, kind});
        }
        offset += library->size;
      }
      std::string error;
      if (!guest_import_provider_->Preflight(requests, &error)) {
        XELOGE("Native guest import preflight failed for {}: {}", name_, error);
        return false;
      }
    }
    while (library_offset < opt_import_libraries->size) {
      auto library =
          reinterpret_cast<xex2_import_library*>(library_data + library_offset);
      if (!library->size) {
        break;
      }
      size_t library_name_index = library->name_index & 0xFF;
      assert_true(library_name_index <
                  opt_import_libraries->string_table.count);
      assert_not_null(string_table[library_name_index]);
      auto library_name = std::string(string_table[library_name_index]);

      if (!kernel_state_->IsModuleLoaded(library_name)) {
        kernel::object_ref<kernel::UserModule> module;
        for (const auto& name :
             ImportLibraryNames(name_, title_id, library_name)) {
          // A name other than its own is only tried beside this module where
          // it exists, so as not to log not finding it.
          std::string load_path = name;
          if (name != library_name) {
            load_path = xe::utf8::join_guest_paths(
                xe::utf8::find_base_guest_path(path_), name);
            if (!kernel_state_->file_system()->ResolvePath(load_path)) {
              continue;
            }
          }
          module = kernel_state_->LoadUserModule(load_path);
          if (module) {
            break;
          }
        }
        if (module) {
          kernel_state_->FinishLoadingUserModule(module, false);
          // Its name can differ from the import's.
          library_name = module->path();
        } else {
          missing_import_libs_.push_back(library_name);
        }
      }

      if (!SetupLibraryImports(library_name, library)) {
        return false;
      }
      library_offset += library->size;
    }
  }

  // Load a specified module map and diff.
  if (cvars::load_module_map.size()) {
    if (!ReadMap(cvars::load_module_map.c_str())) {
      return false;
    }
  }

  // Plugins need writable code segments to patch the game in place.
  const bool writable_code_segments =
      cvars::writable_code_segments || cvars::allow_plugins;

  // Setup memory protection.
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    auto address = base_address_ + (page * page_size);
    auto size = desc.page_count * page_size;
    switch (desc.info) {
      case XEX_SECTION_CODE:
      case XEX_SECTION_READONLY_DATA:
        heap->Protect(address, size,
                      writable_code_segments
                          ? kMemoryProtectRead | kMemoryProtectWrite
                          : kMemoryProtectRead);
        break;
      case XEX_SECTION_DATA:
        heap->Protect(address, size, kMemoryProtectRead | kMemoryProtectWrite);
        break;
    }

    page += desc.page_count;
  }

  return true;
}

void XexModule::Precompile() { Precompile(true); }

void XexModule::Precompile(bool compile_discovered) {
  if (!has_code()) {
    return;
  }
  unsigned high_code = this->high_address_ - this->low_address_;

  mbedtls_sha1(memory()->TranslateVirtual(this->low_address_), high_code,
               image_sha_bytes_);

  image_sha_str_.clear();
  for (unsigned i = 0; i < sizeof(image_sha_bytes_); ++i) {
    image_sha_str_ += fmt::format("{:02X}", image_sha_bytes_[i]);
  }

  if (image_sha_str_.size() != sizeof(image_sha_bytes_) * 2) {
    XELOGE("XEX hash is the wrong length!");
  }

  // Find __savegprlr_* and __restgprlr_* and the others.
  // We can flag these for special handling (inlining/etc).
  if (!FindSaveRest()) {
    return;
  }

  info_cache_.Init(this);
  // Emulator::CompleteLaunch compiles the executable after plugins patch it.
  if (compile_discovered && !is_executable()) {
    PrecompileDiscoveredFunctions();
  }
}
bool XexModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;
  guest_host_bridges_.clear();

  // If this isn't a patch, just deallocate the memory occupied by the exe
  if (!is_patch()) {
    assert_not_zero(base_address_);

    memory()->LookupHeap(base_address_)->Release(base_address_);
  }

  xex_header_mem_.resize(0);

  return true;
}

bool XexModule::BindGuestExportToHost(uint16_t ordinal,
                                      std::string_view host_library,
                                      uint16_t host_ordinal) {
  if (!loaded_) {
    return false;
  }
  const uint32_t address = GetProcAddress(ordinal);
  auto* resolver = processor_->export_resolver();
  auto* target = resolver
                     ? resolver->GetExportByOrdinal(host_library, host_ordinal)
                     : nullptr;
  if (!target || target->get_type() != Export::Type::kFunction ||
      !target->is_implemented() || !target->function_data.trampoline) {
    return false;
  }
  // The export table supplies the entry identity. Legacy callers don't supply
  // a full original extent/hash; reserve only the two-instruction overlay.
  return InstallGuestFunctionBridge(
      address, 8, {},
      reinterpret_cast<GuestFunction::ExternHandler>(
          target->function_data.trampoline),
      target, target->name, EntryWitness::kExport);
}

bool XexModule::BindGuestExportToHost(uint16_t ordinal,
                                      std::string_view host_library,
                                      uint16_t host_ordinal, uint32_t full_size,
                                      std::string_view full_sha256) {
  if (!loaded_ || full_sha256.size() != 64) {
    return false;
  }
  const uint32_t address = GetProcAddress(ordinal);
  auto* resolver = processor_->export_resolver();
  auto* target = resolver
                     ? resolver->GetExportByOrdinal(host_library, host_ordinal)
                     : nullptr;
  if (!target || target->get_type() != Export::Type::kFunction ||
      !target->is_implemented() || !target->function_data.trampoline) {
    return false;
  }
  return InstallGuestFunctionBridge(
      address, full_size, full_sha256,
      reinterpret_cast<GuestFunction::ExternHandler>(
          target->function_data.trampoline),
      target, target->name, EntryWitness::kExportFullExtent);
}

bool XexModule::BindGuestFunctionToHost(uint32_t address, uint32_t size,
                                        std::string_view expected_sha256,
                                        std::string_view host_library,
                                        uint16_t host_ordinal) {
  auto* resolver = processor_->export_resolver();
  auto* target = resolver
                     ? resolver->GetExportByOrdinal(host_library, host_ordinal)
                     : nullptr;
  if (!target || target->get_type() != Export::Type::kFunction ||
      !target->is_implemented() || !target->function_data.trampoline) {
    return false;
  }
  return InstallGuestFunctionBridge(
      address, size, expected_sha256,
      reinterpret_cast<GuestFunction::ExternHandler>(
          target->function_data.trampoline),
      target, target->name, EntryWitness::kDirectCall);
}

bool XexModule::BindGuestFunctionToHost(
    uint32_t address, uint32_t size, std::string_view expected_sha256,
    GuestFunction::ExternHandler host_handler, std::string_view host_name) {
  return InstallGuestFunctionBridge(address, size, expected_sha256,
                                    host_handler, nullptr, host_name,
                                    EntryWitness::kDirectCall);
}

bool XexModule::BindGuestTaskCallbackToHost(
    uint32_t address, uint32_t size, std::string_view expected_sha256,
    const GuestTaskCallbackRegistration& registration,
    GuestFunction::ExternHandler host_handler, std::string_view host_name) {
  return InstallGuestFunctionBridge(
      address, size, expected_sha256, host_handler, nullptr, host_name,
      EntryWitness::kNativeTaskRegistration, &registration);
}

const uint8_t* XexModule::TranslateCode(uint32_t address) const {
  auto it = guest_host_bridges_.upper_bound(address);
  if (it != guest_host_bridges_.begin()) {
    --it;
    const uint32_t offset = address - it->first;
    if (offset < it->second.translation.size()) {
      return it->second.translation.data() + offset;
    }
  }
  return Module::TranslateCode(address);
}

Symbol::Status XexModule::DeclareFunction(uint32_t address,
                                          Function** out_function) {
  auto it = guest_host_bridges_.upper_bound(address);
  if (it != guest_host_bridges_.begin()) {
    --it;
    if (address != it->first &&
        uint64_t(address) < uint64_t(it->first) + it->second.original_size) {
      *out_function = nullptr;
      return Symbol::Status::kFailed;
    }
  }
  return Module::DeclareFunction(address, out_function);
}

bool XexModule::InstallGuestFunctionBridge(
    uint32_t address, uint32_t size, std::string_view expected_sha256,
    GuestFunction::ExternHandler handler, Export* export_data,
    std::string_view host_name, EntryWitness entry_witness,
    const GuestTaskCallbackRegistration* task_registration) {
  // No lock can undo a literal machine-code target embedded in a caller.
  // The caller must keep this module quiescent from load until installation.
  auto lock = xe::global_critical_region::AcquireDirect();
  const bool require_full_extent = entry_witness != EntryWitness::kExport;
  const bool require_task_registration =
      entry_witness == EntryWitness::kNativeTaskRegistration;
  if (require_task_registration != (task_registration != nullptr)) {
    return false;
  }
  const uint64_t end = uint64_t(address) + size;
  const uint64_t image_end =
      uint64_t(base_address_) + security_info_.image_size;
  if (!loaded_ || !base_address_ || !handler || host_name.empty() ||
      (address & 3) || (size & 3) || size < 8 || size > 1024 * 1024 ||
      address < base_address_ || end > image_end || end > 0x100000000ull ||
      image_end > 0x100000000ull ||
      (require_full_extent && expected_sha256.size() != 64)) {
    return false;
  }
  const auto readable_image = [&](uint32_t start, uint64_t length) {
    return length && length <= UINT32_MAX && start >= base_address_ &&
           uint64_t(start) + length <= image_end &&
           memory()->QueryRangeAccess(start, uint32_t(length)) !=
               xe::memory::PageAccess::kNoAccess;
  };
  const auto code_owned = [&](uint32_t start, uint32_t length) {
    if (!readable_image(start, length)) {
      return false;
    }
    for (uint64_t current = start; current < uint64_t(start) + length;
         current = (current & ~uint64_t(0xFFF)) + 0x1000) {
      xex2_section_type section;
      if (!GetPageSectionType(uint32_t(current), &section) ||
          section != XEX_SECTION_CODE) {
        return false;
      }
    }
    // CPU modules may claim narrower ranges than XEX allocation pages.
    for (uint64_t current = start; current < uint64_t(start) + length;
         current += 4) {
      if (processor_->LookupModule(uint32_t(current)) != this) {
        return false;
      }
    }
    return processor_->LookupModule(start + length - 1) == this;
  };
  if (!code_owned(address, size)) {
    return false;
  }
  const auto hash_matches = [&](uint32_t start, uint32_t length,
                                std::string_view expected) {
    if (expected.size() != 64 || !code_owned(start, length)) {
      return false;
    }
    const auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    std::array<uint8_t, 32> digest;
    mbedtls_sha256(memory()->TranslateVirtual<const uint8_t*>(start), length,
                   digest.data(), 0);
    for (size_t i = 0; i < digest.size(); ++i) {
      const int high = hex(expected[i * 2]);
      const int low = hex(expected[i * 2 + 1]);
      if (high < 0 || low < 0 || digest[i] != ((high << 4) | low)) {
        return false;
      }
    }
    return true;
  };
  if (!expected_sha256.empty() &&
      !hash_matches(address, size, expected_sha256)) {
    return false;
  }
  if (require_task_registration) {
    const auto& witness = *task_registration;
    const uint64_t schedule_end =
        uint64_t(witness.schedule_address) + witness.schedule_size;
    const uint64_t registration_end =
        uint64_t(witness.registration_address) + witness.registration_size;
    if (witness.registration_size != 9 * 4 ||
        (witness.registration_address & 3) || (witness.schedule_address & 3) ||
        (witness.schedule_size & 3) || witness.schedule_size < 8 ||
        witness.schedule_size > 1024 * 1024 || schedule_end > image_end ||
        registration_end > image_end ||
        (witness.schedule_address < end && address < schedule_end) ||
        (witness.registration_address < end && address < registration_end) ||
        (witness.registration_address < schedule_end &&
         witness.schedule_address < registration_end) ||
        !hash_matches(witness.registration_address, witness.registration_size,
                      witness.registration_sha256) ||
        !hash_matches(witness.schedule_address, witness.schedule_size,
                      witness.schedule_sha256)) {
      return false;
    }
    // Version-verified native registration template. The unrelated stores and
    // argument preparation cannot clobber r10 between lis and addi r3. There
    // is no branch before the direct BL and no address from guest host data.
    std::array<uint32_t, 9> words;
    for (size_t i = 0; i < words.size(); ++i) {
      words[i] = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(
          witness.registration_address + uint32_t(i) * 4));
    }
    if ((words[0] & 0xFFFF0000) != 0x39610000 ||  // addi r11,r1,args
        (words[1] & 0xFFFF0000) != 0x3D400000 ||  // lis r10,callback@ha
        (words[2] & 0xFFFF0000) != 0x38C10000 ||  // addi r6,r1,out
        (words[3] & 0xFFFF0000) != 0x38A10000 ||  // addi r5,r1,args
        words[4] != 0x38800000 ||                 // li r4,0
        words[5] != 0xFBEB0000 ||                 // std r31,0(r11)
        (words[6] & 0xFFFF0000) != 0x93C10000 ||  // stw r30,args(r1)
        (words[7] & 0xFFFF0000) != 0x386A0000 ||  // addi r3,r10,callback@l
        (words[8] & 0xFC000003) != 0x48000001 ||  // relative BL
        uint16_t(words[0]) != uint16_t(words[3]) ||
        uint16_t(words[0]) != uint16_t(words[6])) {
      return false;
    }
    const uint32_t callback = (uint32_t(uint16_t(words[1])) << 16) +
                              uint32_t(int32_t(int16_t(words[7])));
    int64_t displacement = words[8] & 0x03FFFFFC;
    if (displacement & 0x02000000) {
      displacement -= 0x04000000;
    }
    const uint32_t schedule =
        uint32_t(int64_t(witness.registration_address) + 8 * 4 + displacement);
    if (callback != address || schedule != witness.schedule_address) {
      return false;
    }
  }
  for (const auto& [start, bridge] : guest_host_bridges_) {
    for (const auto& dependency : bridge.dependencies) {
      if (dependency.size && uint64_t(dependency.address) < end &&
          address < uint64_t(dependency.address) + dependency.size) {
        return false;
      }
    }
    const uint64_t bridge_end = uint64_t(start) + bridge.original_size;
    if ((uint64_t(start) < end && address < bridge_end) ||
        (require_task_registration &&
         ((uint64_t(start) < uint64_t(task_registration->schedule_address) +
                                 task_registration->schedule_size &&
           task_registration->schedule_address < bridge_end) ||
          (uint64_t(start) < uint64_t(task_registration->registration_address) +
                                 task_registration->registration_size &&
           task_registration->registration_address < bridge_end)))) {
      return false;
    }
  }
  bool conflict = false;
  ForEachFunction([&](Function* function) {
    const uint32_t start = function->address();
    const uint64_t function_end = function->has_end_address()
                                      ? uint64_t(function->end_address()) + 4
                                      : uint64_t(start) + 4;
    if (uint64_t(start) >= end || function_end <= address) {
      return;
    }
    if (start != address || !function->is_guest() || function->IsSaverest() ||
        function->behavior() != Function::Behavior::kDefault ||
        function->status() != Symbol::Status::kDeclared ||
        static_cast<GuestFunction*>(function)->machine_code() ||
        (require_full_extent && function->has_end_address() &&
         function_end != end)) {
      conflict = true;
    }
  });
  for (uint32_t entry : GetAddressedFunctions()) {
    if (entry >= address && uint64_t(entry) < end) {
      auto* symbol = LookupSymbol(entry, false);
      if (entry != address || !symbol ||
          symbol->type() != Symbol::Type::kFunction) {
        conflict = true;
      }
    }
  }
  if (conflict || processor_->QueryFunction(address)) {
    return false;
  }
  // PE runtime entries and real exports must not hide an interior alias.
  bool exact_task_entry = false;
  bool exact_schedule_entry = false;
  if (const auto* pdata = GetPESection(".pdata"); pdata && pdata->raw_size) {
    if ((pdata->raw_size & 7) ||
        !readable_image(pdata->address, pdata->raw_size)) {
      return false;
    }
    for (uint32_t offset = 0; offset < pdata->raw_size; offset += 8) {
      const uint32_t entry = xe::load_and_swap<uint32_t>(
          memory()->TranslateVirtual(pdata->address + offset));
      if (entry > address && uint64_t(entry) < end) {
        return false;
      }
      if (require_task_registration) {
        const auto& witness = *task_registration;
        const uint32_t packed = xe::load_and_swap<uint32_t>(
            memory()->TranslateVirtual(pdata->address + offset + 4));
        const uint32_t length = ((packed >> 8) & 0x3FFFFF) * 4;
        const uint64_t runtime_end = uint64_t(entry) + length;
        if ((entry != address && entry < end && runtime_end > address) ||
            (entry != witness.schedule_address &&
             uint64_t(entry) <
                 uint64_t(witness.schedule_address) + witness.schedule_size &&
             runtime_end > witness.schedule_address)) {
          return false;
        }
        if (entry == address) {
          if (exact_task_entry || length != size) {
            return false;
          }
          exact_task_entry = true;
        }
        if (entry == witness.schedule_address) {
          if (exact_schedule_entry || length != witness.schedule_size) {
            return false;
          }
          exact_schedule_entry = true;
        }
        if (entry > witness.schedule_address &&
            uint64_t(entry) <
                uint64_t(witness.schedule_address) + witness.schedule_size) {
          return false;
        }
      }
    }
  }
  if (require_task_registration &&
      (!exact_task_entry || !exact_schedule_entry)) {
    return false;
  }
  if (security_info_.export_table) {
    const uint32_t table_address = security_info_.export_table;
    constexpr uint32_t kHeader = offsetof(xex2_export_table, ordOffset);
    if (!readable_image(table_address, kHeader)) {
      return false;
    }
    const auto* table =
        memory()->TranslateVirtual<const xex2_export_table*>(table_address);
    const uint32_t count = table->count;
    if (count > 0x10000 ||
        !readable_image(table_address, kHeader + uint64_t(count) * 4)) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t entry = (uint64_t(table->imagebaseaddr) << 16) +
                             uint32_t(table->ordOffset[i]);
      if (entry > address && entry < end) {
        return false;
      }
    }
  } else {
    xex2_opt_data_directory* directory_info = nullptr;
    if (GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &directory_info)) {
      const uint64_t directory_address =
          uint64_t(base_address_) + directory_info->offset;
      if (directory_address > UINT32_MAX ||
          !readable_image(uint32_t(directory_address),
                          sizeof(X_IMAGE_EXPORT_DIRECTORY))) {
        return false;
      }
      const auto* directory =
          memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
              uint32_t(directory_address));
      const uint32_t count = directory->NumberOfFunctions;
      const uint64_t table = directory_address + directory->AddressOfFunctions;
      if (count > 0x10000 || table > UINT32_MAX ||
          (count && !readable_image(uint32_t(table), uint64_t(count) * 4))) {
        return false;
      }
      const auto* entries =
          memory()->TranslateVirtual<const uint32_t*>(uint32_t(table));
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry = uint64_t(base_address_) + entries[i];
        if (entry > address && entry < end) {
          return false;
        }
      }
    }
  }
  if (require_full_extent) {
    const auto branch_target = [](uint32_t pc, uint32_t word) {
      const bool conditional = (word >> 26) == 16;
      int64_t displacement = word & (conditional ? 0xFFFC : 0x03FFFFFC);
      if (displacement & (conditional ? 0x8000 : 0x02000000)) {
        displacement -= conditional ? 0x10000 : 0x04000000;
      }
      return uint32_t((word & 2) ? displacement : int64_t(pc) + displacement);
    };
    const uint32_t terminal = xe::load_and_swap<uint32_t>(
        memory()->TranslateVirtual(uint32_t(end - 4)));
    const uint32_t terminal_target = branch_target(uint32_t(end - 4), terminal);
    if (terminal != 0x4E800020 && terminal != 0x4E800420 &&
        !((terminal >> 26) == 18 && !(terminal & 1) &&
          (terminal_target < address || uint64_t(terminal_target) >= end))) {
      return false;
    }
    bool called_entry = false;
    for (uint64_t page = base_address_; page < image_end;
         page = (page & ~uint64_t(0xFFF)) + 0x1000) {
      xex2_section_type section;
      if (!GetPageSectionType(uint32_t(page), &section)) {
        return false;
      }
      if (section != XEX_SECTION_CODE) {
        continue;
      }
      const uint32_t page_size = uint32_t(
          std::min<uint64_t>(0x1000 - (page & 0xFFF), image_end - page));
      if (!code_owned(uint32_t(page), page_size)) {
        return false;
      }
      for (uint32_t offset = 0; offset < page_size; offset += 4) {
        const uint32_t pc = uint32_t(page) + offset;
        const uint32_t word =
            xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(pc));
        const uint32_t opcode = word >> 26;
        if (opcode != 18 && opcode != 16) {
          continue;
        }
        const uint32_t target = branch_target(pc, word);
        called_entry |= opcode == 18 && (word & 1) && target == address;
        if (target > address && uint64_t(target) < end &&
            ((word & 1) || pc < address || uint64_t(pc) >= end)) {
          return false;
        }
      }
    }
    if (entry_witness == EntryWitness::kDirectCall && !called_entry) {
      return false;
    }
  }
  // All validation precedes symbol/cache mutation. No compiled version was
  // accepted, so existing callers can only use a resolvable indirection slot.
  processor_->InvalidateCodeRange(address, size);
  Function* function = nullptr;
  const auto status = DeclareFunction(address, &function);
  if (status != Symbol::Status::kNew && status != Symbol::Status::kDeclared) {
    return false;
  }
  auto* guest = static_cast<GuestFunction*>(function);
  try {
    guest->set_name(host_name);
    GuestHostBridge bridge{size, {}};
    if (require_task_registration) {
      bridge.dependencies[0] = {task_registration->registration_address,
                                task_registration->registration_size};
      bridge.dependencies[1] = {task_registration->schedule_address,
                                task_registration->schedule_size};
    }
    xe::store_and_swap<uint32_t>(bridge.translation.data(), 0x44000042);
    xe::store_and_swap<uint32_t>(bridge.translation.data() + 4, 0x4E800020);
    guest_host_bridges_.emplace(address, std::move(bridge));
  } catch (...) {
    guest->set_status(Symbol::Status::kFailed);
    throw;
  }
  // The function end is the inclusive final synthetic instruction.
  guest->set_end_address(address + 4);
  guest->SetupExtern(handler, export_data);
  guest->set_status(Symbol::Status::kDeclared);
  XELOGI("Guest function bridge: {} address={:08X} original_size={} -> {}",
         name_, address, size, host_name);
  return true;
}

bool XexModule::SetupLibraryImports(const std::string_view name,
                                    const xex2_import_library* library) {
  ExportResolver* kernel_resolver = nullptr;
  if (kernel_state_->IsKernelModule(name)) {
    kernel_resolver = processor_->export_resolver();
  }

  auto user_module = kernel_state_->GetModule(name);

  auto base_name = utf8::find_base_name_from_guest_path(name);

  ImportLibrary library_info;
  library_info.name = base_name;
  library_info.id = library->id;
  library_info.version.value = library->version().value;
  library_info.min_version.value = library->version_min().value;
  std::unordered_set<uint16_t> function_ordinals;
  if (guest_import_provider_) {
    for (uint32_t i = 0; i < library->count; ++i) {
      const uint32_t value = xe::load_and_swap<uint32_t>(
          memory()->TranslateVirtual(uint32_t(library->import_table[i])));
      if ((value >> 24) == 1) {
        function_ordinals.insert(uint16_t(value));
      }
    }
  }

  // Imports are stored as {import descriptor, thunk addr, import desc, ...}
  // Even thunks have an import descriptor (albeit unused/useless)
  for (uint32_t i = 0; i < library->count; i++) {
    uint32_t record_addr = library->import_table[i];
    assert_not_zero(record_addr);

    auto record_slot =
        memory()->TranslateVirtual<xe::be<uint32_t>*>(record_addr);
    uint32_t record_value = *record_slot;

    uint16_t record_type = (record_value & 0xFF000000) >> 24;
    uint16_t ordinal = record_value & 0xFFFF;

    Export* kernel_export = nullptr;
    uint32_t user_export_addr = 0;

    if (kernel_resolver) {
      kernel_export = kernel_resolver->GetExportByOrdinal(name, ordinal);
      // A module that enters user mode runs guest code no module claims, and
      // every call site has to be translated for it, so before precompiling.
      if (kernel_export &&
          std::strcmp(kernel_export->name, "KeCreateUserMode") == 0) {
        processor_->EnableDynamicCode();
      }
    } else if (user_module) {
      user_export_addr = user_module->GetProcAddressByOrdinal(ordinal);
    }
    if (guest_import_provider_) {
      const auto kind =
          kernel_export
              ? (kernel_export->get_type() == Export::Type::kFunction
                     ? GuestImportProvider::Kind::kFunction
                     : GuestImportProvider::Kind::kData)
              : (record_type == 1 || function_ordinals.contains(ordinal)
                     ? GuestImportProvider::Kind::kFunction
                     : GuestImportProvider::Kind::kData);
      const auto resolution =
          guest_import_provider_->Resolve(name, ordinal, kind);
      if (resolution.disposition ==
          GuestImportProvider::Disposition::kRejected) {
        XELOGE("Native guest import rejected for {}: {}:{:03X}", name_, name,
               ordinal);
        return false;
      }
      if (resolution.disposition == GuestImportProvider::Disposition::kBound) {
        kernel_export = nullptr;
        user_export_addr = resolution.address;
        XELOGI("Native guest import: {} {}:{:03X} -> {:08X}", name_, name,
               ordinal, user_export_addr);
      }
    }

    // Import not resolved?
    if (!kernel_export && !user_export_addr) {
      XELOGW(
          "WARNING: an import variable was not resolved! (library: {}, import "
          "lib: {}, ordinal: {:03X})",
          name_, name, ordinal);
    }

    StringBuffer import_name;
    if (record_type == 0) {
      // Variable.

      ImportLibraryFn import_info{};
      import_info.ordinal = ordinal;
      import_info.value_address = record_addr;
      library_info.imports.push_back(import_info);

      import_name.Append("__imp__");
      if (kernel_export) {
        import_name.Append(kernel_export->name);
      } else {
        import_name.AppendFormat("{}_{:03X}", base_name, ordinal);
      }

      if (kernel_export) {
        if (kernel_export->get_type() == Export::Type::kFunction) {
          // Not exactly sure what this should be...
          // Appears to be ignored.
          *record_slot = 0xDEADC0DE;
        } else if (kernel_export->get_type() == Export::Type::kVariable) {
          // Kernel import variable
          if (kernel_export->is_implemented()) {
            // Implemented - replace with pointer.
            *record_slot = kernel_export->variable_ptr;
          } else {
            // Not implemented - write with a dummy value.
            *record_slot = 0xD000BEEF | (kernel_export->ordinal & 0xFFF) << 16;
            XELOGCPU("WARNING: imported a variable with no value: {}",
                     kernel_export->name);
          }
        }
      } else if (user_export_addr) {
        *record_slot = user_export_addr;
      } else {
        *record_slot = 0xF00DF00D;
      }

      // Setup a variable and define it.
      Symbol* var_info;
      DeclareVariable(record_addr, &var_info);
      var_info->set_name(import_name.to_string_view());
      var_info->set_status(Symbol::Status::kDeclared);
      DefineVariable(var_info);
      var_info->set_status(Symbol::Status::kDefined);
    } else if (record_type == 1) {
      // Thunk.
      if (library_info.imports.size() > 0) {
        auto& prev_import =
            library_info.imports[library_info.imports.size() - 1];
        assert_true(prev_import.ordinal == ordinal);
        prev_import.thunk_address = record_addr;
      }

      if (kernel_export) {
        import_name.Append(kernel_export->name);
      } else {
        import_name.AppendFormat("__{}_{:03X}", base_name, ordinal);
      }

      Function* function;
      DeclareFunction(record_addr, &function);
      function->set_end_address(record_addr + 16 - 4);
      function->set_name(import_name.to_string_view());

      if (user_export_addr) {
        // Rewrite PPC code to set r11 to the target address
        // So we'll have:
        //    lis r11, user_export_addr
        //    ori r11, r11, user_export_addr
        //    mtspr CTR, r11
        //    bctr
        uint16_t hi_addr = (user_export_addr >> 16) & 0xFFFF;
        uint16_t low_addr = user_export_addr & 0xFFFF;

        uint8_t* p = memory()->TranslateVirtual(record_addr);
        xe::store_and_swap<uint32_t>(p + 0x0, 0x3D600000 | hi_addr);
        xe::store_and_swap<uint32_t>(p + 0x4, 0x616B0000 | low_addr);
      } else {
        // On load we have something like this in memory:
        //     li r3, 0
        //     li r4, 0x1F5
        //     mtspr CTR, r11
        //     bctr
        // Real consoles rewrite this with some code that sets r11.
        // If we did that we'd still have to put a thunk somewhere and do the
        // dynamic lookup. Instead, we rewrite it to use syscalls.
        // We use sc with a LEV operand of 2, which is reserved usage and
        // should never see actual usage outside of our rewrite.
        // CPU backends can either take the special form syscall or do
        // something smarter.
        //     sc 2
        //     blr
        //     nop
        //     nop
        uint8_t* p = memory()->TranslateVirtual(record_addr);
        xe::store_and_swap<uint32_t>(p + 0x0, 0x44000042);
        xe::store_and_swap<uint32_t>(p + 0x4, 0x4E800020);
        xe::store_and_swap<uint32_t>(p + 0x8, 0x60000000);
        xe::store_and_swap<uint32_t>(p + 0xC, 0x60000000);

        // Note that we may not have a handler registered - if not, eventually
        // we'll get directed to UndefinedImport.
        GuestFunction::ExternHandler handler = nullptr;
        if (kernel_export) {
          if (kernel_export->function_data.trampoline) {
            handler = (GuestFunction::ExternHandler)
                          kernel_export->function_data.trampoline;
          } else {
            //__debugbreak();
            // handler =
            //     (GuestFunction::ExternHandler)kernel_export->function_data.shim;
          }
        } else {
          XELOGW("WARNING: Imported kernel function {} is unimplemented!",
                 import_name.to_string_view());
        }
        static_cast<GuestFunction*>(function)->SetupExtern(handler,
                                                           kernel_export);
      }
      function->set_status(Symbol::Status::kDeclared);
    } else {
      // Bad.
      assert_always();
    }
  }

  import_libs_.push_back(library_info);

  return true;
}

bool XexModule::ContainsAddress(uint32_t address) {
  return address >= low_address_ && address < high_address_;
}

bool XexModule::GetPageSectionType(uint32_t address,
                                   xex2_section_type* out_type) const {
  if (!loaded_ || !base_address_ || address < base_address_ ||
      address - base_address_ >= xex_security_info()->image_size) {
    return false;
  }
  auto heap = memory()->LookupHeap(base_address_);
  if (!heap) {
    return false;
  }
  const uint32_t page = (address - base_address_) / heap->page_size();
  auto sec_header = xex_security_info();
  for (uint32_t i = 0, end = 0; i < sec_header->page_descriptor_count; i++) {
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);
    end += desc.page_count;
    if (page < end) {
      *out_type = desc.info;
      return true;
    }
  }
  return false;
}

std::unique_ptr<Function> XexModule::CreateFunction(uint32_t address) {
  return std::unique_ptr<Function>(
      processor_->backend()->CreateGuestFunction(this, address));
}
void XexInfoCache::Init(XexModule* xexmod) {
  // If context promotion is disabled then disable instruction info cache as
  // well, otherwise XMA will write to unknown registers.
  if (cvars::disable_instruction_infocache ||
      cvars::disable_context_promotion) {
    return;
  }

  auto emu = xexmod->kernel_state_->emulator();
  std::filesystem::path infocache_path = emu->cache_root();

  infocache_path.append(L"modules");

  infocache_path.append(xexmod->image_sha_str_);

  std::filesystem::create_directories(infocache_path);
  infocache_path.append("executable_addr_flags.bin");

  unsigned num_codebytes = xexmod->high_address_ - xexmod->low_address_;
  num_codebytes += 3;  // round up to nearest multiple of 4
  num_codebytes &= ~3;

  auto try_open = [this, &infocache_path, num_codebytes]() {
    bool did_exist = true;
    const size_t file_size = sizeof(InfoCacheFlagsHeader) +
                             (sizeof(InfoCacheFlags) * (num_codebytes / 4));

    if (!std::filesystem::exists(infocache_path)) {
      xe::filesystem::CreateEmptyFile(infocache_path);
      std::filesystem::resize_file(infocache_path, file_size);
      did_exist = false;
    }

    // todo: prepopulate with stuff from pdata, dll exports
    this->executable_addr_flags_ = MappedMemory::Open(
        infocache_path, xe::MappedMemory::Mode::kReadWrite, 0,
        file_size);  // one infocacheflags entry for each PPC instr-sized addr

    return did_exist;
  };

  bool did_exist = try_open();
  if (!GetHeader()) {
    return;
  }

  if (!did_exist) {
    GetHeader()->version = CURRENT_INFOCACHE_VERSION;

  } else {
    if (GetHeader()->version != CURRENT_INFOCACHE_VERSION) {
      this->executable_addr_flags_->Close();
      std::filesystem::remove(infocache_path);
      try_open();
    }
  }
}
InfoCacheFlags* XexModule::GetInstructionAddressFlags(uint32_t guest_addr) {
  auto bridge = guest_host_bridges_.upper_bound(guest_addr);
  if (bridge != guest_host_bridges_.begin()) {
    --bridge;
    if (uint64_t(guest_addr) <
        uint64_t(bridge->first) + bridge->second.original_size) {
      // The persistent cache is keyed by original guest code, not this
      // module's explicit host-binding policy. Neither consume old native
      // instruction facts nor persist synthetic syscall facts under that hash.
      return nullptr;
    }
  }
  if (guest_addr < low_address_ || guest_addr > high_address_) {
    return nullptr;
  }

  guest_addr -= low_address_;

  return info_cache_.LookupFlags(guest_addr);
}
void XexModule::PrecompileDiscoveredFunctions() {
  if (!cvars::enable_early_precompilation) {
    return;
  }
  auto start_time = std::chrono::steady_clock::now();
  auto others = PreanalyzeCode();
  others.erase(std::remove_if(others.begin(), others.end(),
                              [this](uint32_t address) {
                                return !ContainsAddress(address);
                              }),
               others.end());
  size_t compiled = processor_->ResolveFunctionsInParallel(std::move(others));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_time);
  XELOGI("Precompiled {} discovered functions in {} ms", compiled,
         elapsed.count());
}
void XexModule::PrecompileKnownFunctions() {
  if (!cvars::enable_early_precompilation) {
    return;
  }
  uint32_t start = 0;
  uint32_t end = (high_address_ - low_address_) / 4;
  auto flags = info_cache_.LookupFlags(0);
  if (!flags) {
    return;
  }
  // maybe should pre-acquire global crit?
  for (uint32_t i = 0; i < end; i++) {
    if (flags[i].was_resolved) {
      uint32_t addr = low_address_ + (i * 4);
      auto sym = processor_->LookupFunction(addr);

      if (!sym || sym->status() != Symbol::Status::kDefined) {
        processor_->ResolveFunction(addr);
      }
    }
  }
}

static uint32_t GetBLCalledFunction(XexModule* xexmod, uint32_t current_base,
                                    ppc::PPCOpcodeBits wrd) {
  int32_t displ = static_cast<int32_t>(ppc::XEEXTS26(wrd.I.LI << 2));

  if (wrd.I.AA) {
    return static_cast<uint32_t>(displ);
  } else {
    return static_cast<uint32_t>(static_cast<int32_t>(current_base) + displ);
  }
}
static bool IsOpcodeBL(unsigned w) {
  return (w >> (32 - 6)) == 18 && ppc::PPCOpcodeBits{w}.I.LK;
}

constexpr uint32_t kScanDepth = 3;
constexpr uint32_t kMaxScannedFunctions = 128;
constexpr uint32_t kMaxScannedInstructions = 2048;
constexpr uint32_t kMaxTableBytes = 0x40000;

std::vector<uint32_t> XexModule::FindStaticInitializers() const {
  std::vector<uint32_t> initializers;
  uint32_t entry_point = 0;
  if (!GetOptHeader(XEX_HEADER_ENTRY_POINT, &entry_point) ||
      !IsCodeAddress(entry_point)) {
    return initializers;
  }

  auto read = [this](uint32_t address) -> uint32_t {
    return *memory()->TranslateVirtualBE<uint32_t>(address);
  };
  // A table holds code addresses and 0 or -1 padding between NULL sentinels.
  auto add_tables = [&](std::vector<uint32_t>& constants) {
    std::sort(constants.begin(), constants.end());
    constants.erase(std::unique(constants.begin(), constants.end()),
                    constants.end());
    for (auto begin_it = constants.begin(); begin_it != constants.end();
         ++begin_it) {
      uint32_t begin = *begin_it;
      const PESection* section = nullptr;
      for (auto& candidate : pe_sections_) {
        if (!(candidate.flags & kXEPESectionContainsCode) &&
            begin >= candidate.address &&
            begin - candidate.address < candidate.raw_size) {
          section = &candidate;
          break;
        }
      }
      if (!section || (begin & 3) || read(begin)) {
        continue;
      }
      uint32_t section_end =
          section->address + std::min(section->size, section->raw_size);
      uint32_t limit = std::min(section_end - 4, begin + kMaxTableBytes);
      uint32_t valid_end = begin;
      while (valid_end < limit) {
        uint32_t value = read(valid_end);
        if (value && value != UINT32_MAX && !IsCodeAddress(value)) {
          break;
        }
        valid_end += 4;
      }
      uint32_t table_end = begin;
      for (auto end_it = begin_it + 1;
           end_it != constants.end() && *end_it <= valid_end; ++end_it) {
        if (!(*end_it & 3) && !read(*end_it)) {
          table_end = *end_it;
        }
      }
      for (uint32_t address = begin; address < table_end; address += 4) {
        uint32_t value = read(address);
        if (value && value != UINT32_MAX) {
          initializers.push_back(value);
        }
      }
    }
  };

  // _cinit hands _initterm its [begin, end) bounds as lis/addi constants.
  std::vector<uint32_t> functions = {entry_point};
  std::unordered_set<uint32_t> queued = {entry_point};
  for (uint32_t depth = 0; depth < kScanDepth; ++depth) {
    std::vector<uint32_t> next_depth;
    for (uint32_t function : functions) {
      std::array<uint32_t, 32> registers = {};
      std::vector<uint32_t> constants;
      for (uint32_t i = 0, address = function;
           i < kMaxScannedInstructions && address < high_address_;
           ++i, address += 4) {
        uint32_t code = read(address);
        ppc::PPCOpcodeBits bits{code};
        uint32_t opcode = code >> 26;
        if (code == 0x4E800020) {
          break;
        } else if (IsOpcodeBL(code)) {
          uint32_t callee = GetBLCalledFunction(nullptr, address, bits);
          if (depth + 1 < kScanDepth && IsCodeAddress(callee) &&
              next_depth.size() < kMaxScannedFunctions &&
              queued.insert(callee).second) {
            next_depth.push_back(callee);
          }
        } else if (opcode == 15 && !bits.D.RA) {
          registers[bits.D.RT] = static_cast<uint32_t>(bits.D.DS) << 16;
        } else if (opcode == 14 && bits.D.RA) {
          registers[bits.D.RT] =
              registers[bits.D.RA] +
              static_cast<uint32_t>(ppc::XEEXTS16(bits.D.DS));
          constants.push_back(registers[bits.D.RT]);
        } else if (opcode == 24) {
          registers[bits.D.RA] = registers[bits.D.RT] | bits.D.DS;
          constants.push_back(registers[bits.D.RA]);
        }
      }
      add_tables(constants);
    }
    functions.swap(next_depth);
  }

  std::sort(initializers.begin(), initializers.end());
  initializers.erase(std::unique(initializers.begin(), initializers.end()),
                     initializers.end());
  return initializers;
}

void XexModule::PrecompileStaticInitializers() {
  auto start_time = std::chrono::steady_clock::now();
  std::vector<uint32_t> initializers = FindStaticInitializers();
  if (initializers.empty()) {
    return;
  }
  size_t initializer_count = initializers.size();

  // Lazy compiles during static init can reorder the threads it starts.
  auto add_callees = [this](Function* function, std::vector<uint32_t>& found) {
    if (!function->has_end_address()) {
      return;
    }
    // The scanner's end address is the last instruction, not one past it.
    uint32_t start = function->address();
    uint32_t last = function->end_address();
    for (uint32_t instr = start; instr <= last; instr += 4) {
      uint32_t code = *memory()->TranslateVirtualBE<uint32_t>(instr);
      if ((code >> 26) != 18) {
        continue;
      }
      uint32_t target =
          GetBLCalledFunction(this, instr, ppc::PPCOpcodeBits{code});
      bool is_tail_call = target < start || target > last;
      if ((IsOpcodeBL(code) || is_tail_call) && IsCodeAddress(target)) {
        found.push_back(target);
      }
    }
  };
  size_t compiled = processor_->ResolveFunctionsInParallel(
      std::move(initializers), add_callees);

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_time);
  XELOGI(
      "Precompiled {} static initializers and callees ({} functions) in {} ms",
      initializer_count, compiled, elapsed.count());
}

std::vector<uint32_t> XexModule::PreanalyzeCode() {
  uint32_t low_8_aligned = xe::align<uint32_t>(low_address_, 8);

  uint32_t highest_exec_addr = 0;

  for (auto&& sec : pe_sections_) {
    if ((sec.flags & kXEPESectionContainsCode)) {
      highest_exec_addr =
          std::max<uint32_t>(highest_exec_addr, sec.address + sec.size);
    }
  }
  uint32_t high_8_aligned = highest_exec_addr & ~(8U - 1);
  std::vector<uint32_t> funcstarts;
  funcstarts.reserve((high_8_aligned - low_8_aligned) / 8);
  {
    // all functions seem to start on 8 byte boundaries, except for obvious ones
    // like the save/rest funcs
    uint32_t* range_start =
        (uint32_t*)memory()->TranslateVirtual(low_8_aligned);
    uint32_t* range_end = (uint32_t*)memory()->TranslateVirtual(
        high_8_aligned);  // align down to multiple of 8

    constexpr uint8_t mfspr_r12_lr[4] = {0x7D, 0x88, 0x02, 0xA6};

    // a blr instruction, with 4 zero bytes afterwards to pad the next address
    // to 8 byte alignment
    // if we see this prior to our address, we can assume we are a function
    // start
    constexpr uint8_t blr[4] = {0x4E, 0x80, 0x0, 0x20};

    uint32_t blr32 = *reinterpret_cast<const uint32_t*>(&blr[0]);

    uint32_t mfspr_r12_lr32 =
        *reinterpret_cast<const uint32_t*>(&mfspr_r12_lr[0]);

    auto add_new_func = [&funcstarts](uint32_t addr) {
      funcstarts.push_back(addr);
    };
    /*
                First pass: detect save of the link register at an eight byte
       aligned address
        */
    for (uint32_t* first_pass = range_start; first_pass < range_end;
         first_pass += 2) {
      if (*first_pass == mfspr_r12_lr32) {
        // Push our newly discovered function start into our list
        // All addresses in the list are sorted until the second pass
        add_new_func(
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first_pass) -
                                  reinterpret_cast<uintptr_t>(range_start)) +
            low_8_aligned);
      } else if (first_pass[-1] == 0 && *first_pass != 0) {
        // originally i checked for blr followed by 0, but some functions are
        // actually aligned to greater boundaries. something that appears to be
        // longjmp (it occurs in most games, so standard library, and loads ctx,
        // so longjmp) is aligned to 16 bytes in most games
        uint32_t* check_iter = &first_pass[-2];

        while (!*check_iter) {
          --check_iter;
        }

        XE_LIKELY_IF(*check_iter == blr32) {
          add_new_func(
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first_pass) -
                                    reinterpret_cast<uintptr_t>(range_start)) +
              low_8_aligned);
        }
      }
    }
    uint32_t current_guestaddr = low_8_aligned;
    // Second pass: detect branch with link instructions and decode the target
    // address. We can safely assume that if bl is to address, that address is
    // the start of the function
    for (uint32_t* second_pass = range_start; second_pass < range_end;
         second_pass++, current_guestaddr += 4) {
      uint32_t current_call = xe::byte_swap(*second_pass);

      if (IsOpcodeBL(current_call)) {
        uint32_t called_function = GetBLCalledFunction(
            this, current_guestaddr, ppc::PPCOpcodeBits{current_call});
        // must be 8 byte aligned and in range
        if ((called_function & (8 - 1)) == 0 &&
            called_function >= low_address_ &&
            called_function < high_address_) {
          add_new_func(called_function);
        }
      }
    }

    // Third pass: functions whose address is formed with lis + addi/ori, such
    // as callbacks stored into a table. A leaf that is never the target of a bl
    // has no .pdata entry and can follow the previous blr without padding, so
    // none of the passes above find it and it compiles on first call.
    auto in_code_section = [this](uint32_t address) {
      for (auto& section : pe_sections_) {
        if ((section.flags & kXEPESectionContainsCode) &&
            address >= section.address &&
            address - section.address < section.size) {
          return true;
        }
      }
      return false;
    };
    auto read = [this](uint32_t address) -> uint32_t {
      return *memory()->TranslateVirtualBE<uint32_t>(address);
    };
    auto is_address_taken_function = [&](uint32_t address) {
      if ((address & 3) || !in_code_section(address) ||
          !in_code_section(address - 4)) {
        return false;
      }
      uint32_t first = read(address);
      // A table of code addresses (a jump table) rather than code.
      if (!first || in_code_section(first)) {
        return false;
      }
      uint32_t previous = read(address - 4);
      bool previous_is_b =
          (previous >> 26) == 18 && !ppc::PPCOpcodeBits{previous}.I.LK;
      return !previous || previous == 0x4E800020 || previous_is_b;
    };
    // How far an addi/ori may follow the lis its value started from.
    constexpr uint32_t kConstantWindowBytes = 64;
    for (auto& section : pe_sections_) {
      if (!(section.flags & kXEPESectionContainsCode)) {
        continue;
      }
      std::array<uint32_t, 32> registers = {};
      std::array<uint32_t, 32> lis_address = {};
      auto clear = [&](uint32_t reg) { lis_address[reg] = 0; };
      uint32_t section_end = section.address + (section.size & ~3u);
      for (uint32_t address = section.address; address < section_end;
           address += 4) {
        uint32_t code = read(address);
        ppc::PPCOpcodeBits bits{code};
        uint32_t opcode = code >> 26;
        if (opcode == 15 && !bits.D.RA) {
          registers[bits.D.RT] = static_cast<uint32_t>(bits.D.DS) << 16;
          lis_address[bits.D.RT] = address;
          continue;
        }
        uint32_t source = 32, dest = 32, value = 0;
        if (opcode == 14 && bits.D.RA) {
          source = bits.D.RA;
          dest = bits.D.RT;
          value = registers[source] +
                  static_cast<uint32_t>(ppc::XEEXTS16(bits.D.DS));
        } else if (opcode == 24) {
          source = bits.D.RT;
          dest = bits.D.RA;
          value = registers[source] | bits.D.DS;
        }
        if (source != 32 && lis_address[source] &&
            address - lis_address[source] <= kConstantWindowBytes) {
          registers[dest] = value;
          lis_address[dest] = lis_address[source];
          if (is_address_taken_function(value)) {
            add_new_func(value);
          }
          continue;
        }
        // Anything else that may write a register ends its constant. Clearing
        // too much only loses a candidate; a stale high half invents one.
        bool is_branch = opcode == 16 || opcode == 18 || opcode == 19;
        if (is_branch && (code & 1)) {
          // A call clobbers the volatile registers.
          clear(0);
          for (uint32_t reg = 3; reg <= 12; ++reg) {
            clear(reg);
          }
        } else if (!is_branch && opcode != 10 && opcode != 11) {
          // Update forms and logical ops write RA, most others RT.
          clear(bits.D.RT);
          clear(bits.D.RA);
        }
      }
    }

    auto pdata = this->GetPESection(".pdata");

    if (pdata) {
      uint32_t* pdata_base =
          (uint32_t*)this->memory()->TranslateVirtual(pdata->address);

      uint32_t n_pdata_entries = pdata->raw_size / 8;

      for (uint32_t i = 0; i < n_pdata_entries; ++i) {
        uint32_t funcaddr = xe::load_and_swap<uint32_t>(&pdata_base[i * 2]);
        if (funcaddr >= low_address_ && funcaddr <= highest_exec_addr) {
          add_new_func(funcaddr);
        } else {
          // we hit 0 for func addr, that means we're done
          break;
        }
      }
    }
  }

  // Sort the list of function starts and then ensure that all addresses are
  // unique
  std::sort(funcstarts.begin(), funcstarts.end());
  funcstarts.erase(std::unique(funcstarts.begin(), funcstarts.end()),
                   funcstarts.end());
  return funcstarts;
}
bool XexModule::FindSaveRest() {
  // Special stack save/restore functions.
  // http://research.microsoft.com/en-us/um/redmond/projects/invisible/src/crt/md/ppc/xxx.s.htm
  // It'd be nice to stash these away and mark them as such to allow for
  // special codegen.
  // __savegprlr_14 to __savegprlr_31
  // __restgprlr_14 to __restgprlr_31
  static constexpr uint32_t gprlr_code_values[] = {
      0x68FFC1F9,  // __savegprlr_14
      0x70FFE1F9,  // __savegprlr_15
      0x78FF01FA,  // __savegprlr_16
      0x80FF21FA,  // __savegprlr_17
      0x88FF41FA,  // __savegprlr_18
      0x90FF61FA,  // __savegprlr_19
      0x98FF81FA,  // __savegprlr_20
      0xA0FFA1FA,  // __savegprlr_21
      0xA8FFC1FA,  // __savegprlr_22
      0xB0FFE1FA,  // __savegprlr_23
      0xB8FF01FB,  // __savegprlr_24
      0xC0FF21FB,  // __savegprlr_25
      0xC8FF41FB,  // __savegprlr_26
      0xD0FF61FB,  // __savegprlr_27
      0xD8FF81FB,  // __savegprlr_28
      0xE0FFA1FB,  // __savegprlr_29
      0xE8FFC1FB,  // __savegprlr_30
      0xF0FFE1FB,  // __savegprlr_31
      0xF8FF8191, 0x2000804E,
      0x68FFC1E9,  // __restgprlr_14
      0x70FFE1E9,  // __restgprlr_15
      0x78FF01EA,  // __restgprlr_16
      0x80FF21EA,  // __restgprlr_17
      0x88FF41EA,  // __restgprlr_18
      0x90FF61EA,  // __restgprlr_19
      0x98FF81EA,  // __restgprlr_20
      0xA0FFA1EA,  // __restgprlr_21
      0xA8FFC1EA,  // __restgprlr_22
      0xB0FFE1EA,  // __restgprlr_23
      0xB8FF01EB,  // __restgprlr_24
      0xC0FF21EB,  // __restgprlr_25
      0xC8FF41EB,  // __restgprlr_26
      0xD0FF61EB,  // __restgprlr_27
      0xD8FF81EB,  // __restgprlr_28
      0xE0FFA1EB,  // __restgprlr_29
      0xE8FFC1EB,  // __restgprlr_30
      0xF0FFE1EB,  // __restgprlr_31
      0xF8FF8181, 0xA603887D, 0x2000804E,
  };
  // __savefpr_14 to __savefpr_31
  // __restfpr_14 to __restfpr_31
  static constexpr uint32_t fpr_code_values[] = {
      0x70FFCCD9,  // __savefpr_14
      0x78FFECD9,  // __savefpr_15
      0x80FF0CDA,  // __savefpr_16
      0x88FF2CDA,  // __savefpr_17
      0x90FF4CDA,  // __savefpr_18
      0x98FF6CDA,  // __savefpr_19
      0xA0FF8CDA,  // __savefpr_20
      0xA8FFACDA,  // __savefpr_21
      0xB0FFCCDA,  // __savefpr_22
      0xB8FFECDA,  // __savefpr_23
      0xC0FF0CDB,  // __savefpr_24
      0xC8FF2CDB,  // __savefpr_25
      0xD0FF4CDB,  // __savefpr_26
      0xD8FF6CDB,  // __savefpr_27
      0xE0FF8CDB,  // __savefpr_28
      0xE8FFACDB,  // __savefpr_29
      0xF0FFCCDB,  // __savefpr_30
      0xF8FFECDB,  // __savefpr_31
      0x2000804E,
      0x70FFCCC9,  // __restfpr_14
      0x78FFECC9,  // __restfpr_15
      0x80FF0CCA,  // __restfpr_16
      0x88FF2CCA,  // __restfpr_17
      0x90FF4CCA,  // __restfpr_18
      0x98FF6CCA,  // __restfpr_19
      0xA0FF8CCA,  // __restfpr_20
      0xA8FFACCA,  // __restfpr_21
      0xB0FFCCCA,  // __restfpr_22
      0xB8FFECCA,  // __restfpr_23
      0xC0FF0CCB,  // __restfpr_24
      0xC8FF2CCB,  // __restfpr_25
      0xD0FF4CCB,  // __restfpr_26
      0xD8FF6CCB,  // __restfpr_27
      0xE0FF8CCB,  // __restfpr_28
      0xE8FFACCB,  // __restfpr_29
      0xF0FFCCCB,  // __restfpr_30
      0xF8FFECCB,  // __restfpr_31
      0x2000804E,
  };
  // __savevmx_14 to __savevmx_31
  // __savevmx_64 to __savevmx_127
  // __restvmx_14 to __restvmx_31
  // __restvmx_64 to __restvmx_127
  static constexpr uint32_t vmx_code_values[] = {
      0xE0FE6039,  // __savevmx_14
      0xCE61CB7D, 0xF0FE6039, 0xCE61EB7D, 0x00FF6039, 0xCE610B7E, 0x10FF6039,
      0xCE612B7E, 0x20FF6039, 0xCE614B7E, 0x30FF6039, 0xCE616B7E, 0x40FF6039,
      0xCE618B7E, 0x50FF6039, 0xCE61AB7E, 0x60FF6039, 0xCE61CB7E, 0x70FF6039,
      0xCE61EB7E, 0x80FF6039, 0xCE610B7F, 0x90FF6039, 0xCE612B7F, 0xA0FF6039,
      0xCE614B7F, 0xB0FF6039, 0xCE616B7F, 0xC0FF6039, 0xCE618B7F, 0xD0FF6039,
      0xCE61AB7F, 0xE0FF6039, 0xCE61CB7F, 0xF0FF6039,  // __savevmx_31
      0xCE61EB7F, 0x2000804E,

      0x00FC6039,  // __savevmx_64
      0xCB610B10, 0x10FC6039, 0xCB612B10, 0x20FC6039, 0xCB614B10, 0x30FC6039,
      0xCB616B10, 0x40FC6039, 0xCB618B10, 0x50FC6039, 0xCB61AB10, 0x60FC6039,
      0xCB61CB10, 0x70FC6039, 0xCB61EB10, 0x80FC6039, 0xCB610B11, 0x90FC6039,
      0xCB612B11, 0xA0FC6039, 0xCB614B11, 0xB0FC6039, 0xCB616B11, 0xC0FC6039,
      0xCB618B11, 0xD0FC6039, 0xCB61AB11, 0xE0FC6039, 0xCB61CB11, 0xF0FC6039,
      0xCB61EB11, 0x00FD6039, 0xCB610B12, 0x10FD6039, 0xCB612B12, 0x20FD6039,
      0xCB614B12, 0x30FD6039, 0xCB616B12, 0x40FD6039, 0xCB618B12, 0x50FD6039,
      0xCB61AB12, 0x60FD6039, 0xCB61CB12, 0x70FD6039, 0xCB61EB12, 0x80FD6039,
      0xCB610B13, 0x90FD6039, 0xCB612B13, 0xA0FD6039, 0xCB614B13, 0xB0FD6039,
      0xCB616B13, 0xC0FD6039, 0xCB618B13, 0xD0FD6039, 0xCB61AB13, 0xE0FD6039,
      0xCB61CB13, 0xF0FD6039, 0xCB61EB13, 0x00FE6039, 0xCF610B10, 0x10FE6039,
      0xCF612B10, 0x20FE6039, 0xCF614B10, 0x30FE6039, 0xCF616B10, 0x40FE6039,
      0xCF618B10, 0x50FE6039, 0xCF61AB10, 0x60FE6039, 0xCF61CB10, 0x70FE6039,
      0xCF61EB10, 0x80FE6039, 0xCF610B11, 0x90FE6039, 0xCF612B11, 0xA0FE6039,
      0xCF614B11, 0xB0FE6039, 0xCF616B11, 0xC0FE6039, 0xCF618B11, 0xD0FE6039,
      0xCF61AB11, 0xE0FE6039, 0xCF61CB11, 0xF0FE6039, 0xCF61EB11, 0x00FF6039,
      0xCF610B12, 0x10FF6039, 0xCF612B12, 0x20FF6039, 0xCF614B12, 0x30FF6039,
      0xCF616B12, 0x40FF6039, 0xCF618B12, 0x50FF6039, 0xCF61AB12, 0x60FF6039,
      0xCF61CB12, 0x70FF6039, 0xCF61EB12, 0x80FF6039, 0xCF610B13, 0x90FF6039,
      0xCF612B13, 0xA0FF6039, 0xCF614B13, 0xB0FF6039, 0xCF616B13, 0xC0FF6039,
      0xCF618B13, 0xD0FF6039, 0xCF61AB13, 0xE0FF6039, 0xCF61CB13,
      0xF0FF6039,  // __savevmx_127
      0xCF61EB13, 0x2000804E,

      0xE0FE6039,  // __restvmx_14
      0xCE60CB7D, 0xF0FE6039, 0xCE60EB7D, 0x00FF6039, 0xCE600B7E, 0x10FF6039,
      0xCE602B7E, 0x20FF6039, 0xCE604B7E, 0x30FF6039, 0xCE606B7E, 0x40FF6039,
      0xCE608B7E, 0x50FF6039, 0xCE60AB7E, 0x60FF6039, 0xCE60CB7E, 0x70FF6039,
      0xCE60EB7E, 0x80FF6039, 0xCE600B7F, 0x90FF6039, 0xCE602B7F, 0xA0FF6039,
      0xCE604B7F, 0xB0FF6039, 0xCE606B7F, 0xC0FF6039, 0xCE608B7F, 0xD0FF6039,
      0xCE60AB7F, 0xE0FF6039, 0xCE60CB7F, 0xF0FF6039,  // __restvmx_31
      0xCE60EB7F, 0x2000804E,

      0x00FC6039,  // __restvmx_64
      0xCB600B10, 0x10FC6039, 0xCB602B10, 0x20FC6039, 0xCB604B10, 0x30FC6039,
      0xCB606B10, 0x40FC6039, 0xCB608B10, 0x50FC6039, 0xCB60AB10, 0x60FC6039,
      0xCB60CB10, 0x70FC6039, 0xCB60EB10, 0x80FC6039, 0xCB600B11, 0x90FC6039,
      0xCB602B11, 0xA0FC6039, 0xCB604B11, 0xB0FC6039, 0xCB606B11, 0xC0FC6039,
      0xCB608B11, 0xD0FC6039, 0xCB60AB11, 0xE0FC6039, 0xCB60CB11, 0xF0FC6039,
      0xCB60EB11, 0x00FD6039, 0xCB600B12, 0x10FD6039, 0xCB602B12, 0x20FD6039,
      0xCB604B12, 0x30FD6039, 0xCB606B12, 0x40FD6039, 0xCB608B12, 0x50FD6039,
      0xCB60AB12, 0x60FD6039, 0xCB60CB12, 0x70FD6039, 0xCB60EB12, 0x80FD6039,
      0xCB600B13, 0x90FD6039, 0xCB602B13, 0xA0FD6039, 0xCB604B13, 0xB0FD6039,
      0xCB606B13, 0xC0FD6039, 0xCB608B13, 0xD0FD6039, 0xCB60AB13, 0xE0FD6039,
      0xCB60CB13, 0xF0FD6039, 0xCB60EB13, 0x00FE6039, 0xCF600B10, 0x10FE6039,
      0xCF602B10, 0x20FE6039, 0xCF604B10, 0x30FE6039, 0xCF606B10, 0x40FE6039,
      0xCF608B10, 0x50FE6039, 0xCF60AB10, 0x60FE6039, 0xCF60CB10, 0x70FE6039,
      0xCF60EB10, 0x80FE6039, 0xCF600B11, 0x90FE6039, 0xCF602B11, 0xA0FE6039,
      0xCF604B11, 0xB0FE6039, 0xCF606B11, 0xC0FE6039, 0xCF608B11, 0xD0FE6039,
      0xCF60AB11, 0xE0FE6039, 0xCF60CB11, 0xF0FE6039, 0xCF60EB11, 0x00FF6039,
      0xCF600B12, 0x10FF6039, 0xCF602B12, 0x20FF6039, 0xCF604B12, 0x30FF6039,
      0xCF606B12, 0x40FF6039, 0xCF608B12, 0x50FF6039, 0xCF60AB12, 0x60FF6039,
      0xCF60CB12, 0x70FF6039, 0xCF60EB12, 0x80FF6039, 0xCF600B13, 0x90FF6039,
      0xCF602B13, 0xA0FF6039, 0xCF604B13, 0xB0FF6039, 0xCF606B13, 0xC0FF6039,
      0xCF608B13, 0xD0FF6039, 0xCF60AB13, 0xE0FF6039, 0xCF60CB13,
      0xF0FF6039,  // __restvmx_127
      0xCF60EB13, 0x2000804E,
  };

  // TODO(benvanik): these are almost always sequential, if present.
  //     It'd be smarter to search around the other ones to prevent
  //     3 full module scans.
  uint32_t gplr_start = 0;
  uint32_t fpr_start = 0;
  uint32_t vmx_start = 0;

  auto page_size = base_address_ <= 0x90000000 ? 64 * 1024 : 4 * 1024;
  auto sec_header = xex_security_info();
  std::vector<uint32_t> resolve_on_exit{};
  resolve_on_exit.reserve(256);
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    const auto start_address = base_address_ + (page * page_size);
    const auto end_address = start_address + (desc.page_count * page_size);

    if (desc.info == XEX_SECTION_CODE) {
      if (!gplr_start) {
        gplr_start = memory_->SearchAligned(start_address, end_address,
                                            gprlr_code_values,
                                            xe::countof(gprlr_code_values));
      }
      if (!fpr_start) {
        fpr_start =
            memory_->SearchAligned(start_address, end_address, fpr_code_values,
                                   xe::countof(fpr_code_values));
      }
      if (!vmx_start) {
        vmx_start =
            memory_->SearchAligned(start_address, end_address, vmx_code_values,
                                   xe::countof(vmx_code_values));
      }
      if (gplr_start && fpr_start && vmx_start) {
        break;
      }
    }

    page += desc.page_count;
  }

  // Add function stubs.
  char name[32];

  auto AddXexFunction = [this, &resolve_on_exit](uint32_t address,
                                                 Function** function) {
    DeclareFunction(address, function);
    resolve_on_exit.push_back(address);
  };
  if (gplr_start) {
    uint32_t address = gplr_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savegprlr_{}", n);
      Function* function;

      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 2 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveGprLr;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::GPR, false, n);

      address += 4;
    }
    address = gplr_start + 20 * 4;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restgprlr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 3 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestGprLr;
      function->set_behavior(Function::Behavior::kEpilogReturn);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::GPR, true, n);
      address += 4;
    }
  }
  if (fpr_start) {
    uint32_t address = fpr_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savefpr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 1 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveFpr;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);

      function->SetSaverest(cpu::SaveRestoreType::FPR, false, n);
      address += 4;
    }
    address = fpr_start + (18 * 4) + (1 * 4);
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restfpr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 1 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestFpr;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::FPR, true, n);
      address += 4;
    }
  }
  if (vmx_start) {
    // vmx is:
    // 14-31 save
    // 64-127 save
    // 14-31 rest
    // 64-127 rest
    uint32_t address = vmx_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savevmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveVmx;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, false, n);
      address += 2 * 4;
    }
    address += 4;
    for (int n = 64; n <= 127; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savevmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveVmx;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, false, n);
      address += 2 * 4;
    }
    address = vmx_start + (18 * 2 * 4) + (1 * 4) + (64 * 2 * 4) + (1 * 4);
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restvmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestVmx;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, true, n);
      address += 2 * 4;
    }
    address += 4;
    for (int n = 64; n <= 127; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restvmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestVmx;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, true, n);
      address += 2 * 4;
    }
  }
  if (cvars::enable_early_precompilation) {
    for (auto&& to_ensure_precompiled : resolve_on_exit) {
      // we want to make sure an address for these functions is available before
      // any other functions are compiled for code generation purposes but we do
      // it outside of our loops, because we also want to make sure we've marked
      // up the symbol with info about it being save/rest and whatnot
      processor_->ResolveFunction(to_ensure_precompiled);
    }
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
