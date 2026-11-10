/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_NOTIFICATION_AUDIO_H_
#define XENIA_KERNEL_NATIVE_NOTIFICATION_AUDIO_H_
#include <array>
#include "xenia/apu/audio_system.h"
#include "xenia/xbox.h"
namespace xe::vfs {
class VirtualFileSystem;
}
namespace xe::kernel {
struct StockNotificationAudio {
  X_STATUS resource_status = X_STATUS_UNSUCCESSFUL;
  std::array<uint8_t, 32> resource_sha256{};
  apu::AudioDecodeStatus decode_status = apu::AudioDecodeStatus::kInvalidInput;
  int codec_error = 0;
  apu::IndependentAudioPreparation playback;
};
// Service only: read actual flash xam skin/NotifyPopup.xma, decode owned bytes,
// prepare normal APU output. NEVER starts sound or routes a notification. The
// common event owner may Start only once its fallback visual first appears;
// native-accepted events never call this preparation/start path.
StockNotificationAudio PrepareStockNotificationAudio(
    vfs::VirtualFileSystem&, apu::AudioSystem&,
    const std::atomic<bool>* cancelled = nullptr);
}  // namespace xe::kernel
#endif
