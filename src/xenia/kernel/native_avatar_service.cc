/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_avatar_service.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_avatar_contract.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_content.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/title_execution_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/device.h"
namespace xe::kernel {
namespace {
constexpr uint32_t kNotReady = 0x8007139F, kFailed = 0x80004005;
// Records only the outcome of this call. Destruction occurs after local owner
// locks are released; tracing never calls guest code or changes admission.
// Sampling counters are diagnostic metadata, not Task/request identities.
enum class AvatarPublicReason : uint8_t {
  kUnobserved = 0,
  kNoOwner,
  kCallerArgumentsOrProvider,
  kFrameOrTitle,
  kCallerLr,
  kCallerCode,
  kOwnerState,
  kExecutionFailed,
  kReturned,
  kException,
  kAssetSizesUnavailable,
};
struct AvatarPublicObservation {
  NativeSystemUiPublicCall call;
  AvatarPublicReason reason = AvatarPublicReason::kUnobserved;
  NativeSystemUiPublicCallResult::Disposition disposition =
      NativeSystemUiPublicCallResult::Disposition::kInactive;
  uint32_t status = 0;
  NativeSystemUiPublicCallResult Finish(
      AvatarPublicReason observed, NativeSystemUiPublicCallResult value) noexcept {
    reason = observed;
    disposition = value.disposition;
    status = value.value;
    return value;
  }
  ~AvatarPublicObservation() noexcept {
    const char* event = nullptr;
    switch (reason) {
      case AvatarPublicReason::kNoOwner: event = "denied_no_owner"; break;
      case AvatarPublicReason::kCallerArgumentsOrProvider: event = "denied_caller_arguments_or_provider"; break;
      case AvatarPublicReason::kFrameOrTitle: event = "denied_frame_or_title"; break;
      case AvatarPublicReason::kCallerLr: event = "denied_caller_lr"; break;
      case AvatarPublicReason::kCallerCode: event = "denied_caller_code"; break;
      case AvatarPublicReason::kOwnerState: event = "denied_owner_state"; break;
      case AvatarPublicReason::kExecutionFailed: event = "execution_failed"; break;
      case AvatarPublicReason::kReturned: event = "public_return"; break;
      case AvatarPublicReason::kException: event = "public_exception"; break;
      case AvatarPublicReason::kAssetSizesUnavailable: event = "denied_native_asset_sizes"; break;
      case AvatarPublicReason::kUnobserved: return;
    }
    static xe::NativeSystemUiTraceLimiter trace;
    trace.EmitTransition(
        (uint64_t(uint16_t(call)) << 48) |
            (uint64_t(disposition) << 40) |
            (uint64_t(reason) << 32) | status,
        "avatar", event,
        {{"ordinal", uint16_t(call)},
         {"disposition", uint32_t(disposition)},
         {"reason", uint8_t(reason)},
         {"status", status}});
  }
};
void TraceAvatarIngressDenied(const cpu::ppc::PPCContext& c, uint32_t address,
                             uint32_t status, uint32_t task=0) noexcept {
  static xe::NativeSystemUiTraceLimiter trace;
  trace.EmitTransition((uint64_t(address) << 32) | status,
                       "avatar", "native_ingress_denied",
                       {{"address", address}, {"status", status},
                        {"r3", uint32_t(c.r[3])}, {"r4", uint32_t(c.r[4])},
                        {"r5", uint32_t(c.r[5])}, {"r6", uint32_t(c.r[6])},
                        {"sp", uint32_t(c.r[1])}, {"lr", uint32_t(c.lr)},
                        {"pcr", uint32_t(c.r[13])}, {"task", task}});
}
constexpr uint32_t kStart = 0x816A2CF0, kCopy = 0x816A2A48,
                   kDispatch = 0x816A2C80, kComplete = 0x816A2588,
                   kFinalize = 0x819657E0, kCoreDispatch = 0x819659D0,
                   kF2 = 0x81965AC0, kF3 = 0x81981740,
                   kF3Body = 0x81981560, kCoreInit = 0x819652F8,
                   kCoreStop = 0x81965040,
                   kCurrentTask = 0x816B8098, kCloseTask = 0x816B7B80,
                   kUnload = 0x816BD6A0, kLoad = 0x816BD620,
                   kLoadTask = 0x816BD4D8, kUnloadTask = 0x816BD560,
                   kSchedule = 0x816BA440, kTocRelease = 0x8197BEB0,
                   kFrontend = 0x816BDE90, kBuiltin = 0x816BD270,
                   kF3Lock = 0x81982088, kAnimationJob = 0x81964E70,
                   kAnimationCore = 0x81968390, kAnimationWriter = 0x81977310;
struct Code { uint32_t at, bytes; const char* sha; };
constexpr Code kCode[] = {
    {kFrontend,172,"7689eff393cd3d203d2de6f2b6564b7a2c2649715a6849b41ac25b0974c5a7b8"},
    {kBuiltin,428,"00f60b27dfd041a59df148de241743c8a54369169d064d93c67363df234eb4ee"},
    {0x816A2B90,116,"3dfc6fc1d67c11edbebfa7dbcf4a547ac510bb742d6c567fc6ef5a00d70b0bf8"},
    {0x816A29C0,136,"c2eba3f74a1f3c6c838caf8c35345c5dd68f63e3bb20335c06dad73dcaf8c1ce"},
    {kF3Lock,12,"d6b16ec68de02b67d42afa083887c0d36d138a8ad0415c0ecc629699b5cc2237"},
    {0x816BD620,128,"4d921ee9633441ed924652f62b40f071c62f9a0dacf7a72f24622cda480e8904"},
    {0x816BD4D8,136,"ce540234897ac486826c20e1e14e9283ab6f0427b442589a1bf3dd6425210bfb"},
    {0x816BD560,124,"30a3bf4b099aed525b358b0f08339d19da8ae4ba702f535aaff9a480d858d3b2"},
    {0x816BA440,232,"54499cbb31f67e590a4c36e36708688274d2c696f1a03529bd26782955c73fee"},
    {0x81965270,136,"86e45610ac36b2956b99a816acf4e68ef262adbcc6796d6a8b35dd0218704ba3"},
    {0x8197BEB0,108,"3183dda0d4548c928a824291be5be7c2f8c7e3f100dd836f0fb105ba3644738b"},
    {0x8197C998,8,"724f578d924f6316e12b2e35f2acf052fbdb325c46124e052405c4053f5fc54e"},
    {0x8197DD10,8,"fb5981102746187eb1ee496a032098d11a9f12b924729ea4ab9fd096db16c3c0"},
    {0x816BD6A0, 128, "4061d035adcdb2333c86f774c4aa22e353e9a892768428159d312b5d90347a37"},
    {0x81674940, 12, "29ef149b5c35c7293435787d1e5452bd7512979291def903f8398aa8507d6517"},
    {0x81674950, 112, "17a7a28f96a7b41cff8bdabe93005204dd70c8c6d91b3f80ae798666318b8f9a"},
    {0x81674A70, 104, "679af48429b8a772df8fcc0a5cd9f2e2a08948d39140a3e33d1c9466a29a32da"},
    {0x81674AD8, 148, "61530c9835f633f75976229959fef2d9349fcfcf9502ca3bcd338518f7462487"},
    {0x81674DE0, 124, "7bfbf817fe0b93b940533db13adc5021055a5f4afac1119e3eb17fe42f26f984"},
    {0x81675258, 152, "1d258cad8c81e3eeaf0e41b3eb4e534533866ec6426952da397e9246b1172c79"},
    {0x816752F0, 100, "1ec3c7580d78f1d013895b6a165aabc23d387f677329b88738d4b24e02777beb"},
    {0x81675358, 112, "7b23e0c61b74c91ab4d3b9530728c6567c390cff15e02a8397f29196d856a724"},
    {0x816755E8, 228, "4b7eceaaaee759f0ac9585337c83635793118744e823eebccb6b1261706bb919"},
    {0x816A2588, 364, "cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
    {0x816A2A48, 128, "7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
    {0x816A2C80, 108, "21da95341366e83781ab2e46696f0c04afa43d71ae7c11f91dc8e108d1b1db94"},
    {0x816A2CF0, 656, "e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a"},
    {0x816B7B80, 24, "bae8c269c47e9baa51a35f7e1663f73501e0ace0306e34e2d55fee6114dde1c8"},
    {0x816B8098, 48, "e6c9e34282cfbf21ced6b5aa991148d7e52fd0daeaa76782f4850444c1a39b57"},
    {0x816BCC38, 108, "649c43fbcffe629316d6b1fcf4c61b58f25964e6902b12ccc01a8b70955956a9"},
    {0x816BCD08, 600, "a7630ef93675d8db35d1c13e15ed517d54ad8e66211a1d88acb2f6ae8d438987"},
    {0x81965040, 180, "2b293c11c8a76701f75dec03851cd6d387e8ae66f96e2d4105aae539daff0a1c"},
    {0x819652F8, 200, "703bc5eb56af819b7f7e629458e2c7b4974a3ce57142a6a3fa738c148915ae01"},
    {0x819657E0, 96, "ca1aa9403658991e4ebb252d3b4e63d2079cdeed31996d960362ff4e565f5594"},
    {0x819659D0, 236, "4074eedf1546792a271cdc0a9519edcf1a4c74c2eb89d14f714d5f64e4ea9944"},
    {0x81965AC0, 212, "b639f41580c41ae0647a95db08f183859bb431d9d75840f1a40fb0a7e6edc8ad"},
    {0x8196B728, 416, "2b862cdc942ef0e07566aa9380cf18f21163b8deaefb324bcf8525a5ef68d34a"},
    {0x81981560, 480, "6fb09c74946ac7fdece8306fd3fa095b3bebbe6fa07179504245a1a00a6d6a41"},
    {0x81981740, 8, "c1561beada5759e1fcd5953ab0c0f69c5d949b31cb85ba37cc9f668b622d6f79"},
    {0x819650F8, 376, "a1ac4a3d62a7b595b41b25c875bf3263228a577feb7bd9a52bd70913f2e6e4ad"},
    {0x816BD7A0, 264, "dceb073367fdee00df4f741e05316edae64175911e621d68a03a3d1a5e2bd43d"},
    {0x816BCF60, 212, "02d1b0ddcc2ab0fedaea8b4b900c9def951ae86092341b7823dace02b790f2dd"},
    {0x8196B5F0, 204, "acccd47bf435daddc3c8826982b3bb3f0e54c9eb7e9eb4af77a7082de79d8665"},
};
// Finite native producer/consumer witness; only the optional route requires it.
constexpr Code kAnimationCode[] = {
    {0x8196B470, 184, "64095e8ea319bc482f220b921912b55a61b6541f33fe4f0b919ba9c66b715665"},
    {0x81977310, 896, "6edb1aaade2239a7fa64472029fc756c56def2dc5fe3b3aed76847ba0d7ef27c"},
    {0x81976080, 132, "c98ca24911b1dbae02c04b5312533d61b5e23e2cee9d4d8aa7c94061d28958c5"},
    {0x81979558, 116, "06a74cb7868abe2f156826093c66d4becb77f5e16be388b5792909cecb6ca919"},
    {0x81964E70, 52, "2b9116dae91e7c693ee9dcb62f2015d80bd971e74ecc38546fd190369e5c8a15"},
    {0x81968390, 480, "02723a32c8226ac01be10ff1c2773f7a339a2982f93b7f8b3a66347f5390f18f"},
    {0x81977218, 248, "69ebaa79995a6c1a9bc2273dc7f1a51787d20a2233098e4fa166251ea5a5200a"},
    {0x81977010, 224, "2581f8e91ed83165dcf592c611396fa84b0de858608acc0c181a7428b77fc95f"},
    {0x81976E58, 192, "8439009056ba0a55a39ad2a2d00d8ffa5abab1630cdb942fcfe5048bf4b8cb36"},
    {0x8197A388, 332, "b19d73bc0337781bf73fed677a6b974be52d6f1fcf7a4dbace5c2f3a84c849c3"},
    {0x819659D0, 236, "4074eedf1546792a271cdc0a9519edcf1a4c74c2eb89d14f714d5f64e4ea9944"},
    {0x8197E190, 164, "1f213ca28e80380513e558f03a1d4ee852a6c114421f9246ec840de2a371a6f1"},
    {0x8197E020, 276, "4648a35c06b3ac0882755a0ece18888fc628e058ea6fe655892dbac67b1cd958"},
    {0x8197D398, 100, "48b0b0a85d5ffd90e9ceeb87b6da5fee42d2f1b55f0c28e4d50c803ed92a871d"},
    {0x8197D5C8, 108, "306aeccdb8e6e614800072b6e92b9c5ae71489e3c4be8934ea3fd14cc85fc1e8"},
    {0x8197D250, 160, "344d8a9bdc53a83c5497912490b730f807ec8148e7c51a7673947f53dc4bc7c0"},
    {0x81975F20, 172, "78e569565db7d0d47d49dc00f101f2d68d07a97edb8e98fe43a1150688bd9c91"},
    {0x819770F0, 292, "01c98ad805d649364d2f0732342f6542aeaaba220d5b702b0a570b04878285fc"},
    {0x81976F18, 244, "f28e84826b490992dd54832418fa4c889b45f9994c75be7fa7a28060aeae726f"},
    {0x81976CB0, 196, "b9e5a2971e496e396a4e339b731a24f5cb64c3380240650affd2ccfe7b2c14e0"},
    {0x8197C420, 80, "b76422c1fcf7bac149d11c1e347b7483af3e0b9c71ec862bf8eafc8b17fa8d40"},
    {0x81965840, 168, "08b700dd5bf6170e561dbfeffb695a1b888c7bafa3ff705a865dcac3b243297d"},
    {0x81965580, 12, "60d9bb1d1e06a9e76f1d67f3be220f6fc8d5eb236afce0779f1a7d5cabd8cd18"},
    {0x81975E20, 20, "7c6023bdc98a689282c4dcf88eb06d52bdb153cd34deceb0c17d0604eb71c22c"},
    {0x8197AC30, 40, "1979ecec02fc59335c419f06d98fcf6f27d45bade8d27158ec3ee0a0156de984"},
    {0x8197AAD8, 20, "50169413578d8ba9006995f3ca37354d256f6b48e59c8e25d6a13ce4ae083dbe"},
    {0x81976AB8, 164, "2c42516373b2b8cd20d5d80ce31e249e9ce0d5242d98d5b33080cb443d844994"},
    {0x8197D408, 108, "e832ef891348b2aca87b840fee4b2e28470e2049b819329e7c522a97065e67c4"},
    {0x8197D558, 108, "c2f8c9b086d0bc7dbd27c2ca47b8afc98f7943259f9487fbddcf5c2f1269eb70"},
    {0x8197DB78, 288, "86fdd5390f3d278b8c18a98ed8516e21d1e1b9b11c89b25d85af03aec4b5a9be"},
    {0x8197BCF8, 168, "b35126c58226ee3f3403e521cf29ad2dc1fc42a2d6829620a8b25b87ab8a1f8b"},
    {0x8197D108, 164, "1a7c6062c0cf1c9b45306c82a95dc943182d8abca0507828a9d1aaa8246e352d"},
    {0x81976D78, 220, "54dbbd8eec32a94bff658a44204fe6d1863424c1f0da627279235311632c918d"},
    {0x816A2A48, 128, "7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
    {0x8197CC18, 556, "9c4e51e8dc4232e82f682a183a0da68d82b19860ce4942999a080c6b6c58d26e"},
    {0x8197C278, 188, "122471c8c6ae3b8ded8f049a2ca04717f7713dff75aae1fb3a2dfdf4853e0e4f"},
    {0x81969B18, 108, "d09c19e358db068506cc37b2fb6fd4ebe20a5dbbc0605eca10b6a991cd5cafe6"},
    {0x8197AB40, 120, "5f33b3be47f5f17bdaa009d5822de0478bd2ac5631d0f55a1a81cef62700cc62"},
    {0x8196F138, 44, "680f4303880b6469298a55a5ffd33e92c7a437698d93ed0fc965db20d2851200"},
    {0x81976B60, 164, "c3aaf11e35f1db71fe1807b3d251478b9ec5d3edc826109f9b2c5168534b2b39"},
    {0x81976C08, 164, "3214a097ee2c9afe91245c7ca17c9323f0cbade5c58a7561ce54c167b97e2c04"},
    {0x81970A50, 164, "a6ea4ef1eea641354b21f7eb147595fce452552bb6e8966744f195a89cc83b72"},
    {0x81909780, 4, "f332ea5b5437103cbb6f1508679da89eec9288ad775c96c439a17fccabe3de8e"},
    {0x8197E300, 292, "8ab49e04fdaf909599ce5820ae06e7cad9f444d8ea70e7ca90dc62ce59c716c6"},
    {0x81979640, 116, "5b0a5f96ddc126a92254e2c62c3fcb21bdd39950726ac8d088b2c0f7833369c5"},
    {0x8196DBE8, 416, "f60913d55c3219c1aa2c0a9591aa02a52188f5e1b140e7c538af728fb63bdd5a"},
    {0x81674D48, 152, "79dfc783867f65459133dc1938776d496134763bb54e304a97fc95726b147a96"},
    {0x8197C0A0, 84, "13c30f4f5ee59c3a3bf44e91306f0a72dc83442cdc96466d8cce9f3476809232"},
    {0x8197C1A8, 88, "6869fa1616d73ab19582e36d188d491cf496cbf1a68dad757ef56c2043278c59"},
    {0x8196D5E8, 8, "f60da705bd5fc20b1d32cbe5d970e90ac697454f78c055bd369fd4e495491a32"},
    {0x819658E8, 232, "fa88262a556961b0561d4a5d0cec1503b3b879b3125e9340ee8967cd98194164"},
    {0x819650F8, 376, "a1ac4a3d62a7b595b41b25c875bf3263228a577feb7bd9a52bd70913f2e6e4ad"},
    {0x819657E0, 96, "ca1aa9403658991e4ebb252d3b4e63d2079cdeed31996d960362ff4e565f5594"},
    {0x81965458, 212, "c651cbc1e4012a199ae3f172a4ab6142e76db6ec7e5cd61a05d39ad8b4ca54d2"},
};
constexpr Code kAnimationData[] = {
    {0x81634384, 56, "dd14fb5635b4b2ee902b9fa981d02438d9d0a9a4816529ffc308d5717a5fe69c"},
    {0x81634198, 56, "6bac71a9acb37c656ebe4a76a5af092696478daeb43e716bf900a7076b6c7556"},
    {0x81633790, 24, "cfd6497dc5f991f1d575804064c585ab921dd5e783f0b0df55ba509e216f26d9"},
    {0x81632DCC, 8, "265fdf8d42b463e411dc0701e89358cce9a0a327de6eae682d80b219e761a0b1"},
    {0x81634024, 24, "409eca34d906f3561418d62f8f9e20638314fad276581fc428b1d3338116db28"},
    {0x81632D90, 8, "c1f1e37e835889ba1ec551a3cf70795e45484b32eb4680ecd61a20750bf45d6e"},
    {0x81632F64, 16, "f697925e7fc49e2eaf5b1aea39d68367999279beaefb6ee17ab298c838b59db7"},
};
const Code* CodeAt(uint32_t at) {
  for (const auto& p : kCode) if (p.at == at) return &p;
  for (const auto& p : kAnimationCode) if (p.at == at) return &p;
  return nullptr;
}
std::optional<uint32_t> Pointer(uint64_t value) {
  const auto low = uint32_t(value);
  return value == low || value == uint64_t(int64_t(int32_t(low)))
             ? std::optional(low) : std::nullopt;
}
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  std::array<uint8_t, 4> raw;
  return internal::ReadNativeWaitSnapshot(memory, at, raw)
             ? std::optional(xe::load_and_swap<uint32_t>(raw.data()))
             : std::nullopt;
}
bool CheckCode(KernelState& kernel, UserModule& module, uint32_t at) {
  const auto* p = CodeAt(at);
  if (!p) return false;
  auto guard = global_critical_region::AcquireDirect();
  auto& memory = *kernel.memory();
  if (memory.QueryRangeAccess(p->at, p->bytes) == memory::PageAccess::kNoAccess)
    return false;
  for (uint32_t n = 0; n < p->bytes; n += 4) {
    xex2_section_type section;
    if (kernel.GetUserModuleForAddress(p->at + n).get() != &module ||
        !module.xex_module()->GetPageSectionType(p->at + n, &section) ||
        section != XEX_SECTION_CODE ||
        module.xex_module()->TranslateCode(p->at + n) !=
            memory.TranslateVirtual<const uint8_t*>(p->at + n)) return false;
  }
  std::array<uint8_t, 32> sum{};
  if (mbedtls_sha256(memory.TranslateVirtual<const uint8_t*>(at), p->bytes,
                     sum.data(), 0)) return false;
  constexpr char hex[] = "0123456789abcdef";
  for (size_t n = 0; n < sum.size(); ++n)
    if (p->sha[n*2] != hex[sum[n] >> 4] ||
        p->sha[n*2+1] != hex[sum[n] & 15]) return false;
  return true;
}
const NativeSystemUiPublicCallProfile* PublicAt(uint32_t at) {
  for (const auto& p : NativeSystemUiPublicProfiles()) {
    if (p.address != at) continue;
    switch (p.call) {
      case NativeSystemUiPublicCall::kAvatarInitialize:
      case NativeSystemUiPublicCall::kAvatarShutdown:
      case NativeSystemUiPublicCall::kAvatarGetAssetsResultSize:
      case NativeSystemUiPublicCall::kAvatarGetAssets:
      case NativeSystemUiPublicCall::kAvatarLoadAnimation:
      case NativeSystemUiPublicCall::kAvatarGetMetadataRandom:
      case NativeSystemUiPublicCall::kAvatarGetManifestLocalUser:
      case NativeSystemUiPublicCall::kAvatarSetManifest:
      case NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount:
      case NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile:
        return &p;
      default: return nullptr;
    }
  }
  return nullptr;
}
struct Message { uint32_t app, message, bytes; };
Message MessageFor(NativeSystemUiPublicCall call) {
  const auto* p=NativeAvatarRequestStart(call);
  return p?Message{p->app,p->message,p->payload_bytes}:Message{};
}
bool IsF2(NativeSystemUiPublicCall call) {
  const auto* p = NativeSystemUiPublicProfile(call);
  return p && p->family == NativeSystemPublicFamily::kAvatarEngine;
}
bool SameSource(const xam::ContentSourceState&,const xam::ContentSourceState&);
struct Frame {
  XThread* thread = nullptr;
  uint32_t pcr = 0, sp = 0, lr = 0, process = 0;
  bool operator==(const Frame&) const = default;
};
std::optional<Frame> ActualFrame(cpu::ppc::PPCContext& c, KernelState* kernel) {
  auto* t = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  const auto sp = Pointer(c.r[1]), pcr = Pointer(c.r[13]), lr = Pointer(c.lr);
  if (!t || !XThread::IsInThread(t) || t->kernel_state() != kernel ||
      !t->thread_state() || t->thread_state()->context() != &c ||
      c.thread_state != t->thread_state() || c.kernel_state != kernel ||
      c.processor != kernel->processor() || !sp || !*sp || (*sp & 15) ||
      !pcr || *pcr != t->pcr_ptr() || !lr) return {};
  const auto process = t->native_frame_thread_identity().process_type;
  if (process != X_PROCTYPE_TITLE && process != X_PROCTYPE_SYSTEM) return {};
  return Frame{t,*pcr,*sp,*lr,process};
}
}  // namespace
struct NativeAvatarService::State {
  struct CodeOwner {
    object_ref<UserModule> module;
    std::shared_ptr<KernelState::UserModuleExecutionLease> lease;
  };
  struct Session {
    uint64_t serial = 0;
    std::shared_ptr<const NativeAvatarBackingLease> arena;
    object_ref<UserModule> title;
    std::shared_ptr<KernelState::UserModuleExecutionLease> title_code;
    std::vector<CodeOwner> callback_code;
    std::array<uint32_t,5> callbacks{};
    uint32_t arena_base = 0, arena_bytes = 0, coordinate = 0, options = 0;
    uint32_t native_worker_handle = 0, worker_reference_handle = 0, initialize_task_body = 0;
    std::shared_ptr<NativeProfileIdentityOwner::TaskReference> worker_reference_receipt;
    std::optional<Frame> worker_frame;
    object_ref<XThread> worker_thread;
    std::optional<Frame> stop_frame;
    bool stop_returned = false;
    uint32_t stop_task_body = 0;
    std::optional<Frame> asset_cleanup_frame;
    uint32_t asset_cleanup_task_body = 0;
    std::array<uint32_t,2> cleanup_objects{};
    std::array<std::optional<Frame>,2> cleanup_release_frames;
    std::array<bool,2> cleanup_release_returned{};
    bool asset_cleanup_complete = false, asset_opening = false, asset_closing = false;
    bool worker_reference = false, worker_close_attempted = false,
         worker_exited = false, asset_closed = false;
    std::shared_ptr<vfs::Device> asset_mount;
    xam::ContentSourceState source;
    bool callbacks_copied = false, ready = false, shutdown_entered = false;
  };
  using Pair = NativeAvatarCallPair<Frame>;
  struct FrontendResident {
    Frame caller;
    object_ref<XThread> thread;
    uint32_t task_reference_handle = 0, task_body = 0, argument = 0;
    Pair frontend, builtin, setup, load, callback;
    std::optional<Frame> adapter;
    uint32_t frontend_result = 0, load_result = 0, callback_result = 0;
    bool lock_entered = false, observed = false;
    bool setup_adopted = false, load_adopted = false;
    bool aborting = false;
    std::shared_ptr<const NativeAvatarBackingLease> packet;
  };
  struct Control {
    uint32_t app=0;
    bool loading=false;
    Pair entry,schedule,worker;
    uint32_t output=0,expected_task_handle=0,task_reference_handle=0,schedule_result=kFailed;
    bool task_reference=false,close_attempted=false,uncertain=false;
    std::shared_ptr<NativeProfileIdentityOwner::TaskReference> task_reference_receipt;
    std::shared_ptr<const NativeAvatarBackingLease> stack;
  };
  struct Request {
    std::array<std::shared_ptr<Control>,2> controls;
    uint64_t serial = 0;
    NativeSystemUiPublicCall call;
    Message message;
    std::vector<uint64_t> args;
    Frame public_frame;
    object_ref<XThread> caller;
    std::vector<CodeOwner> caller_code;
    NativeImportDomain::Operation domain;
    std::shared_ptr<const NativeAvatarBackingLease> backing;
    std::shared_ptr<const NativeAvatarBackingLease> overlap_backing,parameters_backing;
    std::shared_ptr<Session> session;
    Pair start, copy, request_schedule, dispatch, completion, finalize, core_init, core_dispatch;
    Pair animation_job, animation_core, animation_writer;
    std::optional<NativeAvatarAnimationBuffers> animation;
    std::array<uint8_t,16> animation_id{};
    uint32_t animation_job_address = 0;
    uint32_t packet = 0, payload = 0, overlap = 0, task_reference_handle = 0;
    uint32_t input = 0, start_parameters = 0, copy_output = 0, start_result = kFailed,
             core_init_result = kFailed;
    uint32_t result = kFailed;
    bool public_returned = false, start_returned = false, client_published = false;
    bool core_dispatch_returned = false, cleanup_done = false;
    bool complete_returned = false, dispatch_returned = false;
    bool task_reference = false, task_close_attempted = false;
    std::shared_ptr<NativeProfileIdentityOwner::TaskReference> task_reference_receipt;
    bool core_init_returned = false, prepare_entered = false,
         prepare_returned = false;
    bool uncertain = false, done = false;
  };
  enum class RequestPairKind : uint8_t {
    kNone, kStart, kCopy, kDispatch, kComplete, kFinalize, kCoreInit,
    kCoreDispatch, kAnimationJob, kAnimationCore, kAnimationWriter
  };
  using RequestEntry = NativeAvatarRequestEntry<Request,RequestPairKind>;
  static Pair* RequestPair(Request& r, RequestPairKind kind) {
    switch (kind) {
      case RequestPairKind::kStart: return &r.start;
      case RequestPairKind::kCopy: return &r.copy;
      case RequestPairKind::kDispatch: return &r.dispatch;
      case RequestPairKind::kComplete: return &r.completion;
      case RequestPairKind::kFinalize: return &r.finalize;
      case RequestPairKind::kCoreInit: return &r.core_init;
      case RequestPairKind::kCoreDispatch: return &r.core_dispatch;
      case RequestPairKind::kAnimationJob: return &r.animation_job;
      case RequestPairKind::kAnimationCore: return &r.animation_core;
      case RequestPairKind::kAnimationWriter: return &r.animation_writer;
      default: return nullptr;
    }
  }
  static RequestPairKind RequestKind(uint32_t at) {
    switch (at) {
      case kStart: return RequestPairKind::kStart;
      case kCopy: return RequestPairKind::kCopy;
      case kDispatch: return RequestPairKind::kDispatch;
      case kComplete: return RequestPairKind::kComplete;
      case kFinalize: return RequestPairKind::kFinalize;
      case kCoreInit: return RequestPairKind::kCoreInit;
      case kCoreDispatch: return RequestPairKind::kCoreDispatch;
      case kAnimationJob: return RequestPairKind::kAnimationJob;
      case kAnimationCore: return RequestPairKind::kAnimationCore;
      case kAnimationWriter: return RequestPairKind::kAnimationWriter;
      default: return RequestPairKind::kNone;
    }
  }
  // Called at the successful admitted-entry cut under mutex. This owns only
  // the exact observed Request/attempt, not a native Task or completion.
  bool EnterRequestPair(const std::shared_ptr<Request>& r, RequestPairKind kind,
                        const Frame& frame, uint32_t value, RequestEntry& entry) {
    auto* pair = r ? RequestPair(*r, kind) : nullptr;
    if (!pair || entry.request || !r->serial || !next_request_entry ||
        next_request_entry == UINT64_MAX) return false;
    const auto invocation = next_request_entry++;
    if (!pair->Enter(frame, value, invocation)) return false;
    entry = {r, r->serial, invocation, kind};
    return true;
  }
  struct Invocation {
    NativeSystemUiPublicCall call;
    std::vector<uint64_t> args;
    Frame original;
    std::vector<CodeOwner> caller_code;
    // Captured before the delegated entry installs its synthetic return LR.
    // These counts belong only to this exact argument/caller/Session scope.
    std::optional<NativeAvatarAssetSizes> asset_sizes;
    std::shared_ptr<Session> sizing_session;
    bool bootstrap_close = false;
    bool shutdown_admission_observed = false;  // advisory first entry only
  };
  explicit State(Config c) : config(std::move(c)) {}
  Config config;
  std::shared_ptr<const GuestRuntimeCallExecutor::AvatarProviderBinding> binding;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  NativeImportDomain::Operation resident_domain;
  mutable std::mutex mutex;
  std::condition_variable changed;
  bool stopping = false, uncertain = false, f3_setup = false, f3_ready = false;
  bool f3_loaded = false;
  bool shutdown_done = false, shutdown_claimed = false, f3_shutdown_entered = false;
  std::atomic<bool> stopped_retired{false};
  bool animation_enabled = false; // Immutable optional plan extension after Create.
  uint64_t next_serial = 1, next_request_entry = 1;
  std::shared_ptr<Session> session;
  std::shared_ptr<Control> f3_unload;
  std::map<XThread*,std::vector<std::shared_ptr<Invocation>>> invocations;
  std::map<XThread*,std::vector<std::shared_ptr<Request>>> public_calls;
  std::map<uint64_t,std::shared_ptr<Request>> requests;
  std::map<std::pair<XThread*,uint32_t>,NativeAvatarEntryStack<Frame,RequestEntry>> entries;
  std::shared_ptr<FrontendResident> frontend_resident;
  // Shared native functions can run unowned callbacks while an owned call
  // waits on the SAME thread. Caller holds mutex and proved the exact live
  // frame/code. Pair only existing nesting; no standalone Avatar entry/owner.
  void TrackUnownedNestedEntryLocked(const Frame& frame,uint32_t at) {
    const auto it=entries.find({frame.thread,at});
    if(it!=entries.end())it->second.EnterPassthroughIfNested(frame);
  }
  // Retained typed Request Start, not a bare common-dispatch callback, classifies this
  // schedule. Strict ABI/session checks remain in EnterInternal below.
  bool IsOwnedRequestSchedule(const Frame& frame,const cpu::ppc::PPCContext& c) const {
    if(Pointer(c.r[3])!=kDispatch)return false;
    std::lock_guard lock(mutex);
    const auto it=public_calls.find(frame.thread);
    if(it==public_calls.end() || it->second.empty())return false;
    const auto& r=it->second.back();
    return NativeAvatarRequestStart(r->call) &&
        r->start.entered && !r->start_returned && r->copy.returned && r->packet &&
        !r->uncertain && !r->public_returned && Pointer(c.r[4])==r->packet;
  }
  // Advisory, preallocated receipts. Each immutable payload is published once
  // with 0 -> 1 -> 2; readers never inspect a partially written state1 cell.
  enum class DiagnosticPhase : uint32_t {
    kPublicEntry = 1, kPrepareEntry, kPrepareReturn, kControlScheduleReturn,
    kControlWorkerReturn, kControlReturn, kStartEntry, kStartReturn,
    kCoreEntry, kCoreReturn, kCompletionPublication, kPublicReturn,
    kAssetOpenEntry, kAssetOpenAdmission, kAssetOpenReturn, kGuardDenial,
    kAssetOpenArguments, kCompleteEntry, kAnimationWriterEntry,
    kAnimationWriterReturn, kAnimationCoreReturn, kAnimationJobReturn,
    kBootstrapShutdownAdmission, kCount
  };
  struct DiagnosticPayload {
    uint32_t phase=0,line=0,address=0,status=0,status_kind=0,available=0;
    uint32_t pcr=0,sp=0,lr=0,process=0,actual_task_body=0;
    uint32_t request_task_reference=0,control_task_reference=0,worker_task_reference=0;
    uint32_t initialize_task_body=0,callback_mask=0,control_app=0,packet=0,payload=0,overlap=0;
    uint32_t evaluated=0,failed=0,core_evaluated=0,core_passed=0;
    uint32_t core_candidates=0,last_core_task_reference=0;
    uint32_t animation_control=0,animation_capacity=0,animation_data=0,animation_job=0;
    uint64_t thread_wrapper=0,request=0,session=0,source_generation=0;
    uint64_t observed_source_generation=0,flags=0,last_core_request=0;
    xam::BorrowedContentMountDiagnostic mount;
  };
  struct DiagnosticCell {
    std::atomic<uint32_t> state{0};
    DiagnosticPayload payload;
    void Publish(const DiagnosticPayload& value) noexcept {
      uint32_t empty=0;
      if(!state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
      payload=value;
      state.store(2,std::memory_order_release);
    }
  };
  std::array<DiagnosticCell,size_t(DiagnosticPhase::kCount)> diagnostic_phases;
  DiagnosticCell first_negative;
  // Stack-only advisory cuts for the already-owned bootstrap Shutdown call.
  // Each original predicate is evaluated once, in its original short-circuit
  // position; no authority or resource is acquired by this receipt.
  struct ShutdownAdmissionCut {
    enum Stage : uint32_t {
      kFrame=1, kGuardRoot, kPublicRoot, kReader, kUncertain, kF3,
      kStopping, kCapacity, kSerial, kSession, kSessionReady,
      kShutdownEntered, kTitle, kDomain, kFinalTitle, kFinalReader,
      kFinalUncertain, kFinalF3, kFinalStopping, kFinalCapacity,
      kSessionChanged, kFinalSerial, kFinalSession, kFinalShutdownEntered,
      kFinalSessionReady, kWaitUncertain, kWaitDeadline, kOtherGuardExit
    };
    uint32_t evaluated=0, failed=0, line=0;
    bool admitted=false;
    bool Refused(Stage stage, bool value, uint32_t source) noexcept {
      evaluated|=uint32_t(1)<<uint32_t(stage);
      if(value && !failed){failed=uint32_t(stage);line=source;}
      return value;
    }
  };
  // First ORIGINAL uncertainty assignment, independent of negative return
  // phases. Site IDs map to the pinned original source; no resource authority.
  struct UncertaintyPayload {
    uint32_t line=0,site=0,available=0,function=0,evaluated=0,predicate_true=0;
    uint32_t pcr=0,sp=0,lr=0,process=0;
    uint32_t expected_pcr=0,expected_sp=0,expected_lr=0,expected_process=0;
    uint64_t thread_wrapper=0,expected_thread_wrapper=0;
  };
  struct UncertaintyCell {
    std::atomic<uint32_t> state{0};
    UncertaintyPayload payload;
  } first_uncertainty;
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  static bool ObserveUncertaintyPredicate(uint32_t& evaluated,uint32_t& truth,
      uint32_t bit,bool value) noexcept {
    evaluated|=1u<<bit;if(value)truth|=1u<<bit;return value;
  }
  void RecordFirstUncertainty(uint32_t line,uint32_t site,const Frame* frame=nullptr,
      uint32_t function=0,const Frame* expected=nullptr,
      uint32_t evaluated=0,uint32_t predicate_true=0) noexcept {
    uint32_t empty=0;
    if(!first_uncertainty.state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
    UncertaintyPayload value;value.line=line;value.site=site;value.function=function;
    value.evaluated=evaluated;value.predicate_true=predicate_true;if(evaluated)value.available|=8;
    if(function)value.available|=4;
    if(frame){value.available|=1;value.thread_wrapper=uint64_t(reinterpret_cast<uintptr_t>(frame->thread));
      value.pcr=frame->pcr;value.sp=frame->sp;value.lr=frame->lr;value.process=frame->process;}
    if(expected){value.available|=2;value.expected_thread_wrapper=uint64_t(reinterpret_cast<uintptr_t>(expected->thread));
      value.expected_pcr=expected->pcr;value.expected_sp=expected->sp;value.expected_lr=expected->lr;
      value.expected_process=expected->process;}
    first_uncertainty.payload=value;
    first_uncertainty.state.store(2,std::memory_order_release);
  }
  void RecordDiagnostic(const DiagnosticPayload& value) noexcept {
    diagnostic_phases[size_t(value.phase)].Publish(value);
    // HRESULT and Win32 outcomes are distinct. Guard denials never become an
    // observed native return, even when their status has its high bit set.
    if((value.status_kind==1 && int32_t(value.status)<0) ||
       (value.status_kind==2 && value.status!=0))first_negative.Publish(value);
  }
  static DiagnosticPayload PartialDiagnostic(DiagnosticPhase phase,uint32_t line,
      uint32_t status,uint32_t kind,const Frame& frame) noexcept {
    DiagnosticPayload value;
    value.phase=uint32_t(phase);value.line=line;value.status=status;value.status_kind=kind;
    value.available=1;value.thread_wrapper=uint64_t(reinterpret_cast<uintptr_t>(frame.thread));
    value.pcr=frame.pcr;value.sp=frame.sp;value.lr=frame.lr;value.process=frame.process;
    return value;
  }
  // Called only under an ORIGINAL owner metadata lock. No native/provider
  // query, allocation, resource copy, wait, logging or additional lock here.
  DiagnosticPayload DiagnosticLocked(DiagnosticPhase phase,uint32_t line,
      uint32_t status,uint32_t kind,const Frame& frame,
      const Request* r=nullptr,const Control* control=nullptr,uint32_t task=0) const noexcept {
    auto value=PartialDiagnostic(phase,line,status,kind,frame);
    value.available|=2;value.actual_task_body=task;if(task)value.available|=4;
    const Session* held=r && r->session?r->session.get():session.get();
    value.flags=uint64_t(stopping) | (uint64_t(uncertain)<<1) | (uint64_t(f3_ready)<<2);
    if(r){
      value.available|=8;value.request=r->serial;value.request_task_reference=r->task_reference_handle;
      value.packet=r->packet;value.payload=r->payload;value.overlap=r->overlap;
      if(r->animation){value.available|=128;value.animation_control=r->animation->control;
        value.animation_capacity=r->animation->capacity;value.animation_data=r->animation->data;
        value.animation_job=r->animation_job_address;}
      value.flags|=(uint64_t(r->uncertain)<<3) | (uint64_t(r->prepare_entered)<<4) |
          (uint64_t(r->prepare_returned)<<5) | (uint64_t(r->start.entered)<<6) |
          (uint64_t(r->start_returned)<<7) | (uint64_t(r->core_init.entered)<<8) |
          (uint64_t(r->core_init_returned)<<9) | (uint64_t(r->client_published)<<10) |
          (uint64_t(r->complete_returned)<<11) | (uint64_t(r->task_reference)<<12);
    }
    if(held){
      value.available|=16;value.session=held->serial;value.source_generation=held->source.generation;
      value.worker_task_reference=held->worker_reference_handle;
      value.initialize_task_body=held->initialize_task_body;
      for(size_t n=0;n<held->callbacks.size();++n)if(held->callbacks[n])value.callback_mask|=1u<<n;
      value.flags|=(uint64_t(held->callbacks_copied)<<13) | (uint64_t(held->ready)<<14) |
          (uint64_t(bool(held->asset_mount))<<15) | (uint64_t(held->asset_opening)<<16) |
          (uint64_t(held->asset_closed)<<17) | (uint64_t(held->asset_cleanup_complete)<<18) |
          (uint64_t(held->worker_reference)<<19) | (uint64_t(held->worker_exited)<<20);
    }
    if(control){
      value.available|=32;value.control_app=control->app;
      value.control_task_reference=control->task_reference_handle;
      value.flags|=(uint64_t(control->loading)<<21) | (uint64_t(control->uncertain)<<22) |
          (uint64_t(control->task_reference)<<23);
    }
    return value;
  }
  bool RootCurrent(uint32_t at) const {
    if(stopped_retired.load(std::memory_order_acquire) || !config.calls ||
        !config.kernel || !config.domain || !config.allocation)return false;
    auto module = config.calls->module();
    return module && root_code && module->load_generation() == config.calls->load_generation() &&
        config.kernel == config.calls->kernel() && module->native_import_ready() &&
        config.domain->active() && config.allocation->IsAccepting() &&
        config.allocation->MatchesDomain(config.domain) &&
        config.domain->root() == NativeImportDomain::Member{module->hmodule_ptr(),module->load_generation()} &&
        binding && binding->issuer_ == config.calls.get() &&
        CheckCode(*config.kernel,*module,at);
  }
  bool RootData(uint32_t at,uint32_t bytes) const {
    const auto module=config.calls->module();
    if(!module || !bytes || uint64_t(at)+bytes>(uint64_t(1)<<32))return false;
    auto guard=global_critical_region::AcquireDirect();
    if(config.kernel->memory()->QueryRangeAccess(at,bytes)==memory::PageAccess::kNoAccess)return false;
    for(uint64_t p=at;p<uint64_t(at)+bytes;p=(p&~uint64_t(0xFFF))+0x1000){
      xex2_section_type section;
      if(config.kernel->GetUserModuleForAddress(uint32_t(p)).get()!=module.get() ||
         !module->xex_module()->GetPageSectionType(uint32_t(p),&section) ||
         (section!=XEX_SECTION_DATA && section!=XEX_SECTION_READONLY_DATA))return false;
    }
    return true;
  }
  bool AnimationCurrent() const {
    if(!animation_enabled)return false;
    for(const auto& code:kAnimationCode)if(!RootCurrent(code.at))return false;
    // Fixed tables/marker only, never the mutable provider globals or guessed
    // host asset bytes. Same root/data/code leases as the existing owner.
    auto guard=global_critical_region::AcquireDirect();
    for(const auto& data:kAnimationData){
      if(!RootData(data.at,data.bytes))return false;
      std::array<uint8_t,32> sum{};
      if(mbedtls_sha256(config.kernel->memory()->TranslateVirtual<const uint8_t*>(data.at),
                       data.bytes,sum.data(),0))return false;
      constexpr char hex[]="0123456789abcdef";
      for(size_t n=0;n<sum.size();++n)
        if(data.sha[2*n]!=hex[sum[n]>>4] || data.sha[2*n+1]!=hex[sum[n]&15])return false;
    }
    return true;
  }
  bool ReaderCurrent() const {
    const auto pm = config.kernel->xam_state()->profile_manager();
    return pm && config.profile_reader && config.profile_reader->Current(*pm);
  }
  bool TitleCurrent(bool stopping_allowed = false) const {
    auto* emulator = config.kernel->emulator();
    const auto title = config.kernel->GetExecutableModule();
    if (!emulator || !emulator->OwnsTitleExecution(config.kernel) || !title ||
        title->hmodule_ptr() != config.title.hmodule ||
        title->load_generation() != config.title.load_generation) return false;
    const auto f = emulator->title_execution_state().snapshot();
    return f.generation == config.title.foreground_generation &&
        f.hmodule == config.title.hmodule && f.main_thread_id == config.title.main_thread_id &&
        (f.phase == TitleExecutionState::Phase::kRunning ||
         (stopping_allowed && (f.phase == TitleExecutionState::Phase::kStopping ||
                               f.phase == TitleExecutionState::Phase::kPreparing)));
  }
  std::optional<CodeOwner> RetainCode(uint32_t at, bool title_process) {
    if (!at || (at & 3)) return {};
    auto module = config.kernel->GetUserModuleForAddress(at);
    xex2_section_type section;
    if (!module || module->module_format() != UserModule::kModuleFormatXex ||
        !module->xex_module()->GetPageSectionType(at,&section) || section != XEX_SECTION_CODE)
      return {};
    if (module->is_system_app_module() &&
        !config.domain->Contains({module->hmodule_ptr(),module->load_generation()})) return {};
    if (!module->is_system_app_module() && !title_process) return {};
    auto lease = config.kernel->AcquireUserModuleExecutionLease(module);
    if (!lease) return {};
    return CodeOwner{std::move(module),std::move(lease)};
  }
  void Poison(uint32_t line,uint32_t site,const std::shared_ptr<Request>& request = {},
      const Frame* frame=nullptr,uint32_t function=0) {
    std::lock_guard lock(mutex);
    RecordFirstUncertainty(line,site,frame,function);
    uncertain = true;
    if (request) request->uncertain = true;
    changed.notify_all();
  }
  std::optional<uint32_t> CurrentTaskCached(XThread& thread) const {
    auto& memory = *config.kernel->memory();
    const auto slot = Word(memory,0x81A82570);
    uint32_t tls = 0;
    if (!slot || !thread.GetTLSValue(*slot,&tls)) return {};
    const auto native_context = tls ? Word(memory,tls) : std::nullopt;
    const auto body=native_context && *native_context && *native_context <= UINT32_MAX-0xC
               ? Word(memory,*native_context+0xC) : std::nullopt;
    return body && *body>=0x1000 && !(*body&3) ? body : std::nullopt;
  }
  cpu::GuestCallResult Helper(XThread& t, uint32_t at,
                               std::span<const uint32_t> args) {
    if ((at != kCurrentTask && at != kCloseTask && at != kUnload) ||
        !RootCurrent(at) || !XThread::IsInThread(&t)) return {};
    std::vector<cpu::GuestCallArgument> packed;
    for (auto arg : args) packed.push_back(cpu::GuestCallArgument::UInt32(arg));
    return cpu::ExecuteGuestCall(config.kernel->processor(),t.thread_state(),at,packed);
  }
  using TaskReference=NativeProfileIdentityOwner::TaskReference;
  static NativeProfileIdentityOwner::TaskReferenceFrame ReferenceFrame(const Frame& f,uint32_t at) {
    return {at,f.pcr,f.sp,f.lr};
  }
  struct OwnedAcquire {
    cpu::GuestCallResult call;
    std::optional<uint32_t> current;
    bool proven=false;
  };
  OwnedAcquire AcquireOwnedTask(XThread& thread,const Frame& frame,uint32_t at,
      std::shared_ptr<void> producer,std::shared_ptr<TaskReference>& slot,
      NativeProfileIdentityOwner::TaskReferenceAcquireKind kind=NativeProfileIdentityOwner::TaskReferenceAcquireKind::kWork) {
    const auto before=CurrentTaskCached(thread);
    if(!before)return {};
    auto acquisition=NativeProfileIdentityOwner::BeforeNativeTaskReferenceAcquire(
        config.kernel,thread,ReferenceFrame(frame,at),*before,std::move(producer),kind);
    if(!acquisition.admitted())return {};
    {
      std::lock_guard lock(mutex);
      if(slot)return {}; // A partial acquisition can never be retried.
      slot=acquisition.reference(); // Attach BEFORE the actual native increment.
    }
    OwnedAcquire result;
    result.call=Helper(thread,kCurrentTask,{});
    result.current=CurrentTaskCached(thread);
    result.proven=acquisition.Complete(result.call.executed,result.call.value,result.current.value_or(0));
    return result;
  }
  bool CloseOwnedTask(XThread& thread,uint32_t task,const std::shared_ptr<TaskReference>& reference,
      const Frame& actual,uint32_t at,const Frame& baseline,uint32_t callback,
      std::shared_ptr<void> producer) {
    // The scope spans the complete call/result tail. Guard-aborted Any return
    // alone is not evidence that the actual native reference was released.
    auto close=NativeProfileIdentityOwner::BeforeNativeTaskReferenceClose(
        config.kernel,thread,task,reference,ReferenceFrame(actual,at),
        ReferenceFrame(baseline,callback),std::move(producer));
    if(!close.admitted())return false;
    const std::array<uint32_t,1> args{task};
    const auto actual_close=Helper(thread,kCloseTask,args);
    return close.Complete(actual_close.executed,uint32_t(actual_close.value));
  }
  std::vector<std::shared_ptr<Control>> Controls() const {
    std::vector<std::shared_ptr<Control>> result;
    if(f3_unload)result.push_back(f3_unload);
    for(const auto& [serial,r]:requests)for(const auto& c:r->controls)if(c)result.push_back(c);
    return result; // Caller owns metadata mutex; destruction outside it.
  }
  bool CloseRequestTask(XThread&,const std::shared_ptr<Request>&,const Frame&,uint32_t);
  void ReapCompleted();
  bool EnterPublic(cpu::ppc::PPCContext&, const Frame&,
                     const NativeSystemUiPublicCallProfile&,
                     ShutdownAdmissionCut* = nullptr);
  void LeavePublic(cpu::ppc::PPCContext&, const Frame&,
                     const NativeSystemUiPublicCallProfile&);
  bool EnterResident(cpu::ppc::PPCContext&, const Frame&, uint32_t);
  void LeaveResident(cpu::ppc::PPCContext&, const Frame&, uint32_t);
  bool EnterInternal(cpu::ppc::PPCContext&, const Frame&, uint32_t, RequestEntry&);
  void LeaveInternal(cpu::ppc::PPCContext&, const Frame&, uint32_t, const RequestEntry&);
  std::optional<NativeAvatarAssetSizes> NativeSizes(XThread&,uint16_t);
};

std::optional<NativeAvatarAssetSizes>
NativeAvatarService::State::NativeSizes(XThread& thread, uint16_t mask) {
  const uint32_t scratch = config.kernel->memory()->SystemHeapAlloc(8);
  if (!scratch) return {};
  std::optional<NativeAvatarAssetSizes> sizes;
  try {
    const std::array<uint64_t,3> args{mask,scratch,scratch+4};
    const auto called = NativeAvatarService::TryInvokeOnOriginalCaller(
        config.kernel,NativeSystemUiPublicCall::kAvatarGetAssetsResultSize,args);
    if (called.disposition == NativeSystemUiPublicCallResult::Disposition::kReturned &&
        int32_t(called.value) >= 0) {
      sizes = NativeAvatarAssetSizes::CaptureActualReturn(mask,true,called.value,
          Word(*config.kernel->memory(),scratch),
          Word(*config.kernel->memory(),scratch+4));
    }
  } catch (...) { Poison(__LINE__,1,{},nullptr,0); }
  // The nested normal return has already reaped its finished backing token.
  config.kernel->memory()->SystemHeapFree(scratch);
  return sizes;
}

bool NativeAvatarService::State::EnterPublic(
    cpu::ppc::PPCContext& c, const Frame& frame,
    const NativeSystemUiPublicCallProfile& profile, ShutdownAdmissionCut* shutdown_cut) {
  using Cut=ShutdownAdmissionCut;
  const auto refused=[&](Cut::Stage stage,bool value,uint32_t line) noexcept {
    return shutdown_cut?shutdown_cut->Refused(stage,value,line):value;
  };
  if (refused(Cut::kPublicRoot,!RootCurrent(profile.address),__LINE__) ||
      refused(Cut::kReader,!ReaderCurrent(),__LINE__) ||
      (profile.call==NativeSystemUiPublicCall::kAvatarLoadAnimation && !AnimationCurrent())) return false;
  std::vector<uint64_t> args;
  for (size_t n = 0; n < profile.argument_count; ++n) {
    const auto value = Pointer(c.r[n+3]);
    if (!value) return false;
    args.push_back(*value);
  }
  std::shared_ptr<Invocation> invocation;
  std::shared_ptr<Session> captured_session;
  bool closing = false;
  {
    std::lock_guard lock(mutex);
    const auto it = invocations.find(frame.thread);
    if (it != invocations.end() && !it->second.empty()) {
      const auto& candidate = it->second.back();
      if (candidate->call == profile.call && candidate->args == args)
        invocation = candidate;
    }
    closing = invocation && invocation->bootstrap_close;
    if (refused(Cut::kUncertain,uncertain,__LINE__) ||
        refused(Cut::kF3,!f3_ready,__LINE__) ||
        refused(Cut::kStopping,(stopping && !closing),__LINE__) ||
        refused(Cut::kCapacity,requests.size() >= 64,__LINE__) ||
        refused(Cut::kSerial,next_serial == UINT64_MAX,__LINE__)) return false;
    captured_session = session;
    if (profile.call == NativeSystemUiPublicCall::kAvatarInitialize) {
      if (captured_session) return false;
    } else if (IsF2(profile.call)) {
      if (refused(Cut::kSession,!captured_session,__LINE__) ||
          refused(Cut::kSessionReady,(!captured_session->ready && !closing),__LINE__) ||
          refused(Cut::kShutdownEntered,captured_session->shutdown_entered,__LINE__)) return false;
    }
  }
  if (refused(Cut::kTitle,!TitleCurrent(closing),__LINE__)) return false;
  auto request = std::make_shared<Request>();
  request->call = profile.call;
  request->message = MessageFor(profile.call);
  if(profile.call==NativeSystemUiPublicCall::kAvatarLoadAnimation && !animation_enabled)return false;
  request->args = args;
  request->public_frame = frame;
  request->caller = retain_object(frame.thread);
  request->domain = config.domain->Acquire();
  if (refused(Cut::kDomain,!request->domain,__LINE__)) return false;
  if (invocation) request->caller_code = invocation->caller_code;
  else {
    if (frame.lr < 4 || (frame.lr & 3)) return false;
    auto code = RetainCode(frame.lr-4,frame.process == X_PROCTYPE_TITLE);
    if (!code) return false;
    request->caller_code.push_back(std::move(*code));
  }
  std::vector<NativeAvatarBackingRange> backing;
  const auto add = [&](uint32_t at, uint32_t bytes, bool writable) {
    if (bytes) backing.push_back({at,bytes,writable});
  };
  const auto overlap = [&](uint32_t at) {
    if (at) add(at,sizeof(XAM_OVERLAPPED),true);
  };
  if (profile.call == NativeSystemUiPublicCall::kAvatarInitialize) {
    if (args[5] < 0x100000u || args[5] > UINT32_MAX-args[4]) return false;
    auto created = std::make_shared<Session>();
    created->title = config.kernel->GetExecutableModule();
    created->title_code = config.kernel->AcquireUserModuleExecutionLease(created->title);
    created->arena_base = uint32_t(args[4]); created->arena_bytes = uint32_t(args[5]);
    created->coordinate = uint32_t(args[0]); created->options = uint32_t(args[1]);
    const auto source = config.content->CaptureAvatarSource(*config.calls);
    if (!created->title_code || !source) return false;
    created->source = *source;
    const std::array<NativeAvatarBackingRange,1> arena{{{created->arena_base,created->arena_bytes,true}}};
    created->arena = config.kernel->memory()->BorrowNativeAvatarBacking(arena);
    if (!created->arena) return false;
    add(uint32_t(args[3]),20,false);
    request->session = std::move(created);
  } else {
    request->session = IsF2(profile.call) ? captured_session : nullptr;
    switch (profile.call) {
      case NativeSystemUiPublicCall::kAvatarGetAssetsResultSize:
        add(uint32_t(args[1]),4,true); add(uint32_t(args[2]),4,true); break;
      case NativeSystemUiPublicCall::kAvatarGetAssets: {
        // A host-delegated call already ran the real calculator while its
        // authentic caller LR was present. Never query again from the guest
        // call sentinel. Direct native callers retain their original path.
        if (invocation && invocation->sizing_session != captured_session) return false;
        const auto observed = invocation ? invocation->asset_sizes :
            NativeSizes(*frame.thread,uint16_t(args[1]));
        const auto sizes = observed ? observed->ForMask(uint16_t(args[1])) : std::nullopt;
        if (!sizes) return false;
        add(uint32_t(args[0]),1000,false);
        add(uint32_t(args[3]),sizes->first,true);
        add(uint32_t(args[4]),sizes->second,true);
        overlap(uint32_t(args[5])); break;
      }
      case NativeSystemUiPublicCall::kAvatarLoadAnimation: {
        auto& memory=*config.kernel->memory();
        if(!args[0] || !args[2] ||
           args[2]>UINT32_MAX-NativeAvatarAnimationBuffers::kControlBytes+1)return false;
        const auto capacity=Word(memory,uint32_t(args[2])+NativeAvatarAnimationBuffers::kCapacityOffset);
        const auto data=Word(memory,uint32_t(args[2])+NativeAvatarAnimationBuffers::kDataOffset);
        if(!capacity || !data ||
           !internal::ReadNativeWaitSnapshot(memory,uint32_t(args[0]),request->animation_id))return false;
        request->animation=CaptureNativeAvatarAnimationBuffers(uint32_t(args[2]),*capacity,*data);
        if(!request->animation)return false;
        add(uint32_t(args[0]),16,false);
        add(request->animation->control,NativeAvatarAnimationBuffers::kControlBytes,true);
        add(request->animation->data,request->animation->capacity,true);
        overlap(uint32_t(args[3]));break;
      }
      case NativeSystemUiPublicCall::kAvatarGetMetadataRandom:
        if (args[1] > UINT32_MAX/1000u) return false;
        add(uint32_t(args[2]),uint32_t(args[1])*1000u,true);
        overlap(uint32_t(args[3])); break;
      case NativeSystemUiPublicCall::kAvatarGetManifestLocalUser:
      case NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfile:
        add(uint32_t(args[1]),1000,true); overlap(uint32_t(args[2])); break;
      case NativeSystemUiPublicCall::kAvatarSetManifest:
        add(uint32_t(args[1]),1000,false); overlap(uint32_t(args[2])); break;
      case NativeSystemUiPublicCall::kAvatarGetMetadataSignedOutProfileCount:
        add(uint32_t(args[0]),4,true); overlap(uint32_t(args[1])); break;
      case NativeSystemUiPublicCall::kAvatarShutdown: break;
      default: return false;
    }
  }
  if (!backing.empty()) {
    request->backing = config.kernel->memory()->BorrowNativeAvatarBacking(backing);
    if (!request->backing) return false;
  }
  if (refused(Cut::kFinalTitle,!TitleCurrent(closing),__LINE__) ||
      refused(Cut::kFinalReader,!ReaderCurrent(),__LINE__)) return false;
  {
    std::lock_guard lock(mutex);
    if (refused(Cut::kFinalUncertain,uncertain,__LINE__) ||
        refused(Cut::kFinalF3,!f3_ready,__LINE__) ||
        refused(Cut::kFinalStopping,(stopping && !closing),__LINE__) ||
        refused(Cut::kFinalCapacity,requests.size() >= 64,__LINE__) ||
        refused(Cut::kSessionChanged,session != captured_session,__LINE__) ||
        refused(Cut::kFinalSerial,next_serial == UINT64_MAX,__LINE__)) return false;
    if (IsF2(profile.call) &&
        profile.call != NativeSystemUiPublicCall::kAvatarInitialize &&
        (refused(Cut::kFinalSession,!captured_session,__LINE__) ||
         refused(Cut::kFinalShutdownEntered,captured_session->shutdown_entered,__LINE__) ||
         refused(Cut::kFinalSessionReady,(!captured_session->ready && !closing),__LINE__))) return false;
    request->serial = next_serial++;
    if (profile.call == NativeSystemUiPublicCall::kAvatarInitialize) {
      request->session->serial = request->serial;
      session = request->session;
      shutdown_done = false;
    }
    if (profile.call == NativeSystemUiPublicCall::kAvatarShutdown)
      request->session->shutdown_entered = true;
    requests.emplace(request->serial,request);
    public_calls[frame.thread].push_back(request);
    if(profile.call==NativeSystemUiPublicCall::kAvatarInitialize)
      RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kPublicEntry,__LINE__,0,0,frame,request.get()));
  }
  if(profile.call==NativeSystemUiPublicCall::kAvatarShutdown){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;){
      bool ready=true;
      {std::lock_guard lock(mutex);
        if(refused(Cut::kWaitUncertain,uncertain,__LINE__))return false;
        for(const auto& [serial,other]:requests)
          if(other!=request && other->session==request->session &&
             !other->public_returned)ready=false;
      }
      if(ready)break;
      if(refused(Cut::kWaitDeadline,std::chrono::steady_clock::now()>=deadline,__LINE__)){Poison(__LINE__,2,request,&frame,profile.address);return false;}
      // Same actual original caller/context, no service/Kernel lock held.
      // Already-returned asynchronous jobs remain for original native Stop.
      frame.thread->Delay(0,0,uint64_t(-160000ll));
    }
  }
  return true;
}

bool NativeAvatarService::State::CloseRequestTask(
    XThread& thread,const std::shared_ptr<Request>& request,const Frame& actual,uint32_t at) {
  uint32_t task = 0;
  Frame baseline;
  std::shared_ptr<TaskReference> reference;
  {
    std::lock_guard lock(mutex);
    if (!request->cleanup_done || !request->complete_returned || request->uncertain)
      return false;
    if (!request->task_reference) { request->done = true; return true; }
    if (request->task_close_attempted) return false;
    request->task_close_attempted = true;
    task = request->task_reference_handle;
    reference=request->task_reference_receipt;
    const auto& dispatch=request->dispatch;
    if(dispatch.entered && dispatch.frame.thread==&thread && dispatch.frame.pcr==actual.pcr &&
        (!dispatch.returned || (at==kDispatch && dispatch.frame==actual)))baseline=dispatch.frame;
  }
  const auto current = CurrentTaskCached(thread);
  const bool closed = current && NativeAvatarTaskReferenceMatchesBody(task,*current) &&
      CloseOwnedTask(thread,task,reference,actual,at,baseline,kDispatch,request);
  if (!closed) { Poison(__LINE__,3,request,nullptr,0); return false; }
  {
    std::lock_guard lock(mutex);
    request->task_reference = false;
    request->done = true;
    changed.notify_all();
  }
  return true;
}

void NativeAvatarService::State::ReapCompleted() {
  std::vector<std::shared_ptr<Request>> released;
  {
    std::lock_guard lock(mutex);
    for (auto it = requests.begin(); it != requests.end();) {
      if (it->second->done && it->second->public_returned &&
          !it->second->uncertain && !it->second->task_reference) {
        released.push_back(std::move(it->second));
        it = requests.erase(it);
      } else ++it;
    }
  }
  // Backing/module/domain destructors always follow metadata lock release.
}

void NativeAvatarService::State::LeavePublic(
    cpu::ppc::PPCContext& c, const Frame& frame,
    const NativeSystemUiPublicCallProfile& profile) {
  std::shared_ptr<Request> request;
  {
    std::lock_guard lock(mutex);
    const auto f = public_calls.find(frame.thread);
    if (f == public_calls.end() || f->second.empty()) return;
    request = f->second.back();
    if (request->call != profile.call || request->public_frame != frame) {
      (RecordFirstUncertainty(__LINE__,27,&frame,profile.address), uncertain = true); request->uncertain = true; return;
    }
    f->second.pop_back();
    if (f->second.empty()) public_calls.erase(f);
    request->public_returned = true;
    request->result = uint32_t(c.r[3]);
    if(profile.call==NativeSystemUiPublicCall::kAvatarInitialize)
      RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kPublicReturn,__LINE__,request->result,1,frame,request.get()));
    if (!request->message.app) request->done = true;
    if (request->message.app && !request->start.entered) {
      // Initialization may fail in the original named-load path before any IO
      // request was allocated. Other successful wrappers must show transport.
      if (int32_t(request->result) < 0) request->done = true;
      else { request->uncertain = true; (RecordFirstUncertainty(__LINE__,28,&frame,profile.address), uncertain = true); }
    }
    changed.notify_all();
  }
  if (profile.call == NativeSystemUiPublicCall::kAvatarGetAssetsResultSize &&
      int32_t(request->result) >= 0) {
    const auto cpu = Word(*config.kernel->memory(),uint32_t(request->args[1]));
    const auto gpu = Word(*config.kernel->memory(),uint32_t(request->args[2]));
    if (!cpu || !*cpu || !gpu) {
      Poison(__LINE__,4,request,&frame,profile.address); c.r[3] = uint64_t(int64_t(int32_t(kFailed)));
    }
  }
  if (profile.call == NativeSystemUiPublicCall::kAvatarInitialize) {
    const auto state = Word(*config.kernel->memory(),0x81B4AF50);
    const auto pending = Word(*config.kernel->memory(),0x81B4AF54);
    const auto worker = Word(*config.kernel->memory(),0x81B4AF58);
    const auto toc = Word(*config.kernel->memory(),0x81B4B1FC);
    bool ready = state == 1u && pending && worker && *worker && toc && *toc &&
        config.content->CaptureAvatarSource(*config.calls).has_value();
    {
      std::lock_guard lock(mutex);
      ready = ready && !request->uncertain && request->session == session &&
          session && session->callbacks_copied && session->asset_mount &&
          !session->asset_mount->namespace_retiring() && !session->worker_exited &&
          (!session->worker_reference || session->worker_reference_handle==*worker);
      if (int32_t(request->result) >= 0) {
        if (ready) { session->native_worker_handle = *worker; session->ready = true; }
        else { (RecordFirstUncertainty(__LINE__,29,&frame,profile.address), uncertain = true); request->uncertain = true;
               c.r[3] = uint64_t(int64_t(int32_t(kFailed))); }
      }
    }
  }
  if(profile.call==NativeSystemUiPublicCall::kAvatarInitialize &&
     int32_t(uint32_t(c.r[3]))<0){
    auto& memory=*config.kernel->memory();
    const auto module=Word(memory,0x81AA1478);
    const bool native_stopped=Word(memory,0x81B4AF50)==0u &&
        Word(memory,0x81B4AF54)==0u && Word(memory,0x81B4AF58)==0u &&
        Word(memory,0x81AA1480)==0u && module && (!*module || *module==UINT32_MAX);
    std::shared_ptr<Session> released;
    {std::lock_guard lock(mutex);
      const auto held=request->session;
      const auto& load=request->controls[0];const auto& unload=request->controls[1];
      const bool no_load_task=load && load->entry.returned && load->schedule.returned &&
          int32_t(load->schedule_result)<0 && !load->worker.entered && !load->uncertain;
      const bool real_unload=unload && unload->entry.returned && !unload->uncertain &&
          !unload->task_reference;
      if(native_stopped && held && session==held && !uncertain &&
         (no_load_task || real_unload) && !held->worker_reference &&
         (!held->worker_frame || held->worker_exited) &&
         (!held->asset_mount || (held->asset_closed && held->asset_cleanup_complete)))
        released=std::move(session);
      // Otherwise retain the actual partial/uncertain native owner. Negative
      // HRESULT alone never grants permission to release the caller arena.
    }
  }
  if(profile.call==NativeSystemUiPublicCall::kAvatarShutdown){
    auto& memory=*config.kernel->memory();
    const auto module=Word(memory,0x81AA1478);
    bool stopped=Word(memory,0x81B4AF50)==0u && Word(memory,0x81B4AF54)==0u &&
        Word(memory,0x81B4AF58)==0u && Word(memory,0x81AA1480)==0u &&
        Word(memory,0x81AA14F4)==0u && Word(memory,0x81AA14F8)==0u &&
        Word(memory,0x81AA147C)==0u && module && (!*module || *module==UINT32_MAX);
    std::shared_ptr<Session> released;
    {
      std::lock_guard lock(mutex);
      const auto held=request->session;
      stopped=stopped && held && session==held && !uncertain &&
          !held->worker_reference && (!held->worker_frame || held->worker_exited) &&
          (!held->ready || (held->stop_frame && held->stop_returned)) &&
          (!held->asset_mount || (held->asset_closed && held->asset_cleanup_complete));
      for(const auto& [serial,r]:requests)
        if(r!=request && r->session==held && (!r->done || !r->public_returned))stopped=false;
      if(stopped){held->ready=false;released=std::move(session);}
      else {(RecordFirstUncertainty(__LINE__,30,&frame,profile.address), uncertain=true);request->uncertain=true;}
      changed.notify_all();
    }
    // Module/callback/arena/device owners drop after the metadata lock. A
    // failed close/drain leaves the entire Session discoverable and retained.
  }
  ReapCompleted();
}

