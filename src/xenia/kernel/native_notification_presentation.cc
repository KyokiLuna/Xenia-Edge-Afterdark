#include "xenia/kernel/native_notification_presentation.h"
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/gpu/system_command_buffer.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_notification_frame_protocol.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_notifications.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/title_execution_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
using Sample = notification_detail::FrameSample;
using Modal = notification_detail::ModalSnapshot;
using Protocol = notification_detail::FrameProtocol;
using Decision = notification_detail::FrameDecision;
std::mutex installations_mutex;
std::map<KernelState*, std::weak_ptr<NativeNotificationPresentationObserver>>
    installations;
std::optional<uint32_t> Pointer(uint64_t value) {
  const uint32_t low = uint32_t(value);
  return value == low || value == uint64_t(int64_t(int32_t(low)))
             ? std::optional(low)
             : std::nullopt;
}
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  if (!at || uint64_t(at) + 4 > 0x100000000ull ||
      memory.QueryRangeAccess(at, 4) == memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
}
struct CodeProfile {
  uint32_t at, bytes;
  const char* sha;
};
constexpr CodeProfile profiles[] = {
    {0x816cd6e8, 668,
     "822358ef7670541ca7214898644196699e832fc1f9cda9d9730a8c779c4d7e6a"},
    {0x816ccf00, 2020,
     "02431c49d69ca45aa6a073b1991aff96ae8ce3a50557828dba68e57131495bc6"},
    {0x816ab2a8, 756,
     "afb89ebf404a17f18d754795518a1dd69862f96ac8df8c8a4ed722d27191be10"},
    {0x816ce060, 164,
     "4548934ff27abcbcc437c38919ebca8ff2ab54b884985ed44952f0688204ae24"},
    {0x816cb3d0, 3076,
     "c1424b437f9454f449e650fb1fe4008cb55cc4cc20f07515154673d5ac5b62d0"},
    {0x816c9a20, 148,
     "f3bd2e50127dd0ea949bc71da36d653a9d9e352ac748101e06353e3a8235867a"},
    {0x816ab5a0, 144,
     "007c641e8a380a40cc5e3eafb5058439dddf05b28c62ebd8ee4973b171643207"},
    {0x816aaaa0, 92,
     "d3cee9a06cb05cc1894eb057cf05d70372f2bd267a0de0efe65d8bcdf40d526f"},
    {0x81842b08, 256,
     "13879250fdaf1430c146a1b7370ac631630e9c799833b53a023fc178e527e6be"},
    {0x816CE108, 1956,
     "dae999d52ae924106fb27a841eea9bf9b98caa634f4e6f9d6b21ad97d24ffe69"},
    {0x81692978, 60,
     "fcf64b72a0b249dd8b0f90d0a9adc51abac32bfa7b713af6050d951b3740be64"},
};
// Diagnostic-only profiles: never added to presentation startup admission.
constexpr CodeProfile text_profiles[] = {
    {0x817C8868, 84,
     "4da68d6cc61e95cf7f7aa921ffa2b206d30d354402ace780cd7b14f4aec671da"},
    {0x816AA85C, 248,
     "c7475c610b20faf1c5138b02b73869d261da1355de180527d171c45676dc0d74"},
    {0x817AD098, 80,
     "306d8eeb9fbee60746d3f9c1f0b9f64f05ecc1df8fe30988269532e8e073e167"},
    {0x818062A8, 40,
     "e5a39f6947c76abd176ba981c4c856bf4d9f844ed02e0dbf2a1d7f65fbce28fc"},
    {0x817E16A8, 76,
     "ba6722860f8fbefa5b37d8c7a95c6b4e895e23d545551d53f24ee971ffc0ca19"},
};
// Measurement-only byte contracts, including both wrappers and the canonical
// save/restore family. Never part of normal presentation startup admission.
constexpr CodeProfile measure_profiles[] = {
    {0x816AA900, 284, "15b0b7783788a6f533f99c7e7f24b1ed0f655467aa3e4c1f00211a598bc09c92"},
    {0x816AA280, 168, "6ba32a057c97f7b5807218000da7af10e80623f4ab2643ce93c908871c66f78e"},
    {0x817BEBD0, 300, "eb7345651214aed32c4ad6a44a654aaa83685a6ce4fe1b9001d92accd39cd37d"},
    {0x817C0720, 100, "f17450c486eeb525a9cf1f7b1c1f09842831fc47772df45388aa908ff14b5d8b"},
    {0x817C12E0, 100, "64cb0779cd70513c93774cd92bcaa2e3d7f1d2a2cf96bd1d2eb3b80866b54758"},
    {0x8172D390, 164, "15f88a986a14963be0b342b2d3477b8091b165de9061444c3879aab1e52e7b5c"},
    {0x81792D88, 328, "83cd65a94c989808f2c0b9d06aa2da76766bd8b13884273cb1bfea7258717ec2"},
    {0x817925A0, 784, "1f699b880ad14a333c0d055ee045d5d50251af7a94eebcb45af5c7edae803f5f"},
    {0x817DE6F8, 192, "d524cfbba112ee534832bed33ba686999615a9ebbb59680e4c17d63eede232e8"},
    {0x817DE820, 160, "770ce68e06e7b07dc4d259e3b4efaa35c3dde19d7dcf08f53e46de23799d193b"},
    {0x817945A0, 212, "cc7f5a98b5bf7b28bd635df49b8b2c92e705fbe09ccdec06feb18dc88677fb20"},
    {0x81794880, 384, "1fe690da08163ed7cfdfe4d68328c13e21cff7b80efe93bff03d1d47ed6828e9"},
    {0x816A9E68, 104, "d1136d5fb6885fbb5107dd1178cb2d4e3be094d592c2c716091f1d27f96d51eb"},
};
// Full finite acquisition-pipeline code and immutable native interface bytes.
constexpr CodeProfile font_profiles[] = {
    {0x817925A0, 780, "9b601629e4990530aa324a575654ce8cd19d14645e1d9e1e5d603e09dc5b6eaf"},
    {0x8178F5C8, 276, "8cac0d740348614245ccb7e97ad84dad51ce98712ed94139530a991814ba38d9"},
    {0x8178EAD0, 108, "17e624dbc252a48663702f0dc7e58780428f0c474f19883606ff19e8920aeada"},
    {0x81791898, 560, "c7c99d64852c8483033cc2e4f6c74a2978151723cc07afafa827894c7e32829d"},
    {0x81783938, 192, "4739c715ed953815bc06b1d673ee02749c5496c880510f8b864155fc7726350b"},
    {0x81798218, 84, "90b51328ca3d6059c0751248880904732821251a10d7e23723abec6bc5a6520a"},
    {0x8178EED8, 1716, "f7881087ff5f3bfbf2ff349a552fb48015962b79c0f90a0e55598db6611c9576"},
    {0x817963B0, 312, "da0e80c3163c30e5b8127bbdad0f256aac3c24b46b8543b766f761f14d57de5b"},
    {0x817962E8, 200, "c08b85e702ae8e341c5e10041c08743272712ded68574a9cbb9fcd187e9a82fe"},
    {0x817960A8, 572, "7fa0a959197bda06fdb49baf4c1f353d2e44a3a28d550e7ddffab5a7fd53444e"},
    {0x81795B68, 192, "cabe3d12ca317ea805ed26ce5a577ed3900324e1b46992aa788caaf8366b7006"},
    {0x817A2B18, 196, "f9eaeccb16af0c26fadf2d6c7947f9e70c458bb40dfe1ca48c5092de6854db28"},
    {0x817A40F8, 1548, "ae9896cdfa57b78ba772e2cae6cd7fdf441b91c0d89b7ad06faea28c48a53247"},
    {0x81795038, 172, "541221acadaf7437b67598f5a23f58383657ff0f98ae0ae64b40188a86fda2b6"},
    {0x81794EE8, 112, "f0e0ed46c8cbce093582ebc5c1866bfe850b6e655ac212fdb2c5a1fecf47f826"},
    {0x81794E78, 112, "78461e887fc434a85d6bd54bc6b4b258e6685f93d64780a637900c9ba465fe7b"},
    {0x81791AC8, 864, "dc015dc844936ecce08b146e93bd619e2cfb777425458771dbef852fbc764e6a"},
    {0x817BEBD0, 300, "eb7345651214aed32c4ad6a44a654aaa83685a6ce4fe1b9001d92accd39cd37d"},
    {0x81792D88, 328, "83cd65a94c989808f2c0b9d06aa2da76766bd8b13884273cb1bfea7258717ec2"},
    {0x81792CB0, 216, "e4cabafd5e90572a1df01dc59c2a7041d93e6a177bb0980ade7c291d04d11a90"},
    {0x81A701E0, 52, "519a169d7f3987b18d9a4bb4e8a1942a6c53d5380d9229d890b039c8e1689bb7"},
    {0x8178EA98, 56, "95e1a88c760be82937876000c213edf70272ef6d0a7959cbb26d3b912c52b9e9"},
    {0x81790170, 288, "aee63988a33f8c81d83a9de5ebbddf77e225b0069671a6c9dc02a3c0cc3d2dc0"},
    {0x81791590, 160, "f76461014ae1afb3f329a8412830b652ec3e4502029216b4d40c30d39e561ac5"},
    {0x8178E330, 24, "e8e4bfa81ba42f2debab9a972b12bb4319b8bbb8ef85394e079fafd1dab09cf9"},
    {0x81792BA0, 176, "77d5deaa7f151f7c95b68bd45d296a81435eb49ba39f78b1b27aff66a17b67b2"},
    {0x817DE6F8, 192, "d524cfbba112ee534832bed33ba686999615a9ebbb59680e4c17d63eede232e8"},
    {0x817DE820, 124, "af1eab28058169f052b724be2098171a535f297e4fecdc0af3f9b9dff8d56abd"},
    {0x817DE8A0, 32, "b0d44d7cda946e9d0f06cc2b21fddef2c141e4d974c79ece651a044e4d534e62"},
    {0x817DE8C0, 140, "017c3681674bff48f91ad877f258cd153dc9abfa9e3339ea1630093d2a67aca7"},
    {0x8177F908, 228, "3f2603b560fc7da2536f589cb5af9ad6287f9c5b1d80f8546ac445272e7447a3"},
    {0x81783018, 140, "efd8cc5b94c066dedbe4c3736c083305aa7ce214132781896914fb7375fa166b"},
    {0x81780C50, 144, "50c758b75b16fe7db9fd4ec3396c8710a57824ef1a0575707fe1ec62af84fd5a"},
    {0x81780EA0, 68, "2513bcfb978061540e82ad85df5334eb3ff68f5edd102606deccc21b92fe69f8"},
    {0x81798108, 104, "80aa19c7fe53bafd7c0062d0d5a4edcfeb3b119ff45575617b4a1023ae5cb181"},
    {0x8177F760, 416, "b58d47aa38d6a529b51061c68ae5dd5e20aa0eed681fcbda81b55fe5a24d9e81"},
    {0x8179DAC0, 288, "68d7d3b92219a7752633a2d12dbddb804e0e515eb93fc9593158ff32903e26ac"},
    {0x8179DBE0, 16, "5ade784e29775342df4dbdd691b080700ec04f1773242312343e6655dd2127bf"},
    {0x8172D390, 164, "15f88a986a14963be0b342b2d3477b8091b165de9061444c3879aab1e52e7b5c"},
    {0x8178EA18, 4, "1bb3731b03b646b610933f77435ed74aadd55edcd7f6254fd7f485f395ddc27b"},
};
constexpr CodeProfile font_data_profiles[] = {
    {0x816052E0, 36, "8f27bbc263ecf27f031bc61cbfcab17b0e0f6b785755b6be4062c43c5a4fe0b8"},
    {0x81604AD0, 16, "b1a94034b2259d83d3f13bba22b07d0691228c21f9228c28216bb40ebb54dced"},
    {0x81604CC0, 4, "528cc4f668ed6e18b2b29c9192b752f8cc84f9b9b816e7908dd880a4b0a8f954"},
    {0x81604DCC, 4, "95057c85f5024eafbc31eb70f0215414d86d2e32483e235b551aafef07f87a99"},
    {0x81605408, 32, "0ff1ae009a6d6134c0ec46cc0fad2fd718d9abb8aaebb26a0a655262ab684b58"},
    {0x81605280, 8, "48976781d373b621bce1d46317c678ff7fed98bde65a5b0732052b8dfaea656f"},
    {0x815F411C, 10, "4cbdf569da7648a46896d549e670c46bdae72f0db2b278fe25057c4cd0ee70e9"},
};
// Diagnostic-only native sound contracts, never presentation startup admission.
constexpr CodeProfile cue_profiles[] = {
    {0x817DE300, 132, "17beb861b4e9badcac531a6f7a029fcb4e2efbab04b9bd45af99ce3bb0ef53f4"},
    {0x817DE388, 76, "06372586fa3210e090fbcd60bcff924f70d3fa2c020e2924653dbbb94012539a"},
    {0x817DE098, 608, "3b280ffc001eaf0a7d8b47a35aa6275f3b8eb98c619e5bb14d3171d1049a8168"},
    {0x817DDD38, 864, "389fb851e034ba657d3c6c8fa4e76853dac8ac5889a1ba9e42bbe0d09275f9fb"},
    {0x817DDAC8, 124, "1d7eec3292d9a3aa93151f93afacaaaf6a78542e83c79ffc684b1e99c98ba51f"},
    {0x817DDCA0, 148, "59140b9afb92720c223ced8e4eaf70e814b0aa7b927c51ca94d28ca6b7564afb"},
    {0x819A80F8, 248, "f0cc896eb725cca743905f9f8dbdadae90bab53926aa0683e4a19fd8b8fb1117"},
    {0x819A7B20, 216, "df3dab112d81d7e29b0b6ae1293af8f99d0de58bad0ecccc9ac9388cf5b49839"},
    {0x819A3D10, 92, "941431fb8f045b7b7b0b032c36bb1a970dd8bc4ad75562b7528ad6711465f313"},
    {0x819A3878, 96, "ee032e40abba22814e5c421759f8acab8dc7243cae83b276c959d67f713d7dd2"},
    {0x819A8500, 256, "f19ce538cdfb5156684adf13d0e2c5f85e4e5d509b228a2aecb2690c52f9345f"},
    {0x819A3930, 72, "ad12340325a8dc400b21b27df4858e1d5327c3ab3410452b6133fbd4b99f8664"},
    {0x819A3978, 84, "0f1880f932c89de37068aee8f934980772ab6a870cba009e22db351094ed8dde"},
    {0x819A38D8, 88, "2088294ec6c35da026197baf56120b26c1246b49b8b758a59dc1d58d084a49fe"},
    {0x819A3640, 100, "a9b4840f51b14c5f052201e9e428621b7889375bff6cc425894a7805143cc962"},
    {0x819AC540, 104, "095929927e72a49ebbe4898f87010761906d00e1d7062a617f691f60987286a2"},
    {0x819AC740, 96, "b9bc3f95797a8e287a3d9132884f8a05776ce69ab9914927bbbee08795235343"},
    {0x819A5878, 280, "716b730cc3dfb1aa95fd7e32dcb49a72aceadd2a90d5116c2b962b8b1bb9ef67"},
    {0x819A3B28, 212, "303932ff69b9dbedf3686dd7898ad813664e586f5c4ec96d10a3f6277eecd95c"},
    {0x819A3C00, 124, "8a03584dfe53cb315eb76c653e0401ba4d8b8e6eda2bdaf8099df9a9f9b02793"},
    {0x819A51F8, 1216, "8d844e7027f028b50b194416b6289a198d03ba2e40a147473140acec0e98a6df"},
    {0x819AE4E8, 12, "cca910647f9e6a6f833c58c572eef98767317d44bc2d4621850695710f7c0ab2"},
    {0x817DE2F8, 8, "8d208e5509b8df7e3696608de72be6fd0d3d26d8d306456cb02767afd28e8c85"},
    {0x8172D390, 164, "15f88a986a14963be0b342b2d3477b8091b165de9061444c3879aab1e52e7b5c"},
};
bool CheckCode(KernelState& kernel, UserModule& module,
               std::span<const CodeProfile> checked = profiles) {
  for (const auto& profile : checked) {
    auto* memory = kernel.memory();
    if (memory->QueryRangeAccess(profile.at, profile.bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint32_t at = profile.at; at < profile.at + profile.bytes; at += 4) {
      xex2_section_type type;
      if (kernel.GetUserModuleForAddress(at).get() != &module ||
          !module.xex_module()->GetPageSectionType(at, &type) ||
          type != XEX_SECTION_CODE ||
          module.xex_module()->TranslateCode(at) !=
              memory->TranslateVirtual<const uint8_t*>(at)) {
        return false;
      }
    }
    std::array<uint8_t, 32> hash{};
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(profile.at),
                   profile.bytes, hash.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < hash.size(); ++i) {
      if (profile.sha[i * 2] != hex[hash[i] >> 4] ||
          profile.sha[i * 2 + 1] != hex[hash[i] & 15]) {
        return false;
      }
    }
  }
  return true;
}
uint32_t CueOffset(uint32_t base, uint32_t offset) {
  return base && base <= UINT32_MAX - offset ? base + offset : 0;
}
std::optional<uint32_t> CueWord(Memory& memory, uint32_t at) {
  std::array<uint8_t, 4> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, at, bytes)) { return {}; }
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | bytes[3];
}
bool CheckCueHelpers(UserModule& module) {
  for (const unsigned first : {21u, 25u, 28u, 29u}) {
    for (const bool restore : {false, true}) {
      const uint32_t at = (restore ? 0x8172D3E0u : 0x8172D390u) +
                          (first - 14) * 4;
      auto* symbol = module.xex_module()->LookupSymbol(at, false);
      if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction) { return false; }
      auto* function = static_cast<cpu::Function*>(symbol);
      if (!function->IsSaverest() ||
          function->SaverestType() != cpu::SaveRestoreType::GPR ||
          function->SaverestIndex() != first ||
          (restore ? !function->IsRestore() : !function->IsSave()) ||
          function->behavior() != (restore ? cpu::Function::Behavior::kEpilogReturn
                                           : cpu::Function::Behavior::kProlog)) {
        return false;
      }
    }
  }
  return true;
}
bool CheckFontProfiles(KernelState& kernel, UserModule& module) {
  if (!CheckCode(kernel, module, font_profiles)) { return false; }
  for (unsigned first = 14; first <= 31; ++first) {
    for (bool restore : {false, true}) {
      const uint32_t at = (restore ? 0x8172D3E0u : 0x8172D390u) + (first - 14) * 4;
      auto* symbol = module.xex_module()->LookupSymbol(at, false);
      if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction ||
          symbol->address() != at || symbol->module() != module.xex_module()) { return false; }
      auto* function = static_cast<cpu::Function*>(symbol);
      if (!function->IsSaverest() || function->SaverestType() != cpu::SaveRestoreType::GPR ||
          function->SaverestIndex() != first ||
          (restore ? !function->IsRestore() : !function->IsSave()) ||
          function->behavior() != (restore ? cpu::Function::Behavior::kEpilogReturn
                                          : cpu::Function::Behavior::kProlog)) { return false; }
    }
  }
  for (const auto& profile : font_data_profiles) {
    std::array<uint8_t, 256> bytes{};
    xex2_section_type section;
    if (!profile.bytes || profile.bytes > bytes.size() ||
        kernel.GetUserModuleForAddress(profile.at).get() != &module ||
        kernel.GetUserModuleForAddress(profile.at + profile.bytes - 1).get() != &module ||
        !module.xex_module()->GetPageSectionType(profile.at, &section) ||
        (section != XEX_SECTION_DATA && section != XEX_SECTION_READONLY_DATA) ||
        !internal::ReadNativeWaitSnapshot(*kernel.memory(), profile.at,
            std::span<uint8_t>(bytes).first(profile.bytes))) { return false; }
    std::array<uint8_t, 32> hash{};
    mbedtls_sha256(bytes.data(), profile.bytes, hash.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i != hash.size(); ++i) {
      if (profile.sha[i * 2] != hex[hash[i] >> 4] ||
          profile.sha[i * 2 + 1] != hex[hash[i] & 15]) { return false; }
    }
  }
  return true;
}
bool CheckMeasureHelpers(UserModule& module) {
  auto check = [&](uint32_t address, bool restore) {
    auto* symbol = module.xex_module()->LookupSymbol(address, false);
    if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction) { return false; }
    auto* function = static_cast<cpu::Function*>(symbol);
    return function->IsSaverest() &&
           function->SaverestType() == cpu::SaveRestoreType::GPR &&
           function->SaverestIndex() == 25 &&
           (restore ? function->IsRestore() : function->IsSave()) &&
           function->behavior() == (restore ? cpu::Function::Behavior::kEpilogReturn
                                            : cpu::Function::Behavior::kProlog);
  };
  return check(0x8172D3BC, false) && check(0x8172D40C, true);
}
std::string MeasureTextHex(const notification_detail::TextSenderText& text) {
  std::string result;
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < text.count && i < text.units.size(); ++i) {
    for (int shift : {12, 8, 4, 0}) { result.push_back(digits[(text.units[i] >> shift) & 15]); }
  }
  return result;
}
template <size_t N>
std::string MeasureWordsHex(const std::array<uint32_t, N>& words) {
  std::string result;
  constexpr char digits[] = "0123456789abcdef";
  for (const auto word : words) {
    for (int shift = 28; shift >= 0; shift -= 4) { result.push_back(digits[(word >> shift) & 15]); }
  }
  return result;
}
void EmitMeasureDiagnostic(const char* event, Memory& memory,
    const notification_detail::TextMeasureEntry& entry,
    const notification_detail::TextSenderEntry& caption, uint32_t ordinal,
    bool result_known, uint32_t hresult, std::optional<uint32_t> task) {
  // Every callback reacquires bounded RAM snapshots. These copied words are
  // neither an atomic object snapshot nor an object/provider/GPU authority.
  const auto state = notification_detail::ReadTextMeasureState(memory, entry.state);
  const auto face = notification_detail::ReadTextSenderText(
      memory, state.readable ? state.words[4] : 0);
  const auto font = notification_detail::ReadTextMeasureFont(
      memory, state.readable ? state.words[5] : 0);
  const auto bounds = notification_detail::ReadTextMeasureBounds(memory, entry.output);
  const auto& id = entry.identity;
  XELOGI("NativeSystemUITrace {{\"schema\":1,\"component\":\"notification_text_measure\","
         "\"event\":\"{}\",\"ordinal\":{},\"kernel\":{},\"domain\":{},"
         "\"root_generation\":{},\"foreground\":{},\"thread\":{},\"task_known\":{},\"task\":{},\"pcr\":{},"
         "\"sp\":{},\"caller\":{},\"state\":{},\"object\":{},\"text_pointer\":{},"
         "\"output\":{},\"count_raw\":{},\"sender_handle\":{},\"sender_thread\":{},"
         "\"sender_task_known\":{},\"sender_task\":{},"
         "\"same_sender_object\":{},\"same_sender_text_pointer\":{},"
         "\"text_units\":{},\"utf16be_hex\":\"{}\",\"result_known\":{},\"hresult\":{},"
         "\"state_readable\":{},\"state_words_be_hex\":\"{}\","
         "\"face_terminated\":{},\"face_read_failed\":{},\"face_utf16be_hex\":\"{}\","
         "\"cache_readable\":{},\"cache_words_be_hex\":\"{}\","
         "\"descriptor_readable\":{},\"descriptor_words_be_hex\":\"{}\","
         "\"name0_hex\":\"{}\",\"name1_hex\":\"{}\",\"name2_hex\":\"{}\",\"name3_hex\":\"{}\","
         "\"names_terminated\":[{},{},{},{}],\"names_read_failed\":[{},{},{},{}],"
         "\"bounds_readable\":{},\"bounds_float_bits_be_hex\":\"{}\"}}",
         event, ordinal, id.kernel, id.domain, id.root_generation, id.foreground,
         id.thread, bool(task), task.value_or(0), id.pcr, id.sp, id.lr, entry.state, entry.object,
         entry.text_pointer, entry.output, entry.count, caption.handle,
         caption.identity.thread, caption.task_known, caption.task, entry.object == caption.handle,
         entry.text_pointer == caption.text_pointer, entry.text.count,
         MeasureTextHex(entry.text), result_known, hresult,
         state.readable, MeasureWordsHex(state.words), face.terminated,
         face.read_failed, MeasureTextHex(face), font.cache_readable,
         MeasureWordsHex(font.cache_words), font.descriptor_readable,
         MeasureWordsHex(font.descriptor), MeasureTextHex(font.names[0]),
         MeasureTextHex(font.names[1]), MeasureTextHex(font.names[2]), MeasureTextHex(font.names[3]),
         font.names[0].terminated, font.names[1].terminated, font.names[2].terminated, font.names[3].terminated,
         font.names[0].read_failed, font.names[1].read_failed, font.names[2].read_failed, font.names[3].read_failed,
         bounds.readable, MeasureWordsHex(bounds.bits));
}
notification_detail::FontAcquireEntry ReadFontAcquireEntry(
    Memory& memory, const cpu::ppc::PPCContext& ctx,
    const notification_detail::FontAcquireIdentity& identity,
    uint32_t ordinal, uint32_t parent) {
  using namespace notification_detail;
  FontAcquireEntry entry;
  entry.identity = identity;
  entry.ordinal = ordinal;
  entry.parent = parent;
  entry.gcrc_held = global_critical_region::is_held_by_current_thread();
  for (size_t i = 0; i != entry.arguments.size(); ++i) { entry.arguments[i] = ctx.r[3 + i]; }
  entry.f1_bits = std::bit_cast<uint64_t>(ctx.f[1]);
  const auto argument = [&](size_t i) { return Pointer(entry.arguments[i]).value_or(0); };
  const auto site = FindFontAcquireSite(identity.function)->site;
  using S = FontAcquireSite;
  switch (site) {
    case S::kCacheCtor:
      entry.descriptor = ReadFontAcquireDescriptor(memory, argument(1));
      entry.engine = ReadFontAcquireEngine(memory);
      entry.object = ReadFontAcquireWords(memory, argument(0), 19);
      if (entry.object.readable) { entry.graph = ReadFontAcquireGraph(memory, entry.object.words[15]); }
      break;
    case S::kEngineCreate: case S::kEngineMetrics:
      entry.engine = ReadFontAcquireEngine(memory);
      // Receiver is the actual native call argument; retain the separately
      // sampled global cell even if it changed. No identity is inferred.
      entry.engine.object = ReadFontAcquireWords(memory, argument(0), 8);
      entry.engine.table = entry.engine.object.readable
          ? ReadFontAcquireWords(memory, entry.engine.object.words[0], 9) : FontAcquireWords{};
      if (site == S::kEngineCreate) {
        entry.descriptor = ReadFontAcquireDescriptor(memory, argument(1));
        entry.output_address = argument(5);  // r8; f1 occupies the r5 ABI slot.
        entry.output_words = 1;
      } else {
        entry.output_address = argument(2);  // r5, six-float metrics.
        entry.output_words = 6;
      }
      break;
    case S::kAtlas: case S::kGlyphInit:
      entry.object = ReadFontAcquireWords(memory, argument(0), 19);
      if (entry.object.readable) { entry.graph = ReadFontAcquireGraph(memory, entry.object.words[15]); }
      break;
    case S::kRenderAllocate:
      entry.object = ReadFontAcquireWords(memory, argument(0), 8);
      entry.graph = ReadFontAcquireGraph(memory, argument(0));
      entry.output_address = argument(3);  // r6.
      entry.output_words = 1;
      break;
    case S::kBackendAllocate: {
      entry.object = ReadFontAcquireWords(memory, argument(0), 8);
      entry.graph.backend = argument(0);
      if (entry.object.readable) {
        entry.graph.backend_table = entry.object.words[0];
        const auto target = ReadFontAcquireWords(memory,
            FontAcquireOffset(entry.graph.backend_table, 0x1C), 1);
        entry.graph.backend_known = target.readable;
        entry.graph.allocate_target = target.words[0];
      }
      entry.output_address = argument(7);  // r10, not a stack argument.
      entry.output_words = 1;
      break;
    }
    case S::kFaceLoad:
      entry.object = ReadFontAcquireWords(memory, argument(0), 13);
      entry.descriptor = ReadFontAcquireDescriptor(memory, argument(1));
      break;
    case S::kFileLoad:
      entry.filename = ReadTextSenderText(memory, argument(0));
      entry.output_address = argument(1);
      entry.output_words = 1;
      break;
    case S::kFileInit: case S::kContainer:
      entry.object = ReadFontAcquireWords(memory, argument(0), 12);
      break;
    case S::kVerify:
      entry.output_address = argument(3);  // caller-owned20B hash at r6.
      entry.output_words = 5;
      break;
    case S::kDecompress:
      entry.output_address = argument(1);  // in/out produced-size DWORD.
      entry.output_words = 1;
      break;
    case S::kParse:
      entry.object = ReadFontAcquireWords(memory, argument(0), 8);
      break;
    case S::kFaceInstance:
      entry.output_address = argument(4);  // r7 after f1's r4 ABI slot.
      entry.output_words = 1;
      break;
    case S::kHeaderRead: case S::kHeaderCompat:
      entry.output_address = argument(1);
      entry.output_words = 64;
      break;
  }
  return entry;
}
notification_detail::FontAcquireResult ReadFontAcquireReturn(
    Memory& memory, notification_detail::FontAcquireEntry entry, uint32_t result) {
  using namespace notification_detail;
  FontAcquireResult value;
  value.raw_result = result;
  value.entry = std::move(entry);
  const auto& original = value.entry;
  if (original.output_words) {
    value.output = ReadFontAcquireWords(memory, original.output_address, original.output_words);
  }
  const auto site = FindFontAcquireSite(original.identity.function)->site;
  const auto receiver = Pointer(original.arguments[0]).value_or(0);
  using S = FontAcquireSite;
  switch (site) {
    case S::kCacheCtor: case S::kAtlas: case S::kGlyphInit:
      value.object_after = ReadFontAcquireWords(memory, receiver, 19); break;
    case S::kFaceLoad:
      value.object_after = ReadFontAcquireWords(memory, receiver, 13); break;
    case S::kFileInit: case S::kContainer:
      value.object_after = ReadFontAcquireWords(memory, receiver, 12); break;
    case S::kParse:
      value.object_after = ReadFontAcquireWords(memory, receiver, 8); break;
    case S::kFileLoad:
      // Only a success output owns a live retained file context. Failures may
      // already have destroyed their private candidate and are never followed.
      if (!result && value.output.readable && value.output.words[0]) {
        value.object_after = ReadFontAcquireWords(memory, value.output.words[0], 12);
      }
      break;
    default: break;
  }
  return value;
}
std::string FontAcquireHex(const notification_detail::FontAcquireWords& value) {
  std::string text;
  if (!value.readable) { return text; }
  text.reserve(value.count * 8);
  for (uint32_t i = 0; i < value.count; ++i) { text += fmt::format("{:08X}", value.words[i]); }
  return text;
}
std::string FontArgumentsHex(const std::array<uint64_t, 8>& values) {
  std::string text;
  text.reserve(values.size() * 16);
  for (uint64_t value : values) { text += fmt::format("{:016X}", value); }
  return text;
}
void EmitFontAcquireReport(const notification_detail::FontAcquireReport& report) {
  const auto& m = report.measure;
  const auto& id = m.identity;
  const auto& engine = report.engine_at_measure;
  XELOGI("NativeSystemUITrace {{\"schema\":1,\"component\":\"notification_font_acquire\","
         "\"event\":\"common_return\",\"kernel\":{},\"domain\":{},\"root_generation\":{},"
         "\"foreground\":{},\"thread\":{},\"pcr\":{},\"sp\":{},\"lr\":{},\"task\":{},\"return_task\":{},"
         "\"ordinal\":{},\"state\":{},\"object\":{},\"text_pointer\":{},\"text_units\":{},"
         "\"count_raw\":{},\"bounds_address\":{},\"bounds_readable\":{},\"bounds_raw\":\"{}\","
         "\"raw_result\":{},\"normal_return\":{},\"completed\":{},\"pending_at_end\":{},"
         "\"canceled\":{},\"ambiguous\":{},\"overflow\":{},"
         "\"state_entry_readable\":{},\"state_entry\":\"{}\","
         "\"state_return_readable\":{},\"state_return\":\"{}\","
         "\"engine_cell_readable\":{},\"engine_cell\":\"{}\","
         "\"engine_object_readable\":{},\"engine_object\":\"{}\","
         "\"engine_table_readable\":{},\"engine_table\":\"{}\","
         "\"default_name_readable\":{},\"default_name_cell\":\"{}\","
         "\"default_name\":\"{}\",\"default_name_terminated\":{},\"default_name_failed\":{},"
         "\"font_or_pixel_acceptance\":false}}",
         id.kernel, id.domain, id.root_generation, id.foreground, id.thread, id.pcr,
         id.sp, id.lr, report.task, report.return_task, report.measure_ordinal, m.state, m.object,
         m.text_pointer, m.text.count, m.count, m.output, report.bounds_at_return.readable,
         MeasureWordsHex(report.bounds_at_return.bits), report.common_result, report.normal_return,
         report.completed, report.pending_at_end, report.canceled, report.ambiguous, report.overflow,
         report.state_at_measure.readable, MeasureWordsHex(report.state_at_measure.words),
         report.state_at_return.readable, MeasureWordsHex(report.state_at_return.words),
         engine.cell.readable, FontAcquireHex(engine.cell), engine.object.readable,
         FontAcquireHex(engine.object), engine.table.readable, FontAcquireHex(engine.table),
         engine.default_name_cell.readable, FontAcquireHex(engine.default_name_cell),
         MeasureTextHex(engine.default_name), engine.default_name.terminated, engine.default_name.read_failed);
  for (uint32_t i = 0; i != report.completed; ++i) {
    const auto& value = report.results[i];
    const auto& entry = value.entry;
    const auto& graph = entry.graph;
    const auto* site = notification_detail::FindFontAcquireSite(entry.identity.function);
    XELOGI("NativeSystemUITrace {{\"schema\":1,\"component\":\"notification_font_acquire\","
           "\"event\":\"stage_return\",\"measure_ordinal\":{},\"task\":{},\"sequence\":{},"
           "\"ordinal\":{},\"parent\":{},\"function\":{},\"site\":{},\"sp\":{},\"lr\":{},"
           "\"arguments\":\"{}\",\"f1_bits\":\"{:016X}\",\"entry_gcrc_held\":{},"
           "\"raw_result\":{},\"output_address\":{},\"output_readable\":{},\"output_words\":{},\"output\":\"{}\","
           "\"object_readable\":{},\"object\":\"{}\",\"object_after_readable\":{},\"object_after\":\"{}\","
           "\"descriptor_readable\":{},\"descriptor\":\"{}\",\"name0\":\"{}\",\"name1\":\"{}\","
           "\"name2\":\"{}\",\"name3\":\"{}\",\"names_terminated\":[{},{},{},{}],\"names_failed\":[{},{},{},{}],"
           "\"filename\":\"{}\",\"filename_terminated\":{},\"filename_failed\":{},"
           "\"engine_cell_readable\":{},\"engine_cell\":\"{}\",\"engine_object_readable\":{},\"engine_object\":\"{}\","
           "\"engine_table_readable\":{},\"engine_table\":\"{}\","
           "\"device\":{},\"device_table\":{},\"atlas_target\":{},\"device_known\":{},\"atlas_known\":{},"
           "\"backend\":{},\"backend_table\":{},\"allocate_target\":{},\"backend_known\":{},"
           "\"font_or_pixel_acceptance\":false}}",
           report.measure_ordinal, report.task, i, entry.ordinal, entry.parent,
           entry.identity.function, uint32_t(site->site), entry.identity.sp, entry.identity.lr,
           FontArgumentsHex(entry.arguments), entry.f1_bits, entry.gcrc_held, value.raw_result,
           entry.output_address, value.output.readable, value.output.count, FontAcquireHex(value.output),
           entry.object.readable, FontAcquireHex(entry.object), value.object_after.readable, FontAcquireHex(value.object_after),
           entry.descriptor.data.readable, FontAcquireHex(entry.descriptor.data),
           MeasureTextHex(entry.descriptor.names[0]), MeasureTextHex(entry.descriptor.names[1]),
           MeasureTextHex(entry.descriptor.names[2]), MeasureTextHex(entry.descriptor.names[3]),
           entry.descriptor.names[0].terminated, entry.descriptor.names[1].terminated,
           entry.descriptor.names[2].terminated, entry.descriptor.names[3].terminated,
           entry.descriptor.names[0].read_failed, entry.descriptor.names[1].read_failed,
           entry.descriptor.names[2].read_failed, entry.descriptor.names[3].read_failed,
           MeasureTextHex(entry.filename), entry.filename.terminated, entry.filename.read_failed,
           entry.engine.cell.readable, FontAcquireHex(entry.engine.cell), entry.engine.object.readable,
           FontAcquireHex(entry.engine.object), entry.engine.table.readable, FontAcquireHex(entry.engine.table),
           graph.device, graph.table, graph.atlas_target, graph.device_known, graph.atlas_known,
           graph.backend, graph.backend_table, graph.allocate_target, graph.backend_known);
  }
}
std::optional<Modal> ReadModal(KernelState* kernel) {
  auto current = NativeSystemUiNotifications::Lookup(kernel);
  if (!current || !current->IsAccepting()) {
    return {};
  }
  const auto status = current->snapshot();
  if (!status.accepting || status.poisoned) {
    return {};
  }
  return Modal{status.known, status.last_sequence,
               status.foreground_generation};
}
}  // namespace
struct NativeNotificationPresentationObserver::State {
  enum class Stage { kQuery, kDraw, kRecord, kClear };
  struct Operation {
    std::shared_ptr<State> state;
    NativeImportDomain::Operation domain;
    ~Operation() {
      domain = {};
      if (state) {
        std::lock_guard lock(state->mutex);
        --state->active;
        state->changed.notify_all();
      }
    }
  };
  Config config;
  KernelState* kernel;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  std::mutex mutex;
  std::condition_variable changed;
  size_t active = 0;
  bool stopping = false, retired = false;
  Protocol protocol;
  notification_detail::TextSenderDiagnostic text_diagnostic;
  notification_detail::TextMeasureDiagnostic measure_diagnostic;
  notification_detail::FontAcquireDiagnostic font_diagnostic;
  std::optional<notification_detail::TextMeasureIdentity> ReadMeasureIdentity(
      const cpu::ppc::PPCContext& ctx) {
    if (!XThread::IsInThread()) { return {}; }
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]);
    const uint32_t caller = uint32_t(ctx.lr);
    if (!sp || !pcr || *pcr != thread->pcr_ptr() ||
        (caller != notification_detail::kTextMeasureWrapperCaller &&
         caller != notification_detail::kTextMeasureExportCaller) ||
        thread->thread_state()->context() != &ctx ||
        ctx.kernel_state != kernel || ctx.processor != kernel->processor() ||
        ctx.thread_state != thread->thread_state() || thread->kernel_state() != kernel ||
        thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
        kernel->ResolveNativeImportCaller(caller).domain != config.domain ||
        kernel->GetUserModuleForAddress(caller - 4).get() != config.catalog->calls()->module().get() ||
        kernel->GetUserModuleForAddress(notification_detail::kTextMeasureAddress).get() !=
            config.catalog->calls()->module().get()) { return {}; }
    const auto foreground = kernel->emulator()->title_execution_state().snapshot();
    if (!kernel->emulator()->OwnsTitleExecution(kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning || !foreground.generation) { return {}; }
    notification_detail::TextMeasureIdentity identity{
        uint64_t(reinterpret_cast<uintptr_t>(kernel)), config.domain->id(),
        config.domain->root().generation, foreground.generation,
        thread->thread_id(), *pcr, *sp, caller};
    return identity.valid() ? std::optional(identity) : std::nullopt;
  }
  std::optional<notification_detail::FontAcquireIdentity> ReadFontIdentity(
      const cpu::ppc::PPCContext& ctx, uint32_t function) {
    notification_detail::TextMeasureIdentity measure;
    uint32_t task, ordinal;
    {
      std::lock_guard lock(mutex);
      if (!font_diagnostic.accepting() || !measure_diagnostic.pending()) { return {}; }
      const auto* active = font_diagnostic.active();
      measure = active->measure.identity;
      task = active->task;
      ordinal = active->measure_ordinal;
    }
    if (!XThread::IsInThread()) { return {}; }
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]), lr = Pointer(ctx.lr);
    if (!sp || !pcr || !lr || *lr < 4 || *pcr != thread->pcr_ptr() ||
        thread->thread_id() != measure.thread || *pcr != measure.pcr ||
        thread->thread_state()->context() != &ctx || ctx.kernel_state != kernel ||
        ctx.processor != kernel->processor() || ctx.thread_state != thread->thread_state() ||
        thread->kernel_state() != kernel ||
        thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
        kernel->ResolveNativeImportCaller(*lr).domain != config.domain ||
        kernel->GetUserModuleForAddress(*lr - 4).get() != config.catalog->calls()->module().get() ||
        kernel->GetUserModuleForAddress(function).get() != config.catalog->calls()->module().get()) { return {}; }
    const auto foreground = kernel->emulator()->title_execution_state().snapshot();
    if (!kernel->emulator()->OwnsTitleExecution(kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning ||
        foreground.generation != measure.foreground || ReadTextTask() != std::optional<uint32_t>(task)) { return {}; }
    notification_detail::FontAcquireIdentity identity{measure, ordinal, task, function, *sp, *lr};
    return identity.valid() ? std::optional(identity) : std::nullopt;
  }
  struct CueControl {
    uint32_t derived = 0, hxui = 0;
    uint64_t serial = 0;
    bool live = false;
  };
  std::array<CueControl, 256> cue_controls{};
  uint64_t cue_control_serial = 0, cue_request_serial = 0, cue_start_serial = 0;
  uint64_t cue_publication_serial = 0;
  notification_detail::NativeCueCalls cue_calls;
  std::shared_ptr<const apu::NativeAudioClientKey> cue_registration;
  uint32_t cue_engine = 0, cue_driver = 0;
  bool cue_claimed = false, cue_closed = false, cue_metadata_lost = false;
  bool cue_code_validated = false;
  bool cue_selected_retiring = false;
  NativeCueObservationResult cue_result;
  std::shared_ptr<apu::NativeAudioObservation> cue_capture;
  CueControl* FindCueControl(uint32_t derived) {
    for (auto& control : cue_controls) {
      if (control.live && control.derived == derived) { return &control; }
    }
    return nullptr;
  }
  void LoseCueMetadata() {
    cue_metadata_lost = cue_result.metadata_lost = true;
    cue_calls.Stop();
    if (cue_capture) { cue_capture->Fail(); }
  }
  void RecordCueEvent(const notification_detail::NativeCueCall& call, uint32_t result) {
    if (!cue_result.armed || cue_closed) { return; }
    if (cue_result.event_count == cue_result.events.size()) {
      cue_result.events_overflow = true;
      LoseCueMetadata();
      return;
    }
    cue_result.events[cue_result.event_count++] = {call, result};
  }
  std::optional<notification_detail::NativeCueCallIdentity> ReadCueIdentity(
      const cpu::ppc::PPCContext& ctx, uint32_t function) {
    if (!XThread::IsInThread()) { return {}; }
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]), caller = Pointer(ctx.lr);
    if (!sp || !pcr || !caller || *caller < 4 || *pcr != thread->pcr_ptr() ||
        thread->thread_state()->context() != &ctx || ctx.kernel_state != kernel ||
        ctx.processor != kernel->processor() || ctx.thread_state != thread->thread_state() ||
        thread->kernel_state() != kernel ||
        thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
        kernel->ResolveNativeImportCaller(*caller).domain != config.domain ||
        kernel->GetUserModuleForAddress(*caller - 4).get() != config.catalog->calls()->module().get() ||
        kernel->GetUserModuleForAddress(function).get() != config.catalog->calls()->module().get()) {
      return {};
    }
    const auto foreground = kernel->emulator()->title_execution_state().snapshot();
    notification_detail::NativeCueCallIdentity identity{
        uint64_t(reinterpret_cast<uintptr_t>(kernel)), config.domain->id(),
        config.domain->root().generation,
        foreground.phase == TitleExecutionState::Phase::kRunning ? foreground.generation : 0,
        thread->thread_id(), *pcr, *sp, *caller, function};
    return identity.valid() ? std::optional(identity) : std::nullopt;
  }
  bool ValidateCueCode() {
    {
      std::lock_guard lock(mutex);
      if (cue_metadata_lost) { return false; }
      if (cue_code_validated) { return true; }
    }
    bool valid;
    {
      auto global = global_critical_region::AcquireDirect();
      auto& module = *config.catalog->calls()->module();
      valid = CheckCode(*kernel, module, cue_profiles) && CheckCueHelpers(module);
    }
    std::lock_guard lock(mutex);
    if (!valid) { LoseCueMetadata(); }
    else { cue_code_validated = true; }
    return valid && !cue_metadata_lost;
  }
  bool ReadCueDriver(uint32_t driver) {
    auto& memory = *kernel->memory();
    const auto table = CueWord(memory, driver);
    const auto submit = table ? CueWord(memory, CueOffset(*table, 0x18)) : std::nullopt;
    const auto registration = table ? CueWord(memory, CueOffset(*table, 0x1C)) : std::nullopt;
    if (!driver || !submit || *submit != 0x819AC740 ||
        !registration || *registration != 0x819AC540) { return false; }
    auto module = config.catalog->calls()->module();
    if (kernel->GetUserModuleForAddress(*submit).get() != module.get() ||
        kernel->GetUserModuleForAddress(*registration).get() != module.get()) { return false; }
    return true;
  }
  bool ReadCueEngine(uint32_t& engine, uint32_t& driver) {
    auto& memory = *kernel->memory();
    const auto e = CueWord(memory, 0x81B5B5E4);
    const auto wrapper = e ? CueWord(memory, CueOffset(*e, 0x40)) : std::nullopt;
    const auto d = wrapper ? CueWord(memory, CueOffset(*wrapper, 0x44)) : std::nullopt;
    if (!e || !*e || !d || !ReadCueDriver(*d)) { return false; }
    engine = *e;
    driver = *d;
    return true;
  }
  std::optional<notification_detail::TextSenderIdentity> ReadTextIdentity(
      const cpu::ppc::PPCContext& ctx) {
    if (!XThread::IsInThread()) { return {}; }
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]);
    if (!sp || !pcr || *pcr != thread->pcr_ptr() ||
        uint32_t(ctx.lr) != notification_detail::kTextSenderCaller ||
        thread->thread_state()->context() != &ctx ||
        ctx.kernel_state != kernel || ctx.processor != kernel->processor() ||
        ctx.thread_state != thread->thread_state() ||
        thread->kernel_state() != kernel ||
        thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
        kernel->ResolveNativeImportCaller(uint32_t(ctx.lr)).domain != config.domain ||
        kernel->GetUserModuleForAddress(uint32_t(ctx.lr) - 4).get() !=
            config.catalog->calls()->module().get() ||
        kernel->GetUserModuleForAddress(notification_detail::kTextSenderAddress).get() !=
            config.catalog->calls()->module().get()) { return {}; }
    const auto foreground = kernel->emulator()->title_execution_state().snapshot();
    if (!kernel->emulator()->OwnsTitleExecution(kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning ||
        !foreground.generation) { return {}; }
    notification_detail::TextSenderIdentity identity{
        uint64_t(reinterpret_cast<uintptr_t>(kernel)), config.domain->id(),
        config.domain->root().generation, foreground.generation,
        thread->thread_id(), *pcr, *sp, uint32_t(ctx.lr)};
    return identity.valid() ? std::optional(identity) : std::nullopt;
  }
  std::optional<uint32_t> ReadTextTask() {
    auto global = global_critical_region::AcquireDirect();
    auto& memory = *kernel->memory();
    auto word = [&](uint32_t at) -> std::optional<uint32_t> {
      std::array<uint8_t, 4> raw;
      if (!internal::ReadNativeWaitSnapshot(memory, at, raw)) { return {}; }
      return xe::load_and_swap<uint32_t>(raw.data());
    };
    // Actual current-thread native TLS indirection, not a fabricated Task or
    // an assertion that this pointer grants object/work ownership.
    const auto slot = word(0x81A82570);
    uint32_t tls = 0;
    auto* thread = XThread::GetCurrentThread();
    if (!slot || !thread->GetTLSValue(*slot, &tls)) { return {}; }
    const auto work = word(tls);
    return work && *work && *work <= UINT32_MAX - 0xC
               ? word(*work + 0xC) : std::nullopt;
  }

  State(Config value, KernelState* actual)
      : config(std::move(value)),
        kernel(actual),
        protocol(config.catalog->allocation()->owner(), config.domain->id(),
                 config.domain->root().generation) {
    if (cvars::native_system_ui_trace) { font_diagnostic.Prepare(); }
  }
  bool AcquireInto(const std::shared_ptr<State>& self, Operation& operation) {
    {
      std::lock_guard lock(mutex);
      if (stopping || retired || !protocol.accepting() || active == SIZE_MAX) {
        return false;
      }
      ++active;
      operation.state = self;
    }
    operation.domain = config.domain->Acquire();
    return bool(operation.domain) && config.catalog->allocation()->IsAccepting();
  }
  std::shared_ptr<Operation> Acquire(const std::shared_ptr<State>& self) {
    auto operation = std::make_shared<Operation>();
    return AcquireInto(self, *operation) ? operation : nullptr;
  }
  std::optional<Sample> Read(
      const cpu::ppc::PPCContext& ctx, Stage stage,
      uint32_t out_display_info = 0,
      const gpu::SystemCommandDescriptor* descriptor = nullptr) {
    if (!XThread::IsInThread()) {
      return {};
    }
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]);
    const uint32_t lr = uint32_t(ctx.lr);
    if (!sp || !*sp || (*sp & 15) || !pcr || *pcr != thread->pcr_ptr() ||
        thread->thread_state()->context() != &ctx ||
        ctx.kernel_state != kernel || ctx.processor != kernel->processor() ||
        ctx.thread_state != thread->thread_state() ||
        thread->kernel_state() != kernel ||
        thread->native_frame_thread_identity().process_type !=
            X_PROCTYPE_SYSTEM ||
        kernel->ResolveNativeImportCaller(lr).domain != config.domain ||
        kernel->GetUserModuleForAddress(lr - 4).get() !=
            config.catalog->calls()->module().get()) {
      return {};
    }
    if ((stage == Stage::kQuery && lr != 0x816CD70C) ||
        (stage == Stage::kDraw && lr != 0x816CB410) ||
        ((stage == Stage::kRecord || stage == Stage::kClear) &&
         lr != 0x81842BFC)) {
      return {};
    }
    const auto foreground =
        kernel->emulator()->title_execution_state().snapshot();
    if (!kernel->emulator()->OwnsTitleExecution(kernel) ||
        foreground.phase != TitleExecutionState::Phase::kRunning ||
        !foreground.generation) {
      return {};
    }
    const auto modal = ReadModal(kernel);
    if (!modal) {
      return {};
    }
    Sample sample;
    sample.owner = config.catalog->allocation()->owner();
    sample.domain = config.domain->id();
    sample.root_generation = config.domain->root().generation;
    sample.foreground = foreground.generation;
    sample.thread = thread->thread_id();
    sample.modal = *modal;
    {
      auto global = global_critical_region::AcquireDirect();
      auto& memory = *kernel->memory();
      auto word = [&](uint32_t at) { return Word(memory, at); };
      const auto slot = word(0x81A82570);
      uint32_t tls = 0;
      if (!slot || !thread->GetTLSValue(*slot, &tls)) {
        return {};
      }
      const auto work = word(tls);
      if (!work || *work > UINT32_MAX - 0xC) {
        return {};
      }
      const auto task = word(*work + 0xC),
                 tagged =
                     word(config.catalog->symbols().manager_startup_task_cell);
      if (!task || !tagged || !*task || (*task & 3) ||
          *task > UINT32_MAX - 0x30 || *tagged != (*task | 1u) ||
          word(*task + 0x30) != 0x816CE108u) {
        return {};
      }
      sample.task = *tagged;
      const auto counter = word(0x81AA1B10);
      if (!counter) {
        return {};
      }
      sample.counter = *counter;
      uint64_t frame_sp = 0;
      if (stage == Stage::kQuery) {
        if (*sp < 0x140) {
          return {};
        }
        frame_sp = *sp - 0x140;
      } else if (stage == Stage::kDraw) {
        frame_sp = uint64_t(*sp) + 0x4A0;
        if (uint64_t(*sp) + 0x4A0 > UINT32_MAX ||
            out_display_info != uint64_t(*sp) + 0x1B0 ||
            memory.QueryRangeAccess(out_display_info, 0x58) ==
                memory::PageAccess::kNoAccess ||
            word(*sp + 0x348) != 0x816CE0D0u ||
            word(*sp + 0x3C8) != 0x816AB56Cu ||
            word(*sp + 0x498) != 0x816CD3F8u || ctx.r[15]) {
          return {};
        }
        const auto manager = word(0x81AA1B54),
                   dc = word(config.catalog->symbols().dc_cell);
        const auto scene = Pointer(ctx.r[21]);
        if (!manager || !*manager || *manager > UINT32_MAX - 0xF0 || !dc ||
            !*dc || !scene || !*scene || word(*manager) != 0x815F87D4u ||
            word(*manager + 0xC) != dc || word(*manager + 0x98) != scene) {
          return {};
        }
      } else {
        if (!descriptor || uint64_t(*sp) + 0x190 > UINT32_MAX ||
            Pointer(ctx.r[3]) != std::optional<uint32_t>(*sp + 0x50) ||
            memory.QueryRangeAccess(*sp + 0x50,
                                    gpu::SystemCommandDescriptor::kSize) ==
                memory::PageAccess::kNoAccess) {
          return {};
        }
        const auto actual = gpu::SystemCommandDescriptor::Copy(
            {memory.TranslateVirtual<const uint8_t*>(*sp + 0x50),
             gpu::SystemCommandDescriptor::kSize});
        if (!actual || *actual != *descriptor) {
          return {};
        }
        if (stage == Stage::kRecord) {
          if (descriptor->empty() || word(*sp + 0x118) != 0x816CD678u) {
            return {};
          }
          frame_sp = uint64_t(*sp) + 0x120;
        } else {
          const auto parent = word(*sp + 0x188);
          if (!descriptor->empty() || word(*sp + 0x118) != 0x816C9A64u ||
              (parent != 0x816CD6BCu && parent != 0x816CE3B0u)) {
            return {};
          }
          frame_sp = uint64_t(*sp) + 0x190;
        }
        if (frame_sp > UINT32_MAX - 0x140) {
          return {};
        }
        if ((stage == Stage::kRecord || word(*sp + 0x188) == 0x816CD6BCu) &&
            word(uint32_t(frame_sp) + 0x138) != 0x816CD71Cu) {
          return {};
        }
      }
      if (!frame_sp || frame_sp > UINT32_MAX - 0x140) {
        return {};
      }
      sample.frame_sp = uint32_t(frame_sp);
      if (word(*work + 0xC) != task ||
          word(config.catalog->symbols().manager_startup_task_cell) != tagged) {
        return {};
      }
    }
    if (ReadModal(kernel) != modal ||
        kernel->emulator()->title_execution_state().snapshot().generation !=
            foreground.generation) {
      return {};
    }
    return sample.valid() ? std::optional(sample) : std::nullopt;
  }
  NativeNonmodalFrameWitness Witness(const Decision& decision) {
    NativeNonmodalFrameWitness value;
    value.kind_ = decision.kind == Decision::Kind::kEligible
                      ? NativeNonmodalFrameWitness::Kind::kEligibleDrawCandidate
                  : decision.kind == Decision::Kind::kClear
                      ? NativeNonmodalFrameWitness::Kind::kNativeEmptyClear
                      : NativeNonmodalFrameWitness::Kind::kInactiveFrame;
    value.owner_ = decision.frame.owner;
    value.domain_id_ = decision.frame.domain;
    value.root_generation_ = decision.frame.root_generation;
    value.foreground_generation_ = decision.frame.foreground;
    value.revision_ = decision.revision;
    value.frame_serial_ = decision.serial;
    value.frame_counter_ = decision.frame.counter;
    value.thread_id_ = decision.frame.thread;
    value.task_ = decision.frame.task;
    value.frame_sp_ = decision.frame.frame_sp;
    value.modal_known_ = decision.frame.modal.known;
    value.modal_sequence_ = decision.frame.modal.sequence;
    value.modal_foreground_ = decision.frame.modal.foreground;
    value.source_ = decision.source;
    return value;
  }
};
struct NativeNotificationPresentationObserver::DrawObservation::State {
  std::shared_ptr<NativeNotificationPresentationObserver::State::Operation>
      operation;
  Sample sample;
  const cpu::ppc::PPCContext* context;
  uint32_t output;
  bool completed = false;
  ~State() {
    if (!completed && operation) {
      std::lock_guard lock(operation->state->mutex);
      operation->state->protocol.CancelFrame(sample);
    }
  }
};
struct NativeNotificationPresentationObserver::SetObservation::State {
  std::shared_ptr<NativeNotificationPresentationObserver::State::Operation>
      operation;
  Sample sample;
  const cpu::ppc::PPCContext* context;
  gpu::SystemCommandDescriptor descriptor;
  std::shared_ptr<const NativeNonmodalOutputProvenance> provenance;
  std::shared_ptr<const NativeNonmodalControlProvenance> control;
  bool clear;
  bool completed = false;
  ~State() {
    if (!completed && operation) {
      std::lock_guard lock(operation->state->mutex);
      operation->state->protocol.CancelFrame(sample);
    }
  }
};
NativeNotificationPresentationObserver::NativeNotificationPresentationObserver(
    std::shared_ptr<State> value)
    : state_(std::move(value)) {}
