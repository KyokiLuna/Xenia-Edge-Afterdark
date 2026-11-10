/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/tile_request.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/base/byte_order.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "third_party/stb/stb_image.h"
#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xui_resource.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/module_resource.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::kernel::xam {
namespace {
constexpr X_HRESULT kNotFound = X_HRESULT_FROM_WIN32(X_ERROR_FILE_NOT_FOUND);
constexpr X_HRESULT kNotReady = X_HRESULT_FROM_WIN32(X_ERROR_NOT_READY);
constexpr X_HRESULT kNoMemory = 0x8007000E;
constexpr uint32_t kDashboardTitle = 0xFFFE07D1;
constexpr char kPrivateDevice[] = "\\Device\\OwnedTileFlash";
constexpr std::array<uint8_t, 8> kPngSignature = {137, 80, 78, 71,
                                                  13,  10, 26, 10};

uint32_t PngWord(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 |
         uint32_t(at[3]);
}
// Reuse the existing image decoder, but do not let advertised PNG chunk sizes
// or a deflate bomb allocate beyond the admitted image. This only validates
// chunk extents and the compressed stream; stb still owns PNG pixel decoding.
X_HRESULT CheckPngCompression(std::span<const uint8_t> png, uint32_t width,
                              uint32_t height) {
  std::array<uint8_t, TileService::kTextureEncodedLimit> compressed;
  size_t compressed_size = 0;
  bool ended = false;
  for (size_t at = 8; at <= png.size() && png.size() - at >= 12;) {
    const uint32_t length = PngWord(png.data() + at);
    const uint32_t type = PngWord(png.data() + at + 4);
    if (length > png.size() - at - 12 ||
        (at == 8 && (type != 0x49484452 || length != 13))) {
      return X_E_FAIL;
    }
    if (type == 0x49444154) {  // IDAT
      if (length > compressed.size() - compressed_size) {
        return X_E_FAIL;
      }
      std::copy_n(png.data() + at + 8, length,
                  compressed.data() + compressed_size);
      compressed_size += length;
    } else if (type == 0x49454E44) {  // IEND
      if (length) {
        return X_E_FAIL;
      }
      ended = true;
      break;
    }
    at += size_t(length) + 12;
  }
  if (!ended || !compressed_size) {
    return X_E_FAIL;
  }
  zng_stream stream{};
  const auto initialized = zng_inflateInit(&stream);
  if (initialized != Z_OK) {
    return initialized == Z_MEM_ERROR ? kNoMemory : X_E_FAIL;
  }
  struct EndStream {
    zng_stream* stream;
    ~EndStream() { zng_inflateEnd(stream); }
  } end_stream{&stream};
  stream.next_in = compressed.data();
  stream.avail_in = uint32_t(compressed_size);
  // At most 8 bytes/pixel for 16-bit RGBA plus a conservative seven Adam7
  // filter/packing rows per source row. Bounded by the admitted dimensions.
  const uint64_t expanded_limit =
      uint64_t(width) * height * 8 + uint64_t(height) * 7 + 8;
  std::array<uint8_t, 4096> scratch;
  while (true) {
    const auto input_before = stream.avail_in;
    stream.next_out = scratch.data();
    stream.avail_out = uint32_t(scratch.size());
    const auto status = zng_inflate(&stream, Z_NO_FLUSH);
    if (stream.total_out > expanded_limit) {
      return kNoMemory;
    }
    if (status == Z_STREAM_END) {
      return X_E_SUCCESS;
    }
    if (status != Z_OK) {
      return status == Z_MEM_ERROR ? kNoMemory : X_E_FAIL;
    }
    if (input_before == stream.avail_in && stream.avail_out == scratch.size()) {
      return X_E_FAIL;
    }
  }
}

X_HRESULT ResourceError(X_STATUS status) {
  switch (status) {
    case X_STATUS_SUCCESS:
      return X_E_SUCCESS;
    case X_STATUS_NOT_FOUND:
    case X_STATUS_NO_SUCH_FILE:
    case X_STATUS_OBJECT_NAME_NOT_FOUND:
    case X_STATUS_OBJECT_PATH_NOT_FOUND:
      return kNotFound;
    case X_STATUS_ACCESS_DENIED:
      return X_E_ACCESS_DENIED;
    case X_STATUS_BUFFER_TOO_SMALL:
      return X_E_INSUFFICIENT_BUFFER;
    case X_STATUS_NO_MEMORY:
    case X_STATUS_INSUFFICIENT_RESOURCES:
      return kNoMemory;
    case X_STATUS_NOT_SUPPORTED:
      return X_E_NOT_IMPLEMENTED;
    default:
      return X_E_FAIL;
  }
}
std::string StockName(TileService::StockPicture picture) {
  char name[64];
  std::snprintf(name, sizeof(name), "%u_fffe07d10002%04x0001%04x.png",
                picture.small ? 32u : 64u, picture.index, picture.index);
  return name;
}
}  // namespace