// The caller/Task was retained by the existing startup producer before this
// exact FE ExecuteGuestCall. Hashes cover every internal frame adjustment and
// callback dispatch used below. No package/source readiness is conferred here.
bool NativeAvatarService::State::EnterResident(
    cpu::ppc::PPCContext& c, const Frame& frame, uint32_t at) {
  const auto task = CurrentTaskCached(*frame.thread);
  std::shared_ptr<FrontendResident> resident;
  { std::lock_guard lock(mutex); resident = frontend_resident; }
  if (!resident || !task || *task!=resident->task_body ||
      !NativeAvatarTaskReferenceMatchesBody(resident->task_reference_handle,*task) ||
      frame.process != X_PROCTYPE_SYSTEM || frame.thread != resident->caller.thread ||
      frame.pcr != resident->caller.pcr) return false;
  auto& memory = *config.kernel->memory();
  const auto edge = [&](NativeAvatarResidentEdge kind, const Pair& parent) {
    return parent.entered && !parent.returned &&
           MatchesNativeAvatarResidentEdge(kind, parent.frame.sp, frame.sp, frame.lr);
  };
  if (at == kFrontend) {
    std::lock_guard lock(mutex);
    return !uncertain && !stopping && !resident->observed &&
        resident->caller.sp >= cpu::kGuestCallExecuteStackPadding &&
        frame.sp == resident->caller.sp - cpu::kGuestCallExecuteStackPadding &&
        frame.lr == 0xBCBCBCBC && c.r[3] == resident->argument &&
        resident->frontend.Enter(frame, resident->argument);
  }
  if (at == kBuiltin) {
    std::lock_guard lock(mutex);
    return !uncertain && !stopping && c.r[3] == 1 && c.r[4] == resident->argument &&
        edge(NativeAvatarResidentEdge::kFrontendBuiltin, resident->frontend) &&
        resident->builtin.Enter(frame, 1);
  }
  if (at == 0x816BCC38) {
    std::lock_guard lock(mutex);
    return !uncertain && !stopping && Pointer(c.r[3]) == 0x81AA13B4u && c.r[4] == 0xF3 &&
        Pointer(c.r[5]) == 0x815F915Cu && Pointer(c.r[6]) == kF3 && !c.r[7] && !c.r[8] &&
        edge(NativeAvatarResidentEdge::kBuiltinSetup, resident->builtin) &&
        resident->setup.Enter(frame, 0x81AA13B4);
  }
  if (at == 0x816BCD08) {
    if (frame.sp < 0x60 || Pointer(c.r[3]) != 0x81AA13B4u || c.r[4] || c.r[5] || c.r[6])
      return false;
    const std::array ranges{NativeAvatarBackingRange{frame.sp - 0x60, 0x20, true}};
    auto packet = memory.BorrowNativeAvatarBacking(ranges);
    if (!packet) return false;
    std::lock_guard lock(mutex);
    if (uncertain || stopping || !resident->setup.returned || resident->packet ||
        !edge(NativeAvatarResidentEdge::kBuiltinLoad, resident->builtin) ||
        !resident->load.Enter(frame, 0x81AA13B4)) return false;
    resident->packet = std::move(packet);
    return true;
  }
  if (at == kF3 || at == kF3Body) {
    // BCD08's own frame−B0; packet+60/output+50. A2B90/A29C0
    // add−90/−80; adapter tails body without changing entry SP/LR.
    uint32_t load_sp=0;
    std::shared_ptr<const NativeAvatarBackingLease> packet;
    { std::lock_guard lock(mutex);load_sp=resident->load.frame.sp;packet=resident->packet; }
    if (!packet || !packet->current() || load_sp < 0x60 ||
        Pointer(c.r[3]) != 0x80000004u || Pointer(c.r[4]) != load_sp - 0x50 ||
        (at == kF3Body && c.r[5]) ||
        Word(memory, load_sp - 0x50) != 1u ||
        Word(memory, load_sp - 0x4C) != 0u ||
        Word(memory, load_sp - 0x48) != config.calls->module()->hmodule_ptr() ||
        Word(memory, load_sp - 0x44) != Word(memory, 0x81AA13F4)) return false;
    std::lock_guard lock(mutex);
    if (uncertain || stopping || !edge(NativeAvatarResidentEdge::kLoadCallback, resident->load))
      return false;
    if (at == kF3) {
      if(resident->adapter || resident->callback.entered)return false;
      resident->adapter=frame;return true;
    }
    return resident->adapter && *resident->adapter==frame &&
        resident->callback.Enter(frame, load_sp - 0x50);
  }
  if (at == kF3Lock) {
    std::lock_guard lock(mutex);
    if (uncertain || stopping || resident->lock_entered ||
        !edge(NativeAvatarResidentEdge::kCallbackLock, resident->callback)) return false;
    resident->lock_entered = true;
    return true;  // Its native tail completes within the observed body return.
  }
  return false;
}
void NativeAvatarService::State::LeaveResident(
    cpu::ppc::PPCContext& c, const Frame& frame, uint32_t at) {
  const bool descriptor = Word(*config.kernel->memory(), 0x81AA13B8) == 0xF3u &&
      Word(*config.kernel->memory(), 0x81AA13C0) == kF3;
  const bool loaded = descriptor &&
      Word(*config.kernel->memory(), 0x81AA13BC) == config.calls->module()->hmodule_ptr() &&
      Word(*config.kernel->memory(), 0x81AA13C4) == 1u;
  std::lock_guard lock(mutex);
  const auto& r = frontend_resident;
  if (!r) { (RecordFirstUncertainty(__LINE__,31,&frame,at), uncertain = true); return; }
  Pair* pair = at == kFrontend ? &r->frontend : at == kBuiltin ? &r->builtin :
      at == 0x816BCC38 ? &r->setup : at == 0x816BCD08 ? &r->load :
      at == kF3Body ? &r->callback : nullptr;
  if (!pair || !pair->Finish(frame)) { (RecordFirstUncertainty(__LINE__,32,&frame,at), uncertain = true); return; }
  if (at == 0x816BCC38) { if (!descriptor) (RecordFirstUncertainty(__LINE__,33,&frame,at), uncertain = true); else f3_setup = true; }
  if (at == kF3Body) {
    r->callback_result = uint32_t(c.r[3]);
    if (!r->lock_entered || r->callback_result) (RecordFirstUncertainty(__LINE__,34,&frame,at), uncertain = true);
  }
  if (at == 0x816BCD08) {
    r->load_result = uint32_t(c.r[3]);
    if (int32_t(r->load_result) >= 0) {
      if (!loaded || !r->callback.returned || !r->lock_entered) (RecordFirstUncertainty(__LINE__,35,&frame,at), uncertain = true);
      else f3_loaded = true;
    }
  }
  if (at == kFrontend) r->frontend_result = uint32_t(c.r[3]);
}