std::shared_ptr<NativeNotificationPresentationObserver>
NativeNotificationPresentationObserver::Create(Config config) {
  if (!config.catalog || !config.domain || !config.domain->active() ||
      !config.catalog->allocation()->IsAccepting() ||
      !config.catalog->allocation()->MatchesDomain(config.domain)) {
    return {};
  }
  auto* kernel = config.catalog->calls()->kernel();
  auto module = config.catalog->calls()->module();
  if (!kernel || config.domain->root() !=
                     NativeImportDomain::Member{
                         module->hmodule_ptr(),
                         config.catalog->calls()->load_generation()}) {
    return {};
  }
  auto code = kernel->AcquireUserModuleExecutionLease(module);
  auto domain = config.domain->Acquire();
  if (!code || !domain) {
    return {};
  }
  {
    auto global = global_critical_region::AcquireDirect();
    if (!module->native_import_ready() || !CheckCode(*kernel, *module)) {
      return {};
    }
  }
  auto state = std::make_shared<State>(std::move(config), kernel);
  state->code = std::move(code);
  auto observer = std::shared_ptr<NativeNotificationPresentationObserver>(
      new NativeNotificationPresentationObserver(state));
  struct Retirement {
    explicit Retirement(
        std::shared_ptr<NativeNotificationPresentationObserver> owner)
        : value(std::move(owner)) {}
    std::shared_ptr<NativeNotificationPresentationObserver> value;
    ~Retirement() { value->RetireAfterKernelStop(); }
  };
  kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<Retirement>(observer));
  std::lock_guard lock(installations_mutex);
  if (!installations[kernel].expired()) {
    observer->BeginStop();
    return {};
  }
  installations[kernel] = observer;
  return observer;
}
std::shared_ptr<NativeNotificationPresentationObserver>
NativeNotificationPresentationObserver::Lookup(KernelState* kernel) {
  std::lock_guard lock(installations_mutex);
  auto found = installations.find(kernel);
  return found == installations.end() ? nullptr : found->second.lock();
}
std::shared_ptr<NativeNotificationPresentationObserver>
NativeNotificationPresentationObserver::LookupForConfig(Config config) {
  if (!config.catalog || !config.domain) {
    return {};
  }
  // Lookup releases the registry lock before any observer/stop lock is taken.
  auto observer = Lookup(config.catalog->calls()->kernel());
  if (!observer) {
    return {};
  }
  {
    std::lock_guard lock(observer->state_->mutex);
    if (observer->state_->config.catalog != config.catalog ||
        observer->state_->config.domain != config.domain) {
      return {};
    }
  }
  // Each subsequent observation still acquires its existing domain operation.
  return observer->IsAccepting() ? observer : nullptr;
}
bool NativeNotificationPresentationObserver::IsAccepting() const noexcept {
  std::lock_guard lock(state_->mutex);
  return !state_->stopping && !state_->retired &&
         state_->protocol.accepting() && state_->config.domain->active() &&
         state_->config.catalog->allocation()->IsAccepting();
}
bool NativeNotificationPresentationObserver::ObserveSuccessfulNativeFrameQuery(
    const cpu::ppc::PPCContext& context) noexcept try {
  auto operation = state_->Acquire(state_);
  const auto sample =
      operation ? state_->Read(context, State::Stage::kQuery) : std::nullopt;
  if (!sample) {
    return false;
  }
  std::lock_guard lock(state_->mutex);
  return state_->protocol.Arm(*sample);
} catch (...) {
  BeginStop();
  return false;
}
void NativeNotificationPresentationObserver::ObserveNativeModalTransition(
    const NativeSystemUiVisibilityEvent& event) noexcept try {
  auto operation = state_->Acquire(state_);
  if (!operation) {
    return;
  }
  if (event.owner() != state_->config.catalog->allocation()->owner() ||
      event.domain_id() != state_->config.domain->id() ||
      event.root_load_generation() !=
          state_->config.domain->root().generation ||
      !event.sequence()) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->protocol.ModalTransition(
      {true, event.sequence(), event.foreground_generation()});
} catch (...) {
  BeginStop();
}
NativeNotificationPresentationObserver::DrawObservation
NativeNotificationPresentationObserver::BeforeNativeDisplayInformation(
    KernelState* kernel, const cpu::ppc::PPCContext* context,
    uint32_t output) try {
  if (!context || uint32_t(context->lr) != 0x816CB410) {
    return {};
  }
  auto observer = Lookup(kernel);
  auto operation =
      observer ? observer->state_->Acquire(observer->state_) : nullptr;
  const auto sample =
      operation ? observer->state_->Read(*context, State::Stage::kDraw, output)
                : std::nullopt;
  if (!sample) {
    return {};
  }
  auto value = std::make_shared<DrawObservation::State>();
  value->operation = std::move(operation);
  value->sample = *sample;
  value->context = context;
  value->output = output;
  return DrawObservation(std::move(value));
} catch (...) {
  if (auto observer = Lookup(kernel)) {
    observer->BeginStop();
  }
  return {};
}
NativeNotificationPresentationObserver::DrawObservation::DrawObservation(
    std::shared_ptr<State> value)
    : state_(std::move(value)) {}
