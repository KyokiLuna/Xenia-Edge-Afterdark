/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_media_contract.h"
#include <array>
#include <cassert>
namespace xe::kernel {
namespace {
constexpr NativeMediaCodeProfile kCode[]={
    {0x819CF4A0,12,"b626c3d67264c6d5eeb83b7ce2777211415982a7c53e0b883e43eb1e9ae1ee67"},
    {0x819CF4D8,124,"87cf2614d7cfc68f9a85a38358b3fbb7d2b4771a24c5f146feff01ccbd1c303d"},
    {0x819D69B0,68,"545a8a8c44d74f287616935bf54c6f0152220b2b4ed0f4191951888d52c219e2"},
    {0x81860E98,192,"58c160ae1228ab6770707af653320ded7300de020bca44510dbcc042c25614f3"},
    {0x818613D0,32,"05a1643db51ca9e7dbae753d5e9f974b7c38629977f9eb6d83a02e79f0b8140c"},
    {0x81861750,32,"1f044a6c2d0212cc0199221e326a081595b353be0f0938db55f4f800cd631f0e"},
    {0x819CF4C0,20,"43ac24d1bbb326ce7ef34c8b20d1d6084fe55df1f51cf81172fe8c6d081be6ca"},
    {0x818610E8,192,"2f958a656d21250af3d8ec2858b641973c9e012e7dc73377e6945c86eef1994e"},
    {0x818611A8,96,"438346058b919c575100b7281ac6426a298e57dd60a4560f8bc80452553c1025"},
    {0x81863130,196,"8878bb9afc180f84cd03b8b1eef3029a8e8445cf9e70f00d54dfe7cb833f1ac7"},
    {0x81863508,96,"6b2e044c010ac3eb91691b7ed545cfa3c2892960ac6fde6600e0b7f9462662a6"},
    {0x8187A5D8,196,"bf451e9c4cde8f1089d949115f329461b894feb36ac6d35c241df02dab1da531"},
    {0x8187F310,176,"b6f3904e127ab42bb42eebfe03f266ed270e6371af3d38be3759539d1bae8990"},
    {0x8187F3C0,76,"25b17dd563baaada3c8c538b93200061e56424283ac77ed3504cc74469b79133"},
    {0x81880538,204,"c64ceac12279092100120b58d79c0aec7d26d1e5e01666da4ce39ff2fb070a81"},
    {0x81885DD0,100,"651f05c93daeb77ce676f5097c7b74a4cec906f15a6b83e9ef5468042b79ace7"},
    {0x819CF630,236,"008006569ab66ff7143b4e0f7d418de7bc9495bf79a378eff2ee58d9c19e1ebc"},
    {0x819CF7E8,276,"dcd84c30ae43636f0ea4fea143b0f42dacd634b7ffa6e794a359a0c3af2e1e1d"},
    {0x819CF900,188,"ce0f72337fbd431d002a42062162f47b1f09e20a2d399d8522681fe7f16dcbeb"},
    {0x819CFB80,64,"f3334c3b47b4f9c3bc36f48806678ea5d047f65351a0eaf0303d7086f8cea99d"},
    {0x819D6A30,116,"47d0f4e62e153cdb8547f4ef690ddeacd2411be89eacd4ef3a250c8faa5d78c5"},
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
bool NativeMediaDirectOriginalArgumentsValid(uint16_t ordinal,
    std::span<const uint32_t> args) noexcept {
  return ordinal==0x1F4 && args.size()==4 && args[0]==kNativeMediaApp &&
      (args[1]==kNativeMediaSourceMetadata || args[1]==kNativeMediaSourceStream);
}
bool NativeMediaDirectPayloadValid(uint32_t message,
                                  std::span<const uint32_t> words) noexcept {
  if(words.size()!=5 || !words[4])return false;
  if(message==kNativeMediaSourceMetadata)return true;
  return message==kNativeMediaSourceStream && words[1];
}
bool NativeMediaFileIdentityValid(std::span<const uint8_t> bytes) noexcept {
  if(bytes.size()!=kNativeMediaFileIdentityBytes)return false;
  const auto descriptor=bytes.subspan(kNativeMediaFileDescriptorOffset);
  const uint8_t tag=descriptor[kNativeMediaFileNameBytes];
  if(tag!=1 && tag!=2)return false;
  // Real row writer8187ECB0 allows261 UTF16 units INCLUDING the terminator.
  // Tag2's native factory skips the first six bytes before consuming the name.
  for(size_t at=tag==2?6:0;at<kNativeMediaFileNameBytes;at+=2)
    if(!descriptor[at] && !descriptor[at+1])return true;
  return false;
}
} // namespace xe::kernel