bool NativeAvatarService::State::EnterInternal(
    cpu::ppc::PPCContext& c, const Frame& frame, uint32_t at, RequestEntry& entry) {
  auto& memory = *config.kernel->memory();
  if (at == kFrontend || at == kBuiltin || at == kF3Lock)
    return EnterResident(c, frame, at);
  // These six source-pinned callbacks run on actual native SYSTEM Tasks.
  // A genuine TLS Task handle alone does not prove the process capability.
  // Public Init/Start/copy/setup and synchronous sizes keep their real caller.
  if ((at == kDispatch || at == kComplete || at == kFinalize ||
       at == kCoreInit || at == kCoreDispatch || at == 0x819650F8 ||
       at == kAnimationJob || at == kAnimationCore || at == kAnimationWriter) &&
      frame.process != X_PROCTYPE_SYSTEM) return false;
  const auto top = [&]() -> std::shared_ptr<Request> {
    std::lock_guard lock(mutex);
    const auto it = public_calls.find(frame.thread);
    return it != public_calls.end() && !it->second.empty() ? it->second.back() : nullptr;
  };
  const auto by_packet = [&](uint32_t packet) -> std::shared_ptr<Request> {
    std::lock_guard lock(mutex);
    for (const auto& [serial,r] : requests)
      if (!r->done && !r->client_published && r->packet == packet && r->copy.returned && !r->uncertain) return r;
    return {};
  };
  if (at == kStart) {
    const auto app = Pointer(c.r[3]), message = Pointer(c.r[4]);
    if (!app || !message || (*app != 0xF2 && *app != 0xF3)) return true;
    const auto r = top();
    const auto explicit_overlap = Pointer(c.r[5]), input = Pointer(c.r[6]), bytes = Pointer(c.r[7]);
    // Reject unowned F2/F3 submission BEFORE native allocation, overlap/context
    // writes or scheduler publication. Other native app families are untouched.
    if (!r || !explicit_overlap || !input || !bytes ||
        r->message.app != *app || r->message.message != *message ||
        r->message.bytes != *bytes || !*input || frame.sp < 0x70) return false;
    if(r->call==NativeSystemUiPublicCall::kAvatarLoadAnimation){
      std::array<uint8_t,24> outer{};
      if(!r->animation || r->public_frame.sp<0xA0 ||
         frame.thread!=r->public_frame.thread || frame.pcr!=r->public_frame.pcr ||
         frame.sp!=r->public_frame.sp-0xA0 || frame.lr!=0x81674DC0 ||
         frame.sp>UINT32_MAX-0x50 || *input!=frame.sp+0x50 || c.r[8] ||
         *explicit_overlap!=r->args[3] ||
         !internal::ReadNativeWaitSnapshot(memory,*input,outer) ||
         !std::equal(r->animation_id.begin(),r->animation_id.end(),outer.begin()) ||
         xe::load_and_swap<uint32_t>(outer.data()+16)!=r->args[1] ||
         xe::load_and_swap<uint32_t>(outer.data()+20)!=r->animation->control)return false;
    }
    const auto* producer=NativeAvatarRequestStart(r->call);
    const auto parameters=Pointer(c.r[8]);
    if(!producer || !parameters ||
       frame.thread!=r->public_frame.thread || frame.pcr!=r->public_frame.pcr ||
       frame.process!=r->public_frame.process ||
       !MatchesNativeAvatarRequestStartEdge(r->call,r->public_frame.sp,frame.sp,
           frame.lr,*input,*parameters) ||
       *explicit_overlap!=(producer->overlap_argument==0xFF?0u:uint32_t(r->args[producer->overlap_argument])))return false;
    std::shared_ptr<const NativeAvatarBackingLease> parameter_backing;
    if(producer->options_offset){
      // MetadataRandom's ORIGINAL wrapper supplies this real native 8B
      // parameter pair. Other wrappers pass null; no default is invented.
      if(Word(memory,*parameters)!=0x20080002u || Word(memory,*parameters+4)!=0u)return false;
      const std::array ranges{NativeAvatarBackingRange{*parameters,8,false}};
      parameter_backing=memory.BorrowNativeAvatarBacking(ranges);
      if(!parameter_backing)return false;
    }
    const uint32_t overlap = *explicit_overlap ? *explicit_overlap : frame.sp-0x70;
    // Authenticated native prolog is stwu r1,-D0; local overlap is SP+60.
    // Preserve the real native choice rather than constructing another object.
    const std::array<NativeAvatarBackingRange,1> range{{{overlap,sizeof(XAM_OVERLAPPED),true}}};
    auto backing = config.kernel->memory()->BorrowNativeAvatarBacking(range);
    if (!backing) return false;
    {
      std::lock_guard lock(mutex);
      for (const auto& [serial,other] : requests)
        if (other != r && !other->done && !other->client_published && other->overlap == overlap) return false;
      if (!EnterRequestPair(r,RequestPairKind::kStart,frame,overlap,entry)) { r->uncertain = true; (RecordFirstUncertainty(__LINE__,36,&frame,at), uncertain = true); return false; }
      r->overlap = overlap; r->input = *input; r->overlap_backing = std::move(backing);
      r->start_parameters=*parameters;r->parameters_backing=std::move(parameter_backing);
      if(r->call==NativeSystemUiPublicCall::kAvatarInitialize)
        RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kStartEntry,__LINE__,0,0,frame,r.get()));
    }
    return true;
  }
  if (at == kCopy) {
    const auto r = top();
    if (!r || !r->start.entered || r->start_returned) return true;
    const auto input = Pointer(c.r[3]), bytes = Pointer(c.r[4]), output = Pointer(c.r[5]);
    if (frame.lr != 0x816A2D30 || !input || !bytes || !output ||
        *input != r->input || *bytes != r->message.bytes ||
        frame.sp > UINT32_MAX-0x50 || *output != frame.sp+0x50) {
      Poison(__LINE__,5,r,&frame,at); return false;
    }
    std::lock_guard lock(mutex);
    if (!EnterRequestPair(r,RequestPairKind::kCopy,frame,*output,entry)) { r->uncertain = true; (RecordFirstUncertainty(__LINE__,37,&frame,at), uncertain = true); return false; }
    r->copy_output = *output;
    return true;
  }
  if (at == kDispatch) {
    const auto packet = Pointer(c.r[3]);
    if (!packet) return false;
    const auto app = Word(memory,*packet);
    if (!app || (*app != 0xF2 && *app != 0xF3)) return true;
    const auto r = by_packet(*packet);
    if (!r || *packet > UINT32_MAX-0x1C) return false;
    const auto message = Word(memory,*packet+4), overlap = Word(memory,*packet+8),
               payload = Word(memory,*packet+0xC), native_task = Word(memory,*packet+0x18);
    if (!message || *message != r->message.message || !overlap || *overlap != r->overlap ||
        !payload || *payload != r->payload || !native_task) { Poison(__LINE__,6,r,&frame,at); return false; }
    bool already_referenced = false;
    {
      std::lock_guard lock(mutex);
      already_referenced = r->task_reference;
      if (r->dispatch.entered && (!r->dispatch.returned || r->complete_returned)) {
        r->uncertain = true; (RecordFirstUncertainty(__LINE__,38,&frame,at), uncertain = true); return false;
      }
      // A genuine native reschedule may reenter the same packet on the same
      // still-retained Task. It cannot mint another host reference or consume
      // a replacement packet/Task generation.
      r->dispatch = {};
      r->dispatch_returned = false;
      if(!EnterRequestPair(r,RequestPairKind::kDispatch,frame,*packet,entry))return false;
    }
    const auto acquisition = already_referenced ? OwnedAcquire{} :
        AcquireOwnedTask(*frame.thread,frame,at,r,r->task_reference_receipt);
    const auto acquired=acquisition.call;
    if (!already_referenced && acquired.executed && uint32_t(acquired.value)) {
      std::lock_guard lock(mutex);
      r->task_reference_handle = uint32_t(acquired.value);
      r->task_reference = true;
    }
    const auto cached = already_referenced?CurrentTaskCached(*frame.thread):acquisition.current;
    if ((!already_referenced && (!acquisition.proven || !acquired.executed || !uint32_t(acquired.value))) ||
        !cached || !*cached || !NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*cached) ||
        (*native_task && !NativeAvatarTaskReferenceMatchesBody(*native_task,*cached))) { Poison(__LINE__,7,r,&frame,at); return false; }
    {
      std::lock_guard lock(mutex);
      if (r->call == NativeSystemUiPublicCall::kAvatarInitialize)
        r->session->initialize_task_body = *cached;
    }
    return true;
  }
  if (at == kComplete) {
    const auto overlap = Pointer(c.r[3]);
    const auto task = CurrentTaskCached(*frame.thread);
    if (!overlap || !task) return true;
    std::shared_ptr<Request> r;
    {
      std::lock_guard lock(mutex);
      for (const auto& [serial,q] : requests) {
        if (!q->done && !q->client_published && q->overlap == *overlap) {
          if(!NativeAvatarTaskReferenceMatchesBody(q->task_reference_handle,*task) || !q->task_reference){(RecordFirstUncertainty(__LINE__,39,&frame,at), uncertain=true);q->uncertain=true;return false;}
          if (r) { (RecordFirstUncertainty(__LINE__,40,&frame,at), uncertain = true); q->uncertain = true; r->uncertain = true; return false; }
          r = q;
        }
      }
    }
    if (!r) return true;
    const bool exact_caller = frame.lr == 0x81965B40 || frame.lr == 0x81981724 ||
                              frame.lr == 0x81965A64 || frame.lr == 0x81965814;
    const uint32_t hr = uint32_t(c.r[5]);
    if (!exact_caller || uint32_t(c.r[4]) != (int32_t(hr)<0 ? 0x65Bu : 0u) || c.r[6]) {
      Poison(__LINE__,8,r,&frame,at); return false;
    }
    std::lock_guard lock(mutex);
    if (!EnterRequestPair(r,RequestPairKind::kComplete,frame,*overlap,entry)) { r->uncertain=true;(RecordFirstUncertainty(__LINE__,41,&frame,at), uncertain=true);return false; }
    if(r->call==NativeSystemUiPublicCall::kAvatarLoadAnimation){
      const bool ran=r->animation_job.entered;
      const uint32_t expected=r->animation_job.value==0x8000000Au?0x80004005u:r->animation_job.value;
      // No worker receipt is legitimate only at CoreDispatch's ORIGINAL
      // setup-failure Complete. A later finalizer cannot turn a missed/denied
      // descriptor callback into that pre-worker failure lane.
      if((!ran && (frame.lr!=0x81965A64 || int32_t(hr)>=0 ||
                   !r->core_dispatch.entered || r->core_dispatch.returned)) ||
         (ran && (frame.lr!=0x81965814 || !r->animation_job.returned ||
                  !r->animation_core.returned || hr!=expected)) ||
         (int32_t(hr)>=0 && (!ran || !r->animation_writer.returned || r->animation_writer.value!=1))){
        r->uncertain=true;(RecordFirstUncertainty(__LINE__,67,&frame,at), uncertain=true);return false;
      }
    }
    r->result = hr;
    if(r->call==NativeSystemUiPublicCall::kAvatarInitialize)
      RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kCompleteEntry,__LINE__,hr,1,frame,r.get(),nullptr,*task));
    return true;
  }
  if (at == kFinalize) {
    const auto object = Pointer(c.r[3]);
    const auto task = CurrentTaskCached(*frame.thread);
    if (!object || !task || *object > UINT32_MAX-0x44) return false;
    const auto packet = Word(memory,*object+0x3C), result = Word(memory,*object+0x40),
               request_task = Word(memory,*object+0x18);
    const auto r = packet ? by_packet(*packet) : nullptr;
    if (!r || !result || !request_task || !NativeAvatarTaskReferenceMatchesBody(*request_task,*task) ||
        !NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) ||
        r->message.app != 0xF2 || r->call == NativeSystemUiPublicCall::kAvatarInitialize)
      return false;
    std::lock_guard lock(mutex);
    if (!EnterRequestPair(r,RequestPairKind::kFinalize,frame,*object,entry)) { r->uncertain=true;(RecordFirstUncertainty(__LINE__,42,&frame,at), uncertain=true);return false; }
    r->result = *result;
    return true;
  }
  if (at == kCoreInit) {
    const auto payload = Pointer(c.r[3]);
    const auto task = CurrentTaskCached(*frame.thread);
    if (!payload || !task) return false;
    std::shared_ptr<Request> r;
    {
      std::lock_guard lock(mutex);
      for (const auto& [serial,q] : requests)
        if (!q->done && q->call == NativeSystemUiPublicCall::kAvatarInitialize &&
            q->payload == *payload && NativeAvatarTaskReferenceMatchesBody(q->task_reference_handle,*task)) r=q;
    }
    if (!r || !r->session || *payload > UINT32_MAX-40) return false;
    std::array<uint8_t,40> copied{};
    if (!internal::ReadNativeWaitSnapshot(memory,*payload,copied)) return false;
    const auto get = [&](size_t n) { return xe::load_and_swap<uint32_t>(copied.data()+n); };
    if (get(0) != r->args[0] || get(4) != r->args[1] || get(8) != r->args[2] ||
        get(32) != r->session->arena_base || get(36) != 0x100000u) return false;
    std::array<uint32_t,5> callbacks{};
    std::vector<CodeOwner> owners;
    for (size_t n=0;n<callbacks.size();++n) {
      callbacks[n]=get(12+n*4);
      if (!callbacks[n]) continue;
      auto owner=RetainCode(callbacks[n],true);
      if (!owner) return false;
      if (std::none_of(owners.begin(),owners.end(),[&](const auto& held) {
            return held.module.get()==owner->module.get(); })) owners.push_back(std::move(*owner));
    }
    std::lock_guard lock(mutex);
    if (r->session != session || session->callbacks_copied ||
        !EnterRequestPair(r,RequestPairKind::kCoreInit,frame,*payload,entry)) return false;
    session->callbacks=callbacks;
    session->callback_code=std::move(owners);
    session->callbacks_copied=true;
    RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kCoreEntry,__LINE__,0,0,frame,r.get(),nullptr,*task));
    return true;
  }
  // Common SysApp mutation is gated by the same exact logical owner. A name,
  // matching arena address, numeric F2/F3 ID or source-global Ready bit alone
  // never permits descriptor replacement/load/unload.
  if (at == 0x816BD7A0) {
    const auto r=top();
    if (uint32_t(c.r[3])!=0xF2) return true;
    if (!r || r->call!=NativeSystemUiPublicCall::kAvatarInitialize || !r->session ||
        c.r[4]!=uint64_t(r->session->arena_base)+0x100000u ||
        c.r[5]!=r->session->arena_bytes-0x100000u || c.r[6]!=1) return false;
    std::lock_guard lock(mutex);
    if (r->prepare_entered) return false;
    r->prepare_entered=true;
    RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kPrepareEntry,__LINE__,0,0,frame,r.get()));
    return true;
  }
  if(at==0x816BCC38 || at==0x816BCD08 || at==0x816BCF60){
    const auto descriptor=Pointer(c.r[3]);
    if(!descriptor || (*descriptor!=0x81AA1470 && *descriptor!=0x81AA13B4))return true;
    const auto task=CurrentTaskCached(*frame.thread);
    if(*descriptor==0x81AA13B4 && at!=0x816BCF60)
      return EnterResident(c,frame,at);
    std::lock_guard lock(mutex);
    if(*descriptor==0x81AA13B4)
      return f3_unload && f3_unload->worker.entered &&
          !f3_unload->worker.returned && f3_unload->task_reference && task &&
          NativeAvatarTaskReferenceMatchesBody(f3_unload->task_reference_handle,*task) && f3_unload->worker.frame.thread==frame.thread;
    if(!session)return false;
    for(const auto& [serial,r]:requests){
      if(r->session!=session)continue;
      if(at==0x816BCC38 && r->call==NativeSystemUiPublicCall::kAvatarInitialize &&
          r->public_frame.thread==frame.thread && r->public_frame.pcr==frame.pcr &&
          r->public_frame.process==frame.process && r->prepare_entered &&
          !r->prepare_returned && !r->public_returned)return true;
      const auto& control=r->controls[at==0x816BCF60?1:0];
      if(control && control->worker.entered && !control->worker.returned &&
          control->task_reference && task && NativeAvatarTaskReferenceMatchesBody(control->task_reference_handle,*task) &&
          control->worker.frame.thread==frame.thread &&
          control->worker.frame.pcr==frame.pcr)return true;
    }
    return false;
  }
  if(at==kTocRelease){
    const auto task=CurrentTaskCached(*frame.thread);
    std::lock_guard lock(mutex);
    if(!session || !session->asset_cleanup_frame)return true;
    const auto& cleanup=*session->asset_cleanup_frame;
    const size_t n=frame.lr==0x8196B62C?0:frame.lr==0x8196B650?1:2;
    if(n==2)return true;
    const uint32_t adjustment=n==0?12:8;
    if(!task || *task!=session->asset_cleanup_task_body ||
       frame.thread!=cleanup.thread || frame.pcr!=cleanup.pcr ||
       frame.process!=cleanup.process || cleanup.sp<0x70 || frame.sp!=cleanup.sp-0x70 ||
       !session->cleanup_objects[n] || session->cleanup_objects[n]>UINT32_MAX-adjustment ||
       c.r[3]!=uint64_t(session->cleanup_objects[n])+adjustment ||
       session->cleanup_release_frames[n])return false;
    session->cleanup_release_frames[n]=frame;
    return true;
  }
  if(at==0x8196B5F0){
    const auto primary=Word(memory,0x81B4B1FC),legacy=Word(memory,0x81B4B200);
    const auto task=CurrentTaskCached(*frame.thread);
    if(!task || !*task || !primary || !legacy || frame.process!=X_PROCTYPE_SYSTEM)return false;
    std::lock_guard lock(mutex);
    if(!session || session->asset_cleanup_frame)return false;
    bool allowed=session->stop_frame && !session->stop_returned &&
        session->stop_task_body==*task && session->stop_frame->thread==frame.thread &&
        session->stop_frame->pcr==frame.pcr;
    for(const auto& [serial,r]:requests)
      allowed|=r->session==session && NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) &&
               r->core_init.entered && !r->core_init.returned;
    if(!allowed)return false;
    session->asset_cleanup_frame=frame;session->asset_cleanup_complete=false;
    session->asset_cleanup_task_body=*task;session->cleanup_objects={*primary,*legacy};
    session->cleanup_release_frames={};session->cleanup_release_returned={};
    return true;
  }
  if(at==kLoad || at==kUnload){
    const auto app=Pointer(c.r[3]);
    if(!app || (*app!=0xF2 && *app!=0xF3))return true;
    if(c.r[4]!=1)return false;
    const bool loading=at==kLoad;
    auto control=std::make_shared<Control>();control->app=*app;control->loading=loading;
    if(!control->entry.Enter(frame,*app))return false;
    const auto r=top();
    const auto current_task=CurrentTaskCached(*frame.thread);
    std::lock_guard lock(mutex);
    if(*app==0xF3){
      if(loading || frame.process!=X_PROCTYPE_SYSTEM || !stopping ||
         !f3_shutdown_entered || f3_unload ||
         (frame.thread->thread_id()!=config.allocation->bootstrap_thread_id() &&
          (!frontend_resident || !frontend_resident->aborting ||
           frontend_resident->caller.thread!=frame.thread ||
           frontend_resident->caller.pcr!=frame.pcr ||
           frontend_resident->caller.sp<cpu::kGuestCallExecuteStackPadding ||
           frame.sp!=frontend_resident->caller.sp-cpu::kGuestCallExecuteStackPadding ||
           frame.lr!=0xBCBCBCBC ||
           (!current_task || !NativeAvatarTaskReferenceMatchesBody(frontend_resident->task_reference_handle,*current_task)))))return false;
      f3_unload=std::move(control);return true;
    }
    if(!r || !r->session || r->session!=session || r->controls[loading?0:1])return false;
    if(loading){
      if(r->call!=NativeSystemUiPublicCall::kAvatarInitialize ||
         !r->prepare_entered || r->prepare_returned)return false;
    }else if(r->call==NativeSystemUiPublicCall::kAvatarShutdown){
      if(!r->session->shutdown_entered || r->public_frame!=frame)return false;
    }else if(r->call!=NativeSystemUiPublicCall::kAvatarInitialize ||
             !((r->start_returned && int32_t(r->start_result)<0) ||
               (r->core_init_returned && int32_t(r->core_init_result)<0)))return false;
    r->controls[loading?0:1]=std::move(control);return true;
  }
  if(at==kSchedule){
    const auto callback=Pointer(c.r[3]),app=Pointer(c.r[4]);
    if(callback && *callback==kDispatch){
      const auto r=top();const auto options=Pointer(c.r[5]),output=Pointer(c.r[6]);
      if(!r || !app || !options || !output)return false;
      std::lock_guard lock(mutex);
      const bool init=r->call==NativeSystemUiPublicCall::kAvatarInitialize;
      const bool engine=r->message.app==0xF2;
      if(!NativeAvatarRequestStart(r->call) ||
          (engine && (!r->session || r->session!=session || (!init && !r->session->ready))) ||
          (!engine && (r->message.app!=0xF3 || r->session || !f3_ready)) ||
          r->uncertain || r->public_returned || (init && !r->prepare_returned) ||
          !r->start.entered || r->start_returned || !r->copy.returned ||
          r->caller.get()!=frame.thread || r->start.frame.thread!=frame.thread ||
          r->start.frame.pcr!=frame.pcr || r->start.frame.process!=frame.process ||
          r->public_frame.thread!=frame.thread || r->public_frame.pcr!=frame.pcr ||
          r->public_frame.process!=frame.process ||
          !MatchesNativeAvatarRequestStartEdge(r->call,r->public_frame.sp,r->start.frame.sp,
              r->start.frame.lr,r->input,r->start_parameters) ||
          !MatchesNativeAvatarRequestScheduleEdge(r->start.frame.sp,frame.sp,frame.lr,r->packet,
              *callback,*app,*options,r->start_parameters,*output))return false;
      return r->request_schedule.Enter(frame,r->packet);
    }
    if(!callback || !app || (*callback!=kLoadTask && *callback!=kUnloadTask) ||
       (*app!=0xF2 && *app!=0xF3))return true;
    const bool loading=*callback==kLoadTask;
    std::shared_ptr<Control> control;
    {std::lock_guard lock(mutex);
      for(const auto& item:Controls())if(item->app==*app && item->loading==loading &&
          item->entry.entered && !item->entry.returned &&
          item->entry.frame.thread==frame.thread){if(control)return false;control=item;}
    }
    if(!control || frame.lr!=(loading?0x816BD664u:0x816BD6E4u) ||
       control->entry.frame.sp<0x80 || frame.sp!=control->entry.frame.sp-0x80 ||
       frame.sp>UINT32_MAX-0x60 || c.r[5]!=uint64_t(frame.sp)+0x58 ||
       c.r[6]!=uint64_t(frame.sp)+0x50 ||
       Word(memory,frame.sp+0x58)!=0x04000000u || Word(memory,frame.sp+0x5C)!=0u)return false;
    const std::array<NativeAvatarBackingRange,2> ranges{{
        {frame.sp+0x50,4,true},{frame.sp+0x58,8,false}}};
    auto stack=config.kernel->memory()->BorrowNativeAvatarBacking(ranges);
    if(!stack)return false;
    std::lock_guard lock(mutex);
    if(!control->schedule.Enter(frame,*callback))return false;
    control->output=frame.sp+0x50;control->stack=std::move(stack);return true;
  }
  if(at==kLoadTask || at==kUnloadTask){
    const auto app=Pointer(c.r[3]);
    if(!app || (*app!=0xF2 && *app!=0xF3))return true;
    const auto cached=CurrentTaskCached(*frame.thread);
    if(!cached || !*cached || frame.process!=X_PROCTYPE_SYSTEM)return false;
    std::shared_ptr<Control> control;
    {std::lock_guard lock(mutex);
      for(const auto& item:Controls())if(item->app==*app && item->loading==(at==kLoadTask) &&
          item->entry.entered && !item->entry.returned && item->schedule.entered &&
          !item->worker.entered){if(control)return false;control=item;}
      if(!control || (control->expected_task_handle && !NativeAvatarTaskReferenceMatchesBody(control->expected_task_handle,*cached)) ||
         !control->worker.Enter(frame,*app))return false;
    }
    const auto acquisition=AcquireOwnedTask(*frame.thread,frame,at,control,control->task_reference_receipt,
        control->loading?NativeProfileIdentityOwner::TaskReferenceAcquireKind::kWork:
                         NativeProfileIdentityOwner::TaskReferenceAcquireKind::kRetainedUnload);
    const auto acquired=acquisition.call;
    if(acquired.executed && uint32_t(acquired.value)){
      std::lock_guard lock(mutex);control->task_reference_handle=uint32_t(acquired.value);control->task_reference=true;
      if(control->expected_task_handle && control->expected_task_handle!=control->task_reference_handle){
        control->uncertain=true;(RecordFirstUncertainty(__LINE__,43,&frame,at), uncertain=true);return false;
      }
    }
    if(!acquisition.proven || !acquired.executed || !acquisition.current ||
       !NativeAvatarTaskReferenceMatchesBody(uint32_t(acquired.value),*cached) ||
       !NativeAvatarTaskReferenceMatchesBody(uint32_t(acquired.value),*acquisition.current)){
      Poison(__LINE__,9,{},&frame,at);return false;}
    return true;
  }
  if(at==kAnimationJob || at==kAnimationCore || at==kAnimationWriter){
    const auto task=CurrentTaskCached(*frame.thread);
    const auto first=Pointer(c.r[3]),second=Pointer(c.r[4]);
    if(!task || !*task || !first || (at!=kAnimationJob && !second) || !animation_enabled)return false;
    std::shared_ptr<Request> r;
    uint32_t job=0,packet=0,descriptor=0,job_task=0;
    if(at==kAnimationJob){
      job=*first;
      if(job>UINT32_MAX-0x44)return false;
      const auto table=Word(memory,job),envelope=Word(memory,job+0x3C),
                 desc=Word(memory,job+0x1C),target=Word(memory,job+0x18);
      if(table!=0x81632DCCu || !envelope || !desc || !target ||
         *desc!=0x81632D90u || Word(memory,0x81632DCC+4)!=0x81965580u ||
         Word(memory,0x81632D90)!=kAnimationJob ||
         Word(memory,0x81632D94)!=0x81A97454u)return false;
      packet=*envelope;descriptor=*desc;job_task=*target;r=by_packet(packet);
    }else{
      std::lock_guard lock(mutex);
      for(const auto& [serial,candidate]:requests){
        const auto& parent=at==kAnimationCore?candidate->animation_job:candidate->animation_core;
        if(candidate->call==NativeSystemUiPublicCall::kAvatarLoadAnimation &&
           parent.entered && !parent.returned && parent.frame.thread==frame.thread &&
           parent.frame.pcr==frame.pcr){if(r)return false;r=candidate;}
      }
    }
    std::shared_ptr<const NativeAvatarBackingLease> client_backing;
    if(!r || !r->animation)return false;
    {std::lock_guard lock(mutex);client_backing=r->backing;}
    if(!client_backing || !client_backing->current())return false;
    // Caller values are an API lifetime obligation, not frozen by page pins.
    // Recheck only at actual ingress; never substitute or reset output bytes.
    if(at==kAnimationWriter){
      const auto capacity=Word(memory,r->animation->control+NativeAvatarAnimationBuffers::kCapacityOffset);
      const auto data=Word(memory,r->animation->control+NativeAvatarAnimationBuffers::kDataOffset);
      if(!capacity || !data || !r->animation->Matches(*first,*capacity,*data)){
        Poison(__LINE__,69,r,&frame,at);return false;
      }
    }
    std::lock_guard lock(mutex);
    const auto held=r->session;
    if(uncertain || r->uncertain || r->done || r->client_published || !r->task_reference ||
       r->call!=NativeSystemUiPublicCall::kAvatarLoadAnimation || !held || held!=session ||
       !held->ready || held->worker_exited || !held->worker_reference || !held->worker_frame ||
       !NativeAvatarTaskReferenceMatchesBody(held->worker_reference_handle,*task) ||
       held->worker_frame->thread!=frame.thread || held->worker_frame->pcr!=frame.pcr ||
       !r->core_dispatch.entered)return false;
    if(at==kAnimationJob){
      if(!NativeAvatarTaskReferenceMatchesBody(job_task,r->task_reference_handle&~uint32_t(3)) ||
         !MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kWorkerJob,
             held->worker_frame->sp,frame.sp,frame.lr) ||
         descriptor!=0x81632D90 || packet!=r->packet ||
         !EnterRequestPair(r,RequestPairKind::kAnimationJob,frame,job,entry))return false;
      r->animation_job_address=job;return true;
    }
    if(at==kAnimationCore){
      if(*first!=r->payload || *second!=r->animation->control ||
         !MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kJobCore,
             r->animation_job.frame.sp,frame.sp,frame.lr))return false;
      return EnterRequestPair(r,RequestPairKind::kAnimationCore,frame,*first,entry);
    }
    if(*first!=r->animation->control || frame.sp>UINT32_MAX-0x190 || *second!=frame.sp+0x190 ||
       !MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge::kCoreWriter,
           r->animation_core.frame.sp,frame.sp,frame.lr))return false;
    if(!EnterRequestPair(r,RequestPairKind::kAnimationWriter,frame,*first,entry))return false;
    RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kAnimationWriterEntry,__LINE__,0,0,frame,r.get(),nullptr,*task));
    return true;
  }
  if (at==kCoreDispatch) {
    const auto packet=Pointer(c.r[3]);
    const auto task=CurrentTaskCached(*frame.thread);
    const auto r=packet ? by_packet(*packet) : nullptr;
    if (!r || !task || !NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) || r->message.app!=0xF2) return false;
    std::lock_guard lock(mutex);
    return EnterRequestPair(r,RequestPairKind::kCoreDispatch,frame,*packet,entry);
  }
  if (at==0x819650F8) {
    // This is the actual native worker scheduled by the owned CoreInit. An
    // added GetCurrentTask reference is retained until its own normal return.
    std::shared_ptr<Session> held;
    { std::lock_guard lock(mutex);
      held=session;
      if (!held || !held->callbacks_copied || held->worker_frame || uncertain)
        return false;
      held->worker_frame=frame;
      held->worker_thread=retain_object(frame.thread);
    }
    const auto acquisition=AcquireOwnedTask(*frame.thread,frame,at,held,held->worker_reference_receipt);
    const auto acquired=acquisition.call;
    const auto current=acquisition.current;
    if (acquired.executed && uint32_t(acquired.value)) {
      std::lock_guard lock(mutex);
      held->worker_reference_handle=uint32_t(acquired.value); held->worker_reference=true;
      // Init may have returned (or raced this acquisition) before the worker
      // entered. Join that already captured native worker under this mutex;
      // the reverse order remains checked at Init's public normal return.
      if (held->native_worker_handle && held->native_worker_handle != held->worker_reference_handle) {
        (RecordFirstUncertainty(__LINE__,44,&frame,at), uncertain = true); return false;  // Keep the actual added Task ref.
      }
    }
    if (!acquisition.proven || !acquired.executed || !uint32_t(acquired.value) || !current ||
        !NativeAvatarTaskReferenceMatchesBody(uint32_t(acquired.value),*current)) { Poison(__LINE__,10,{},&frame,at); return false; }
    return true;
  }
  if (at==kCoreStop) {
    const auto task=CurrentTaskCached(*frame.thread);
    if (!task || !*task || frame.process!=X_PROCTYPE_SYSTEM) return false;
    std::lock_guard lock(mutex);
    if (!session || (session->stop_frame && !session->stop_returned)) return false;
    bool owned=false;
    for(const auto& control:Controls())
      owned|=!control->loading && control->app==0xF2 && control->worker.entered &&
          !control->worker.returned && control->task_reference && NativeAvatarTaskReferenceMatchesBody(control->task_reference_handle,*task);
    for (const auto& [serial,r]:requests)
      owned|=r->session==session && r->core_init.entered &&
             !r->core_init.returned && NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task);
    if (!owned) return false;
    session->stop_frame=frame;session->stop_returned=false;session->stop_task_body=*task;
    return true;
  }
  if (at==kF2 || at==kF3 || at==kF3Body) {
    const auto message=Pointer(c.r[3]), payload=Pointer(c.r[4]);
    if(!message || !payload)return false;
    if (at==kF2 && *message==0x600005) {
      const auto r=top();
      if(*payload>UINT32_MAX-12)return false;
      const auto cpu=Word(memory,*payload+4),gpu=Word(memory,*payload+8);
      // Native mask is a WORD at packet+0. Reading it separately avoids
      // treating the adjacent reserved halfword as caller authority.
      std::array<uint8_t,2> raw{};
      return r && r->call==NativeSystemUiPublicCall::kAvatarGetAssetsResultSize &&
          cpu && gpu &&
          internal::ReadNativeWaitSnapshot(memory,*payload,raw) &&
          xe::load_and_swap<uint16_t>(raw.data())==uint16_t(r->args[0]) &&
          *cpu==r->args[1] && *gpu==r->args[2];
    }
    const auto task=CurrentTaskCached(*frame.thread);
    if(!task || !*task || frame.process!=X_PROCTYPE_SYSTEM)return false;
    const uint32_t app=at==kF2?0xF2:0xF3;
    if (*message==0x80000007) {
      const auto r=by_packet(*payload);
      return r && r->message.app==app && NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) &&
             r->dispatch.entered && !r->dispatch.returned;
    }
    if (at==kF3Body && (*message&0xFFFF0000)==0x600000) {
      // The F3 adapter tails into this body, whose 80000007 branch recursively
      // invokes the same body with the actual copied payload + real overlap.
      std::lock_guard lock(mutex);
      for (const auto& [serial,r]:requests)
        if (!r->done && !r->uncertain && r->message.app==app &&
            r->message.message==*message && r->payload==*payload &&
            NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) && uint32_t(c.r[5])==r->overlap &&
            r->dispatch.entered && !r->dispatch.returned) return true;
      return false;
    }
    if(app==0xF3 && *message==0x80000004)
      return EnterResident(c,frame,at);
    if(*message==0x8000000B || *message==0x8000000C){
      // These two source-pinned F2/F3 lifecycle notifications are native no-op
      // branches. Permit their original return before a task's user callback
      // has entered; this does not adopt a Task or admit any request/output.
      std::lock_guard lock(mutex);return app==0xF2?bool(session):f3_setup;
    }
    if (*message==0x80000004 || *message==0x80000003 ||
        *message==0x80000005 || *message==0x80000006 ||
        *message==0x8000000B || *message==0x8000000C ||
        *message==0x8000000F) {
      std::lock_guard lock(mutex);
      for(const auto& control:Controls())
        if(control->app==app && control->worker.entered && !control->worker.returned &&
           control->task_reference && NativeAvatarTaskReferenceMatchesBody(control->task_reference_handle,*task) &&
           control->worker.frame.thread==frame.thread)return true;
      if (app==0xF3) {

      } else if (session) {
        if (session->worker_reference && NativeAvatarTaskReferenceMatchesBody(session->worker_reference_handle,*task) &&
            (*message==0x8000000B || *message==0x8000000C)) return true;
      }
      for (const auto& [serial,r]:requests)
        if (!r->done && !r->uncertain && r->message.app==app && NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task))
          return true;
      // Descriptor load has its own real Task before the public IO request.
      const auto f=public_calls.find(frame.thread);
      if (app==0xF2 && f!=public_calls.end() && !f->second.empty()) {
        const auto& r=f->second.back();
        return r->call==NativeSystemUiPublicCall::kAvatarInitialize &&
               r->prepare_entered && !r->prepare_returned;
      }
      return false;
    }
    return false;
  }
  return true;
}