NativeNotificationPresentationObserver::DrawObservation::DrawObservation(
    DrawObservation&&) noexcept = default;
NativeNotificationPresentationObserver::DrawObservation&
NativeNotificationPresentationObserver::DrawObservation::operator=(
    DrawObservation&&) noexcept = default;
NativeNotificationPresentationObserver::DrawObservation::~DrawObservation() =
    default;
void NativeNotificationPresentationObserver::DrawObservation::
    Complete() noexcept try {
  auto value = std::exchange(state_, {});
  if (!value) {
    return;
  }
  auto owner = value->operation->state;
  const auto sample =
      owner->Read(*value->context,
                  NativeNotificationPresentationObserver::State::Stage::kDraw,
                  value->output);
  if (!sample || *sample != value->sample) {
    return;
  }
  std::array<uint8_t, 0x58> information;
  {
    // Copy the genuine completed display-info result while the same native
    // call and module/domain operation still own its stack. Never reread it
    // after the Source is frozen or from a presenter/backend thread.
    auto global = global_critical_region::AcquireDirect();
    auto& memory = *owner->kernel->memory();
    if (memory.QueryRangeAccess(value->output, information.size()) ==
        memory::PageAccess::kNoAccess) {
      return;
    }
    std::memcpy(information.data(), memory.TranslateVirtual(value->output),
                information.size());
  }
  if (owner->Read(*value->context,
                  NativeNotificationPresentationObserver::State::Stage::kDraw,
                  value->output) != sample) {
    return;
  }
  std::lock_guard lock(owner->mutex);
  value->completed = owner->protocol.ObserveDraw(*sample, information);
} catch (...) {
  state_.reset();
}
NativeNotificationPresentationObserver::SetObservation
NativeNotificationPresentationObserver::BeforeNativeSet(
    const cpu::ppc::PPCContext& context,
    const gpu::SystemCommandDescriptor& descriptor) noexcept try {
  auto operation = state_->Acquire(state_);
  const bool clear = descriptor.empty();
  const auto sample = operation ? state_->Read(context,
                                               clear ? State::Stage::kClear
                                                     : State::Stage::kRecord,
                                               0, &descriptor)
                                : std::nullopt;
  if (!sample) {
    return {};
  }
  auto value = std::make_shared<SetObservation::State>();
  value->operation = std::move(operation);
  value->sample = *sample;
  value->context = &context;
  value->descriptor = descriptor;
  value->clear = clear;
  // Storage provenance is distinct from visibility eligibility. Native r16
  // denotes transition work, not display enable; its frame still requires
  // honest execution/retirement even when no positive plane is published.
  if (!clear && !context.r[15]) {
    std::optional<Protocol::DrawSnapshot> draw;
    {
      std::lock_guard lock(state_->mutex);
      draw = state_->protocol.PeekDraw(*sample);
    }
    if (draw) {
      auto provenance = std::shared_ptr<NativeNonmodalOutputProvenance>(
          new NativeNonmodalOutputProvenance());
      provenance->owner_ = sample->owner;
      provenance->domain_ = sample->domain;
      provenance->root_generation_ = sample->root_generation;
      provenance->foreground_ = sample->foreground;
      provenance->thread_ = sample->thread;
      provenance->task_ = sample->task;
      provenance->frame_sp_ = sample->frame_sp;
      provenance->counter_ = sample->counter;
      provenance->serial_ = draw->serial;
      provenance->info_ = draw->display_information;
      value->provenance = std::move(provenance);
    }
  }
  if (!clear && !context.r[15] && !context.r[16]) {
    std::optional<uint64_t> serial;
    {
      std::lock_guard lock(state_->mutex);
      serial = state_->protocol.PeekControl(*sample);
    }
    if (serial) {
      auto control = std::shared_ptr<NativeNonmodalControlProvenance>(
          new NativeNonmodalControlProvenance());
      control->owner_ = sample->owner;
      control->domain_ = sample->domain;
      control->root_generation_ = sample->root_generation;
      control->foreground_ = sample->foreground;
      control->thread_ = sample->thread;
      control->task_ = sample->task;
      control->frame_sp_ = sample->frame_sp;
      control->counter_ = sample->counter;
      control->serial_ = *serial;
      value->control = std::move(control);
    }
  }
  return SetObservation(std::move(value));
} catch (...) {
  BeginStop();
  return {};
}
NativeNotificationPresentationObserver::SetObservation::SetObservation(
    std::shared_ptr<State> value)
    : state_(std::move(value)) {}
