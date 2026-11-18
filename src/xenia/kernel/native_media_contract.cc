/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_media_contract.h"
#include <array>
#include <cassert>
namespace xe::kernel {
namespace {
constexpr NativeMediaCodeProfile kCode[]={
    {0x816F0F78,200,"7845482a8c323e655b579879e48561d1a076a01c8327ceee3928b4c8de083e9f"},
    {0x816F1040,48,"afbb46e42943a25e0349b6c2ed591918284e1efb0bc7e60eec1ce83274cb7dba"},
    {0x816F1070,396,"a74b6069229dd63dd3d4f7bfe6b47c97fea3ae4760c6b1c420b4d5a76c17684f"},
    {0x816F1200,184,"82ff73cdeb1bd4af925740a938ebc5c9aef65ee95f43cd5dbc5493a07376de77"},
    {0x816F0F30,72,"3549385979932232442376c3bb07fc90c061b3e43dec7ade36f42930026f0748"},
    {0x816E6B10,4,"daed2a4b8232a23602e84aac7b72cde141e560de5686da79b721bb5dcbfd41ff"},
    {0x816E6CE0,100,"4d84cea1d88a6f3fea39a8a38744a5148615e2a54ec20c53ec67ef66d8a8db88"},
    {0x816A2FF8,300,"ba25abae13eded052f59a02e329eba7add73e847ea975cc67b2eeac809fd9754"},
    {0x816A2F88,108,"54aa9344cb2e1d5b8b6999a513a4356308f1b87452aefe73d1ad574b4b2b3e74"},
    {0x816A2F80,8,"63324809f1fc1f0af90fe10acf82eb80b80f5852144749f4a1341a5a4890a292"},
    {0x816A2CF0,656,"e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a"},
    {0x816A26F8,352,"b1af64f353766148e34db02ed938743371561d2ff3d694bb5a8209e5c205051a"},
    {0x816A2588,364,"cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
    {0x816A2A48,128,"7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
    {0x816A2C80,108,"21da95341366e83781ab2e46696f0c04afa43d71ae7c11f91dc8e108d1b1db94"},
    {0x8187AE98,500,"10719a8a199fab0c595e7988fa8b1f9db53914a569c2d08cc76e4d056b384e80"},
    {0x818621F0,80,"cad803dfa465aeb989dbaba032b13734845d90b63f4cfc69e6983d09c36b4395"},
    {0x81862318,140,"ee8019541a28620a4d2de9c2b4029a502eca9732f3628c4fcffcfe84eabc4ffd"},
    {0x81862240,216,"dbc0c523cb13528347eac9edda81e53b0e7007f6adc7921d9b10d9b119282605"},
    {0x8187EA98,52,"d17381aa046360712db992bc3053a894e803db48b2d99f008b41afed2872fdc5"},
    {0x818625A0,96,"457c56609db2e1890fb2e8d49c23f9dd1368eaf783c1fba4a1a33487d7474f12"},
    {0x81703410,328,"b66a1f979cef85a62d0733dbab132330da58e2892a8b5c367b3408033c1f0a35"},
    {0x81706738,120,"7c29ce339ccfe8aaf8c7f4cfbc18229162c5136032046cb0163ca1cd4fb6f6fc"},
    {0x816FF868,316,"93a4a1f1350a2eb9e9a79b51fb6ea38b0bc4ac35bfb323d821e3523f31c2c8fc"},
    {0x81706478,644,"ca6b952d86f37408cf0128888b5dcde25ff08dac43abf751b4042fc336866c66"},
    {0x81703738,1476,"534ae4c07ee31f4885e5c296c657aaaea8a0fe3b0fa8c85f2939c6370bc1c37f"},
    {0x81702370,216,"9c4f75c5a61a8b248bded46ee00c68dae00f733548f5fb0c639f8a8f73b45d9a"},
    {0x81862BB0,108,"f6bd793223f295bf05160b2b6f7196350f50609d3836cd0cdd8a4106bed01a89"},
};
constexpr std::array<NativeMediaOriginalProfile,8> kOriginal{{
    {NativeMediaOriginalCall::kCreate,0x816F0F78,8,false},
    {NativeMediaOriginalCall::kGetPrivate,0x816F1040,2,false},
    {NativeMediaOriginalCall::kEnumerate,0x816F1070,6,false},
    {NativeMediaOriginalCall::kMessage,0x816A2F88,4,false},
    {NativeMediaOriginalCall::kStart,0x816A2F80,5,false},
    {NativeMediaOriginalCall::kStartEx,0x816A2CF0,6,false},
    {NativeMediaOriginalCall::kCancel,0x816A26F8,2,false},
    {NativeMediaOriginalCall::kCloseProcedure,0x816F1200,2,true},
}};
}
std::span<const NativeMediaCodeProfile> NativeMediaCodeProfiles(){return kCode;}
const NativeMediaCodeProfile* NativeMediaCode(uint32_t address){
  for(const auto& code:kCode)if(code.address==address)return &code;
  return nullptr;
}
const NativeMediaOriginalProfile& NativeMediaOriginal(NativeMediaOriginalCall call){
  const auto index=static_cast<size_t>(call);assert(index<kOriginal.size());return kOriginal[index];
}
bool NativeMediaCreateArgumentsValid(std::span<const uint32_t> args) noexcept {
  return args.size()==8 && args[1]==kNativeMediaApp &&
      args[2]==kNativeMediaQueryRead && args[3]==kNativeMediaQueryClose &&
      args[4]==kNativeMediaQueryBytes && args[7]!=0;
}
} // namespace xe::kernel