std::shared_ptr<const TileService::StockSource>
TileService::StockProvider::Capture(X_HRESULT& status) const {
  status = kNotReady;
  if (global_critical_region::is_held_by_current_thread()) {
    status = X_E_INVALIDARG;
    return {};
  }
  if (!owner_) {
    return {};
  }
  const auto selected = owner_->snapshot();
  // Observe may wait on a provider. No cache lock or caller GCRC spans it.
  if (!selected || !selected->available()) {
    return {};
  }
  {
    std::lock_guard lock(mutex_);
    if (cached_snapshot_ == selected && cached_source_) {
      status = X_E_SUCCESS;
      return cached_source_;
    }
  }
  auto captured = TileService::CaptureStockSource(selected, status);
  if (!captured) {
    return {};  // Fail this request; transient failures are never memoized.
  }
  std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> previous_snapshot;
  std::shared_ptr<const StockSource> previous_source;
  {
    std::lock_guard lock(mutex_);
    // An older in-flight capture cannot evict a newer cache. Pointer identity
    // above also prevents equal counters from identifying another snapshot.
    if (!cached_snapshot_ ||
        cached_snapshot_->generation() <= selected->generation()) {
      previous_snapshot = std::move(cached_snapshot_);
      previous_source = std::move(cached_source_);
      cached_snapshot_ = selected;
      cached_source_ = captured;
    }
  }
  // The request stays bound to selected even if an idle commit raced the IO.
  // ReadStock rechecks that exact source; it never rebinds the request.
  return captured;
}

std::shared_ptr<const TileService::StockSource> TileService::CaptureStockSource(
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> source,
    X_HRESULT& status) {
  status = kNotReady;
  if (global_critical_region::is_held_by_current_thread()) {
    status = X_E_INVALIDARG;
    return {};
  }
  if (!source || !source->generation() || !source->available()) {
    return {};
  }
  std::error_code error;
  auto flash = source->PartitionRoot(
      vfs::InstalledSystemSource::Partition::kFlash, error);
  if (!flash) {
    return {};
  }
  if (!flash->read_only()) {
    status = X_E_ACCESS_DENIED;
    return {};
  }
  try {
    // The supplied capability, not this spelling, is the open authority.
    // Initialize derives its own independent GuestView before the VFS owns it.
    vfs::VirtualFileSystem fs;
    auto device = std::make_unique<vfs::HostPathDevice>(
        kPrivateDevice, flash->path(), true, false, "FATX", flash);
    if (!device->Initialize() || !fs.RegisterDevice(std::move(device))) {
      status = X_E_ACCESS_DENIED;
      return {};
    }
    std::vector<uint8_t> package;
    const auto read = vfs::ReadModuleResource(
        fs, "\\Device\\OwnedTileFlash\\xam.xex", "shrdres", {}, package);
    if (XFAILED(read)) {
      status = ResourceError(read);
      return {};
    }
    const auto probe =
        resource::LookupXuiPackageFile(package, StockName({0, true}));
    if (probe.status == resource::XuiPackageStatus::kInvalid ||
        probe.status == resource::XuiPackageStatus::kUnsupported) {
      status = probe.status == resource::XuiPackageStatus::kUnsupported
                   ? X_E_NOT_IMPLEMENTED
                   : X_E_FAIL;
      return {};
    }
    // Do not publish a new capture after its selected generation retired or
    // its authority was externally replaced while the resource was read.
    if (!source->available() || !flash->Observe().available) {
      return {};
    }
    auto captured = std::shared_ptr<StockSource>(new StockSource);
    captured->source_ = std::move(source);
    captured->flash_ = std::move(flash);
    captured->package_ = std::move(package);
    status = X_E_SUCCESS;
    return captured;
  } catch (const std::bad_alloc&) {
    status = kNoMemory;
    return {};
  }
}