NativeNotificationPresentationObserver::SetObservation::SetObservation(
    SetObservation&&) noexcept = default;
NativeNotificationPresentationObserver::SetObservation&
NativeNotificationPresentationObserver::SetObservation::operator=(
    SetObservation&&) noexcept = default;
NativeNotificationPresentationObserver::SetObservation::~SetObservation() =
    default;
std::shared_ptr<const NativeNonmodalOutputProvenance>
NativeNotificationPresentationObserver::SetObservation::output_provenance()
    const noexcept {
  return state_ && !state_->completed ? state_->provenance : nullptr;
}
std::shared_ptr<const NativeNonmodalControlProvenance>
NativeNotificationPresentationObserver::SetObservation::control_provenance()
    const noexcept {
  return state_ && !state_->completed ? state_->control : nullptr;
}
std::optional<NativeNonmodalFrameWitness>
NativeNotificationPresentationObserver::SetObservation::CompleteSource(
    const gpu::NativeFrameSubmissionIdentity& source) noexcept try {
  auto value = std::exchange(state_, {});
  if (!value || value->clear) {
    return {};
  }
  auto owner = value->operation->state;
  const auto sample =
      owner->Read(*value->context,
                  NativeNotificationPresentationObserver::State::Stage::kRecord,
                  0, &value->descriptor);
  if (!sample || *sample != value->sample) {
    return {};
  }
  std::lock_guard lock(owner->mutex);
  const auto decision = owner->protocol.FinishSource(
      *sample, source, value->context->r[15], value->context->r[16]);
  value->completed = bool(decision);
  return decision ? std::optional(owner->Witness(*decision)) : std::nullopt;
} catch (...) {
  state_.reset();
  return {};
}
std::optional<NativeNonmodalFrameWitness>
NativeNotificationPresentationObserver::SetObservation::
    CompleteNativeClear() noexcept try {
  auto value = std::exchange(state_, {});
  if (!value || !value->clear) {
    return {};
  }
  auto owner = value->operation->state;
  const auto sample =
      owner->Read(*value->context,
                  NativeNotificationPresentationObserver::State::Stage::kClear,
                  0, &value->descriptor);
  if (!sample || *sample != value->sample) {
    return {};
  }
  std::lock_guard lock(owner->mutex);
  const auto decision = owner->protocol.FinishClear(*sample);
  value->completed = bool(decision);
  return decision ? std::optional(owner->Witness(*decision)) : std::nullopt;
} catch (...) {
  state_.reset();
  return {};
}
void NativeNotificationPresentationObserver::ObserveTextSenderEntry(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw ||
      function != notification_detail::kTextSenderAddress ||
      !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadTextIdentity(ctx);
  if (!identity) { return; }
  notification_detail::TextSenderEntry entry;
  entry.identity = *identity;
  entry.handle = ctx.r[3];
  entry.raw_text_pointer = ctx.r[4];
  const auto text = Pointer(ctx.r[4]);
  entry.text_pointer_known = bool(text);
  entry.text_pointer = text.value_or(0);
  bool claimed, ambiguous;
  {
    std::lock_guard lock(owner->mutex);
    ambiguous = owner->text_diagnostic.pending();
    claimed = owner->text_diagnostic.Claim(entry);
    if (ambiguous) { owner->measure_diagnostic.Stop(); }
  }
  if (!claimed) {
    if (ambiguous) {
      static NativeSystemUiTraceLimiter cancelled;
      cancelled.Emit("notification_text", "sender_cancelled_ambiguous_entry",
                     {{"function", function}, {"thread", identity->thread},
                      {"sp", identity->sp}, {"lr", identity->lr}});
    }
    return;
  }
  bool code_matches;
  {
    auto global = global_critical_region::AcquireDirect();
    code_matches = CheckCode(*owner->kernel,
        *owner->config.catalog->calls()->module(), text_profiles);
  }
  if (!code_matches) {
    { std::lock_guard lock(owner->mutex); owner->text_diagnostic.Stop(); }
    static NativeSystemUiTraceLimiter rejected;
    rejected.Emit("notification_text", "sender_code_mismatch", {{"function", function}});
    return;
  }
  const auto task = owner->ReadTextTask();
  entry.task_known = bool(task);
  entry.task = task.value_or(0);
  if (text) {
    entry.text = notification_detail::ReadTextSenderText(*owner->kernel->memory(), *text);
  } else {
    entry.text.read_failed = true;
  }
  {
    std::lock_guard lock(owner->mutex);
    owner->text_diagnostic.RecordEntry(entry);
    owner->measure_diagnostic.SetCaption(entry);
  }
  std::string hex;
  hex.reserve(entry.text.count * 4);
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i != entry.text.count; ++i) {
    const uint16_t unit = entry.text.units[i];
    for (int shift : {12, 8, 4, 0}) { hex.push_back(digits[(unit >> shift) & 15]); }
  }
  XELOGI("NativeSystemUITrace {{\"schema\":1,\"component\":\"notification_text\","
         "\"event\":\"sender_entry\",\"function\":{},\"kernel\":{},"
         "\"root_hmodule\":{},\"root_generation\":{},\"domain\":{},"
         "\"foreground\":{},\"thread\":{},\"pcr\":{},\"sp\":{},\"lr\":{},"
         "\"task_known\":{},\"task\":{},\"handle\":{},\"text_pointer\":{},"
         "\"text_pointer_known\":{},\"text_units\":{},\"terminated\":{},"
         "\"read_failed\":{},\"utf16be_hex\":\"{}\"}}",
         function, identity->kernel, owner->config.domain->root().hmodule,
         identity->root_generation, identity->domain, identity->foreground,
         identity->thread, identity->pcr, identity->sp, identity->lr,
         entry.task_known, entry.task, entry.handle, entry.raw_text_pointer,
         entry.text_pointer_known, entry.text.count, entry.text.terminated,
         entry.text.read_failed, hex);
} catch (...) {
  // Observational failure must not close native/render admission or unwind.
}
void NativeNotificationPresentationObserver::ObserveTextSenderReturn(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw ||
      function != notification_detail::kTextSenderAddress ||
      !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->text_diagnostic.pending()) { return; }
  }
  auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadTextIdentity(ctx);
  if (!identity) { return; }
  std::optional<notification_detail::TextSenderEntry> entry;
  {
    std::lock_guard lock(owner->mutex);
    entry = owner->text_diagnostic.Complete(*identity);
  }
  if (!entry) { return; }
  const auto task = owner->ReadTextTask();
  const auto message = notification_detail::ReadTextSenderMessage(
      *owner->kernel->memory(), *entry);
  XELOGI("NativeSystemUITrace {{\"schema\":1,\"component\":\"notification_text\","
         "\"event\":\"sender_return\",\"function\":{},\"kernel\":{},"
         "\"root_generation\":{},\"domain\":{},\"foreground\":{},"
         "\"thread\":{},\"pcr\":{},\"sp\":{},\"lr\":{},\"handle\":{},"
         "\"hresult\":{},\"task_known\":{},\"task\":{},\"entry_task_known\":{},"
         "\"entry_task\":{},\"message_readable\":{},\"message_matches\":{},"
         "\"header_bytes\":{},\"message_id\":{},\"payload_bytes\":{},"
         "\"handled_raw\":{},\"handled_known\":{},"
         "\"payload\":{},\"payload_text\":{}}}",
         function, identity->kernel, identity->root_generation, identity->domain,
         identity->foreground, identity->thread, identity->pcr, identity->sp,
         identity->lr, entry->handle, uint32_t(ctx.r[3]), bool(task),
         task.value_or(0), entry->task_known, entry->task, message.readable,
         message.matches, message.header_bytes, message.id,
         message.payload_bytes, message.handled, message.matches,
         message.payload, message.text_pointer);
} catch (...) {
}