void NativeAvatarService::State::LeaveInternal(
    cpu::ppc::PPCContext& c, const Frame& frame, uint32_t at, const RequestEntry& entry) {
  bool resident_return = at == kFrontend || at == kBuiltin;
  { std::lock_guard lock(mutex);
    const auto& r = frontend_resident;
    resident_return |= r &&
        ((at == 0x816BCC38 && r->setup.entered && !r->setup.returned && r->setup.frame==frame) ||
         (at == 0x816BCD08 && r->load.entered && !r->load.returned && r->load.frame==frame) ||
         (at == kF3Body && r->callback.entered && !r->callback.returned && r->callback.frame==frame));
  }
  if(resident_return) LeaveResident(c,frame,at);
  if(at==kSchedule){
    bool request_schedule=false,finished=false;std::shared_ptr<Request> request;
    {
      std::lock_guard lock(mutex);
      const auto it=public_calls.find(frame.thread);
      if(it!=public_calls.end() && !it->second.empty()){
        const auto& r=it->second.back();
        if(r->request_schedule.entered && !r->request_schedule.returned &&
            r->request_schedule.frame==frame){
          request=r;request_schedule=true;finished=r->request_schedule.Finish(frame);
        }
      }
    }
    if(request_schedule){
      if(!finished)Poison(__LINE__,66,request,&frame,at);
      return; // Schedule return is not callback, Close or request completion.
    }
  }
  if(at==kLoad || at==kUnload || at==kSchedule || at==kLoadTask || at==kUnloadTask){
    std::shared_ptr<Control> control;
    {std::lock_guard lock(mutex);
      for(const auto& item:Controls()){
        Pair* pair=at==kSchedule?&item->schedule:
            (at==kLoadTask || at==kUnloadTask)?&item->worker:&item->entry;
        if(pair->entered && !pair->returned && pair->frame==frame){
          if(control){(RecordFirstUncertainty(__LINE__,45,&frame,at), uncertain=true);return;}control=item;
        }
      }
    }
    if(!control){Poison(__LINE__,11,{},&frame,at);return;}
    if(at==kSchedule){
      const auto output=Word(*config.kernel->memory(),control->output);
      std::lock_guard lock(mutex);
      control->schedule_result=uint32_t(c.r[3]);
      if(control->app==0xF2)
        RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kControlScheduleReturn,__LINE__,uint32_t(c.r[3]),1,frame,nullptr,control.get()));
      uint32_t evaluated=0,predicate_true=0;
      const auto observed=[&](uint32_t bit,bool value) noexcept {
        return ObserveUncertaintyPredicate(evaluated,predicate_true,bit,value);
      };
      if(observed(0,!control->schedule.Finish(frame)) ||
         (observed(1,int32_t(control->schedule_result)>=0) &&
          (observed(2,!output) || observed(3,!*output) ||
           (observed(4,bool(control->task_reference_handle)) &&
            observed(5,control->task_reference_handle!=*output)))) ||
         (observed(6,int32_t(control->schedule_result)<0) && observed(7,control->worker.entered))){
        control->uncertain=true;(RecordFirstUncertainty(__LINE__,46,&frame,at,nullptr,evaluated,predicate_true), uncertain=true);return;
      }
      if(int32_t(control->schedule_result)>=0)control->expected_task_handle=*output;
    }else if(at==kLoadTask || at==kUnloadTask){
      uint32_t task=0;
      {std::lock_guard lock(mutex);
        if(control->app==0xF2)
          RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kControlWorkerReturn,__LINE__,uint32_t(c.r[3]),1,frame,nullptr,control.get()));
        uint32_t evaluated=0,predicate_true=0;
        const auto observed=[&](uint32_t bit,bool value) noexcept {
          return ObserveUncertaintyPredicate(evaluated,predicate_true,bit,value);
        };
        if(observed(0,!control->task_reference) || observed(1,control->close_attempted) ||
           observed(2,control->uncertain)){(RecordFirstUncertainty(__LINE__,47,&frame,at,nullptr,evaluated,predicate_true), uncertain=true);return;}
        control->close_attempted=true;task=control->task_reference_handle;
      }
      const auto current=CurrentTaskCached(*frame.thread);
      const bool closed=current && NativeAvatarTaskReferenceMatchesBody(task,*current) &&
          CloseOwnedTask(*frame.thread,task,control->task_reference_receipt,frame,at,frame,at,control);
      if(!closed){Poison(__LINE__,12,{},&frame,at);return;}
      {std::lock_guard lock(mutex);control->task_reference=false;
        if(!control->worker.Finish(frame)){control->uncertain=true;(RecordFirstUncertainty(__LINE__,48,&frame,at), uncertain=true);return;}
        control->worker.value=uint32_t(c.r[3]);}
    }else{
      std::shared_ptr<const NativeAvatarBackingLease> released;
      {std::lock_guard lock(mutex);
        if(control->app==0xF2)
          RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kControlReturn,__LINE__,uint32_t(c.r[3]),1,frame,nullptr,control.get()));
        const bool no_task=control->schedule.returned && int32_t(control->schedule_result)<0;
        uint32_t evaluated=0,predicate_true=0;
        const auto observed=[&](uint32_t bit,bool value) noexcept {
          return ObserveUncertaintyPredicate(evaluated,predicate_true,bit,value);
        };
        if(observed(0,!control->entry.Finish(frame)) || observed(1,control->uncertain) ||
           observed(2,!control->schedule.returned) || (observed(3,!no_task) &&
             (observed(4,!control->worker.returned) || observed(5,control->task_reference) ||
              observed(6,control->expected_task_handle!=control->task_reference_handle)))){control->uncertain=true;(RecordFirstUncertainty(__LINE__,49,&frame,at,nullptr,evaluated,predicate_true), uncertain=true);return;}
        released=std::move(control->stack);
      }
    }
  }
  // The exact successful entry owns its Request association across native
  // callbacks and physical-frame reuse. Never rediscover it by scanning all
  // outstanding Requests, or complete an older missing return by inference.
  std::shared_ptr<Request> request;
  {
    std::lock_guard lock(mutex);
    if(entry.request) {
      request=entry.request;
      const auto kind=RequestKind(at);
      if(!entry.Matches(kind)) {
        request->uncertain=true;
        (RecordFirstUncertainty(__LINE__,50,&frame,at), uncertain=true);return;
      }
      auto* own=RequestPair(*request,kind);
      if(!own || !own->Finish(frame,entry.invocation)) {
        request->uncertain=true;
        (RecordFirstUncertainty(__LINE__,51,&frame,at), uncertain=true);return;
      }
    }
  }
  const auto pair=[&](Request& r)->Pair* {return RequestPair(r,RequestKind(at));};
  if(at==kCopy && request) {
    // The original copier returned the actual allocation. Capture before
    // native Start queues it; never derive/adopt an address by a name or scan.
    const auto packet=Word(*config.kernel->memory(),request->copy_output);
    const bool good=int32_t(uint32_t(c.r[3]))>=0 && packet && *packet &&
                    *packet<=UINT32_MAX-0x1C-request->message.bytes;
    if(good){std::lock_guard lock(mutex);request->packet=*packet;request->payload=*packet+0x1C;}
    else if(int32_t(uint32_t(c.r[3]))>=0)Poison(__LINE__,13,request,&frame,at);
  }
  if(request) {
    bool close=false;
    std::shared_ptr<const NativeAvatarBackingLease> released_parameters;
    {
      std::lock_guard lock(mutex);
      if(at==kAnimationJob || at==kAnimationCore || at==kAnimationWriter){
        auto* own=pair(*request);own->value=uint32_t(c.r[3]);
        const bool mismatch=at==kAnimationWriter?own->value>1:
            at==kAnimationCore?
              ((request->animation_writer.entered && !request->animation_writer.returned) ||
               (int32_t(own->value)>=0 && (!request->animation_writer.returned || request->animation_writer.value!=1))):
              (!request->animation_core.returned || own->value!=request->animation_core.value);
        if(mismatch){request->uncertain=true;(RecordFirstUncertainty(__LINE__,68,&frame,at), uncertain=true);}
        const auto phase=at==kAnimationWriter?DiagnosticPhase::kAnimationWriterReturn:
            at==kAnimationCore?DiagnosticPhase::kAnimationCoreReturn:DiagnosticPhase::kAnimationJobReturn;
        RecordDiagnostic(DiagnosticLocked(phase,__LINE__,own->value,at==kAnimationWriter?0:1,frame,request.get()));
      }else if(at==kStart){
        request->start_returned=true;request->start_result=uint32_t(c.r[3]);
        released_parameters=std::move(request->parameters_backing);
        if(request->call==NativeSystemUiPublicCall::kAvatarInitialize)
          RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kStartReturn,__LINE__,request->start_result,1,frame,request.get()));
        // Original Start frees a copied packet on a failure before dispatch.
        // No native work or added Task reference exists in this branch.
        if(int32_t(request->start_result)<0 && !request->dispatch.entered)
          request->done=true;
      }else if(at==kComplete){
        request->complete_returned=true;
        if(!request->client_published){request->uncertain=true;(RecordFirstUncertainty(__LINE__,52,&frame,at), uncertain=true);}
      }else if(at==kFinalize){
        // Actual Finalize returned AFTER Complete, native object cleanup and
        // free. No guest object/envelope is read here: Complete already freed
        // the envelope, and Finalize already freed its private request.
        if(!request->complete_returned){request->uncertain=true;(RecordFirstUncertainty(__LINE__,53,&frame,at), uncertain=true);}
        else request->cleanup_done=true;
      }else if(at==kCoreInit){
        request->core_init_returned=true;request->core_init_result=uint32_t(c.r[3]);
        RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kCoreReturn,__LINE__,request->core_init_result,1,frame,request.get()));
      }else if(at==kCoreDispatch){
        request->core_dispatch_returned=true;
        if(int32_t(uint32_t(c.r[3]))<0 && request->complete_returned)
          request->cleanup_done=true;
      }else if(at==kDispatch){
        request->dispatch_returned=true;
        if(request->complete_returned &&
           (request->message.app==0xF3 ||
            request->call==NativeSystemUiPublicCall::kAvatarInitialize))
          request->cleanup_done=true;
      }
      close=request->complete_returned && request->cleanup_done &&
            !request->uncertain && !request->done;
    }
    released_parameters.reset(); // No temporary stack-page owner crosses native Close.
    if(close)CloseRequestTask(*frame.thread,request,frame,at);
    ReapCompleted();
  }
  if(at==0x819650F8){
    std::shared_ptr<Session> held;uint32_t task=0;
    {std::lock_guard lock(mutex);held=session;
      if(!held || !held->worker_frame || *held->worker_frame!=frame ||
         !held->worker_reference || held->worker_close_attempted){(RecordFirstUncertainty(__LINE__,54,&frame,at), uncertain=true);return;}
      held->worker_close_attempted=true;task=held->worker_reference_handle;
    }
    const auto current=CurrentTaskCached(*frame.thread);
    const bool closed=current && NativeAvatarTaskReferenceMatchesBody(task,*current) &&
        CloseOwnedTask(*frame.thread,task,held->worker_reference_receipt,frame,at,frame,at,held);
    if(!closed){Poison(__LINE__,14,{},&frame,at);return;}
    {std::lock_guard lock(mutex);held->worker_reference=false;held->worker_exited=true;changed.notify_all();}
  }
  if(at==kCoreStop){
    std::lock_guard lock(mutex);
    if(!session || !session->stop_frame || *session->stop_frame!=frame){(RecordFirstUncertainty(__LINE__,55,&frame,at), uncertain=true);return;}
    session->stop_returned=true;
  }
  if(at==0x816BD7A0){
    std::lock_guard lock(mutex);
    const auto it=public_calls.find(frame.thread);
    if(it!=public_calls.end() && !it->second.empty() &&
       it->second.back()->call==NativeSystemUiPublicCall::kAvatarInitialize){
      it->second.back()->prepare_returned=true;
      RecordDiagnostic(DiagnosticLocked(DiagnosticPhase::kPrepareReturn,__LINE__,uint32_t(c.r[3]),1,frame,it->second.back().get()));
    }
  }
  if(at==kTocRelease){
    std::lock_guard lock(mutex);
    if(!session || !session->asset_cleanup_frame)return;
    bool found=false;
    for(size_t n=0;n<2;++n)if(session->cleanup_release_frames[n] &&
        *session->cleanup_release_frames[n]==frame && !session->cleanup_release_returned[n]){
      session->cleanup_release_returned[n]=true;found=true;
    }
    if(!found)(RecordFirstUncertainty(__LINE__,56,&frame,at), uncertain=true);
    // Returned count remains native. No host base/refcount/destructor ABI is
    // inferred from the two source-pinned this-adjustor leaf instructions.
  }
  if(at==0x8196B5F0){
    std::shared_ptr<Session> held;
    {std::lock_guard lock(mutex);held=session;
      if(!held || !held->asset_cleanup_frame || *held->asset_cleanup_frame!=frame){(RecordFirstUncertainty(__LINE__,57,&frame,at), uncertain=true);return;}}
    std::array<uint8_t,1> flag{};
    const bool cleaned=int32_t(uint32_t(c.r[3]))>=0 &&
        Word(*config.kernel->memory(),0x81B4B1FC)==0u &&
        Word(*config.kernel->memory(),0x81B4B200)==0u &&
        internal::ReadNativeWaitSnapshot(*config.kernel->memory(),0x81B4B204,flag) && !flag[0];
    {std::lock_guard lock(mutex);
      held->asset_cleanup_frame.reset();
      held->asset_cleanup_complete=cleaned && (!held->asset_mount || held->asset_closed);
      for(size_t n=0;n<2;++n)
        if(held->cleanup_objects[n] && !held->cleanup_release_returned[n])
          held->asset_cleanup_complete=false;
      // Native B978 ignores cleanup's result and later returns zero. Preserve
      // a failed common Close/open flag; Shutdown's return cannot erase it.
      if(!held->asset_cleanup_complete)(RecordFirstUncertainty(__LINE__,58,&frame,at), uncertain=true);
    }
  }
  if(at==kUnload){
    // Shutdown is an authenticated 12-byte tail wrapper into XamUnloadSysApp.
    // That export waits/closes its real scheduled native unload Task, then
    // returns on the SAME original SP/LR; the wrapper has no epilogue. The
    // deeper commonUnload returns on another Task and is not this proof.
    bool shutdown=false;
    {std::lock_guard lock(mutex);const auto it=public_calls.find(frame.thread);
      shutdown=it!=public_calls.end() && !it->second.empty() &&
          it->second.back()->call==NativeSystemUiPublicCall::kAvatarShutdown &&
          it->second.back()->public_frame==frame;
    }
    if(shutdown)LeavePublic(c,frame,*NativeSystemUiPublicProfile(
        NativeSystemUiPublicCall::kAvatarShutdown));
  }
}

