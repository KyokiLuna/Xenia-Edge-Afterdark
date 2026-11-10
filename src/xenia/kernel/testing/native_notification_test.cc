#include <array>
#include <cstring>
#include <utility>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_notification_frame_protocol.h"
#include "xenia/kernel/native_notification_presentation.h"
#include "xenia/kernel/native_notification_service.h"
namespace xe::kernel::test {
namespace {
using notification_detail::FrameDecision;
using notification_detail::FrameProtocol;
using notification_detail::FrameSample;
const memory::PhysicalAllocationOwner owner{1, 2, 3, 0x12340000};
FrameSample Frame() {
  return {owner, 4, 5, 6, 7, 0x10001, 0x90010000, 0, {false, 0, 0}};
}
gpu::NativeFrameSubmissionIdentity Source(uint64_t serial = 1) {
  return {owner, serial};
}
using Call = NativeSystemUiPublicCall;
using Result = NativeNotificationResult;
}  // namespace
TEST_CASE(
    "Notification queue profiles preserve qword areas and context "
    "independently",
    "[kernel][native_notification][abi]") {
  const auto* normal = NativeSystemUiPublicProfile(Call::kNotifyQueue);
  const auto* extended = NativeSystemUiPublicProfile(Call::kNotifyQueueEx);
  REQUIRE(normal);
  REQUIRE(extended);
  CHECK(normal->argument_count == 5);
  CHECK(normal->uint64_register_mask == 0x14);
  CHECK(extended->argument_count == 8);
  CHECK(extended->uint64_register_mask == 0x84);
  std::array<uint64_t, 8> args{0x1B,       3, 0x1234567800000001, 0x90020000, 0,
                               0x90021000, 0, 0xFEDCBA9876543210};
  CHECK(NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
  for (size_t narrow : {0, 1, 3, 4, 5, 6}) {
    auto bad = args;
    bad[narrow] |= uint64_t(1) << 32;
    CHECK_FALSE(
        NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, bad));
  }
  args[2] = 0x1234567800000004;
  CHECK_FALSE(
      NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
}
TEST_CASE(
    "Native context payload is a bounded packed qword only under its real flag",
    "[kernel][native_notification][abi]") {
  std::array<uint64_t, 8> args{0x1B, 0, 1, 0x90020000,
                               0,    0, 1, (uint64_t(128) << 32) | 0x90021000};
  CHECK(NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
  for (uint32_t count : {0u, 129u, UINT32_MAX}) {
    args[7] = (uint64_t(count) << 32) | 0x90021000;
    CHECK_FALSE(
        NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
  }
  args[7] = uint64_t(10) << 32;
  CHECK_FALSE(
      NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
  args[6] = 0;
  args[7] = UINT64_MAX;
  CHECK(NativeNotificationService::ArgumentsValid(Call::kNotifyQueueEx, args));
}
TEST_CASE(
    "Consumed suppressed coalesced and uncertain native requests cannot replay "
    "as fallback",
    "[kernel][native_notification][ownership]") {
  for (uint32_t value : {0u, 1u, 0x8007000Eu, 0x80070057u}) {
    auto result =
        NativeNotificationService::InterpretReturned(Call::kNotifyQueue, value);
    CHECK_FALSE(result.may_use_fallback());
    CHECK_FALSE(result.inputs_must_remain_until_kernel_stop());
  }
  CHECK(NativeNotificationService::InterpretReturned(Call::kNotifyQueue, 1)
            .disposition == Result::Disposition::kNativeSuppressed);
  CHECK(NativeNotificationService::InterpretReturned(Call::kNotifyDelay, 0x65B)
            .disposition == Result::Disposition::kNativeRejected);
  CHECK(NativeNotificationService::InterpretReturned(Call::kNotifySetOptions,
                                                     0x90000000)
            .value == 0);
  Result uncertain{Result::Disposition::kExecutionUncertain, 0x80004005};
  CHECK(uncertain.inputs_must_remain_until_kernel_stop());
  CHECK_FALSE(uncertain.may_use_fallback());
}
TEST_CASE("Notification catalog cannot enable an unintegrated service",
          "[kernel][native_notification][admission]") {
  const std::array<uint64_t, 5> args{0x1B, 2, 1, 0x90020000, 0};
  REQUIRE_FALSE(NativeSystemPublicFamilyRouteEnabled(
      NativeSystemPublicFamily::kNotifications));
  auto result = NativeNotificationService::TryInvokeOnOriginalCaller(
      nullptr, Call::kNotifyQueue, args);
  CHECK(result.may_use_fallback());
  CHECK_FALSE(NativeNotificationPresentationObserver::Create({}));
  NativeNotificationPresentationObserver::DrawObservation{}.Complete();
}
TEST_CASE(
    "A real completed frame ledger distinguishes draw from observed inactivity",
    "[kernel][native_notification][frame]") {
  const bool draw = GENERATE(false, true);
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  if (draw) {
    REQUIRE(protocol.ObserveDraw(frame));
  }
  const auto decision = protocol.FinishSource(frame, Source(), 0, 0);
  REQUIRE(decision);
  CHECK(decision->kind == (draw ? FrameDecision::Kind::kEligible
                                : FrameDecision::Kind::kInactive));
  CHECK(decision->source == Source());
  CHECK(decision->serial == 1);
  CHECK(decision->revision == 1);
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
}
TEST_CASE("An unobserved frame cannot mint inactive or eligible metadata",
          "[kernel][native_notification][frame]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  CHECK_FALSE(protocol.ObserveDraw(frame));
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
  REQUIRE(protocol.Arm(frame));
  protocol.CancelFrame(frame);
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
}
TEST_CASE(
    "Draw stamps cannot cross a native frame counter stack task or foreground",
    "[kernel][native_notification][frame]") {
  for (unsigned field = 0; field < 7; ++field) {
    FrameProtocol protocol(owner, 4, 5);
    const auto frame = Frame();
    REQUIRE(protocol.Arm(frame));
    auto changed = frame;
    switch (field) {
      case 0:
        ++changed.counter;
        break;
      case 1:
        changed.frame_sp += 16;
        break;
      case 2:
        changed.task += 4;
        break;
      case 3:
        ++changed.thread;
        break;
      case 4:
        ++changed.foreground;
        break;
      case 5:
        ++changed.root_generation;
        break;
      case 6:
        ++changed.domain;
        break;
    }
    CHECK_FALSE(protocol.ObserveDraw(changed));
    CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
  }
}
TEST_CASE(
    "The actual Source owner and full identity remain mandatory after a draw",
    "[kernel][native_notification][ownership]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  auto foreign = Source();
  ++foreign.owner.session_id;
  CHECK_FALSE(protocol.FinishSource(frame, foreign, 0, 0));
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
}
TEST_CASE(
    "A frame that rendered earlier modal content cannot qualify as nonmodal",
    "[kernel][native_notification][frame]") {
  const bool extra_overlay = GENERATE(false, true);
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  CHECK_FALSE(protocol.FinishSource(frame, Source(), extra_overlay ? 0 : 1,
                                    extra_overlay ? 1 : 0));
}
TEST_CASE(
    "Unknown initial modal state is usable but every real transition "
    "invalidates it",
    "[kernel][native_notification][lifecycle]") {
  FrameProtocol protocol(owner, 4, 5);
  auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  protocol.ModalTransition({true, 1, frame.foreground});
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
  frame.modal = {true, 1, frame.foreground};
  REQUIRE(protocol.Arm(frame));
  protocol.ModalTransition(
      {true, 2, frame.foreground});  // Includes true->false hide.
  CHECK_FALSE(protocol.FinishSource(frame, Source(2), 0, 0));
}
TEST_CASE(
    "A genuine native empty clear revokes without a new Source or armed frame",
    "[kernel][native_notification][lifecycle]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  auto draw = protocol.FinishSource(frame, Source(), 0, 0);
  REQUIRE(draw);
  const auto clear = protocol.FinishClear(frame);
  REQUIRE(clear);
  CHECK(clear->kind == FrameDecision::Kind::kClear);
  CHECK_FALSE(clear->source);
  CHECK(clear->revision > draw->revision);
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
}
TEST_CASE("Native observer retirement closes all metadata publication",
          "[kernel][native_notification][lifecycle]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  protocol.BeginStop();
  CHECK_FALSE(protocol.accepting());
  CHECK_FALSE(protocol.armed());
  CHECK_FALSE(protocol.Arm(frame));
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 0));
  CHECK_FALSE(protocol.FinishClear(frame));
}
TEST_CASE("Caption measurement metadata bounds pairs to the same live cohort",
          "[kernel][native_notification][diagnostic][model]") {
  // Synthetic metadata only. Equal text across threads does not establish the
  // actual notification receiver or grant native Task/domain/UI authority.
  using namespace notification_detail;
  TextSenderEntry sender;
  sender.identity = {1, 2, 3, 4, 5, 0x90001000, 0x90002000,
                     kTextSenderCaller};
  sender.text_pointer = 0x90003000;
  sender.text_pointer_known = true;
  sender.text.units[0] = 'A';
  sender.text.units[1] = '\n';
  sender.text.count = 2;
  sender.text.terminated = true;
  TextMeasureEntry measured;
  measured.identity = {1, 2, 3, 4, 15, 0x90004000, 0x90005000,
                       kTextMeasureWrapperCaller};
  measured.state = 0x90006000;
  measured.object = 0x81000007;
  measured.text_pointer = 0x90007000;  // Equal copied text, a different address.
  measured.output = 0x90008000;
  measured.count = UINT32_MAX;
  measured.text = sender.text;
  REQUIRE(measured.identity.valid());
  CHECK(measured.identity.thread != sender.identity.thread);
  CHECK(measured.identity.pcr != sender.identity.pcr);
  TextMeasureDiagnostic slot;
  CHECK_FALSE(slot.accepting());
  CHECK_FALSE(slot.Claim(measured));
  CHECK_FALSE(slot.caption());
  for (unsigned malformed = 0; malformed != 5; ++malformed) {
    auto rejected = sender;
    switch (malformed) {
      case 0: rejected.text.count = 0; break;
      case 1: rejected.text.terminated = false; break;
      case 2: rejected.text.read_failed = true; break;
      case 3: rejected.text.count = 257; break;
      case 4: rejected.identity.lr += 4; break;
    }
    CHECK_FALSE(slot.SetCaption(rejected));
    CHECK_FALSE(slot.caption());
  }
  REQUIRE(slot.SetCaption(sender));
  CHECK(slot.accepting());
  REQUIRE(slot.caption());
  auto replacement = sender;
  replacement.text.units[0] = 'B';
  CHECK_FALSE(slot.SetCaption(replacement));
  CHECK(slot.caption()->text.units[0] == 'A');
  for (unsigned field = 0; field != 4; ++field) {
    auto foreign = measured;
    switch (field) {
      case 0: ++foreign.identity.kernel; break;
      case 1: ++foreign.identity.domain; break;
      case 2: ++foreign.identity.root_generation; break;
      case 3: ++foreign.identity.foreground; break;
    }
    CHECK_FALSE(slot.Claim(foreign));
    CHECK_FALSE(slot.pending());
  }
  for (unsigned malformed = 0; malformed != 9; ++malformed) {
    auto rejected = measured;
    switch (malformed) {
      case 0: rejected.identity.lr += 4; break;
      case 1: rejected.identity.sp = 0x80; break;
      case 2: ++rejected.identity.sp; break;
      case 3: rejected.count = 2; break;
      case 4: rejected.text.units[0] = 'B'; break;
      case 5: rejected.text.count = 1; break;
      case 6: rejected.text.terminated = false; break;
      case 7: rejected.text.read_failed = true; break;
      case 8: rejected.text.count = 257; break;
    }
    CHECK_FALSE(slot.Claim(rejected));
    CHECK_FALSE(slot.pending());
  }
  REQUIRE(slot.Claim(measured));  // Different thread/PCR, same caption cohort.
  for (unsigned field = 0; field != 8; ++field) {
    auto wrong = measured.identity;
    switch (field) {
      case 0: ++wrong.kernel; break;
      case 1: ++wrong.domain; break;
      case 2: ++wrong.root_generation; break;
      case 3: ++wrong.foreground; break;
      case 4: ++wrong.thread; break;
      case 5: wrong.pcr += 0x1000; break;
      case 6: wrong.sp += 16; break;
      case 7: wrong.lr = kTextMeasureExportCaller; break;
    }
    CHECK_FALSE(slot.Complete(wrong));
    CHECK(slot.pending());
    CHECK(slot.observed() == 0);
  }
  const auto complete = slot.Complete(measured.identity);
  REQUIRE(complete);
  CHECK(complete->state == measured.state);
  CHECK(complete->object == measured.object);
  CHECK(complete->text_pointer == measured.text_pointer);
  CHECK(complete->output == measured.output);
  CHECK(complete->text.count == sender.text.count);
  CHECK_FALSE(slot.pending());
  CHECK(slot.observed() == 1);
  CHECK_FALSE(slot.Complete(measured.identity));
  TextMeasureDiagnostic sequential;
  REQUIRE(sequential.SetCaption(sender));
  for (uint32_t i = 0; i != kMaximumTextMeasureObservations; ++i) {
    auto next = measured;
    next.identity.sp += i * 16;
    next.identity.lr = i & 1 ? kTextMeasureExportCaller : kTextMeasureWrapperCaller;
    REQUIRE(sequential.Claim(next));
    REQUIRE(sequential.Complete(next.identity));
    CHECK(sequential.observed() == i + 1);
  }
  CHECK_FALSE(sequential.Claim(measured));
  CHECK_FALSE(sequential.pending());
  CHECK(sequential.observed() == 8);
  CHECK_FALSE(sequential.accepting());
  TextMeasureDiagnostic ambiguous;
  REQUIRE(ambiguous.SetCaption(sender));
  REQUIRE(ambiguous.Claim(measured));
  auto unrelated = measured;
  ++unrelated.identity.domain;
  unrelated.identity.lr = 0;
  CHECK_FALSE(ambiguous.Claim(unrelated));  // Cancel before matching decisions.
  CHECK_FALSE(ambiguous.pending());
  CHECK_FALSE(ambiguous.Complete(measured.identity));
  CHECK_FALSE(ambiguous.Claim(measured));
  CHECK(ambiguous.observed() == 0);
  CHECK_FALSE(ambiguous.accepting());
  TextMeasureDiagnostic stopped;
  REQUIRE(stopped.SetCaption(sender));
  REQUIRE(stopped.Claim(measured));
  CHECK(stopped.Stop());
  CHECK_FALSE(stopped.Stop());
  CHECK_FALSE(stopped.pending());
  CHECK_FALSE(stopped.Complete(measured.identity));
  CHECK_FALSE(stopped.Claim(measured));
  CHECK(stopped.caption());
  CHECK_FALSE(stopped.accepting());
}

TEST_CASE("Caption measurement readers copy exact bounded RAM without mutation",
          "[kernel][native_notification][diagnostic][memory]") {
  // Real owned RAM, synthetic ABI bytes. No actual measurement/font/UI result
  // is claimed, and the readers acquire no lifetime or provider authority.
  using namespace notification_detail;
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t page = memory.SystemHeapAlloc(0x3000, 0x1000);
  REQUIRE(page);
  auto* heap = memory.LookupHeap(page);
  REQUIRE(heap);
  std::memset(memory.TranslateVirtual(page), 0x5A, 0x3000);
  const uint32_t state_address = page + 0x100;
  const uint32_t bounds_address = page + 0x200;
  const uint32_t cache_address = page + 0x300;
  const uint32_t descriptor_address = page + 0x400;
  auto word = [&](uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(address), value);
  };
  std::array<uint32_t, 9> state_words{};
  for (size_t i = 0; i != state_words.size(); ++i) {
    state_words[i] = 0x10203040u + uint32_t(i);
    word(state_address + uint32_t(i * 4), state_words[i]);
  }
  state_words[5] = cache_address;
  word(state_address + 0x14, cache_address);
  const std::array<uint32_t, 4> bounds_bits{
      0x80000000, 0x3F800000, 0x7FC01234, 0xC0200000};
  for (size_t i = 0; i != bounds_bits.size(); ++i) {
    word(bounds_address + uint32_t(i * 4), bounds_bits[i]);
  }
  std::array<uint32_t, 19> cache_words{};
  for (size_t i = 0; i != cache_words.size(); ++i) {
    cache_words[i] = 0x20304050u + uint32_t(i);
    word(cache_address + uint32_t(i * 4), cache_words[i]);
  }
  cache_words[0] = descriptor_address;
  word(cache_address, descriptor_address);
  const std::array<uint32_t, 5> descriptor{
      page + 0x500, page + 0x600, page + 0x700, 0x41800000, page + 0x800};
  for (size_t i = 0; i != descriptor.size(); ++i) {
    word(descriptor_address + uint32_t(i * 4), descriptor[i]);
  }
  constexpr std::array<size_t, 4> name_words{0, 1, 2, 4};
  for (size_t i = 0; i != name_words.size(); ++i) {
    const uint32_t address = descriptor[name_words[i]];
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(address),
                                 uint16_t('A' + i));
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(address + 2), 0x03A9);
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(address + 4), 0);
  }
  const std::array<uint32_t, 9> no_state{};
  const std::array<uint32_t, 4> no_bounds{};
  const std::array<uint32_t, 19> no_cache{};
  const std::array<uint32_t, 5> no_descriptor{};
  SECTION("complete readonly spans preserve raw BE float and descriptor bits") {
    std::array<uint8_t, 0x3000> before;
    std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
    REQUIRE(heap->Protect(page, 0x1000, kMemoryProtectRead));
    const auto state = ReadTextMeasureState(memory, state_address);
    REQUIRE(state.readable);
    CHECK(state.words == state_words);
    const auto bounds = ReadTextMeasureBounds(memory, bounds_address);
    REQUIRE(bounds.readable);
    CHECK(bounds.bits == bounds_bits);
    const auto font = ReadTextMeasureFont(memory, cache_address);
    REQUIRE(font.cache_readable);
    CHECK(font.cache_words == cache_words);
    REQUIRE(font.descriptor_readable);
    CHECK(font.descriptor == descriptor);  // Word3 is scalar, never a name pointer.
    for (size_t i = 0; i != font.names.size(); ++i) {
      CHECK(font.names[i].terminated);
      CHECK_FALSE(font.names[i].read_failed);
      REQUIRE(font.names[i].count == 2);
      CHECK(font.names[i].units[0] == uint16_t('A' + i));
      CHECK(font.names[i].units[1] == 0x03A9);
    }
    for (uint32_t invalid : {0u, UINT32_MAX}) {
      const auto bad_state = ReadTextMeasureState(memory, invalid);
      CHECK_FALSE(bad_state.readable);
      CHECK(bad_state.words == no_state);
      const auto bad_bounds = ReadTextMeasureBounds(memory, invalid);
      CHECK_FALSE(bad_bounds.readable);
      CHECK(bad_bounds.bits == no_bounds);
      const auto bad_font = ReadTextMeasureFont(memory, invalid);
      CHECK_FALSE(bad_font.cache_readable);
      CHECK(bad_font.cache_words == no_cache);
      CHECK_FALSE(bad_font.descriptor_readable);
      CHECK(bad_font.descriptor == no_descriptor);
    }
    CHECK(std::memcmp(before.data(), memory.TranslateVirtual(page), before.size()) == 0);
    REQUIRE(heap->Protect(page, 0x1000,
                          kMemoryProtectRead | kMemoryProtectWrite));
  }
  SECTION("partial fixed spans fail completely before decoding or following") {
    REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
    const auto state = ReadTextMeasureState(memory, page + 0xFE0);
    CHECK_FALSE(state.readable);
    CHECK(state.words == no_state);
    const auto bounds = ReadTextMeasureBounds(memory, page + 0xFF4);
    CHECK_FALSE(bounds.readable);
    CHECK(bounds.bits == no_bounds);
    const auto cache = ReadTextMeasureFont(memory, page + 0xFB8);
    CHECK_FALSE(cache.cache_readable);
    CHECK(cache.cache_words == no_cache);
    CHECK_FALSE(cache.descriptor_readable);
    for (uint32_t invalid : {0u, UINT32_MAX, page + 0xFF0}) {
      word(cache_address, invalid);
      std::array<uint8_t, 0x1000> before;
      std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
      const auto font = ReadTextMeasureFont(memory, cache_address);
      CHECK(font.cache_readable);
      CHECK(font.cache_words[0] == invalid);
      CHECK_FALSE(font.descriptor_readable);
      CHECK(font.descriptor == no_descriptor);
      CHECK(std::memcmp(before.data(), memory.TranslateVirtual(page), before.size()) == 0);
    }
    REQUIRE(heap->Protect(page + 0x1000, 0x1000,
                          kMemoryProtectRead | kMemoryProtectWrite));
  }
  SECTION("names retain null overflow terminated and bounded failure facts") {
    word(descriptor_address, UINT32_MAX);
    word(descriptor_address + 4, page + 0xFFA);
    word(descriptor_address + 8, page + 0xC00);
    word(descriptor_address + 16, 0);
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(page + 0xFFA), 'Z');
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(page + 0xFFC), '\n');
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(page + 0xFFE), 0);
    for (unsigned i = 0; i != 256; ++i) {
      xe::store_and_swap<uint16_t>(memory.TranslateVirtual(page + 0xC00 + i * 2), 'Q');
    }
    std::array<uint8_t, 0x1000> before;
    std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
    REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
    const auto font = ReadTextMeasureFont(memory, cache_address);
    REQUIRE(font.cache_readable);
    REQUIRE(font.descriptor_readable);
    CHECK(font.names[0].read_failed);
    CHECK(font.names[0].count == 0);
    CHECK(font.names[1].terminated);
    CHECK_FALSE(font.names[1].read_failed);
    REQUIRE(font.names[1].count == 2);
    CHECK(font.names[1].units[0] == 'Z');
    CHECK(font.names[1].units[1] == '\n');
    CHECK_FALSE(font.names[2].terminated);
    CHECK_FALSE(font.names[2].read_failed);
    CHECK(font.names[2].count == 256);
    CHECK(font.names[2].units.back() == 'Q');
    CHECK(font.names[3].read_failed);
    CHECK(font.names[3].count == 0);
    CHECK(std::memcmp(before.data(), memory.TranslateVirtual(page), before.size()) == 0);
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(page + 0xFFE), 'R');
    std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
    const auto partial_name = ReadTextMeasureFont(memory, cache_address);
    CHECK(partial_name.names[1].read_failed);
    CHECK_FALSE(partial_name.names[1].terminated);
    CHECK(partial_name.names[1].count == 3);
    CHECK(std::memcmp(before.data(), memory.TranslateVirtual(page), before.size()) == 0);
    REQUIRE(heap->Protect(page + 0x1000, 0x1000,
                          kMemoryProtectRead | kMemoryProtectWrite));
  }
  memory.SystemHeapFree(page);
}

