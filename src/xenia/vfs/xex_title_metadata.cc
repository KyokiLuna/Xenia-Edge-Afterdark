/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/vfs/xex_metadata.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/xex_image.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/kernel/util/xex2_util.h"

namespace xe::vfs {
static std::optional<std::vector<uint8_t>> ExtractResource(
    std::span<const uint8_t> xex, std::string_view section,
    bool (*validate)(std::span<const uint8_t>, std::span<const uint8_t>),
    XexResourceStatus* status = nullptr) {
  if (status) {
    *status = XexResourceStatus::kInvalid;
  }
  if (section.empty() || section.size() > 8 ||
      section.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  // Check input before parsing block tables or allocating decode buffers. A
  // tiny image with huge trailing input must not size normal-CBC/LZX buffers.
  if (xex.size() > kMaxOfflineXexFileBytes) {
    if (status) {
      *status = XexResourceStatus::kTooLarge;
    }
    return std::nullopt;
  }
  // This validates the complete optional-header table before the accessor.
  const auto metadata = ExtractXexMetadata(xex.data(), xex.size(), false);
  if (!metadata) {
    return std::nullopt;
  }
  const auto* header = reinterpret_cast<const xex2_header*>(xex.data());
  if (uint32_t(header->module_flags) &
      (XEX_MODULE_MODULE_PATCH | XEX_MODULE_PATCH_FULL |
       XEX_MODULE_PATCH_DELTA)) {
    return std::nullopt;
  }
  xex2_opt_resource_info* resources = nullptr;
  xex2_opt_file_format_info* file_info = nullptr;
  if (!GetXexOptHeader(header, XEX_HEADER_RESOURCE_INFO, &resources)) {
    if (status) {
      *status = XexResourceStatus::kNotFound;
    }
    return std::nullopt;
  }
  if (!GetXexOptHeader(header, XEX_HEADER_FILE_FORMAT_INFO, &file_info) ||
      resources->size < 4 || (resources->size - 4) % sizeof(xex2_resource) ||
      file_info->info_size < 8) {
    return std::nullopt;
  }
  std::array<char, 8> name{};
  std::copy(section.begin(), section.end(), name.begin());
  const xex2_resource* resource = nullptr;
  for (uint32_t i = 0; i < (resources->size - 4) / sizeof(xex2_resource); ++i) {
    if (!std::memcmp(resources->resources[i].name, name.data(), 8)) {
      resource = &resources->resources[i];
      break;
    }
  }
  if (!resource) {
    if (status) {
      *status = XexResourceStatus::kNotFound;
    }
    return std::nullopt;
  }
  const size_t security_offset = header->security_offset;
  const size_t security_min =
      metadata->format == XexFormat::kXex2
          ? offsetof(xex2_security_info, page_descriptors)
          : offsetof(xex1_security_info, page_descriptors);
  if (security_offset > header->header_size ||
      security_min > header->header_size - security_offset) {
    return std::nullopt;
  }
  uint32_t image_size, base_address;
  const uint8_t* encrypted_key;
  if (metadata->format == XexFormat::kXex2) {
    const auto* security = reinterpret_cast<const xex2_security_info*>(
        xex.data() + security_offset);
    image_size = security->image_size;
    base_address = security->load_address;
    encrypted_key = reinterpret_cast<const uint8_t*>(security->aes_key);
  } else {
    const auto* security = reinterpret_cast<const xex1_security_info*>(
        xex.data() + security_offset);
    image_size = security->image_size;
    base_address = security->load_address;
    encrypted_key = reinterpret_cast<const uint8_t*>(security->aes_key);
  }
  xe::be<uint32_t>* base = nullptr;
  if (GetXexOptHeader(header, XEX_HEADER_IMAGE_BASE_ADDRESS, &base)) {
    base_address = *base;
  }

  XexImageOptions options;
  options.compression = static_cast<XexImageCompression>(
      static_cast<uint16_t>(file_info->compression_type));
  options.encryption = static_cast<XexImageEncryption>(
      static_cast<uint16_t>(file_info->encryption_type));
  std::vector<XexImageBasicBlock> blocks;
  const auto payload = xex.subspan(header->header_size);
  uint64_t decoded_size = image_size;
  if (options.compression == XexImageCompression::kBasic) {
    if ((file_info->info_size - 8) % 8) {
      return std::nullopt;
    }
    uint64_t block_size = 0;
    for (uint32_t i = 0; i < (file_info->info_size - 8) / 8; ++i) {
      const auto& block = file_info->compression_info.basic.blocks[i];
      block_size += uint64_t(block.data_size) + block.zero_size;
      if (block_size > kMaxOfflineXexImageBytes) {
        if (status) {
          *status = XexResourceStatus::kTooLarge;
        }
        return std::nullopt;
      }
      blocks.push_back({block.data_size, block.zero_size});
    }
    decoded_size = std::max(decoded_size, block_size);
    options.basic_blocks = blocks;
  } else if (options.compression == XexImageCompression::kNormal) {
    if (file_info->info_size < 36) {
      return std::nullopt;
    }
    options.normal_window_size = file_info->compression_info.normal.window_size;
    const auto& first = file_info->compression_info.normal.first_block;
    options.normal_first_block_size = first.block_size;
    std::copy_n(first.block_hash, 20, options.normal_first_block_hash.begin());
  } else if (options.compression == XexImageCompression::kNone) {
    decoded_size = std::max<uint64_t>(decoded_size, payload.size());
  } else {
    return std::nullopt;
  }
  // Offline scanning has a bounded, temporary host image, never a guest map.
  if (decoded_size > kMaxOfflineXexImageBytes) {
    if (status) {
      *status = XexResourceStatus::kTooLarge;
    }
    return std::nullopt;
  }
  if (!decoded_size || resource->address < base_address ||
      uint64_t(resource->address) - base_address > decoded_size ||
      resource->size >
          decoded_size - (uint64_t(resource->address) - base_address)) {
    return std::nullopt;
  }
  std::vector<uint8_t> image(static_cast<size_t>(decoded_size));
  // Match the runtime's candidate order for both executable formats.
  for (int candidate = 0; candidate < 4; ++candidate) {
    if (!DecryptXexImageSessionKey(
            std::span<const uint8_t, 16>(encrypted_key, 16),
            std::span<const uint8_t, 16>(
                XexImageMasterKey(candidate >= 2, candidate & 1), 16),
            options.session_key)) {
      continue;
    }
    if (DecodeXexImage(payload, image, options) == XexImageResult::kSuccess) {
      const size_t offset = uint32_t(resource->address) - base_address;
      const auto bytes =
          std::span<const uint8_t>(image).subspan(offset, resource->size);
      // Validation belongs to the resource consumer; a no-compression wrong
      // key can report codec success. Retry the next real key on rejection.
      if (validate(image, bytes)) {
        if (status) {
          *status = XexResourceStatus::kLoaded;
        }
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
      }
    }
    if (options.encryption == XexImageEncryption::kNone) {
      break;
    }
  }
  return std::nullopt;
}
XexResourceResult ExtractXexResource(std::span<const uint8_t> xex,
                                     std::string_view section) {
  XexResourceResult result;
  auto bytes = ExtractResource(
      xex, section,
      [](auto image, auto) {
        if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') {
          return false;
        }
        const uint32_t pe =
            uint32_t(image[0x3C]) | (uint32_t(image[0x3D]) << 8) |
            (uint32_t(image[0x3E]) << 16) | (uint32_t(image[0x3F]) << 24);
        return pe <= image.size() && image.size() - pe >= 4 &&
               std::memcmp(image.data() + pe, "PE\0\0", 4) == 0;
      },
      &result.status);
  if (bytes) {
    result.bytes = std::move(*bytes);
  }
  return result;
}
std::optional<TitleResourceMetadata> ExtractXexTitleResources(
    std::span<const uint8_t> xex) {
  const auto metadata = ExtractXexMetadata(xex.data(), xex.size(), false);
  if (!metadata) {
    return std::nullopt;
  }
  auto resource = ExtractResource(
      xex, fmt::format("{:08X}", metadata->title_id), [](auto, auto bytes) {
        return ExtractTitleResourceMetadata(bytes).has_value();
      });
  return resource ? ExtractTitleResourceMetadata(*resource) : std::nullopt;
}
}  // namespace xe::vfs