NativeAvatarService::NativeAvatarService(std::shared_ptr<State> s)
    : state_(std::move(s)) {}
NativeAvatarService::~NativeAvatarService() = default;

void NativeAvatarService::ReportDiagnosticsOnHost(const char* boundary) const noexcept {
  if(XThread::GetCurrentFiberThread() ||
     global_critical_region::is_held_by_current_thread())return;
  const auto& s=*state_;
  if(boundary && !std::strcmp(boundary,"StopFailure")){
    const auto& cell=s.diagnostic_phases[size_t(State::DiagnosticPhase::kBootstrapShutdownAdmission)];
    const auto captured=cell.state.load(std::memory_order_acquire);
    const auto taint=s.first_uncertainty.state.load(std::memory_order_acquire);
    State::UncertaintyPayload origin;
    if(taint==2)origin=s.first_uncertainty.payload;
    static NativeSystemUiTraceLimiter shutdown;
    if(captured==2){
      const auto& value=cell.payload;
      shutdown.Emit("avatar","bootstrap_shutdown_admission",
          {{"phase",value.phase},{"line",value.line},{"evaluated",value.evaluated},
           {"failed",value.failed},{"admitted",value.flags},{"native_completion",0},
           {"uncertainty_capture_state",taint},{"uncertainty_line",origin.line},
           {"uncertainty_site",origin.site}});
    }else shutdown.Emit("avatar","bootstrap_shutdown_admission_unavailable",
          {{"capture_state",captured},{"native_completion",0}});
    return;  // No existing caller/frame/resource payload is republished here.
  }
  const uint32_t boundary_code=boundary && !std::strcmp(boundary,"Bootstrap")?1u:
      boundary && !std::strcmp(boundary,"Coordinator")?2u:
      boundary && !std::strcmp(boundary,"ForegroundReady")?3u:0u;
  uint64_t completed=0;
  for(size_t n=1;n<s.diagnostic_phases.size();++n)
    if(s.diagnostic_phases[n].state.load(std::memory_order_acquire)==2)completed|=uint64_t(1)<<n;
  const auto first=s.first_negative.state.load(std::memory_order_acquire);
  static NativeSystemUiTraceLimiter summary;
  summary.EmitTransition(completed | (uint64_t(first)<<32),"avatar","causal_host_snapshot",
      {{"boundary",boundary_code},{"completed_phases",completed},{"first_negative_state",first},
       {"retired",s.stopped_retired.load(std::memory_order_acquire)}});
  const auto emit=[&](NativeSystemUiTraceLimiter& trace,const char* event,
                      const State::DiagnosticPayload& p) noexcept {
    trace.EmitTransition((uint64_t(p.phase)<<32)|p.status,"avatar",event,
        {{"boundary",boundary_code},{"phase",p.phase},{"line",p.line},{"address",p.address},{"status",p.status},
         {"status_kind",p.status_kind},{"available",p.available},{"thread_wrapper",p.thread_wrapper},
         {"pcr",p.pcr},{"sp",p.sp},{"lr",p.lr},{"process",p.process},
         {"actual_task_body",p.actual_task_body},{"request",p.request},{"session",p.session},
         {"request_task_reference",p.request_task_reference},{"control_task_reference",p.control_task_reference},
         {"worker_task_reference",p.worker_task_reference},{"initialize_task_body",p.initialize_task_body},
         {"source_generation",p.source_generation},{"observed_source_generation",p.observed_source_generation},
         {"animation_control",p.animation_control},{"animation_capacity",p.animation_capacity},
         {"animation_data",p.animation_data},{"animation_job",p.animation_job},
         {"packet",p.packet},{"payload",p.payload},{"overlap",p.overlap},
         {"callback_mask",p.callback_mask},{"control_app",p.control_app},{"flags",p.flags},
         {"evaluated",p.evaluated},{"failed",p.failed},{"core_evaluated",p.core_evaluated},
         {"core_passed_union",p.core_passed},{"core_candidates",p.core_candidates},
         {"last_core_request",p.last_core_request},{"last_core_task_reference",p.last_core_task_reference},
         {"mount_available",p.mount.available},{"mount_stage",p.mount.stage},
         {"mount_failure_stage",p.mount.failure_stage},{"mount_line",p.mount.line},
         {"mount_failure_result",p.mount.failure_result},{"mount_result",p.mount.result},
         {"mount_evaluated",p.mount.evaluated},{"mount_predicate_true",p.mount.predicate_true},
         {"mount_readonly",p.mount.readonly},{"mount_readers",p.mount.readers},
         {"mount_source_generation",p.mount.source_generation},{"mount_source",p.mount.source},
         {"mount_volume",p.mount.volume},{"mount_parent",p.mount.parent},
         {"mount_certificate",p.mount.certificate},{"mount_native_status",p.mount.native_status},
         {"mount_transferred",p.mount.transferred},{"storage_available",p.mount.storage.available},
         {"storage_operation",p.mount.storage.operation},{"storage_reason",p.mount.storage.reason},
         {"storage_line",p.mount.storage.line},{"storage_error_category",p.mount.storage.error_category},
         {"storage_error_value",uint64_t(int64_t(p.mount.storage.error_value))},
         {"storage_root",p.mount.storage.root},{"storage_space",p.mount.storage.space},
         {"storage_filesystem",p.mount.storage.filesystem},{"storage_file",p.mount.storage.file},
         {"storage_guard_owner",p.mount.storage.guard_owner},{"storage_guard_request",p.mount.storage.guard_request}});
  };
  static std::array<NativeSystemUiTraceLimiter,size_t(State::DiagnosticPhase::kCount)> phases;
  for(size_t n=1;n<s.diagnostic_phases.size();++n)
    if(s.diagnostic_phases[n].state.load(std::memory_order_acquire)==2)
      emit(phases[n],"causal_phase",s.diagnostic_phases[n].payload);
  static NativeSystemUiTraceLimiter negative;
  if(first==2)emit(negative,"causal_first_negative",s.first_negative.payload);
  // Existing verified host boundary only. State1 is explicit unavailable,
  // never a partially read payload. Independent writers never wait here.
  const auto taint=s.first_uncertainty.state.load(std::memory_order_acquire);
  State::UncertaintyPayload origin;
  if(taint==2)origin=s.first_uncertainty.payload;
  static NativeSystemUiTraceLimiter uncertainty;
  uncertainty.EmitTransition(taint,"avatar","first_uncertainty",
      {{"boundary",boundary_code},{"capture_state",taint},{"payload_available",taint==2},
       {"line",origin.line},{"site",origin.site},{"available",origin.available},{"function",origin.function},
       {"evaluated",origin.evaluated},{"predicate_true",origin.predicate_true},
       {"thread_wrapper",origin.thread_wrapper},{"pcr",origin.pcr},{"sp",origin.sp},{"lr",origin.lr},{"process",origin.process},
       {"expected_thread_wrapper",origin.expected_thread_wrapper},{"expected_pcr",origin.expected_pcr},
       {"expected_sp",origin.expected_sp},{"expected_lr",origin.expected_lr},{"expected_process",origin.expected_process}});
}

