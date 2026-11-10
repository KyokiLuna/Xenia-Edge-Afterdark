/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_notification_audio.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/vfs/module_resource.h"
namespace xe::kernel {
StockNotificationAudio PrepareStockNotificationAudio(
    vfs::VirtualFileSystem& fs, apu::AudioSystem& audio,
    const std::atomic<bool>* cancelled) {
  StockNotificationAudio result;
  std::vector<uint8_t> bytes;
  result.resource_status =
      vfs::ReadModuleResource(fs, "\\Device\\Flash\\xam.xex", "skin",
                              "NotifyPopup.xma", bytes, 1024u * 1024u);
  if (XFAILED(result.resource_status)) {
    return result;
  }
  if (mbedtls_sha256(bytes.data(), bytes.size(), result.resource_sha256.data(),
                     0)) {
    result.resource_status = X_STATUS_UNSUCCESSFUL;
    return result;
  }
  apu::AudioDecodeLimits limits;
  limits.maximum_input_bytes = 1024u * 1024u;
  limits.maximum_sample_values = 192000u * 6u * 10u;
  auto decoded = apu::DecodeAudioMemory(std::move(bytes), limits, cancelled);
  result.decode_status = decoded.status;
  result.codec_error = decoded.codec_error;
  if (decoded.status != apu::AudioDecodeStatus::kSuccess) {
    return result;
  }
  result.playback = audio.PrepareIndependentAudio(std::move(decoded.pcm));
  return result;
}
}  // namespace xe::kernel