TEST_CASE("Native cue call pairing keeps nested owners and stops on ambiguity",
          "[kernel][native_notification][diagnostic][cue]") {
  // Synthetic copied call metadata tests pairing only; it cannot mint a real
  // module, native voice, AudioSystem registration or SDL acceptance receipt.
  using namespace notification_detail;
  const NativeCueCallIdentity outer{1, 2, 3, 4, 5, 0x90010000,
                                    0x90002000, 0x817DE160, 0x817DDD38};
  REQUIRE(outer.valid());
  NativeCueCalls calls;
  REQUIRE(calls.Claim(outer));
  NativeCueCall load;
  load.identity = outer;
  load.arguments[0] = 0x90003000;
  load.audio_sequence_at_entry = 72;
  REQUIRE(calls.Record(load));
  auto inner = outer;
  inner.function = 0x819A3930;
  inner.sp -= 0x180;
  inner.lr = 0x817DE074;
  REQUIRE(calls.Claim(inner));
  NativeCueCall start;
  start.identity = inner;
  start.arguments = {0x90004000, 0, 0};
  REQUIRE(calls.Record(start));
  for (unsigned field = 0; field != 7; ++field) {
    auto foreign = inner;
    switch (field) {
      case 0: ++foreign.kernel; break;
      case 1: ++foreign.domain; break;
      case 2: ++foreign.root_generation; break;
      case 3: ++foreign.foreground; break;
      case 4: ++foreign.thread; break;
      case 5: foreign.sp -= 16; break;
      case 6: foreign.lr += 4; break;
    }
    CHECK_FALSE(calls.Complete(foreign));
  }
  const auto finished_start = calls.Complete(inner);
  REQUIRE(finished_start);
  CHECK(finished_start->arguments[0] == start.arguments[0]);
  CHECK_FALSE(calls.Complete(inner));
  const auto finished_load = calls.Complete(outer);
  REQUIRE(finished_load);
  CHECK(finished_load->arguments[0] == load.arguments[0]);
  CHECK(finished_load->audio_sequence_at_entry == 72);
  auto invalid = outer;
  invalid.function = 0x819A3934;  // No arbitrary instruction/source-offset hook.
  CHECK_FALSE(calls.Claim(invalid));
  invalid = outer;
  invalid.sp -= 1;
  CHECK_FALSE(calls.Claim(invalid));
  invalid = outer;
  invalid.lr |= 1;
  CHECK_FALSE(calls.Claim(invalid));
  REQUIRE(calls.Claim(outer));
  CHECK_FALSE(calls.Claim(outer));  // Abandoned identical frame cannot retarget.
  CHECK_FALSE(calls.accepting());
  CHECK_FALSE(calls.Complete(outer));
  CHECK_FALSE(calls.Claim(inner));
  NativeCueCalls bounded;
  for (uint32_t i = 0; i != 64; ++i) {
    auto parallel = outer;
    parallel.thread += i;
    REQUIRE(bounded.Claim(parallel));
  }
  auto overflow = outer;
  overflow.thread += 64;
  CHECK_FALSE(bounded.Claim(overflow));
  CHECK_FALSE(bounded.accepting());
  CHECK_FALSE(bounded.Complete(outer));
  NativeCueCalls canceled;
  REQUIRE(canceled.Claim(outer));
  canceled.Stop();
  CHECK_FALSE(canceled.Complete(outer));
  CHECK_FALSE(canceled.Claim(outer));
}