std::shared_ptr<NativeAvatarService> NativeAvatarService::Lookup(KernelState* kernel) {
  return NativeSystemUiService::CaptureAvatarService(kernel);
}

struct NativeAvatarService::CleanupTaskCreationProof::Data {
  std::atomic<bool> retired{false};
  // Strong exact producer holders, never a new native reference. The identity
  // owner retains this through actual allocator return, then drops holders
  // outside its mutex. Unknown/unreturned native work remains retained.
  std::shared_ptr<State> owner;
  std::shared_ptr<State::Control> control;
  std::shared_ptr<State::Request> request;
  object_ref<XThread> thread;
  const cpu::ppc::PPCContext* context=nullptr;
  cpu::ThreadState* thread_state=nullptr;
  cpu::Processor* processor=nullptr;
  KernelState* kernel=nullptr;
  uint32_t sp=0,lr=0,pcr=0,parameters=0,output=0;
  bool unload=false;
};
NativeAvatarService::CleanupTaskCreationProof::CleanupTaskCreationProof(
    std::shared_ptr<Data> data):data_(std::move(data)) {}
bool NativeAvatarService::CleanupTaskCreationProof::Matches(
    const cpu::ppc::PPCContext& c) const noexcept {
  // PURE scalars only: no owner/kernel lookup, mutex, TLS, IO, allocation,
  // resource release or currentness callback under identity's metadata lock.
  const auto* d=data_.get();
  return d && !d->retired.load(std::memory_order_acquire) && &c==d->context && c.kernel_state==d->kernel &&
      c.thread_state==d->thread_state && c.processor==d->processor &&
      Pointer(c.r[1])==d->sp && Pointer(c.lr)==d->lr &&
      Pointer(c.r[13])==d->pcr && Pointer(c.r[3])==d->parameters &&
      Pointer(c.r[4])==d->output;
}
bool NativeAvatarService::CleanupTaskCreationProof::IsUnload() const noexcept {
  return data_ && !data_->retired.load(std::memory_order_acquire) && data_->unload;
}
void NativeAvatarService::CleanupTaskCreationProof::RetireAfterActualKernelStop() const noexcept {
  if(!data_ || data_->retired.exchange(true,std::memory_order_acq_rel))return;
  // Caller proved actual CPU/IO/GPU stop and host-frame joins; no PPC/free or
  // invented native completion. Historical scalars stay, holders are revoked
  // outside all owner/global locks even if this immutable token escaped.
  data_->thread.reset();data_->request.reset();data_->control.reset();data_->owner.reset();
}
std::shared_ptr<const NativeAvatarService::CleanupTaskCreationProof>
NativeAvatarService::CaptureOwnedTaskAllocation(
    KernelState* kernel,const cpu::ppc::PPCContext& c) noexcept try {
  if(!kernel || c.kernel_state!=kernel ||
     global_critical_region::is_held_by_current_thread())return {};
  const auto service=Lookup(kernel);
  if(!service)return {};
  auto& s=*service->state_;
  const auto frame=ActualFrame(const_cast<cpu::ppc::PPCContext&>(c),kernel);
  if(!frame || !s.RootCurrent(kSchedule) || !s.RootCurrent(kUnload) ||
     !s.RootCurrent(kUnloadTask) || !s.RootCurrent(kLoad) || !s.RootCurrent(kLoadTask) ||
     frame->lr!=0x816BA46C)return {};
  // Native TITLE is observed as TITLE; it gains no SYSTEM/profile/read source.
  const bool title=frame->process==X_PROCTYPE_TITLE;
  if(title && !s.TitleCurrent(true))return {};
  // Peek only an already-admitted exact Request Schedule, then release State
  // before root/reader/source checks. Controls do not acquire new provider
  // prerequisites. The final lock rechecks this same Request and native frame.
  std::shared_ptr<State::Request> request_candidate;
  {std::lock_guard lock(s.mutex);
    const auto it=s.public_calls.find(frame->thread);
    if(it!=s.public_calls.end() && !it->second.empty()){
      const auto& r=it->second.back();
      if(r->request_schedule.entered && !r->request_schedule.returned &&
         r->request_schedule.frame.thread==frame->thread &&
         r->request_schedule.frame.pcr==frame->pcr &&
         r->request_schedule.frame.sp>=0x90 && frame->sp==r->request_schedule.frame.sp-0x90)
        request_candidate=r;
    }
  }
  const bool request_code=request_candidate && s.RootCurrent(kStart) &&
      s.RootCurrent(kDispatch) && s.ReaderCurrent();
  std::optional<xam::ContentSourceState> request_source;
  if(request_code && request_candidate->message.app==0xF2)
    request_source=s.config.content->CaptureAvatarSource(*s.config.calls);
  std::shared_ptr<State::Control> control;
  std::shared_ptr<State::Request> request;
  std::shared_ptr<const NativeAvatarBackingLease> stack;
  Frame schedule;
  uint32_t parameters=0;
  bool unload=false;
  {
    std::lock_guard lock(s.mutex);
    if(s.uncertain || s.stopped_retired)return {};
    for(const auto& candidate:s.Controls()){
      if((s.stopping && candidate->loading) ||
         !candidate->entry.entered || candidate->entry.returned ||
         !candidate->schedule.entered || candidate->schedule.returned ||
         candidate->uncertain || candidate->schedule.frame.thread!=frame->thread ||
         candidate->schedule.frame.pcr!=frame->pcr ||
         candidate->schedule.frame.process!=frame->process ||
         candidate->schedule.frame.sp<0x90 ||
         frame->sp!=candidate->schedule.frame.sp-0x90 ||
         candidate->schedule.frame.sp>UINT32_MAX-0x60 ||
         Pointer(c.r[3])!=candidate->schedule.frame.sp+0x58 ||
         Pointer(c.r[4])!=frame->sp+0x50)continue;
      if(control)return {};control=candidate;
    }
    if(!control){
      // This exact Request's guarded Schedule precedes the genuine allocator;
      // neither a numeric Task nor a matching current TLS pointer can create it.
      if(s.stopping || !request_code)return {};
      const auto it=s.public_calls.find(frame->thread);
      if(it==s.public_calls.end() || it->second.empty())return {};
      const auto& r=it->second.back();
      const auto input=Pointer(c.r[3]),output=Pointer(c.r[4]);
      const bool init=r->call==NativeSystemUiPublicCall::kAvatarInitialize;
      const bool engine=r->message.app==0xF2;
      if(!input || !output || r!=request_candidate || !NativeAvatarRequestStart(r->call) ||
          (engine && (!r->session || r->session!=s.session || !request_source ||
            (!init && !r->session->ready) || !SameSource(r->session->source,*request_source))) ||
          (!engine && (r->message.app!=0xF3 || r->session || !s.f3_ready)) ||
          !r->domain || r->uncertain || r->public_returned || (init && !r->prepare_returned) ||
          !r->start.entered || r->start_returned || !r->copy.returned || !r->packet ||
          !r->request_schedule.entered || r->request_schedule.returned ||
          r->request_schedule.value!=r->packet ||
          r->request_schedule.frame.thread!=frame->thread ||
          r->request_schedule.frame.pcr!=frame->pcr ||
          r->request_schedule.frame.process!=frame->process ||
          !MatchesNativeAvatarRequestStartEdge(r->call,r->public_frame.sp,r->start.frame.sp,
              r->start.frame.lr,r->input,r->start_parameters) ||
          !MatchesNativeAvatarRequestAllocationEdge(r->request_schedule.frame.sp,frame->sp,
              frame->lr,*input,r->start_parameters,*output) || r->caller_code.empty())return {};
      if(title && (r->public_frame.thread!=frame->thread || r->public_frame.pcr!=frame->pcr ||
          r->public_frame.process!=X_PROCTYPE_TITLE || r->caller.get()!=frame->thread ||
          std::none_of(r->caller_code.begin(),r->caller_code.end(),[&](const auto& code){
            return code.module && code.lease && code.module->hmodule_ptr()==s.config.title.hmodule &&
                code.module->load_generation()==s.config.title.load_generation;})))return {};
      request=r;stack=r->start_parameters?r->parameters_backing:r->overlap_backing;
      schedule=r->request_schedule.frame;parameters=r->start_parameters;
    }else{
    if(control->app==0xF2){
      for(const auto& [serial,r]:s.requests)
        if(r->controls[control->loading?0:1]==control && !r->uncertain &&
           r->session && r->session==s.session){
          if(request)return {};request=r;
        }
      if(!request || request->caller_code.empty())return {};
      if(title && (request->public_frame.thread!=frame->thread ||
          request->public_frame.pcr!=frame->pcr ||
          request->public_frame.process!=X_PROCTYPE_TITLE ||
          request->caller.get()!=frame->thread ||
          std::none_of(request->caller_code.begin(),request->caller_code.end(),
              [&](const auto& code){return code.module && code.lease &&
                code.module->hmodule_ptr()==s.config.title.hmodule &&
                code.module->load_generation()==s.config.title.load_generation;})))return {};
    }else if(control->app!=0xF3 || control->loading || title ||
             s.f3_unload!=control)return {};
    stack=control->stack;schedule=control->schedule.frame;unload=!control->loading;
    parameters=schedule.sp+0x58;
    }
  }
  if(!stack || !stack->current())return {};
  // The original Control or typed Request guard proved callback/native options and
  // entry scope. It did not prove a child Task birth. Identity must still use
  // the actual allocator output/generation ledger and original worker calls.
  auto d=std::make_shared<CleanupTaskCreationProof::Data>();
  d->owner=service->state_;d->control=control;d->request=request;
  d->thread=retain_object(frame->thread);d->context=&c;d->thread_state=c.thread_state;
  d->processor=c.processor;d->kernel=kernel;d->sp=frame->sp;d->lr=frame->lr;d->pcr=frame->pcr;
  d->parameters=parameters;d->output=frame->sp+0x50;
  d->unload=unload;
  return std::shared_ptr<const CleanupTaskCreationProof>(new CleanupTaskCreationProof(std::move(d)));
} catch(...){return {};}