std::optional<TileService::StockPicture> TileService::SelectStock(
    uint32_t type, uint32_t title_id, uint64_t item_id) {
  if (title_id != kDashboardTitle) {
    return {};
  }
  uint32_t id;
  bool small;
  switch (type) {
    case 7:
      id = uint32_t(item_id >> 32);
      small = false;
      break;
    case 8:
      id = uint32_t(item_id);
      small = true;
      break;
    case 9:
      id = uint32_t(item_id);
      small = !(id & 0x20000);
      break;
    default:
      return {};
  }
  // Keep out-of-range stock keys classified as stock. ReadStock returns the
  // actual missing-image status without manufacturing a profile dependency.
  return StockPicture{id & 0xFFFF, small};
}

TileService::OwnedPng TileService::ReadStock(const StockSource& source,
                                             StockPicture picture) {
  OwnedPng result;
  result.source_generation = source.generation();
  if (global_critical_region::is_held_by_current_thread()) {
    result.status = X_E_INVALIDARG;
    return result;
  }
  if (!source.source_->available() || !source.flash_->Observe().available) {
    result.status = kNotReady;
    return result;
  }
  if (picture.index >= kStockCount) {
    result.status = kNotFound;
    return result;
  }
  try {
    const auto found =
        resource::LookupXuiPackageFile(source.package_, StockName(picture));
    if (found.status != resource::XuiPackageStatus::kFound) {
      result.status = found.status == resource::XuiPackageStatus::kNotFound
                          ? kNotFound
                          : X_E_FAIL;
      return result;
    }
    if (found.bytes.empty()) {
      result.status = kNotFound;
      return result;
    }
    result.bytes.assign(found.bytes.begin(), found.bytes.end());
    result.status = X_E_SUCCESS;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = kNoMemory;
    return result;
  }
}

TileService::OwnedPng TileService::ReadStock(const StockProvider& provider,
                                             StockPicture picture) {
  X_HRESULT status;
  auto captured = provider.Capture(status);
  return captured ? ReadStock(*captured, picture) : OwnedPng{status};
}

TileService::OwnedPng TileService::FromOwnedPng(std::vector<uint8_t> png) {
  const auto status = png.empty() ? kNotFound : X_E_SUCCESS;
  return {status, std::move(png), 0};
}
TileService::RawResult TileService::PrepareRaw(OwnedPng png,
                                               uint32_t capacity) {
  RawResult result;
  result.source_generation = png.source_generation;
  result.status = png.status;
  if (XFAILED(result.status)) {
    return result;
  }
  if (png.bytes.empty()) {
    result.status = kNotFound;
    return result;
  }
  if (png.bytes.size() > UINT32_MAX) {
    result.status = kNoMemory;
    return result;
  }
  result.required = uint32_t(png.bytes.size());
  if (result.required > capacity) {
    result.status = X_E_INSUFFICIENT_BUFFER;
    return result;
  }
  result.payload = std::move(png.bytes);
  result.status = X_E_SUCCESS;
  return result;
}