TEST_CASE("Native cue ABI snapshots use complete RAM bounds and preserve bytes",
          "[kernel][native_notification][diagnostic][cue][memory]") {
  // Real Memory, synthetic ABI contents: exact five-DWORD class/message and
  // twenty-two-DWORD buffer sizes come from the pinned17559 native bodies.
  using namespace notification_detail;
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t page = memory.SystemHeapAlloc(0x2000, 0x1000);
  REQUIRE(page);
  auto* heap = memory.LookupHeap(page);
  REQUIRE(heap);
  std::array<uint8_t, 88> original{};
  CHECK(NativeCueFrameRamReadable(memory, page));
  CHECK_FALSE(NativeCueFrameRamReadable(memory, 0));
  CHECK_FALSE(NativeCueFrameRamReadable(memory, UINT32_MAX - 3));
  const uint32_t buffer = page + 0x1000 - uint32_t(original.size());
  for (uint32_t i = 0; i != 22; ++i) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(buffer + i * 4),
                                 0x10203040u + i);
  }
  std::memcpy(original.data(), memory.TranslateVirtual(buffer), original.size());
  REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
  CHECK_FALSE(NativeCueFrameRamReadable(memory, page));
  const auto complete = ReadNativeCueBuffer(memory, buffer);
  REQUIRE(complete.readable);
  for (uint32_t i = 0; i != 22; ++i) { CHECK(complete.words[i] == 0x10203040u + i); }
  CHECK(std::memcmp(original.data(), memory.TranslateVirtual(buffer), original.size()) == 0);
  const auto partial_buffer = ReadNativeCueBuffer(memory, buffer + 4);
  CHECK_FALSE(partial_buffer.readable);
  for (uint32_t word : partial_buffer.words) { CHECK(word == 0); }
  const uint32_t message = page + 0x1000 - 20;
  const std::array<uint32_t, 5> header{0x14, 0x7F4, 1, 0, 0};
  for (uint32_t i = 0; i != header.size(); ++i) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(message + i * 4), header[i]);
  }
  const auto readable = ReadNativeCueWords(memory, message);
  REQUIRE(readable.readable);
  CHECK(readable.words == header);
  CHECK_FALSE(ReadNativeCueWords(memory, message + 4).readable);
  CHECK_FALSE(ReadNativeCueWords(memory, 0).readable);
  CHECK_FALSE(ReadNativeCueWords(memory, UINT32_MAX - 3).readable);
  CHECK_FALSE(ReadNativeCueBuffer(memory, 0).readable);
  CHECK_FALSE(ReadNativeCueBuffer(memory, UINT32_MAX - 3).readable);
  // The reader preserves malformed headers as raw facts; public native-call
  // observation, rather than this byte reader, performs the07F4/07F5 filter.
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(message), 0x10);
  CHECK(ReadNativeCueWords(memory, message).words[0] == 0x10);
  REQUIRE(heap->Protect(page + 0x1000, 0x1000,
                        kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(NativeCueFrameRamReadable(memory, page));
  memory.SystemHeapFree(page);
}

TEST_CASE("Font acquisition retains exact nested returns and cancels abandoned ownership",
          "[kernel][native_notification][font][diagnostic]") {
  // Copied metadata only: these records cannot create an actual matched
  // caption, native font, module, renderer, allocation or successful pixels.
  using namespace notification_detail;
  TextMeasureEntry measure;
  measure.identity = {1, 2, 3, 4, 5, 0x90001000, 0x90010000, kTextMeasureWrapperCaller};
  measure.state = 0x90020000;
  measure.output = 0x90021000;
  measure.count = UINT32_MAX;
  measure.text.units[0] = 'A';
  measure.text.count = 1;
  measure.text.terminated = true;
  const uint32_t task = 0x90030000;
  TextMeasureState state;
  state.readable = true;
  TextMeasureBounds bounds;
  bounds.readable = true;
  bounds.bits = {0, 0, 0x43BA8000, 0x42700000};  // Raw partial373x60 on E_FAIL.
  FontAcquireDiagnostic diagnostic;
  diagnostic.Prepare();
  REQUIRE(diagnostic.Begin(measure, 1, task, state, {}));
  const FontAcquireIdentity cache{measure.identity, 1, task, 0x817925A0,
                                  measure.identity.sp - 0x180, 0x81792D40};
  auto record = [&](const FontAcquireIdentity& id) {
    const auto ticket = diagnostic.Claim(id);
    REQUIRE(ticket);
    FontAcquireEntry entry;
    entry.identity = id;
    entry.ordinal = ticket->first;
    entry.parent = ticket->second;
    entry.arguments[0] = 0x90040000;
    REQUIRE(diagnostic.Record(entry));
    return entry;
  };
  SECTION("one nested negative chain preserves raw results and caller bytes") {
    auto rejected = cache;
    rejected.function += 4;
    CHECK_FALSE(diagnostic.Claim(rejected));
    rejected = cache;
    rejected.lr += 4;
    CHECK_FALSE(diagnostic.Claim(rejected));
    rejected = cache;
    ++rejected.measure.foreground;
    CHECK_FALSE(diagnostic.Claim(rejected));
    CHECK(diagnostic.accepting());
    const auto cache_entry = record(cache);
    auto engine = cache;
    engine.function = 0x8178F5C8;
    engine.lr = 0x81792688;
    engine.sp -= 0xC0;
    const auto engine_entry = record(engine);
    CHECK(engine_entry.parent == cache_entry.ordinal);
    auto face = engine;
    face.function = 0x8178EED8;
    face.lr = 0x8178F67C;
    face.sp -= 0x90;
    const auto face_entry = record(face);
    CHECK(face_entry.parent == engine_entry.ordinal);
    for (unsigned field = 0; field != 5; ++field) {
      auto foreign = face;
      if (field == 0) { ++foreign.measure.root_generation; }
      if (field == 1) { ++foreign.measure.thread; }
      if (field == 2) { ++foreign.task; }
      if (field == 3) { ++foreign.measure_ordinal; }
      if (field == 4) { foreign.sp -= 16; }
      CHECK_FALSE(diagnostic.Complete(foreign));
    }
    for (const auto& id : {face, engine, cache}) {
      auto entry = diagnostic.Complete(id);
      REQUIRE(entry);
      CHECK_FALSE(diagnostic.Complete(id));
      FontAcquireResult result;
      result.entry = std::move(*entry);
      result.raw_result = 0x80004005;
      result.output.readable = true;
      result.output.count = 1;
      result.output.words[0] = 0;
      diagnostic.RecordReturn(std::move(result));
    }
    CHECK_FALSE(diagnostic.Finish(measure.identity, 2, task, 0, state, bounds));
    auto report = diagnostic.Finish(measure.identity, 1, task, 0x80004005, state, bounds);
    REQUIRE(report);
    REQUIRE(report->completed == 3);
    CHECK(report->results[0].entry.ordinal == face_entry.ordinal);
    CHECK(report->results[1].entry.ordinal == engine_entry.ordinal);
    CHECK(report->results[2].entry.ordinal == cache_entry.ordinal);
    CHECK(report->common_result == 0x80004005);
    CHECK(report->bounds_at_return.bits == bounds.bits);
    CHECK(report->normal_return);
    CHECK_FALSE(report->canceled);
    REQUIRE(diagnostic.Begin(measure, 2, task, state, {}));
    diagnostic.RecordReturn(report->results[0]);  // Old ordinal cannot retarget.
    auto fresh = diagnostic.Finish(measure.identity, 2, task, 0, state, {});
    REQUIRE(fresh);
    CHECK(fresh->completed == 0);
  }
  SECTION("reentry and outer-before-child return close the capture") {
    record(cache);
    auto child = cache;
    child.function = 0x8178F5C8;
    child.lr = 0x81792688;
    child.sp -= 0xC0;
    record(child);
    CHECK_FALSE(diagnostic.Complete(cache));
    CHECK_FALSE(diagnostic.accepting());
    auto report = diagnostic.Finish(measure.identity, 1, task, 0x80004005, state, bounds);
    REQUIRE(report);
    CHECK(report->ambiguous);
    CHECK(report->canceled);
    CHECK(report->pending_at_end == 2);
    CHECK_FALSE(diagnostic.Begin(measure, 2, task, state, {}));
  }
  SECTION("a duplicate exact entry cannot be mistaken for another invocation") {
    record(cache);
    CHECK_FALSE(diagnostic.Claim(cache));
    auto report = diagnostic.Finish(measure.identity, 1, task, 0, state, {});
    REQUIRE(report);
    CHECK(report->ambiguous);
    CHECK(report->pending_at_end == 1);
  }
  SECTION("a known native call cannot bypass its required parent stage") {
    record(cache);
    auto backend = cache;
    backend.function = 0x81798218;
    backend.lr = 0x81783980;
    backend.sp -= 0xC0;
    CHECK_FALSE(diagnostic.Claim(backend));
    auto report = diagnostic.Finish(measure.identity, 1, task, 0, state, {});
    REQUIRE(report);
    CHECK(report->ambiguous);
    CHECK(report->canceled);
    CHECK(report->pending_at_end == 1);
  }
  SECTION("pending depth and completed work have separate finite bounds") {
    for (uint32_t i = 0; i != 16; ++i) {
      auto recursive = cache;
      recursive.sp -= i * 0x100;
      record(recursive);
    }
    auto overflow = cache;
    overflow.sp -= 16 * 0x100;
    CHECK_FALSE(diagnostic.Claim(overflow));
    auto report = diagnostic.Finish(measure.identity, 1, task, 0, state, {});
    REQUIRE(report);
    CHECK(report->overflow);
    CHECK(report->pending_at_end == 16);
  }
  SECTION("the sixty-fifth node closes without overwriting completed records") {
    record(cache);
    auto child = cache;
    child.function = 0x8178EAD0;
    child.lr = 0x817926E8;
    child.sp -= 0xC0;
    for (uint32_t i = 0; i != 63; ++i) {
      record(child);
      auto entry = diagnostic.Complete(child);
      REQUIRE(entry);
      FontAcquireResult result;
      result.entry = std::move(*entry);
      result.raw_result = i;
      diagnostic.RecordReturn(std::move(result));
    }
    CHECK_FALSE(diagnostic.Claim(child));
    auto report = diagnostic.Finish(measure.identity, 1, task, 0, state, {});
    REQUIRE(report);
    REQUIRE(report->completed == 63);
    CHECK(report->results[62].raw_result == 62);
    CHECK(report->overflow);
    CHECK(report->pending_at_end == 1);
  }
  SECTION("task loss and stop cannot rearm or manufacture a normal stage") {
    record(cache);
    diagnostic.Stop();
    diagnostic.Prepare();
    CHECK_FALSE(diagnostic.Complete(cache));
    CHECK_FALSE(diagnostic.Begin(measure, 2, task, state, {}));
    auto report = diagnostic.Finish(measure.identity, 1, task + 1, 0, state, {});
    REQUIRE(report);
    CHECK(report->canceled);
    CHECK(report->return_task == task + 1);
  }
  SECTION("eight sequential matched measures exhaust only diagnostic storage") {
    REQUIRE(diagnostic.Finish(measure.identity, 1, task, 0, state, {}));
    for (uint32_t ordinal = 2; ordinal <= 8; ++ordinal) {
      REQUIRE(diagnostic.Begin(measure, ordinal, task, state, {}));
      REQUIRE(diagnostic.Finish(measure.identity, ordinal, task, 0, state, {}));
    }
    CHECK_FALSE(diagnostic.Begin(measure, 9, task, state, {}));
  }
}

TEST_CASE("Font acquisition RAM snapshots reject partial and unknown layouts",
          "[kernel][native_notification][font][diagnostic][memory]") {
  // Real RAM with synthetic records. No native code/profile, font parsing,
  // resource lifetime, renderer, guest write or successful rendering is claimed.
  using namespace notification_detail;
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t page = memory.SystemHeapAlloc(0x4000, 0x1000);
  REQUIRE(page);
  auto* heap = memory.LookupHeap(page);
  REQUIRE(heap);
  const auto word = [&](uint32_t at, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(at), value);
  };
  const uint32_t descriptor = page + 0x100;
  for (uint32_t i = 0; i != 3; ++i) {
    const uint32_t text = page + 0x200 + i * 0x20;
    word(descriptor + i * 4, text);
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text), uint16_t('A' + i));
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text + 2), 0);
  }
  word(descriptor + 12, 0x7FC00001);  // Raw float bits, never a fourth string.
  word(descriptor + 16, 0);
  std::array<uint8_t, 0x400> before;
  std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
  const auto read = ReadFontAcquireDescriptor(memory, descriptor);
  REQUIRE(read.data.readable);
  CHECK(read.data.count == 5);
  CHECK(read.data.words[3] == 0x7FC00001);
  for (uint32_t i = 0; i != 3; ++i) {
    CHECK(read.names[i].terminated);
    REQUIRE(read.names[i].count == 1);
    CHECK(read.names[i].units[0] == uint16_t('A' + i));
  }
  CHECK(read.names[3].read_failed);
  CHECK(std::memcmp(before.data(), memory.TranslateVirtual(page), before.size()) == 0);
  for (uint32_t i = 0; i != 64; ++i) { word(page + 0xF00 + i * 4, 0x10203040 + i); }
  REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
  const auto full = ReadFontAcquireWords(memory, page + 0xF00, 64);
  REQUIRE(full.readable);
  CHECK(full.words[63] == 0x1020307F);
  const auto partial = ReadFontAcquireWords(memory, page + 0xF04, 64);
  CHECK_FALSE(partial.readable);
  CHECK(partial.count == 0);
  for (uint32_t value : partial.words) { CHECK(value == 0); }
  CHECK_FALSE(ReadFontAcquireWords(memory, 0, 5).readable);
  CHECK_FALSE(ReadFontAcquireWords(memory, UINT32_MAX - 3, 5).readable);
  CHECK_FALSE(ReadFontAcquireWords(memory, page, 65).readable);
  CHECK_FALSE(ReadFontAcquireWords(memory, page, 0).readable);
  CHECK_FALSE(ReadFontAcquireDescriptor(memory, page + 0xFF0).data.readable);
  REQUIRE(heap->Protect(page + 0x1000, 0x1000,
                        kMemoryProtectRead | kMemoryProtectWrite));
  const uint32_t device = page + 0x1800, table = page + 0x2000;
  word(device, table);
  word(table + 0x10C, 0x81783938);
  word(device + 0x1CC, UINT32_MAX);  // Must not follow an unknown concrete class.
  const auto graph = ReadFontAcquireGraph(memory, device);
  CHECK(graph.device_known);
  CHECK(graph.atlas_known);
  CHECK(graph.atlas_target == 0x81783938);
  CHECK_FALSE(graph.backend_known);
  CHECK(graph.backend == 0);
  CHECK_FALSE(ReadFontAcquireGraph(memory, UINT32_MAX).device_known);
  CHECK(FontAcquireOffset(UINT32_MAX - 3, 8) == 0);
  CHECK(FontAcquireOffset(0, 0x1CC) == 0);
  memory.SystemHeapFree(page);
}

}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Native display metadata belongs to one completed draw arm",
          "[kernel][native_notification][display_provenance]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  std::array<uint8_t, 0x58> bytes{};
  bytes[0] = 5;
  REQUIRE(protocol.Arm(frame));
  CHECK_FALSE(protocol.PeekDraw(frame));
  REQUIRE(protocol.ObserveDraw(frame, bytes));
  bytes[0] = 7;
  auto captured = protocol.PeekDraw(frame);
  REQUIRE(captured);
  CHECK(captured->serial == 1);
  CHECK(captured->display_information[0] == 5);
  auto foreign = frame;
  foreign.task += 4;
  CHECK_FALSE(protocol.PeekDraw(foreign));
  foreign = frame;
  ++foreign.foreground;
  CHECK_FALSE(protocol.PeekDraw(foreign));
  foreign = frame;
  ++foreign.counter;
  CHECK_FALSE(protocol.PeekDraw(foreign));
  REQUIRE(protocol.FinishSource(frame, Source(), 0, 0));
  CHECK_FALSE(protocol.PeekDraw(frame));
}
TEST_CASE("Canceled native draws cannot seed a later output declaration",
          "[kernel][native_notification][display_provenance]") {
  FrameProtocol protocol(owner, 4, 5);
  auto frame = Frame();
  std::array<uint8_t, 0x58> bytes{};
  bytes[0] = 5;
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame, bytes));
  protocol.CancelFrame(frame);
  CHECK_FALSE(protocol.PeekDraw(frame));
  ++frame.counter;
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame));
  CHECK_FALSE(protocol.PeekDraw(frame));
  REQUIRE(protocol.FinishSource(frame, Source(2), 0, 0));
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame, bytes));
  auto captured = protocol.PeekDraw(frame);
  REQUIRE(captured);
  CHECK(captured->serial == 3);
  protocol.BeginStop();
  CHECK_FALSE(protocol.PeekDraw(frame));
}
TEST_CASE("Transition work does not fabricate a visible draw decision",
          "[kernel][native_notification][display_provenance]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.ObserveDraw(frame, std::array<uint8_t, 0x58>{}));
  REQUIRE(protocol.PeekDraw(frame));
  CHECK_FALSE(protocol.FinishSource(frame, Source(), 0, 1));
  CHECK_FALSE(protocol.PeekDraw(frame));
}
TEST_CASE("Control metadata is same-arm and distinct from Draw storage",
          "[kernel][native_notification][control]") {
  FrameProtocol protocol(owner, 4, 5);
  auto frame = Frame();
  CHECK_FALSE(protocol.PeekControl(frame));
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.PeekControl(frame));
  CHECK(*protocol.PeekControl(frame) == 1);
  auto foreign = frame;
  ++foreign.counter;
  CHECK_FALSE(protocol.PeekControl(foreign));
  foreign = frame;
  ++foreign.owner.session_id;
  CHECK_FALSE(protocol.PeekControl(foreign));
  REQUIRE(protocol.ObserveDraw(frame, std::array<uint8_t, 0x58>{}));
  CHECK_FALSE(protocol.PeekControl(frame));
  REQUIRE(protocol.PeekDraw(frame));
  protocol.BeginStop();
  CHECK_FALSE(protocol.PeekControl(frame));
}
TEST_CASE("Completed inactive Source does not manufacture a genuine clear",
          "[kernel][native_notification][control]") {
  FrameProtocol protocol(owner, 4, 5);
  const auto frame = Frame();
  REQUIRE(protocol.Arm(frame));
  REQUIRE(protocol.PeekControl(frame));
  const auto inactive = protocol.FinishSource(frame, Source(), 0, 0);
  REQUIRE(inactive);
  CHECK(inactive->kind == FrameDecision::Kind::kInactive);
  CHECK(inactive->source == Source());
  CHECK_FALSE(protocol.PeekControl(frame));
  CHECK_FALSE(protocol.FinishSource(frame, Source(2), 0, 0));
  const auto clear = protocol.FinishClear(frame);
  REQUIRE(clear);
  CHECK(clear->kind == FrameDecision::Kind::kClear);
  CHECK_FALSE(clear->source);
  CHECK(clear->revision > inactive->revision);
}