void NativeNotificationPresentationObserver::ObserveTextMeasureEntry(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw ||
      function != notification_detail::kTextMeasureAddress || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadMeasureIdentity(ctx);
  if (!identity) { return; }
  bool ambiguous = false;
  {
    std::lock_guard lock(owner->mutex);
    if (owner->measure_diagnostic.pending()) {
      ambiguous = true;
      owner->measure_diagnostic.Stop();
      owner->font_diagnostic.Stop();
    } else if (!owner->measure_diagnostic.accepting()) {
      return;
    }
  }
  if (ambiguous) {
    static NativeSystemUiTraceLimiter canceled;
    canceled.Emit("notification_text_measure", "measure_cancelled_ambiguous_entry",
                  {{"thread", identity->thread}, {"sp", identity->sp}, {"caller", identity->lr}});
    return;
  }
  notification_detail::TextMeasureEntry entry;
  entry.identity = *identity;
  entry.state = Pointer(ctx.r[3]).value_or(0);
  entry.object = Pointer(ctx.r[4]).value_or(0);
  entry.text_pointer = Pointer(ctx.r[5]).value_or(0);
  entry.count = uint32_t(ctx.r[6]);
  entry.output = Pointer(ctx.r[7]).value_or(0);
  entry.text = notification_detail::ReadTextSenderText(*owner->kernel->memory(), entry.text_pointer);
  std::optional<notification_detail::TextSenderEntry> caption;
  uint32_t ordinal;
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->measure_diagnostic.Claim(entry)) { return; }
    caption = owner->measure_diagnostic.caption();
    ordinal = owner->measure_diagnostic.observed() + 1;
  }
  bool code_matches;
  {
    auto global = global_critical_region::AcquireDirect();
    auto& module = *owner->config.catalog->calls()->module();
    code_matches = CheckCode(*owner->kernel, module, measure_profiles) && CheckMeasureHelpers(module);
  }
  if (!code_matches) {
    { std::lock_guard lock(owner->mutex); owner->measure_diagnostic.Stop(); owner->font_diagnostic.Stop(); }
    static NativeSystemUiTraceLimiter rejected;
    rejected.Emit("notification_text_measure", "measure_code_or_helper_mismatch", {});
    return;
  }
  const auto task = owner->ReadTextTask();
  const auto state = notification_detail::ReadTextMeasureState(*owner->kernel->memory(), entry.state);
  if (task && state.readable && !state.words[5]) {
    bool font_code;
    {
      auto global = global_critical_region::AcquireDirect();
      font_code = CheckFontProfiles(*owner->kernel, *owner->config.catalog->calls()->module());
    }
    auto engine = font_code ? notification_detail::ReadFontAcquireEngine(*owner->kernel->memory())
                            : notification_detail::FontAcquireEngine{};
    {
      std::lock_guard lock(owner->mutex);
      if (font_code && owner->measure_diagnostic.pending()) {
        owner->font_diagnostic.Begin(entry, ordinal, *task, state, std::move(engine));
      } else { owner->font_diagnostic.Stop(); }
    }
    if (!font_code) {
      static NativeSystemUiTraceLimiter rejected;
      rejected.Emit("notification_font_acquire", "code_or_helper_mismatch", {});
    }
  }
  EmitMeasureDiagnostic("measure_entry", *owner->kernel->memory(), entry, *caption,
                        ordinal, false, 0, task);
} catch (...) {
  // Diagnostic failure has no presentation/admission effects.
}
void NativeNotificationPresentationObserver::ObserveTextMeasureReturn(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw ||
      function != notification_detail::kTextMeasureAddress || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  // This is the common function's own JIT normal-return epilogue, reached by
  // canonical inline restore25 only when restored LR equals its own return.
  // A shared-helper PC or pre-restoration LR is never treated as completion.
  const auto identity = owner->ReadMeasureIdentity(ctx);
  if (!identity) { return; }
  std::optional<notification_detail::TextMeasureEntry> entry;
  std::optional<notification_detail::TextSenderEntry> caption;
  uint32_t ordinal;
  {
    std::lock_guard lock(owner->mutex);
    entry = owner->measure_diagnostic.Complete(*identity);
    if (!entry) { return; }
    caption = owner->measure_diagnostic.caption();
    ordinal = owner->measure_diagnostic.observed();
  }
  const auto task = owner->ReadTextTask();
  const auto state = notification_detail::ReadTextMeasureState(*owner->kernel->memory(), entry->state);
  const auto bounds = notification_detail::ReadTextMeasureBounds(*owner->kernel->memory(), entry->output);
  std::unique_ptr<notification_detail::FontAcquireReport> font_report;
  {
    std::lock_guard lock(owner->mutex);
    font_report = owner->font_diagnostic.Finish(*identity, ordinal, task.value_or(0), uint32_t(ctx.r[3]), state, bounds);
  }
  EmitMeasureDiagnostic("measure_return", *owner->kernel->memory(), *entry, *caption,
                        ordinal, true, uint32_t(ctx.r[3]), task);
  if (font_report) { EmitFontAcquireReport(*font_report); }
} catch (...) {
}
void NativeNotificationPresentationObserver::ObserveFontAcquireEntry(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || function > UINT32_MAX ||
      !notification_detail::FindFontAcquireSite(uint32_t(function)) || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto owner = observer ? observer->state_ : nullptr;
  if (!owner) { return; }
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->font_diagnostic.accepting()) { return; }
  }
  State::Operation operation;
  if (!owner->AcquireInto(owner, operation)) { return; }
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadFontIdentity(ctx, uint32_t(function));
  if (!identity) { return; }
  std::optional<std::pair<uint32_t, uint32_t>> ticket;
  {
    std::lock_guard lock(owner->mutex);
    ticket = owner->font_diagnostic.Claim(*identity);
  }
  if (!ticket) { return; }  // Reentry/cap cancellation precedes all stage RAM reads.
  auto entry = ReadFontAcquireEntry(*owner->kernel->memory(), ctx, *identity, ticket->first, ticket->second);
  {
    std::lock_guard lock(owner->mutex);
    owner->font_diagnostic.Record(std::move(entry));
  }
} catch (...) {
  // Copied diagnostic failure cannot alter native execution or its result.
}
void NativeNotificationPresentationObserver::ObserveFontAcquireReturn(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || function > UINT32_MAX ||
      !notification_detail::FindFontAcquireSite(uint32_t(function)) || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto owner = observer ? observer->state_ : nullptr;
  if (!owner) { return; }
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->font_diagnostic.accepting()) { return; }
  }
  State::Operation operation;
  if (!owner->AcquireInto(owner, operation)) { return; }
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadFontIdentity(ctx, uint32_t(function));
  if (!identity) { return; }
  std::optional<notification_detail::FontAcquireEntry> entry;
  {
    std::lock_guard lock(owner->mutex);
    entry = owner->font_diagnostic.Complete(*identity);
  }
  if (!entry) { return; }
  // An own-return epilog still precedes its caller's cleanup. Only the copied
  // caller-owned outputs and documented live receiver spans are sampled here.
  auto result = ReadFontAcquireReturn(*owner->kernel->memory(), std::move(*entry), uint32_t(ctx.r[3]));
  {
    std::lock_guard lock(owner->mutex);
    owner->font_diagnostic.RecordReturn(std::move(result));
  }
} catch (...) {
}
void NativeNotificationPresentationObserver::ObserveNativeAudioPublished(
    void* raw, apu::AudioSystem::PreparedClient& prepared,
    uint32_t record, uint32_t output) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadCueIdentity(ctx, 0x819AC540);
  if (!identity || identity->lr != 0x819AC5A0 || !owner->ValidateCueCode()) { return; }
  auto* audio = owner->kernel->emulator()->audio_system();
  const auto key = audio ? audio->DescribePreparedClient(prepared) : nullptr;
  if (!key || key->callback() != 0x819AE4E8 || output < 0x18 ||
      key->identity().process_type != X_PROCTYPE_SYSTEM ||
      key->identity().owner_hmodule != owner->config.domain->root().hmodule) { return; }
  auto& memory = *owner->kernel->memory();
  const auto callback = CueWord(memory, record);
  const auto argument = CueWord(memory, CueOffset(record, 4));
  const auto published = CueWord(memory, output);
  const uint32_t driver = output - 0x18;
  if (!callback || *callback != key->callback() || !argument ||
      *argument != key->callback_arg() || !published ||
      *published != (0x41550000u | uint32_t(key->index())) ||
      !owner->ReadCueDriver(driver)) { return; }
  const auto driver_argument = CueWord(memory, CueOffset(driver, 0xC));
  if (!driver_argument || *driver_argument != key->callback_arg()) { return; }
  std::lock_guard lock(owner->mutex);
  if (owner->cue_publication_serial == UINT64_MAX ||
      (owner->cue_claimed && owner->cue_registration != key)) {
    owner->LoseCueMetadata();
    return;
  }
  owner->cue_registration = key;
  // Engine initialization may register its driver before its caller stores
  // the singleton. Resolve that graph later at arm, never infer it here.
  owner->cue_engine = 0;
  owner->cue_driver = driver;
  ++owner->cue_publication_serial;
} catch (...) {
  // Diagnostics never alter publication/activation or unwind into native code.
}