TileService::TextureResult TileService::PrepareTexture(
    OwnedPng png, uint32_t stride, uint32_t declared_height) {
  TextureResult result;
  result.source_generation = png.source_generation;
  result.status = png.status;
  if (XFAILED(result.status)) {
    return result;
  }
  if (png.bytes.empty()) {
    result.status = kNotFound;
    return result;
  }
  if (png.bytes.size() > kTextureEncodedLimit) {
    result.status = X_E_INSUFFICIENT_BUFFER;
    return result;
  }
  if (png.bytes.size() < kPngSignature.size() ||
      !std::equal(kPngSignature.begin(), kPngSignature.end(),
                  png.bytes.begin())) {
    result.status = X_E_FAIL;
    return result;
  }
  int width, height, channels;
  if (!stbi_info_from_memory(png.bytes.data(), int(png.bytes.size()), &width,
                             &height, &channels)) {
    result.status = X_E_FAIL;
    return result;
  }
  if (width <= 0 || height <= 0 || uint32_t(width) > kDimensionLimit ||
      uint32_t(height) > kDimensionLimit ||
      uint64_t(width) * uint64_t(height) * 4 > kDecodedLimit) {
    result.status = kNoMemory;
    return result;
  }
  const uint64_t row = uint64_t(width) * 4;
  const uint64_t capacity = uint64_t(stride) * declared_height;
  const uint64_t end = uint64_t(height - 1) * stride + row;
  if (!stride || !declared_height || stride < row || end > capacity ||
      end > UINT32_MAX || capacity > UINT32_MAX) {
    result.status = X_E_INVALIDARG;
    return result;
  }
  result.status =
      CheckPngCompression(png.bytes, uint32_t(width), uint32_t(height));
  if (XFAILED(result.status)) {
    return result;
  }
  int decoded_width, decoded_height;
  std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
      stbi_load_from_memory(png.bytes.data(), int(png.bytes.size()),
                            &decoded_width, &decoded_height, &channels,
                            STBI_rgb_alpha),
      stbi_image_free);
  if (!pixels || decoded_width != width || decoded_height != height) {
    result.status = X_E_FAIL;
    return result;
  }
  try {
    result.rgba.assign(pixels.get(), pixels.get() + size_t(width) * height * 4);
    result.width = uint32_t(width);
    result.height = uint32_t(height);
    result.row_bytes = uint32_t(row);
    result.destination_bytes = uint32_t(end);
    result.status = X_E_SUCCESS;
    return result;
  } catch (const std::bad_alloc&) {
    result.status = kNoMemory;
    return result;
  }
}