TEST_CASE("Sender diagnostic metadata pairs only one same live call",
          "[kernel][native_notification][diagnostic][model]") {
  // Host-only metadata model. These values grant no native Task/domain/UI
  // authority; real observer admission is deliberately not fabricated here.
  using namespace notification_detail;
  TextSenderEntry entry;
  entry.identity = {1, 2, 3, 4, 5, 0x90001000, 0x90002000,
                    kTextSenderCaller};
  entry.handle = 0x81000007;
  entry.text_pointer = 0x90003000;
  entry.text_pointer_known = true;
  TextSenderDiagnostic slot;
  REQUIRE(slot.Claim(entry));
  auto untrusted = entry;
  untrusted.identity.lr += 4;
  CHECK_FALSE(slot.Claim(untrusted));
  CHECK(slot.pending());
  for (unsigned field = 0; field != 8; ++field) {
    auto wrong = entry.identity;
    switch (field) {
      case 0: ++wrong.kernel; break;
      case 1: ++wrong.domain; break;
      case 2: ++wrong.root_generation; break;
      case 3: ++wrong.foreground; break;
      case 4: ++wrong.thread; break;
      case 5: wrong.pcr += 0x1000; break;
      case 6: wrong.sp += 16; break;
      case 7: wrong.lr += 4; break;
    }
    CHECK_FALSE(slot.Complete(wrong));
    CHECK(slot.pending());
  }
  auto completed = slot.Complete(entry.identity);
  REQUIRE(completed);
  CHECK(completed->identity == entry.identity);
  CHECK(completed->handle == entry.handle);
  CHECK_FALSE(slot.pending());
  CHECK_FALSE(slot.Complete(entry.identity));
  CHECK_FALSE(slot.Claim(entry));
  CHECK_FALSE(slot.pending());
  // A can abandon its frame, then a later eligible call can reuse every
  // identity field. Refuse its return rather than attributing it to old A.
  TextSenderDiagnostic abandoned;
  REQUIRE(abandoned.Claim(entry));
  CHECK_FALSE(abandoned.Claim(entry));
  CHECK_FALSE(abandoned.pending());
  CHECK_FALSE(abandoned.Complete(entry.identity));
  CHECK_FALSE(abandoned.Claim(entry));
  TextSenderDiagnostic nested;
  REQUIRE(nested.Claim(entry));
  auto another = entry;
  another.identity.sp += 16;
  CHECK_FALSE(nested.Claim(another));
  CHECK_FALSE(nested.Complete(entry.identity));
  CHECK_FALSE(nested.Complete(another.identity));
  TextSenderDiagnostic cancelled;
  REQUIRE(cancelled.Claim(entry));
  CHECK(cancelled.Stop());
  CHECK_FALSE(cancelled.pending());
  CHECK_FALSE(cancelled.Complete(entry.identity));
  CHECK_FALSE(cancelled.Claim(entry));
  CHECK_FALSE(cancelled.Stop());
}