std::shared_ptr<const apu::NativeAudioClientKey>
NativeNotificationPresentationObserver::DescribeNativeCueFrame(
    void* raw, uint32_t handle, uint32_t samples) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || !XThread::IsInThread()) { return {}; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto owner = observer ? observer->state_ : nullptr;
  if (!owner) { return {}; }
  std::shared_ptr<const apu::NativeAudioClientKey> key;
  uint32_t driver;
  uint64_t foreground;
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->cue_result.armed || owner->cue_closed || owner->cue_metadata_lost ||
        !owner->cue_capture || owner->cue_capture->Peek().closed) { return {}; }
    key = owner->cue_registration;
    driver = owner->cue_driver;
    foreground = owner->cue_result.foreground;
  }
  State::Operation operation;
  if (!owner->AcquireInto(owner, operation)) { return {}; }
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadCueIdentity(ctx, notification_detail::kNativeCueSubmitFunction);
  if (!key || !identity || identity->lr != 0x819AC78C ||
      identity->foreground != foreground || !owner->ValidateCueCode() ||
      Pointer(ctx.r[3]) != std::optional<uint32_t>(handle) ||
      Pointer(ctx.r[4]) != std::optional<uint32_t>(samples) ||
      handle != (0x41550000u | uint32_t(key->index())) ||
      CueWord(*owner->kernel->memory(), CueOffset(driver, 0x18)) != std::optional<uint32_t>(handle) ||
      !notification_detail::NativeCueFrameRamReadable(*owner->kernel->memory(), samples)) { return {}; }
  // AudioSystem finally compares this opaque key to its exact current
  // registration AND requires that registration's actual callback scope.
  return key;
} catch (...) { return {}; }