std::shared_ptr<NativeAvatarService> NativeAvatarService::Create(
    Config config, std::string* error) {
  const auto fail=[&](const char* why)->std::shared_ptr<NativeAvatarService>{
    if(error)*error=why;return {};
  };
  if(!config.kernel || !config.calls || !config.domain || !config.allocation ||
     !config.content || !config.profile_reader || !config.title.valid() ||
     CheckNativeAvatarPlanFamily(config.calls->validated(),error)!=NativeAvatarPlanFamily::kComplete)
    return fail("Complete actual native avatar family/owners unavailable");
  auto state=std::make_shared<State>(std::move(config));
  state->animation_enabled=state->config.calls->validated().catalog.contains(
      "native-public-avatar-load-animation");
  state->binding=state->config.calls->CaptureAvatarProviderBinding();
  state->root_code=state->config.kernel->AcquireUserModuleExecutionLease(
      state->config.calls->module());
  state->resident_domain=state->config.domain->Acquire();
  if(!state->binding || !state->root_code || !state->resident_domain ||
     !state->ReaderCurrent())return fail("Actual shared reader/root binding lifetime unavailable");
  // Pins are checked before any F2/F3 startup mutates globals. Provider full
  // entry bodies were checked by the quiescent binding transaction itself.
  for(const auto& code:kCode)
    if(!state->RootCurrent(code.at))return fail("Source-pinned avatar native closure changed");
  if(state->animation_enabled && !state->AnimationCurrent())
    return fail("Source-pinned animation producer/consumer closure changed");
  for(const auto& range:std::array<std::pair<uint32_t,uint32_t>,10>{{
        {0x81AA13B4,0xBC},{0x81AA1470,0xBC},{0x81B4AF50,0x18},
        {0x81A975CC,20},{0x81B4B1FC,12},{0x81A97478,12},{0x81B4B2D0,0x38},
        {0x815F9270,32},{0x815F915C,sizeof("avatarsyscore.xex")},
        {0x815F9170,sizeof("avatarcore.xex")}}})
    if(!state->RootData(range.first,range.second))return fail("Native avatar data has no actual root ownership");
  constexpr uint32_t table[]={0x815F9170,0xF2,kF2,0,0x815F915C,0xF3,kF3,0};
  for(size_t n=0;n<std::size(table);++n)
    if(Word(*state->config.kernel->memory(),0x815F9270+uint32_t(n)*4)!=table[n])
      return fail("Original avatar builtin identity rows changed");
  if(std::memcmp(state->config.kernel->memory()->TranslateVirtual(0x815F915C),
                 "avatarsyscore.xex",sizeof("avatarsyscore.xex")) ||
     std::memcmp(state->config.kernel->memory()->TranslateVirtual(0x815F9170),
                 "avatarcore.xex",sizeof("avatarcore.xex")))
    return fail("Original avatar builtin module names changed");
  const auto descriptor=[&](uint32_t at){
    for(uint32_t offset:{8u,12u,16u,0x84u,0x88u}) {
      const auto value=Word(*state->config.kernel->memory(),at+offset);
      if(!value || *value)return false;
    }
    return true;
  };
  if(!descriptor(0x81AA1470) || !descriptor(0x81AA13B4) ||
     Word(*state->config.kernel->memory(),0x81B4AF50)!=0u ||
     Word(*state->config.kernel->memory(),0x81B4AF54)!=0u ||
     Word(*state->config.kernel->memory(),0x81B4AF58)!=0u)
    return fail("Avatar descriptors/engine already initialized outside this owner");
  if(error)error->clear();
  return std::shared_ptr<NativeAvatarService>(new NativeAvatarService(std::move(state)));
}

bool NativeAvatarService::OwnsOriginalCall(
    GuestRuntimeCallExecutor* calls, XThread& thread,
    NativeSystemUiPublicCall call, std::span<const uint64_t> args) {
  if(!XThread::IsInThread(&thread))return false;
  const auto service=Lookup(thread.kernel_state());
  if(!service)return false;
  auto& state=*service->state_;
  std::lock_guard lock(state.mutex);
  if(state.stopped_retired.load(std::memory_order_acquire) ||
      state.config.calls.get()!=calls)return false;
  const auto it=state.invocations.find(&thread);
  return it!=state.invocations.end() && !it->second.empty() &&
      it->second.back()->call==call &&
      std::equal(args.begin(),args.end(),it->second.back()->args.begin(),
                 it->second.back()->args.end()) && !state.uncertain &&
      (!state.stopping || it->second.back()->bootstrap_close);
}

NativeSystemUiPublicCallResult NativeAvatarService::TryInvokeOnOriginalCaller(
    KernelState* kernel, NativeSystemUiPublicCall call,
    std::span<const uint64_t> arguments) noexcept {
  using Result=NativeSystemUiPublicCallResult;
  using Disposition=Result::Disposition;
  const Result denied{Disposition::kDenied,kNotReady};
  AvatarPublicObservation observation{call};
  try {
    const auto* profile=NativeSystemUiPublicProfile(call);
    if(!profile || !PublicAt(profile->address))return {};
    const auto service=Lookup(kernel);
    if(!service) {
      if (!NativeSystemUiService::AvatarFamilyRequested(kernel)) return {};
      return observation.Finish(AvatarPublicReason::kNoOwner, denied);
    }
    auto& state=*service->state_;
    if(call==NativeSystemUiPublicCall::kAvatarLoadAnimation && !state.animation_enabled)
      return observation.Finish(AvatarPublicReason::kOwnerState, denied);
    auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
    if(!thread || !NativeSystemUiPublicArgumentsValid(call,arguments) ||
       !state.RootCurrent(profile->address) || !state.ReaderCurrent())
      return observation.Finish(AvatarPublicReason::kCallerArgumentsOrProvider, denied);
    const auto frame=ActualFrame(*thread->thread_state()->context(),kernel);
    if(!frame || !state.TitleCurrent())
      return observation.Finish(AvatarPublicReason::kFrameOrTitle, denied);
    auto invocation=std::make_shared<State::Invocation>();
    invocation->call=call;invocation->args.assign(arguments.begin(),arguments.end());
    invocation->original=*frame;
    if(frame->lr<4)return observation.Finish(AvatarPublicReason::kCallerLr, denied);
    const auto code=state.RetainCode(frame->lr-4,frame->process==X_PROCTYPE_TITLE);
    if(!code)return observation.Finish(AvatarPublicReason::kCallerCode, denied);
    invocation->caller_code.push_back(*code);
    if (call == NativeSystemUiPublicCall::kAvatarGetAssets) {
      {
        std::lock_guard lock(state.mutex);
        if (state.stopping || state.uncertain || !state.f3_ready || !state.session ||
            !state.session->ready || state.session->shutdown_entered)
          return observation.Finish(AvatarPublicReason::kOwnerState, denied);
        invocation->sizing_session = state.session;
      }
      // Execute the genuine synchronous 5E0 producer BEFORE 5E1 delegation.
      // ExecuteGuestCall restores this same actual caller's registers/SP/LR;
      // no Owner lock crosses either guest invocation.
      invocation->asset_sizes = state.NativeSizes(*thread,uint16_t(arguments[1]));
      const auto restored = ActualFrame(*thread->thread_state()->context(),kernel);
      if (!invocation->asset_sizes || !restored || *restored != *frame ||
          !state.RootCurrent(profile->address) || !state.ReaderCurrent() ||
          !state.TitleCurrent())
        return observation.Finish(AvatarPublicReason::kAssetSizesUnavailable, denied);
    }
    {
      std::lock_guard lock(state.mutex);
      if(state.stopping || state.uncertain || !state.f3_ready ||
         state.invocations.size()>=64 ||
         (call==NativeSystemUiPublicCall::kAvatarGetAssets &&
          (state.session!=invocation->sizing_session || !state.session ||
           !state.session->ready || state.session->shutdown_entered)))
        return observation.Finish(AvatarPublicReason::kOwnerState, denied);
      state.invocations[thread].push_back(invocation);
    }
    // The immutable per-thread scope is published BEFORE ExecuteGuestCall
    // installs its own return frame. No owner/Kernel/heap lock crosses PPC.
    const auto result=state.config.calls->ExecuteAvatarRegistersOnOriginalCaller(
        *thread,call,arguments);
    {
      std::lock_guard lock(state.mutex);
      const auto it=state.invocations.find(thread);
      if(!result.succeeded || it==state.invocations.end() || it->second.empty() ||
         it->second.back()!=invocation){(state.RecordFirstUncertainty(__LINE__,59,&*frame,profile->address), state.uncertain=true);
        return observation.Finish(AvatarPublicReason::kExecutionFailed, {Disposition::kExecutionFailed,kFailed});}
      it->second.pop_back();
      if(it->second.empty())state.invocations.erase(it);
    }
    return observation.Finish(AvatarPublicReason::kReturned, {Disposition::kReturned,uint32_t(result.return_value)});
  }catch(...){if(const auto s=Lookup(kernel))s->state_->Poison(__LINE__,15,{},nullptr,0);
    return observation.Finish(AvatarPublicReason::kException, {Disposition::kExecutionFailed,kFailed});}
}

namespace {
bool AvatarOnlyIngress(cpu::ppc::PPCContext& c,uint32_t at) {
  if(PublicAt(at) || at==kF2 || at==kF3 || at==kF3Body || at==kF3Lock ||
     at==kCoreInit || at==kCoreStop || at==kCoreDispatch ||
     at==kFinalize || at==0x819650F8 || at==0x8196B5F0 ||
     at==kAnimationJob || at==kAnimationCore || at==kAnimationWriter)return true;
  if(at==kStart || at==0x816BD7A0 || at==kUnload || at==kLoad || at==kLoadTask || at==kUnloadTask)
    return uint32_t(c.r[3])==0xF2 || uint32_t(c.r[3])==0xF3;
  if(at==0x816BCC38 || at==0x816BCD08 || at==0x816BCF60)
    return uint32_t(c.r[3])==0x81AA1470 || uint32_t(c.r[3])==0x81AA13B4;
  if(at==kSchedule)return (uint32_t(c.r[3])==kLoadTask || uint32_t(c.r[3])==kUnloadTask) &&
      (uint32_t(c.r[4])==0xF2 || uint32_t(c.r[4])==0xF3);
  if(at==kDispatch){
    const auto p=Pointer(c.r[3]);
    const auto app=p?Word(*c.kernel_state->memory(),*p):std::nullopt;
    return app && (*app==0xF2 || *app==0xF3);
  }
  return false;
}
}
uint64_t NativeAvatarService::GuardEntry(void* raw,uint64_t address) noexcept {
  State::RequestEntry request_entry;
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state || !HooksFunction(uint32_t(address)))return 1;
  try{
    const auto service=Lookup(c->kernel_state);
    if(!service){
      if(!NativeSystemUiService::AvatarFamilyRequested(c->kernel_state) ||
         !AvatarOnlyIngress(*c,uint32_t(address)))return 1;
      TraceAvatarIngressDenied(*c,uint32_t(address),kNotReady);
      c->r[3]=address==kAnimationWriter?0:uint64_t(int64_t(int32_t(kNotReady)));return 0;
    }
    auto& state=*service->state_;
    // Observe only the exact call already installed by ShutdownOnBootstrap.
    // This holder is retained before its short metadata lookup and released
    // after all original GuardEntry locks, including exceptional unwinding.
    std::shared_ptr<State::Invocation> shutdown_invocation;
    if(address==0x81674940 && XThread::IsInThread()){
      auto* thread=XThread::GetCurrentThread();
      std::lock_guard lock(state.mutex);
      const auto it=state.invocations.find(thread);
      if(it!=state.invocations.end() && !it->second.empty()){
        const auto& candidate=it->second.back();
        if(candidate->bootstrap_close && !candidate->shutdown_admission_observed &&
           candidate->original.thread==thread &&
           candidate->call==NativeSystemUiPublicCall::kAvatarShutdown && candidate->args.empty()){
          candidate->shutdown_admission_observed=true;
          shutdown_invocation=candidate;
        }
      }
    }
    State::ShutdownAdmissionCut shutdown_cut;
    struct ShutdownAdmissionPublication {
      State& state;
      State::ShutdownAdmissionCut& cut;
      bool active;
      ~ShutdownAdmissionPublication() noexcept {
        if(!active)return;
        State::DiagnosticPayload value;
        value.phase=uint32_t(State::DiagnosticPhase::kBootstrapShutdownAdmission);
        value.line=cut.line;
        value.address=0x81674940;
        // This is an admission cut, not an observed native HRESULT/return.
        value.status=0;value.status_kind=0;
        value.evaluated=cut.evaluated;
        value.failed=cut.failed?cut.failed:(cut.admitted?0:uint32_t(State::ShutdownAdmissionCut::kOtherGuardExit));
        value.flags=cut.admitted;
        // Preallocated scalar publication only. No log, guest read, resource
        // release, lock, allocation or callback even under inherited GCRC.
        state.RecordDiagnostic(value);
      }
    } shutdown_publication{state,shutdown_cut,bool(shutdown_invocation)};
    const auto refused=[&](State::ShutdownAdmissionCut::Stage stage,bool value,uint32_t line) noexcept {
      return shutdown_invocation?shutdown_cut.Refused(stage,value,line):value;
    };
    bool resident_scope=false;
    if(address==kFrontend || address==kBuiltin){
      std::lock_guard lock(state.mutex);
      resident_scope=bool(state.frontend_resident);
      if(!resident_scope)return 1;
    }
    const auto frame=ActualFrame(*c,c->kernel_state);
    if(refused(State::ShutdownAdmissionCut::kFrame,!frame,__LINE__) ||
       refused(State::ShutdownAdmissionCut::kGuardRoot,!state.RootCurrent(uint32_t(address)),__LINE__)){
      if(!resident_scope && !AvatarOnlyIngress(*c,uint32_t(address)))return 1;
      TraceAvatarIngressDenied(*c,uint32_t(address),kNotReady);
      c->r[3]=address==kAnimationWriter?0:uint64_t(int64_t(int32_t(kNotReady)));return 0;
    }
    const auto* profile=PublicAt(uint32_t(address));
    // Classification is made once at the authenticated entry. Return-time
    // r3 is a result, not the original packet/app; never reclassify from it.
    const auto passthrough=[&]() -> uint64_t {
      std::lock_guard lock(state.mutex);
      state.TrackUnownedNestedEntryLocked(*frame,uint32_t(address));return 1;
    };
    if(address==kTocRelease){
      std::lock_guard lock(state.mutex);
      if(!state.session || !state.session->asset_cleanup_frame ||
         (frame->lr!=0x8196B62C && frame->lr!=0x8196B650)){
        state.TrackUnownedNestedEntryLocked(*frame,uint32_t(address));return 1;
      }
    }
    if(address==kStart && uint32_t(c->r[3])!=0xF2 && uint32_t(c->r[3])!=0xF3)return passthrough();
    if(address==kDispatch && !AvatarOnlyIngress(*c,uint32_t(address)))return passthrough();
    const bool owned_request_schedule=address==kSchedule && state.IsOwnedRequestSchedule(*frame,*c);
    if((address==0x816BCC38 || address==0x816BCD08 || address==0x816BCF60 ||
        address==0x816BD7A0 || address==kUnload || address==kLoad ||
        address==kLoadTask || address==kUnloadTask || address==kSchedule) &&
       !AvatarOnlyIngress(*c,uint32_t(address)) && !owned_request_schedule)return passthrough();
    if(address==kCopy || address==kComplete){
      bool owned=false;
      {std::lock_guard lock(state.mutex);
        if(address==kCopy){const auto it=state.public_calls.find(frame->thread);
          owned=it!=state.public_calls.end() && !it->second.empty() &&
              it->second.back()->start.entered && !it->second.back()->start_returned;
        }else for(const auto& [serial,r]:state.requests)
          owned|=!r->done && !r->client_published && r->overlap==uint32_t(c->r[3]);
      }
      if(!owned)return passthrough();
    }
    const bool admitted=profile?state.EnterPublic(*c,*frame,*profile,
        shutdown_invocation?&shutdown_cut:nullptr):state.EnterInternal(*c,*frame,uint32_t(address),request_entry);
    shutdown_cut.admitted=admitted;
    if(!admitted){
      if(address==kAnimationJob || address==kAnimationCore || address==kAnimationWriter)
        state.Poison(__LINE__,70,{},&*frame,uint32_t(address));
      // Both emitters still run the own-return hook after a denied entry.
      // Its marker must not consume an older, genuinely owned call.
      passthrough();
      auto diagnostic=State::PartialDiagnostic(State::DiagnosticPhase::kGuardDenial,__LINE__,kNotReady,3,*frame);
      diagnostic.address=uint32_t(address);state.RecordDiagnostic(diagnostic);
      TraceAvatarIngressDenied(*c,uint32_t(address),kNotReady,
        state.CurrentTaskCached(*frame->thread).value_or(0));
      c->r[3]=address==kAnimationWriter?0:uint64_t(int64_t(int32_t(kNotReady)));return 0;}
    // The two authenticated leaf tail wrappers have no own return epilogue.
    // Shutdown is paired at the original Unload export; F3 is guarded, not a work
    // completion. Every other observed own return consumes one exact frame.
    if(address!=0x81674940 && address!=kF3 && address!=kF3Lock){
      std::lock_guard lock(state.mutex);
      state.entries[{frame->thread,uint32_t(address)}].EnterOwned(*frame,request_entry);
    }
    return 1;
  }catch(...){if(const auto s=Lookup(c->kernel_state))s->state_->Poison(__LINE__,16,{},nullptr,0);
    c->r[3]=address==kAnimationWriter?0:uint64_t(int64_t(int32_t(kFailed)));
    TraceAvatarIngressDenied(*c,uint32_t(address),kFailed);return 0;}
}
void NativeAvatarService::BeforeCompletionPublication(void* raw) noexcept{
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state)return;
  try{
    const auto service=Lookup(c->kernel_state);
    if(!service)return;
    auto& s=*service->state_;
    const auto frame=ActualFrame(*c,c->kernel_state);
    if(!frame || frame->process!=X_PROCTYPE_SYSTEM || !s.RootCurrent(kComplete))return;
    const auto task=s.CurrentTaskCached(*frame->thread);
    std::shared_ptr<const NativeAvatarBackingLease> client,overlap;
    {
      std::lock_guard lock(s.mutex);
      std::shared_ptr<State::Request> owned;
      for(const auto& [serial,r]:s.requests){
        const auto& entry=r->completion;
        if(!r->done && !r->client_published && entry.entered && !entry.returned &&
           entry.frame.thread==frame->thread && entry.frame.pcr==frame->pcr &&
           entry.frame.process==frame->process && entry.frame.sp>=0xA0 &&
           entry.frame.sp-0xA0==frame->sp && r->task_reference && task && NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) &&
           uint32_t(c->r[30])==r->overlap && uint32_t(c->r[31])==r->packet &&
           uint32_t(c->r[27])==r->result && uint32_t(c->r[26])==0 &&
           uint32_t(c->r[28])==(int32_t(r->result)<0?0x65Bu:0u)){
          if(owned){(s.RecordFirstUncertainty(__LINE__,60,&*frame,kComplete), s.uncertain=true);r->uncertain=true;owned->uncertain=true;return;}
          owned=r;
        }
      }
      if(!owned)return; // Other app families/default paths are unchanged.
      owned->client_published=true;
      if(owned->call==NativeSystemUiPublicCall::kAvatarInitialize)
        s.RecordDiagnostic(s.DiagnosticLocked(State::DiagnosticPhase::kCompletionPublication,__LINE__,owned->result,1,*frame,owned.get(),nullptr,task.value_or(0)));
      client=std::move(owned->backing);overlap=std::move(owned->overlap_backing);
      // Added native Task ref, packet record, caller code/domain, Session,
      // engine arena and five callback modules remain through actual cleanup.
    }
    // These are backing/protection tokens, not caller allocator ownership.
    // Expire BEFORE the ORIGINAL Status store can wake a poller/event/APC.
    // No guest completion, signal, status write or native free is performed.
    client.reset();overlap.reset();
  }catch(...){if(const auto service=Lookup(c->kernel_state))service->state_->Poison(__LINE__,17,{},nullptr,0);}
}
void NativeAvatarService::ObserveReturn(void* raw,uint64_t address) noexcept {
  State::RequestEntry request_entry;
  auto* c=static_cast<cpu::ppc::PPCContext*>(raw);
  if(!c || !c->kernel_state)return;
  try{
    const auto service=Lookup(c->kernel_state);
    if(!service)return;
    const auto frame=ActualFrame(*c,c->kernel_state);
    if(!frame)return;
    auto& state=*service->state_;
    {
      std::lock_guard lock(state.mutex);
      const auto it=state.entries.find({frame->thread,uint32_t(address)});
      if(it==state.entries.end() || it->second.empty())return; // unobserved ingress
      const auto paired=it->second.ObserveReturn(*frame,&request_entry);
      if(paired==NativeAvatarEntryReturn::kMismatch){
        (state.RecordFirstUncertainty(__LINE__,61,&*frame,uint32_t(address),&it->second.expected()), state.uncertain=true);return;
      }
      if(it->second.empty())state.entries.erase(it);
      // An exact passthrough/denied entry owns no Avatar Request. Retire only
      // its marker, preserving the outer owned frame and real native result.
      if(paired!=NativeAvatarEntryReturn::kOwned)return;
    }
    if(const auto* profile=PublicAt(uint32_t(address)))state.LeavePublic(*c,*frame,*profile);
    else state.LeaveInternal(*c,*frame,uint32_t(address),request_entry);
  }catch(...){if(const auto s=Lookup(c->kernel_state))s->state_->Poison(__LINE__,18,{},nullptr,0);}
}