struct OwnedTileRequest::State {
  KernelState* kernel = nullptr;
  std::shared_ptr<const TileService::StockProvider> root_provider;
  TileRequestArguments args;
  TileService::RawResult raw;
  TileService::TextureResult texture;
  X_HRESULT status = X_E_FAIL;
  std::shared_ptr<ProfileGpdModelAuthority> model;
  std::optional<ProfileGpdMutationOrigin> origin;
};
OwnedTileRequest::OwnedTileRequest(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
OwnedTileRequest::~OwnedTileRequest() = default;
namespace {
bool TileIdentityCurrent(const OwnedTileRequest::State& state) {
  if (!state.model) { return !state.origin; }
  std::error_code error;
  const auto now = state.model->CaptureOrigin(error);
  return now && state.origin && !error && now->xuid == state.origin->xuid &&
      now->user_slot == state.origin->user_slot && now->storage == state.origin->storage &&
      now->stamp.sign_in_generation == state.origin->stamp.sign_in_generation;
  // model_version deliberately does not invalidate earlier owned PNG bytes.
}
TileService::OwnedPng CaptureTilePng(KernelState& kernel,
    const TileRequestArguments& args, OwnedTileRequest::State& state) {
  if (const auto stock = TileService::SelectStock(args.type, args.title, args.item)) {
    return state.root_provider ? TileService::ReadStock(*state.root_provider, *stock)
                              : TileService::OwnedPng{X_HRESULT_FROM_WIN32(X_ERROR_FILE_NOT_FOUND)};
  }
  if (args.type == 12 || args.type > uint32_t(XTileType::kAvatarFullBody)) {
    return {X_E_NOT_IMPLEMENTED};
  }
  auto* manager = kernel.xam_state()->profile_manager();
  // One actual slot/model capture is the producer origin. Splitting a raw
  // profile capture from an XUID model lookup could retag a same-XUID re-login.
  std::error_code error;
  state.model = args.user < XUserMaxUserCount
      ? manager->AcquireProfileGpdModel(uint8_t(args.user), error)
      : manager->AcquireProfileGpdModel(args.item, error);
  if (!state.model || error) { return {X_E_NO_SUCH_USER}; }
  state.origin = state.model->CaptureOrigin(error);
  if (!state.origin || error ||
      (args.user < XUserMaxUserCount && state.origin->user_slot != args.user)) {
    return {X_E_FAIL};
  }
  auto png = TileService::FromOwnedPng(kernel.xam_state()->user_tracker()->GetIcon(
      state.origin->xuid, args.title, XTileType(args.type), args.item));
  if (!TileIdentityCurrent(state)) { return {X_E_FAIL}; }
  return png;
}
}
bool IsTileMessage(uint32_t message) {
  return message == 0xB0030 || message == 0xB0032 ||
         message == 0xB003A || message == 0xB003B;
}
std::shared_ptr<const OwnedTileRequest> CaptureTileRequest(KernelState& kernel,
    const TileRequestArguments& args, X_HRESULT& status) {
  status = X_E_INVALIDARG;
  if (global_critical_region::is_held_by_current_thread()) { return {}; }
  auto state = std::make_unique<OwnedTileRequest::State>();
  state->kernel = &kernel;
  state->root_provider = kernel.xam_state()->stock_tiles();
  state->args = args;
  auto& memory = *kernel.memory();
  uint64_t destination_span = 0;
  if (!args.texture) {
    if (!args.size_address || !util::GuestIoRangeValid(memory, args.size_address, 4, true)) { return {}; }
    std::array<uint8_t, 4> size{};
    if (XFAILED(util::ReadGuestIoBuffer(memory, args.size_address, size))) { return {}; }
    destination_span = xe::load_and_swap<uint32_t>(size.data());
    if (destination_span > TileService::kDecodedLimit ||
        (destination_span && (!args.destination || !util::GuestIoRangeValid(
            memory, args.destination, uint32_t(destination_span), true))) ||
        (destination_span && util::GuestIoRangesOverlap(memory, args.destination,
            uint32_t(destination_span), args.size_address, 4))) { return {}; }
  } else {
    destination_span = uint64_t(args.stride) * args.height;
  }
  if (args.overlapped &&
      (!util::GuestIoRangeValid(memory, args.overlapped, sizeof(XAM_OVERLAPPED), true) ||
       (args.size_address && util::GuestIoRangesOverlap(memory, args.size_address, 4,
            args.overlapped, sizeof(XAM_OVERLAPPED))) ||
       (destination_span && destination_span <= UINT32_MAX &&
        util::GuestIoRangesOverlap(memory, args.destination, uint32_t(destination_span),
            args.overlapped, sizeof(XAM_OVERLAPPED))))) { return {}; }
  if (args.texture) {
    state->status = X_E_INVALIDARG;
    if (args.destination && destination_span && destination_span <= TileService::kDecodedLimit &&
        util::GuestIoRangeValid(memory, args.destination, uint32_t(destination_span), true)) {
      state->texture = TileService::PrepareTexture(CaptureTilePng(kernel, args, *state),
                                                  args.stride, args.height);
      state->status = state->texture.status;
    }
  } else {
    state->raw = TileService::PrepareRaw(CaptureTilePng(kernel, args, *state), uint32_t(destination_span));
    state->status = state->raw.status;
  }
  status = X_E_SUCCESS;  // Captured provider failures still complete truthfully.
  return std::shared_ptr<const OwnedTileRequest>(new OwnedTileRequest(std::move(state)));
}
std::shared_ptr<const OwnedTileRequest> CaptureTileMessage(KernelState& kernel,
    uint32_t message, uint32_t descriptor, uint32_t length,
    uint32_t overlapped, X_HRESULT& status) {
  status = X_E_INVALIDARG;
  if (!IsTileMessage(message) || !descriptor || global_critical_region::is_held_by_current_thread()) { return {}; }
  const bool extended = message == 0xB003A || message == 0xB003B;
  const uint32_t extent = extended ? 0x38 : 0x28;
  // Native builtin callers may omit the optional length. The actual full
  // descriptor must still exist; any nonzero different extent is refused.
  if (length && length != extent) { return {}; }
  std::array<uint8_t, 0x38> bytes{};
  if (XFAILED(util::ReadGuestIoBuffer(*kernel.memory(), descriptor,
          std::span<uint8_t>(bytes).first(extent)))) { return {}; }
  const auto word = [&](size_t at) { return xe::load_and_swap<uint32_t>(bytes.data() + at); };
  TileRequestArguments args;
  args.texture = message == 0xB0032 || message == 0xB003B;
  args.type = word(0); args.title = word(4);
  args.item = xe::load_and_swap<uint64_t>(bytes.data() + 8);
  args.user = word(0x10); args.destination = word(0x14); args.overlapped = overlapped;
  if (extended && (word(0x28) || word(0x2C))) { status = X_E_NOT_IMPLEMENTED; return {}; }
  if (args.type == 12) { status = X_E_NOT_IMPLEMENTED; return {}; }
  if (args.texture) { args.stride = word(0x20); args.height = word(0x24); }
  else { args.size_address = word(0x18); }
  return CaptureTileRequest(kernel, args, status);
}
X_RESULT CompleteTileRequest(KernelState& kernel, const OwnedTileRequest& request,
                             uint32_t& extended_error, uint32_t& length) {
  extended_error = X_E_FAIL; length = 0;
  const auto& state = *request.state_;
  if (global_critical_region::is_held_by_current_thread() || state.kernel != &kernel ||
      state.root_provider != kernel.xam_state()->stock_tiles() || !TileIdentityCurrent(state)) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto status = state.status;
  const auto& args = state.args;
  auto& memory = *kernel.memory();
  if (!args.texture) {
    if (state.raw.required) {
      std::array<uint8_t, 4> required{};
      xe::store_and_swap<uint32_t>(required.data(), state.raw.required);
      const std::array<util::GuestIoWrite, 2> writes{
          {{args.destination, state.raw.payload}, {args.size_address, required}}};
      if (XFAILED(util::WriteGuestIoBuffers(memory, writes))) { status = X_E_INVALIDARG; }
    }
  } else if (XSUCCEEDED(status)) {
    const auto& texture = state.texture;
    std::vector<uint8_t> staged(texture.destination_bytes);
    if (XFAILED(util::ReadGuestIoBuffer(memory, args.destination, staged))) { status = X_E_INVALIDARG; }
    else {
      for (uint32_t y = 0; y < texture.height; ++y) {
        for (uint32_t x = 0; x < texture.width; ++x) {
          const auto* rgba = texture.rgba.data() + (size_t(y) * texture.width + x) * 4;
          auto* argb = staged.data() + size_t(y) * args.stride + x * 4;
          argb[0] = rgba[3]; argb[1] = rgba[0]; argb[2] = rgba[1]; argb[3] = rgba[2];
        }
      }
      if (!TileIdentityCurrent(state) || XFAILED(util::WriteGuestIoBuffer(memory, args.destination, staged))) {
        status = X_E_INVALIDARG;
      }
    }
  }
  extended_error = status;
  return XFAILED(status) ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS;
}

}  // namespace xe::kernel::xam