bool NativeNotificationPresentationObserver::ArmNativeCueObservation(
    XThread& thread, bool sound_enabled) noexcept try {
  if (!cvars::native_system_ui_trace || !XThread::IsInThread() ||
      XThread::GetCurrentThread() != &thread || thread.kernel_state() != state_->kernel ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) { return false; }
  auto operation = state_->Acquire(state_);
  if (!operation || !state_->ValidateCueCode()) { return false; }
  const auto foreground = state_->kernel->emulator()->title_execution_state().snapshot();
  if (foreground.phase != TitleExecutionState::Phase::kRunning || !foreground.generation ||
      !state_->kernel->emulator()->OwnsTitleExecution(state_->kernel)) { return false; }
  std::shared_ptr<const apu::NativeAudioClientKey> key;
  uint32_t expected_driver;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->cue_claimed || state_->cue_metadata_lost || !state_->cue_registration) { return false; }
    state_->cue_claimed = true;
    key = state_->cue_registration;
    expected_driver = state_->cue_driver;
  }
  uint32_t engine = 0, driver = 0;
  if (!state_->ReadCueEngine(engine, driver) || driver != expected_driver) {
    std::lock_guard lock(state_->mutex);
    state_->LoseCueMetadata();
    return false;
  }
  auto capture = apu::NativeAudioObservation::Create(key->identity());
  auto* audio = state_->kernel->emulator()->audio_system();
  if (!capture || !audio || !audio->AttachNativeObservation(key, capture)) { return false; }
  struct Admission {
    apu::AudioSystem* audio;
    std::shared_ptr<const apu::NativeAudioClientKey> key;
    std::shared_ptr<apu::NativeAudioObservation> capture;
    bool committed = false;
    ~Admission() {
      if (!committed) {
        capture->Cancel();
        try { audio->DetachNativeObservation(key, capture); } catch (...) {}
      }
    }
  } admission{audio, key, capture};
  const auto still_current = state_->kernel->emulator()->title_execution_state().snapshot();
  bool publish;
  {
    std::lock_guard lock(state_->mutex);
    publish = !state_->stopping && !state_->retired && !state_->cue_closed && !state_->cue_metadata_lost &&
              state_->cue_registration == key && state_->cue_driver == driver &&
              still_current.phase == TitleExecutionState::Phase::kRunning &&
              still_current.generation == foreground.generation;
    if (publish) {
      auto& result = state_->cue_result;
      result.armed = true;
      result.sound_enabled = sound_enabled;
      result.domain = state_->config.domain->id();
      result.root_generation = state_->config.domain->root().generation;
      result.foreground = foreground.generation;
      result.publication_serial = state_->cue_publication_serial;
      result.engine = engine;
      result.driver = driver;
      result.registration = key;
      state_->cue_engine = engine;
      state_->cue_capture = capture;
    }
  }
  admission.committed = publish;
  return publish;
} catch (...) { return false; }