TEST_CASE("Sender UTF16 diagnostic respects actual page and unit bounds",
          "[kernel][native_notification][diagnostic][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t page = memory.SystemHeapAlloc(0x2000, 0x1000);
  REQUIRE(page);
  auto* heap = memory.LookupHeap(page);
  REQUIRE(heap);
  uint32_t text;
  SECTION("terminator at page end avoids unreadable successor") {
    text = page + 0xFFA;
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text), 'A');
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text + 2), '\n');
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text + 4), 0);
    REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
    const auto value = notification_detail::ReadTextSenderText(memory, text);
    REQUIRE(value.terminated);
    CHECK_FALSE(value.read_failed);
    REQUIRE(value.count == 2);
    CHECK(value.units[0] == 'A');
    CHECK(value.units[1] == '\n');
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text + 4), 'B');
    const auto short_mapping = notification_detail::ReadTextSenderText(memory, text);
    CHECK_FALSE(short_mapping.terminated);
    CHECK(short_mapping.read_failed);
    CHECK(short_mapping.count == 3);
  }
  SECTION("256 nonzero units stop before a protected page") {
    text = page + 0xE00;
    for (unsigned i = 0; i != 256; ++i) {
      xe::store_and_swap<uint16_t>(memory.TranslateVirtual(text + i * 2), 'A');
    }
    REQUIRE(heap->Protect(page + 0x1000, 0x1000, kMemoryProtectNoAccess));
    const auto value = notification_detail::ReadTextSenderText(memory, text);
    CHECK_FALSE(value.terminated);
    CHECK_FALSE(value.read_failed);
    CHECK(value.count == 256);
    CHECK(value.units.back() == 'A');
  }
  REQUIRE(heap->Protect(page + 0x1000, 0x1000,
                        kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(notification_detail::ReadTextSenderText(memory, UINT32_MAX).read_failed);
  memory.SystemHeapFree(page);
}

TEST_CASE("Sender return metadata uses original stack and separates handled",
          "[kernel][native_notification][diagnostic][memory]") {
  // Real owned RAM, synthetic ABI bytes. No actual sender/UI success claimed.
  using namespace notification_detail;
  Memory memory;
  REQUIRE(memory.Initialize());
  const uint32_t page = memory.SystemHeapAlloc(0x1000, 0x1000);
  REQUIRE(page);
  TextSenderEntry entry;
  entry.identity = {1, 2, 3, 4, 5, page, page + 0x900, kTextSenderCaller};
  entry.text_pointer = page + 0x100;
  entry.text_pointer_known = true;
  const uint32_t message = entry.identity.sp - 0x60;
  std::memset(memory.TranslateVirtual(message), 0, 0x48);
  auto word = [&](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(message + offset), value);
  };
  // Exact17559 initializer/attachment layout; header size is distinct from ID.
  word(0, 0x14);
  word(4, 0x7E0);
  word(0xC, 0x28);
  word(0x10, entry.identity.sp - 0x40);
  word(0x2C, entry.text_pointer);
  for (uint32_t handled : {0u, 1u}) {
    word(8, handled);
    std::array<uint8_t, 0x48> before;
    std::memcpy(before.data(), memory.TranslateVirtual(message), before.size());
    const auto value = ReadTextSenderMessage(memory, entry);
    CHECK(value.readable);
    CHECK(value.matches);
    CHECK(value.header_bytes == 0x14);
    CHECK(value.id == 0x7E0);
    CHECK(value.payload_bytes == 0x28);
    CHECK(value.handled == handled);
    CHECK(value.text_pointer == entry.text_pointer);
    CHECK(std::memcmp(before.data(), memory.TranslateVirtual(message), before.size()) == 0);
  }
  for (const auto& [offset, valid] :
       {std::pair{0u, 0x14u}, std::pair{4u, 0x7E0u},
        std::pair{0xCu, 0x28u}}) {
    word(offset, valid - 1);
    const auto malformed = ReadTextSenderMessage(memory, entry);
    CHECK(malformed.readable);
    CHECK_FALSE(malformed.matches);
    CHECK(malformed.handled == 1);
    word(offset, valid);
  }
  word(0x10, entry.identity.sp - 0x30);
  const auto wrong_payload = ReadTextSenderMessage(memory, entry);
  CHECK(wrong_payload.readable);
  CHECK_FALSE(wrong_payload.matches);
  CHECK(wrong_payload.handled == 1);  // Raw fact retained, semantic flag false.
  word(0x10, entry.identity.sp - 0x40);
  word(0x2C, entry.text_pointer + 2);
  CHECK_FALSE(ReadTextSenderMessage(memory, entry).matches);
  entry.identity.sp = 0xA0;
  CHECK_FALSE(ReadTextSenderMessage(memory, entry).readable);
  memory.SystemHeapFree(page);
}

}  // namespace xe::kernel::test