bool NativeAvatarService::PrepareFrontendResident(
    XThread& thread, uint32_t owned_task, uint32_t argument, std::string* error) {
  auto& s = *state_;
  const auto fail = [&](const char* text) { if(error)*error=text; return false; };
  if (!XThread::IsInThread(&thread) || !thread.thread_state() || argument > 1 ||
      owned_task < 0x1000 || (owned_task & 3) != 1 || !s.RootCurrent(kFrontend) || !s.RootCurrent(kBuiltin) ||
      !s.RootCurrent(kF3Lock)) return fail("Native FE resident root/context unavailable");
  const auto frame=ActualFrame(*thread.thread_state()->context(),s.config.kernel);
  const auto task=s.CurrentTaskCached(thread);
  if(!frame || frame->process!=X_PROCTYPE_SYSTEM || !task || !NativeAvatarTaskReferenceMatchesBody(owned_task,*task))
    return fail("Native FE resident actual owned Task mismatch");
  auto resident=std::make_shared<State::FrontendResident>();
  resident->caller=*frame;resident->thread=retain_object(&thread);
  resident->task_reference_handle=owned_task;resident->task_body=*task;resident->argument=argument;
  {
    std::lock_guard lock(s.mutex);
    if(s.stopping || s.uncertain || s.frontend_resident || s.f3_setup ||
       s.f3_loaded || s.f3_ready)return fail("Native FE resident scope already used/closed");
    s.frontend_resident=std::move(resident);
  }
  if(error)error->clear();return true;
}

bool NativeAvatarService::ObserveFrontendResident(
    XThread& thread, uint32_t owned_task,
    const GuestRuntime::ExecutionResult& actual, std::string* error) {
  auto& s=*state_;
  const auto fail=[&](const char* text){s.Poison(__LINE__,19,{},nullptr,0);if(error)*error=text;return false;};
  if(!XThread::IsInThread(&thread) || !thread.thread_state() || !s.RootCurrent(kFrontend))
    return fail("Native FE resident result context changed");
  const auto frame=ActualFrame(*thread.thread_state()->context(),s.config.kernel);
  const auto task=s.CurrentTaskCached(thread);
  std::shared_ptr<const NativeAvatarBackingLease> packet;
  Frame fe,setup,load,callback;
  bool loaded=false;
  {
    std::lock_guard lock(s.mutex);
    const auto& r=s.frontend_resident;
    if(!frame || !task || !r || r->caller!=*frame || r->task_reference_handle!=owned_task ||
       !NativeAvatarTaskReferenceMatchesBody(owned_task,*task) || *task!=r->task_body ||
       s.uncertain || r->observed || !r->frontend.returned ||
       r->frontend_result!=uint32_t(actual.return_value) ||
       (r->builtin.entered && !r->builtin.returned) ||
       (r->setup.entered && !r->setup.returned) ||
       (r->load.entered && !r->load.returned) ||
       (r->callback.entered && !r->callback.returned) ||
       (actual.succeeded && (!r->builtin.returned || !r->setup.returned ||
         !r->load.returned || !r->callback.returned || !r->lock_entered || !s.f3_loaded))) {
      (s.RecordFirstUncertainty(__LINE__,62,frame ? &*frame : nullptr,kFrontend), s.uncertain=true);if(error)*error="Native FE resident actual return/lifecycle incomplete";
      return false;
    }
    r->observed=true;
    fe=r->frontend.frame;setup=r->setup.frame;load=r->load.frame;callback=r->callback.frame;
    loaded=s.f3_loaded;packet=std::move(r->packet);
  }
  static xe::NativeSystemUiTraceLimiter trace;
  trace.EmitTransition((uint64_t(owned_task)<<32)|uint32_t(actual.return_value),
      "avatar","frontend_resident_observed",
      {{"task",owned_task},{"result",uint32_t(actual.return_value)},
       {"resident_loaded",loaded},{"fe_call_succeeded",actual.succeeded},{"fe_sp",fe.sp},{"fe_lr",fe.lr},
       {"setup_sp",setup.sp},{"setup_lr",setup.lr},
       {"load_sp",load.sp},{"load_lr",load.lr},
       {"callback_sp",callback.sp},{"callback_lr",callback.lr}});
  // Native synchronous packet was live through its true callback/load/FE
  // returns; release outside the owner mutex. Failed or unreturned scopes stay.
  if(error)error->clear();return true;
}

bool NativeAvatarService::AdoptStartup(
    std::string_view identity, XThread& thread, std::string* error) {
  auto& s=*state_;
  const bool setup=identity=="native-avatar-f3-descriptor-setup";
  const bool load=identity=="native-avatar-f3-load";
  const auto fail=[&](const char* text){if(error)*error=text;return false;};
  if((!setup && !load) || !XThread::IsInThread(&thread) || !thread.thread_state() ||
     !s.RootCurrent(load?0x816BCD08:0x816BCC38) || !s.ReaderCurrent() ||
     !s.config.content->CaptureAvatarSource(*s.config.calls))
    return fail("Native F3 adoption lacks real reader/lookup/open/source");
  const auto frame=ActualFrame(*thread.thread_state()->context(),s.config.kernel);
  const auto task=s.CurrentTaskCached(thread);
  auto& memory=*s.config.kernel->memory();
  if(Word(memory,0x81AA13B8)!=0xF3u || Word(memory,0x81AA13C0)!=kF3 ||
     Word(memory,0x81AA13BC)!=s.config.calls->module()->hmodule_ptr() ||
     Word(memory,0x81AA13C4)!=1u)
    return fail("Native F3 adoption descriptor/module/init changed");
  Frame original;
  uint32_t origin_task=0,original_result=0;
  {
    std::lock_guard lock(s.mutex);
    const auto& r=s.frontend_resident;
    if(!frame || !task || !r || r->caller!=*frame || !NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task) || *task!=r->task_body ||
       !r->observed || !r->frontend.returned || !r->setup.returned ||
       !r->load.returned || !r->callback.returned || !r->lock_entered ||
       !s.f3_loaded || s.uncertain || s.stopping || r->aborting ||
       (setup && r->setup_adopted) ||
       (load && (!r->setup_adopted || r->load_adopted)))
      return fail("Native F3 receipt adoption once/context/lifetime denied");
    original=setup?r->setup.frame:r->load.frame;
    origin_task=r->task_reference_handle;original_result=load?r->load_result:0;
    if(setup)r->setup_adopted=true;
    else {r->load_adopted=true;s.f3_ready=true;}
  }
  static xe::NativeSystemUiTraceLimiter trace;
  trace.EmitTransition((uint64_t(origin_task)<<32)|(setup?0x816BCC38u:0x816BCD08u),
      "avatar","resident_receipt_adopted",
      {{"address",setup?0x816BCC38u:0x816BCD08u},{"executed_now",0},
       {"origin_fe",kFrontend},{"origin_task",origin_task},
       {"origin_sp",original.sp},{"origin_lr",original.lr},
       {"native_result_observed",load},{"original_native_result",original_result}});
  // This is adoption of actual observed native work. No descriptor writer,
  // loader, RWLock initializer or request output is executed a second time.
  if(error)error->clear();return true;
}

bool NativeAvatarService::AbortFrontendResident(
    XThread& thread, uint32_t owned_task) noexcept try {
  auto& s=*state_;
  if(!XThread::IsInThread(&thread) || !thread.thread_state())return false;
  const auto frame=ActualFrame(*thread.thread_state()->context(),s.config.kernel);
  const auto task=s.CurrentTaskCached(thread);
  {
    std::lock_guard lock(s.mutex);
    const auto& r=s.frontend_resident;
    if(!r)return true; // FE was never prepared; no resident work to close.
    if(!frame || !task || r->caller!=*frame || r->task_reference_handle!=owned_task ||
       !NativeAvatarTaskReferenceMatchesBody(owned_task,*task) || *task!=r->task_body ||
       s.uncertain || !r->observed || !r->frontend.returned)
      return false; // No invented cleanup for an unreturned/unknown FE frame.
    if(!s.f3_setup && !s.f3_loaded)return true;
    if(r->aborting || s.f3_unload || s.session || !s.requests.empty())return false;
    r->aborting=true;s.stopping=true;s.f3_shutdown_entered=true;
  }
  // Allocation/code/providers are still healthy here. This invokes the same
  // original synchronous unload Task protocol used by ordinary healthy stop.
  const std::array<uint32_t,2> args{0xF3,1};
  if(!s.Helper(thread,kUnload,args).executed){s.Poison(__LINE__,20,{},nullptr,0);return false;}
  auto& memory=*s.config.kernel->memory();
  const auto module=Word(memory,0x81AA13BC);
  const bool cleared=module && (!*module || *module==UINT32_MAX) &&
      Word(memory,0x81AA13C0)==0u && Word(memory,0x81AA13C4)==0u &&
      Word(memory,0x81AA1438)==0u && Word(memory,0x81AA143C)==0u;
  {
    std::lock_guard lock(s.mutex);
    if(!cleared || s.uncertain || !s.f3_unload || !s.f3_unload->entry.returned ||
       !s.f3_unload->worker.returned || s.f3_unload->task_reference ||
       s.f3_unload->uncertain){(s.RecordFirstUncertainty(__LINE__,63,nullptr,0), s.uncertain=true);return false;}
    s.f3_loaded=false;s.f3_ready=false;s.f3_setup=false;
  }
  return true;
} catch(...){state_->Poison(__LINE__,21,{},nullptr,0);return false;}

void NativeAvatarService::BeginStop() noexcept {
  try{std::lock_guard lock(state_->mutex);state_->stopping=true;state_->changed.notify_all();}
  catch(...){}
}
bool NativeAvatarService::Drained() const noexcept {
  try{std::lock_guard lock(state_->mutex);
    return state_->stopping && !state_->uncertain && state_->shutdown_done &&
        !state_->f3_ready && !state_->f3_loaded && !state_->session &&
        (!state_->frontend_resident || state_->frontend_resident->observed) &&
        (!state_->f3_unload || (state_->f3_unload->entry.returned &&
          !state_->f3_unload->task_reference && !state_->f3_unload->uncertain)) &&
        state_->requests.empty() && state_->public_calls.empty() &&
        state_->invocations.empty() && state_->entries.empty();
  }catch(...){return false;}
}

namespace {
constexpr std::string_view kAssetRoot="\\System??\\AvatarAssetPack";
bool SameAssetRoot(std::string_view root){
  return root.size()==kAssetRoot.size() && std::equal(root.begin(),root.end(),
      kAssetRoot.begin(),[](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);});
}
bool SameSource(const xam::ContentSourceState& a,const xam::ContentSourceState& b){
  return a.available && b.available && a.source.id==b.source.id &&
      a.generation==b.generation && a.storage==b.storage &&
      a.source.kind==xam::ContentSourceKind::kInstalledSystem && a.source.read_only;
}
}
std::optional<uint32_t> NativeAvatarService::TryOpenAssetPack(
    KernelState* kernel,uint32_t user,std::string_view root,
    std::string_view package,uint32_t flags,uint32_t unknown,
    uint32_t license,uint32_t overlap) noexcept{
  try{
    const auto service=Lookup(kernel);
    if(!service)return {};
    auto& s=*service->state_;
    auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
    const auto frame=thread?ActualFrame(*thread->thread_state()->context(),kernel):std::nullopt;
    if(!frame || frame->lr!=0x8196B758)return {};
    s.RecordDiagnostic(State::PartialDiagnostic(State::DiagnosticPhase::kAssetOpenEntry,__LINE__,0,0,*frame));
    if(frame->process!=X_PROCTYPE_SYSTEM || !SameAssetRoot(root) ||
       user!=0xFF || flags!=0x43 || unknown || license || overlap){
      s.RecordDiagnostic(State::PartialDiagnostic(State::DiagnosticPhase::kAssetOpenArguments,__LINE__,X_ERROR_INVALID_PARAMETER,2,*frame));
      return X_ERROR_INVALID_PARAMETER;
    }
    const auto task=s.CurrentTaskCached(*thread);
    const auto source=s.config.content->CaptureAvatarSource(*s.config.calls);
    const auto observed=s.config.content->snapshot();
    std::shared_ptr<State::Session> held;
    {std::lock_guard lock(s.mutex);held=s.session;
      uint32_t evaluated=0,failed=0,core_evaluated=0,core_passed=0,core_candidates=0,last_core_task_reference=0;
      uint64_t last_core_request=0;
      const auto rejected=[&](uint32_t bit,bool value) noexcept {
        evaluated|=1u<<bit;if(value)failed|=1u<<bit;return value;
      };
      const auto core_observed=[&](uint32_t bit,bool value) noexcept {
        core_evaluated|=1u<<bit;if(value)core_passed|=1u<<bit;return value;
      };
      const bool denied=
          rejected(0,!held) || rejected(1,s.uncertain) ||
          rejected(2,bool(held->asset_mount)) || rejected(3,held->asset_opening) ||
          rejected(4,!task) || rejected(5,!source) || rejected(6,!SameSource(held->source,*source)) ||
          rejected(7,package!=observed.package_path) ||
          rejected(8,std::none_of(s.requests.begin(),s.requests.end(),[&](const auto& item){
            const auto& r=item.second;
            ++core_candidates;last_core_request=r->serial;last_core_task_reference=r->task_reference_handle;
            return core_observed(0,r->session==held) &&
              core_observed(1,NativeAvatarTaskReferenceMatchesBody(r->task_reference_handle,*task)) &&
              core_observed(2,r->core_init.entered) && core_observed(3,!r->core_init.returned) &&
              core_observed(4,!r->uncertain);}));
      auto diagnostic=s.DiagnosticLocked(State::DiagnosticPhase::kAssetOpenAdmission,__LINE__,
          denied?X_ERROR_ACCESS_DENIED:X_ERROR_SUCCESS,2,*frame,nullptr,nullptr,task.value_or(0));
      diagnostic.evaluated=evaluated;diagnostic.failed=failed;
      diagnostic.core_evaluated=core_evaluated;diagnostic.core_passed=core_passed;
      diagnostic.core_candidates=core_candidates;diagnostic.last_core_request=last_core_request;
      diagnostic.last_core_task_reference=last_core_task_reference;
      if(source){diagnostic.available|=64;diagnostic.observed_source_generation=source->generation;}
      s.RecordDiagnostic(diagnostic);
      if(denied)return X_ERROR_ACCESS_DENIED;
      held->asset_opening=true;
    }
    std::shared_ptr<vfs::Device> opened;
    xam::BorrowedContentMountDiagnostic mount;
    const auto result=kernel->content_manager()->OpenContentFile(root,package,
        &held->source,0,&opened,&mount);
    {std::lock_guard lock(s.mutex);held->asset_opening=false;
      auto diagnostic=s.DiagnosticLocked(State::DiagnosticPhase::kAssetOpenReturn,__LINE__,result,2,*frame,nullptr,nullptr,task.value_or(0));
      diagnostic.mount=mount;s.RecordDiagnostic(diagnostic);
      // Even an unexpected/uncertain return retains any actual published
      // device. No destructor or alias-only cleanup can close a replacement.
      if(opened)held->asset_mount=opened;
      if(result==X_ERROR_SUCCESS && (!opened || !opened->is_read_only() ||
          opened->namespace_retiring() || s.session!=held))(s.RecordFirstUncertainty(__LINE__,64,&*frame,0), s.uncertain=true);
    }
    return result;
  }catch(...){if(const auto s=Lookup(kernel))s->state_->Poison(__LINE__,22,{},nullptr,0);return X_ERROR_FUNCTION_FAILED;}
}
std::optional<uint32_t> NativeAvatarService::TryCloseAssetPack(
    KernelState* kernel,std::string_view root,uint32_t overlap) noexcept{
  try{
    const auto service=Lookup(kernel);
    if(!service || !SameAssetRoot(root))return {};
    auto& s=*service->state_;
    auto* thread=XThread::IsInThread()?XThread::GetCurrentThread():nullptr;
    const auto frame=thread?ActualFrame(*thread->thread_state()->context(),kernel):std::nullopt;
    std::shared_ptr<State::Session> held;std::shared_ptr<vfs::Device> device;
    {std::lock_guard lock(s.mutex);held=s.session;
      if(!held || !held->asset_mount)return {};
      // A retained device is an ownership proof, not a name-selected reopen.
      // Foreign public Close cannot retire the active native engine's mount.
      if(!frame || frame->process!=X_PROCTYPE_SYSTEM || frame->lr!=0x8196B674 ||
         overlap || !held->asset_cleanup_frame || held->asset_closing ||
         held->asset_closed)return X_ERROR_ACCESS_DENIED;
      for(size_t n=0;n<2;++n)
        if(held->cleanup_objects[n] && !held->cleanup_release_returned[n])
          return X_ERROR_ACCESS_DENIED;
      device=held->asset_mount;held->asset_closing=true;
    }
    const auto result=kernel->content_manager()->CloseContent(root,device.get());
    {std::lock_guard lock(s.mutex);held->asset_closing=false;
      if(result==X_ERROR_SUCCESS)held->asset_closed=true;
      // Failure remains visible through native open flag and retained owner.
    }
    return result;
  }catch(...){if(const auto s=Lookup(kernel))s->state_->Poison(__LINE__,23,{},nullptr,0);return X_ERROR_FUNCTION_FAILED;}
}

bool NativeAvatarService::RetireAfterActualKernelStop() noexcept try {
  if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return false;
  KernelState* kernel;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->stopped_retired)return true;
    kernel=state_->config.kernel;
  }
  if(!kernel || !Drained())return false;
  for(const auto& thread:kernel->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
    if(thread->is_running())return false;
  Config config;
  std::shared_ptr<const GuestRuntimeCallExecutor::AvatarProviderBinding> binding;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  NativeImportDomain::Operation domain;
  std::shared_ptr<State::Control> control;
  std::shared_ptr<State::FrontendResident> resident;
  {
    std::lock_guard lock(state_->mutex);
    if(state_->config.kernel!=kernel || !state_->stopping || state_->uncertain ||
        !state_->shutdown_done || state_->f3_ready || state_->f3_loaded ||
        state_->session ||
        (state_->frontend_resident && !state_->frontend_resident->observed) ||
        !state_->requests.empty() ||
        !state_->public_calls.empty() || !state_->invocations.empty() || !state_->entries.empty() ||
        (state_->f3_unload && (!state_->f3_unload->entry.returned ||
          state_->f3_unload->task_reference || state_->f3_unload->uncertain)))return false;
    state_->stopped_retired=true;
    config=std::move(state_->config);state_->config={};
    binding=std::move(state_->binding);root_code=std::move(state_->root_code);
    domain=std::move(state_->resident_domain);control=std::move(state_->f3_unload);
    resident=std::move(state_->frontend_resident);
  }
  // Strict normal shutdown was already proved. The actual stopped Kernel now
  // permits resource revocation, not a new native unload/Complete/free claim.
  if(control)control->stack.reset();
  if(resident){resident->packet.reset();resident->thread.reset();}
  return true;
} catch(...) {return false;}

bool NativeAvatarService::ShutdownOnBootstrap(XThread& thread) noexcept{
  try{
    auto& s=*state_;
    if(s.stopped_retired.load(std::memory_order_acquire))return false;
    BeginStop();
    const auto frame=ActualFrame(*thread.thread_state()->context(),s.config.kernel);
    if(!frame || frame->process!=X_PROCTYPE_SYSTEM ||
       thread.thread_id()!=s.config.allocation->bootstrap_thread_id())return false;
    {
      std::lock_guard lock(s.mutex);
      if(s.shutdown_claimed)return s.shutdown_done && !s.uncertain && !s.session &&
          !s.f3_ready && !s.f3_loaded && s.requests.empty() && s.invocations.empty() && s.entries.empty();
      s.shutdown_claimed=true;
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    // Existing original-caller C++ scopes finish while native tasks and every
    // provider are healthy. Do not block a scheduler lock or reassign process.
    for(;;){
      bool ready=false;
      {std::lock_guard lock(s.mutex);if(s.uncertain)return false;
        ready=s.public_calls.empty() && s.invocations.empty();}
      if(ready)break;
      if(std::chrono::steady_clock::now()>=deadline)return false;
      thread.Delay(0,0,uint64_t(-160000ll));
    }
    std::shared_ptr<State::Session> held;
    {std::lock_guard lock(s.mutex);held=s.session;}
    if(held){
      auto invocation=std::make_shared<State::Invocation>();
      invocation->call=NativeSystemUiPublicCall::kAvatarShutdown;
      invocation->original=*frame;invocation->bootstrap_close=true;
      invocation->caller_code.push_back({s.config.calls->module(),s.root_code});
      {std::lock_guard lock(s.mutex);s.invocations[&thread].push_back(invocation);}
      const auto result=s.config.calls->ExecuteAvatarRegistersOnOriginalCaller(
          thread,NativeSystemUiPublicCall::kAvatarShutdown,{});
      {std::lock_guard lock(s.mutex);
        const auto it=s.invocations.find(&thread);
        if(!result.succeeded || it==s.invocations.end() || it->second.empty() ||
           it->second.back()!=invocation || s.session || s.uncertain){(s.RecordFirstUncertainty(__LINE__,65,&*frame,0), s.uncertain=true);return false;}
        it->second.pop_back();if(it->second.empty())s.invocations.erase(it);
      }
    }
    // F3 is a separate resident owner. Finish all real profile requests before
    // its original unload; the returned Start/dispatch count is not drainage.
    for(;;){
      bool ready=false;
      {std::lock_guard lock(s.mutex);if(s.uncertain)return false;ready=s.requests.empty();}
      if(ready)break;
      if(std::chrono::steady_clock::now()>=deadline)return false;
      thread.Delay(0,0,uint64_t(-160000ll));
    }
    bool f3=false;
    {std::lock_guard lock(s.mutex);f3=s.f3_loaded;if(f3)s.f3_shutdown_entered=true;}
    if(f3){
      const std::array<uint32_t,2> args{0xF3,1};
      if(!s.Helper(thread,kUnload,args).executed){s.Poison(__LINE__,24,{},nullptr,0);return false;}
      auto& memory=*s.config.kernel->memory();
      const auto module=Word(memory,0x81AA13BC);
      if(!module || (*module && *module!=UINT32_MAX) ||
         Word(memory,0x81AA13C0)!=0u || Word(memory,0x81AA13C4)!=0u ||
         Word(memory,0x81AA1438)!=0u || Word(memory,0x81AA143C)!=0u){s.Poison(__LINE__,25,{},nullptr,0);return false;}
      {std::lock_guard lock(s.mutex);s.f3_ready=false;s.f3_loaded=false;s.f3_setup=false;}
    }
    {std::lock_guard lock(s.mutex);s.shutdown_done=true;s.changed.notify_all();}
    return Drained();
  }catch(...){state_->Poison(__LINE__,26,{},nullptr,0);return false;}
}
}  // namespace xe::kernel