void NativeNotificationPresentationObserver::ObserveNativeCueEntry(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || function > UINT32_MAX || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadCueIdentity(ctx, uint32_t(function));
  if (!identity) { return; }
  {
    std::lock_guard lock(owner->mutex);
    if (!owner->cue_calls.accepting() || owner->cue_closed ||
        (!owner->cue_result.armed && function != 0x817DE300 && function != 0x817DE388)) { return; }
  }
  if (!owner->ValidateCueCode()) { return; }
  notification_detail::NativeCueCall call;
  call.identity = *identity;
  const size_t argument_count = function == 0x819A3878 ? 3 :
      (function == 0x817DE300 || function == 0x817DE098 || function == 0x819A3D10 ||
       function == 0x819A3930 || function == 0x819A3978 || function == 0x819AC540 ? 2 : 1);
  for (size_t i = 0; i < argument_count; ++i) {
    const auto argument = Pointer(ctx.r[3 + i]);
    if (!argument) { return; }
    call.arguments[i] = *argument;
  }
  auto& memory = *owner->kernel->memory();
  // These are copies at an authenticated function boundary. No host pointer,
  // execution lease or borrowed descriptor survives this callback.
  if (function == 0x817DE098) {
    call.message = notification_detail::ReadNativeCueWords(memory, call.arguments[1]);
    if (!call.message.readable || call.message.words[0] != 0x14 ||
        (call.message.words[1] != 0x7F4 && call.message.words[1] != 0x7F5) ||
        call.message.words[3] || call.message.words[4]) { return; }
  }
  if (function == 0x817DE388 || function == 0x817DE098 || function == 0x817DDD38 ||
      function == 0x817DDAC8 || function == 0x817DDCA0) {
    call.instance = notification_detail::ReadNativeCueWords(memory, call.arguments[0]);
    if (!call.instance.readable) { return; }
    // Destroy frees its filename before entering cleanup. Never follow that
    // member from destroy/cleanup/stop (or an unrelated timer/stop message).
    if (function == 0x817DDD38 ||
        (function == 0x817DE098 && call.message.words[1] == 0x7F4)) {
      call.filename = notification_detail::ReadTextSenderText(memory, call.instance.words[1]);
    }
  }
  if (function == 0x819A3878) {
    call.buffer = notification_detail::ReadNativeCueBuffer(memory, call.arguments[1]);
  }
  std::lock_guard lock(owner->mutex);
  auto& result = owner->cue_result;
  if (!owner->cue_calls.accepting() || owner->cue_closed) { return; }
  if (result.armed && (identity->foreground != result.foreground ||
      identity->domain != result.domain || identity->root_generation != result.root_generation)) {
    owner->LoseCueMetadata();
    return;
  }
  bool relevant = function == 0x817DE300 || function == 0x817DE388;
  if (function == 0x817DE098) {
    auto* control = owner->FindCueControl(call.arguments[0]);
    if (!control || control->hxui != call.instance.words[0]) { return; }
    relevant = true;
    if (call.message.words[1] == 0x7F4) {
      if (result.request_seen) {
        result.multiple_requests = true;
        owner->LoseCueMetadata();
        return;
      }
      else {
        if (owner->cue_request_serial == UINT64_MAX) { owner->LoseCueMetadata(); return; }
        result.request_seen = true;
        result.control = control->derived;
        result.hxui = control->hxui;
        result.control_serial = control->serial;
        result.request_serial = ++owner->cue_request_serial;
        result.filename = call.filename;
        result.voice = call.instance.words[2];
        result.cache = call.instance.words[3];
        result.suppression = call.instance.words[4];
      }
    }
  } else if (result.request_seen && !owner->cue_selected_retiring &&
             owner->FindCueControl(result.control) &&
             owner->FindCueControl(result.control)->serial == result.control_serial) {
    relevant = relevant ||
        ((function == 0x817DDD38 || function == 0x817DDAC8 || function == 0x817DDCA0) &&
         call.arguments[0] == result.control && call.instance.words[0] == result.hxui) ||
        (function == 0x819A3D10 && identity->lr == 0x817DDF6C &&
         call.arguments[1] == CueOffset(result.control, 8)) ||
        ((function == 0x819A3878 || function == 0x819A3930 || function == 0x819A3978 || function == 0x819A3640) &&
         result.voice && call.arguments[0] == result.voice);
  }
  // Destruction owns the derived body until it returns; its nested cleanup
  // and raw release may still be recorded, but cannot seed another start.
  if (result.request_seen && owner->cue_selected_retiring &&
      owner->FindCueControl(result.control) &&
      owner->FindCueControl(result.control)->serial == result.control_serial) {
    relevant = relevant ||
        (function == 0x817DDCA0 && call.arguments[0] == result.control) ||
        ((function == 0x819A3978 || function == 0x819A3640) &&
         result.voice && call.arguments[0] == result.voice);
  }
  if (function == 0x817DE388 && call.arguments[0] == result.control) {
    const auto* control = owner->FindCueControl(result.control);
    if (control && control->serial == result.control_serial) {
      owner->cue_selected_retiring = true;
      if (owner->cue_capture) { owner->cue_capture->Close(); }
    }
  }
  if (function == 0x819A51F8 && call.arguments[0] == owner->cue_engine) {
    relevant = true;
    result.engine_retiring = true;
    if (owner->cue_capture) { owner->cue_capture->Retire(); }
  }
  if (function == 0x819AC540 && call.arguments[0] == owner->cue_driver) {
    relevant = true;
    // Null/replacement callback can withdraw this actual registration before
    // normal return. The AudioSystem also retires its precise registration.
    if (!call.arguments[1] && owner->cue_capture) { owner->cue_capture->Retire(); }
  }
  if (!relevant) { return; }
  if (owner->cue_capture) {
    const auto audio = owner->cue_capture->Peek();
    call.audio_sequence_at_entry = audio.last_sequence;
    call.queued_frames_at_entry = audio.queued_frames;
    call.sink_frames_at_entry = audio.sink_frames;
  }
  if (!owner->cue_calls.Claim(*identity) || !owner->cue_calls.Record(call)) { owner->LoseCueMetadata(); }
} catch (...) {
  // No diagnostic exception may change native function behavior.
}

void NativeNotificationPresentationObserver::ObserveNativeCueReturn(
    void* raw, uint64_t function) noexcept try {
  if (!cvars::native_system_ui_trace || !raw || function > UINT32_MAX || !XThread::IsInThread()) { return; }
  auto observer = Lookup(XThread::GetCurrentThread()->kernel_state());
  auto operation = observer ? observer->state_->Acquire(observer->state_) : nullptr;
  if (!operation) { return; }
  auto owner = operation->state;
  const auto& ctx = *static_cast<const cpu::ppc::PPCContext*>(raw);
  const auto identity = owner->ReadCueIdentity(ctx, uint32_t(function));
  if (!identity) { return; }
  std::optional<notification_detail::NativeCueCall> call;
  bool selected_live = false;
  {
    std::lock_guard lock(owner->mutex);
    call = owner->cue_calls.Complete(*identity);
    const auto* control = owner->FindCueControl(owner->cue_result.control);
    selected_live = !owner->cue_selected_retiring && control &&
        control->serial == owner->cue_result.control_serial;
  }
  if (!call) { return; }
  const uint32_t returned = uint32_t(ctx.r[3]);
  const bool accepted = int32_t(returned) >= 0;
  auto& memory = *owner->kernel->memory();
  // Only the proven normal-return owners are read. Destroy/release/engine
  // teardown never dereference their potentially freed object on return.
  uint32_t created = 0;
  notification_detail::NativeCueWords after;
  if (function == 0x817DE300 && accepted) {
    const auto output = CueWord(memory, call->arguments[1]);
    created = output.value_or(0);
    after = notification_detail::ReadNativeCueWords(memory, created);
  } else if (function == 0x819A3D10 && accepted && selected_live) {
    created = CueWord(memory, call->arguments[1]).value_or(0);
  } else if (selected_live && (function == 0x817DDAC8 || function == 0x817DDCA0 || function == 0x817DDD38)) {
    after = notification_detail::ReadNativeCueWords(memory, call->arguments[0]);
  }
  std::lock_guard lock(owner->mutex);
  if (owner->cue_closed) { return; }
  auto& result = owner->cue_result;
  if (function == 0x817DE300 && accepted) {
    if (!created || !after.readable || after.words[0] != call->arguments[0] ||
        after.words[1] || after.words[2] || after.words[3] || after.words[4] ||
        owner->FindCueControl(created) || owner->cue_control_serial == UINT64_MAX) {
      owner->LoseCueMetadata();
    } else {
      bool stored = false;
      for (auto& control : owner->cue_controls) {
        if (!control.live) {
          control = {created, after.words[0], ++owner->cue_control_serial, true};
          stored = true;
          break;
        }
      }
      if (!stored) { owner->LoseCueMetadata(); }
    }
  } else if (function == 0x817DE388) {
    if (auto* control = owner->FindCueControl(call->arguments[0])) {
      if (control->derived == result.control && control->serial == result.control_serial) { result.class_destroyed = true; }
      control->live = false;
    }
  } else if (owner->cue_selected_retiring && owner->FindCueControl(result.control) &&
             owner->FindCueControl(result.control)->serial == result.control_serial) {
    if (function == 0x817DDCA0) { result.cleanup_returned = true; }
    if (function == 0x819A3640) { result.release_returned = true; }
    if (function == 0x819A3978) {
      result.stop_returned = true;
      result.stop_accepted = accepted;
    }
  } else if (result.request_seen && !owner->cue_selected_retiring &&
             owner->FindCueControl(result.control) &&
             owner->FindCueControl(result.control)->serial == result.control_serial) {
    if (function == 0x819A3D10 && accepted && created) {
      result.voice = created;
      result.voice_created = true;
      result.detached = result.stop_returned = result.stop_accepted = false;
    } else if (function == 0x819A3878 && identity->lr == 0x817DE03C && call->arguments[2] == 1) {
      result.buffer_submitted = accepted && call->buffer.readable;
    } else if (function == 0x819A3930 && identity->lr == 0x817DE074 && call->arguments[1] == 0) {
      result.start_returned = true;
      result.start_accepted = accepted;
      if (owner->cue_start_serial == UINT64_MAX) { owner->LoseCueMetadata(); }
      else { result.start_serial = ++owner->cue_start_serial; }
    } else if (function == 0x819A3978) {
      result.stop_returned = true;
      result.stop_accepted = accepted;
    } else if (function == 0x817DDAC8 && after.readable) {
      result.detached = !after.words[2] && !after.words[3];
      if (result.start_returned && owner->cue_capture) { owner->cue_capture->Close(); }
    } else if (function == 0x817DDCA0) {
      result.cleanup_returned = true;
      if (owner->cue_capture) { owner->cue_capture->Close(); }
    } else if (function == 0x819A3640) {
      result.release_returned = true;  // Raw return, not HRESULT/final free.
    }
    if (after.readable && call->arguments[0] == result.control) {
      result.cache = after.words[3];
      result.suppression = after.words[4];
    }
  }
  owner->RecordCueEvent(*call, returned);
} catch (...) {
}

std::unique_ptr<NativeCueObservationResult>
NativeNotificationPresentationObserver::CloseNativeCueObservation(
    XThread& thread) noexcept try {
  if (!XThread::IsInThread() || XThread::GetCurrentThread() != &thread ||
      thread.kernel_state() != state_->kernel ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) { return {}; }
  auto operation = state_->Acquire(state_);
  if (!operation) { return {}; }
  auto result = std::make_unique<NativeCueObservationResult>();
  std::shared_ptr<apu::NativeAudioObservation> capture;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->cue_result.armed || state_->cue_closed) { return {}; }
    state_->cue_closed = true;
    state_->cue_calls.Stop();
    *result = std::move(state_->cue_result);
    capture = std::exchange(state_->cue_capture, {});
  }
  if (capture) {
    capture->Close();
    if (auto* audio = state_->kernel->emulator()->audio_system()) {
      audio->DetachNativeObservation(result->registration, capture);
    }
    result->audio = capture->Take();
  }
  return result;
} catch (...) { return {}; }

void NativeNotificationPresentationObserver::CancelNativeCueObservation() noexcept try {
  // May also race an uncertain worker before it arms. This cancels only the
  // once-owned diagnostic, with no runtime/client/SDL/provider/file callback.
  std::lock_guard lock(state_->mutex);
  state_->cue_claimed = state_->cue_closed = true;
  state_->cue_calls.Stop();
  if (state_->cue_capture) { state_->cue_capture->Cancel(); }
} catch (...) {}

void NativeNotificationPresentationObserver::BeginStop() noexcept {
  bool cancelled, measure_cancelled;
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->protocol.BeginStop();
    cancelled = state_->text_diagnostic.Stop();
    measure_cancelled = state_->measure_diagnostic.Stop();
    state_->font_diagnostic.Stop();
    state_->cue_calls.Stop();
    if (state_->cue_capture) { state_->cue_capture->Cancel(); }
    state_->changed.notify_all();
  }
  if (cancelled) {
    static NativeSystemUiTraceLimiter stopped;
    stopped.Emit("notification_text", "sender_cancelled_by_stop", {});
  }
  if (measure_cancelled) {
    static NativeSystemUiTraceLimiter stopped;
    stopped.Emit("notification_text_measure", "measure_cancelled_by_stop", {});
  }
}
bool NativeNotificationPresentationObserver::Drain() noexcept {
  if (XThread::IsInThread()) {
    return false;
  }
  std::unique_lock lock(state_->mutex);
  return state_->stopping &&
         state_->changed.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return !state_->active; });
}
void NativeNotificationPresentationObserver::RetireAfterKernelStop() noexcept {
  Config config;
  std::shared_ptr<KernelState::UserModuleExecutionLease> code;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->active) {
      std::quick_exit(EXIT_FAILURE);
    }
    state_->stopping = true;
    state_->retired = true;
    state_->protocol.BeginStop();
    state_->text_diagnostic.Stop();
    state_->measure_diagnostic.Stop();
    state_->font_diagnostic.Stop();
    state_->cue_calls.Stop();
    if (state_->cue_capture) { state_->cue_capture->Retire(); }
    config = std::move(state_->config);
    code = std::move(state_->code);
  }
  std::lock_guard lock(installations_mutex);
  auto found = installations.find(state_->kernel);
  if (found != installations.end() && found->second.lock().get() == this) {
    installations.erase(found);
  }
}
}  // namespace xe::kernel
